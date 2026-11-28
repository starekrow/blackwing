// The Linux surface against a private dbus-daemon that stands for both buses. An MPRIS
// client (written on dbus, as a shell would be) reads what the surface shows and sends it
// the transport's commands; stand-ins for the desktop portal and logind answer Inhibit and
// send PrepareForSleep, on a thread of their own. Skipped where dbus-daemon will not start.
#include "korvid/media_platform/media.h"
#include "korvid/dbus/dbus.h"
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/net.h"
#include "korvid/platform/process.h"
#include <string.h>
#include <unistd.h>

using namespace kv;

namespace {

bool start_daemon(Process& p, char* address, u32 cap) {
    const char* argv[] = {"dbus-daemon", "--session", "--nofork", "--print-address=1"};
    ProcessDesc d;
    d.argv = Span<const char* const>(argv, 4);
    if (!p.spawn(d)) return false;
    u32 n = 0;
    f64 end = time_now() + 5.0;
    while (time_now() < end) {
        p.wait(0.1);
        i32 k = p.read_out(address + n, cap - 1 - n);
        if (k > 0) n += (u32)k;
        address[n] = 0;
        char* nl = strchr(address, '\n');
        if (nl) { *nl = 0; return true; }
        if (k < 0) break;
    }
    return false;
}

// The portal and logind, as far as the surface uses them.
struct Services {
    DbusConnection* c = nullptr;
    OsThread        thread;
    bool            stop = false;          // atomic
    bool            portal_refuses = false; // atomic: answer Inhibit with an error, as no portal would
    u32             portal_inhibits = 0;   // atomic
    u32             portal_flags = 0;
    u32             portal_closes = 0;     // atomic
    u32             idle_inhibits = 0;     // atomic
    u32             sleep_inhibits = 0;    // atomic
    OsMutex         lock;
    NetSocket       held[8];               // the far end of each inhibitor's descriptor
    char            held_what[8][16];
    u32             nheld = 0;
};

const DbusMethodDesc kInhibit[] = {{"Inhibit", "sua{sv}", "o"}};
const DbusInterfaceDesc kPortalIface = {"org.freedesktop.portal.Inhibit", kInhibit, 1};
const DbusMethodDesc kClose[] = {{"Close"}};
const DbusInterfaceDesc kRequestIface = {"org.freedesktop.portal.Request", kClose, 1};
const DbusMethodDesc kLoginInhibit[] = {{"Inhibit", "ssss", "h"}};
const DbusSignalDesc kPrepare[] = {{"PrepareForSleep", "b"}};
const DbusInterfaceDesc kLoginIface = {"org.freedesktop.login1.Manager", kLoginInhibit, 1, nullptr, 0, kPrepare, 1};

void services_run(void* arg) {
    Services& s = *(Services*)arg;
    while (!atomic_load(&s.stop, MO_ACQUIRE)) {
        DbusMessage m;
        if (!dbus_next(s.c, &m)) { time_sleep(0.001); continue; }
        if (m.kind == DbusKind::Call && strcmp(m.interface, "org.freedesktop.portal.Inhibit") == 0) {
            if (atomic_load(&s.portal_refuses, MO_ACQUIRE)) {
                dbus_reply_error(s.c, m, "org.freedesktop.DBus.Error.ServiceUnknown", "no portal here");
            } else {
                DbusReader r = m.reader();
                const char* win; u32 flags = 0;
                r.str(&win); r.u32(&flags);
                s.portal_flags = flags;
                atomic_add_fetch(&s.portal_inhibits, 1u, MO_RELEASE);
                DbusWriter w;
                w.begin("o"); w.path("/org/freedesktop/portal/desktop/request/1/t");
                dbus_reply(s.c, m, &w);
                w.free();
            }
        } else if (m.kind == DbusKind::Call && strcmp(m.interface, "org.freedesktop.portal.Request") == 0) {
            atomic_add_fetch(&s.portal_closes, 1u, MO_RELEASE);
            dbus_reply(s.c, m, nullptr);
        } else if (m.kind == DbusKind::Call && strcmp(m.interface, "org.freedesktop.login1.Manager") == 0) {
            DbusReader r = m.reader();
            const char *what, *who, *why, *mode;
            r.str(&what); r.str(&who); r.str(&why); r.str(&mode);
            NetSocket a, b;
            net_socket_pair(&a, &b);
            DbusWriter w;
            w.begin("h");
            w.fd(a.fd);
            dbus_reply(s.c, m, &w);
            w.free();
            // Ours is closed once the reply has gone, so the far end sees the end only when
            // the surface closes its copy.
            time_sleep(0.02);
            a.close();
            os_mutex_lock(&s.lock);
            if (s.nheld < 8) {
                s.held[s.nheld] = b;
                b.fd = Handle{};
                strncpy(s.held_what[s.nheld], what, 15);
                s.held_what[s.nheld][15] = 0;
                ++s.nheld;
            }
            os_mutex_unlock(&s.lock);
            b.close();
            if (strcmp(what, "idle") == 0 && strcmp(mode, "block") == 0) atomic_add_fetch(&s.idle_inhibits, 1u, MO_RELEASE);
            if (strcmp(what, "sleep") == 0 && strcmp(mode, "delay") == 0) atomic_add_fetch(&s.sleep_inhibits, 1u, MO_RELEASE);
        }
        dbus_message_free(&m);
    }
}

// Whether the far end of the newest inhibitor of `what` has seen the surface let it go.
bool released(Services& s, const char* what, f64 seconds) {
    f64 end = time_now() + seconds;
    while (time_now() < end) {
        os_mutex_lock(&s.lock);
        i32 idx = -1;
        for (u32 i = 0; i < s.nheld; ++i) if (strcmp(s.held_what[i], what) == 0) idx = (i32)i;
        bool gone = false;
        if (idx >= 0) {
            char b[4];
            i32 k = s.held[idx].read(b, sizeof b);
            gone = k < 0;
        }
        os_mutex_unlock(&s.lock);
        if (gone) return true;
        time_sleep(0.005);
    }
    return false;
}

struct Woken { u32 n = 0; };
void on_wake(void* u) { atomic_add_fetch(&((Woken*)u)->n, 1u, MO_RELAXED); }

bool command_within(MediaSurface* s, MediaCommand* c, f64 seconds) {
    f64 end = time_now() + seconds;
    while (time_now() < end) {
        if (media_next_command(s, c)) return true;
        time_sleep(0.002);
    }
    return false;
}

bool interruption_within(MediaSurface* s, MediaInterruption* i, f64 seconds) {
    f64 end = time_now() + seconds;
    while (time_now() < end) {
        if (media_next_interruption(s, i)) return true;
        time_sleep(0.002);
    }
    return false;
}

// A client's call to the player.
bool call(DbusConnection* c, const char* dest, const char* iface, const char* member, DbusWriter* body, DbusMessage* reply) {
    DbusHeader h;
    h.destination = dest;
    h.path = "/org/mpris/MediaPlayer2";
    h.interface = iface;
    h.member = member;
    return dbus_call(c, h, body, 2.0, reply) && reply->kind == DbusKind::Return;
}

bool get_prop(DbusConnection* c, const char* dest, const char* iface, const char* name, DbusMessage* reply) {
    DbusWriter w;
    w.begin("ss"); w.str(iface); w.str(name);
    bool ok = call(c, dest, "org.freedesktop.DBus.Properties", "Get", &w, reply);
    w.free();
    return ok;
}

bool get_string(DbusConnection* c, const char* dest, const char* iface, const char* name, char* out, u32 cap) {
    DbusMessage r;
    if (!get_prop(c, dest, iface, name, &r)) { dbus_message_free(&r); return false; }
    DbusReader rd = r.reader();
    const char* s = "";
    bool ok = rd.variant_begin() && rd.str(&s);
    if (ok) { strncpy(out, s, cap - 1); out[cap - 1] = 0; }
    dbus_message_free(&r);
    return ok;
}

} // namespace

void test_mpris(TestCtx& c) {
    c.begin("media_platform, MPRIS on a private bus");
    Process daemon;
    char address[512] = {};
    if (!start_daemon(daemon, address, sizeof address)) {
        KV_INFO("test", "skipped: dbus-daemon would not start (%s)", daemon.error);
        daemon.close();
        return;
    }
    char why[200] = {};
    Services svc;
    os_mutex_init(&svc.lock);
    svc.c = dbus_open_address(address, 3.0, why, sizeof why);
    KV_CHECK(c, svc.c != nullptr);
    dbus_request_name(svc.c, "org.freedesktop.portal.Desktop", kDbusNameDoNotQueue, 2.0);
    dbus_request_name(svc.c, "org.freedesktop.login1", kDbusNameDoNotQueue, 2.0);
    const DbusInterfaceDesc* p1[] = {&kPortalIface};
    const DbusInterfaceDesc* p2[] = {&kRequestIface};
    const DbusInterfaceDesc* p3[] = {&kLoginIface};
    dbus_export(svc.c, "/org/freedesktop/portal/desktop", p1, 1);
    dbus_export(svc.c, "/org/freedesktop/portal/desktop/request/1/t", p2, 1);
    dbus_export(svc.c, "/org/freedesktop/login1", p3, 1);
    os_thread_start(&svc.thread, services_run, &svc);

    Woken woken;
    MediaDesc d;
    d.identity = "Blackwing (test)";
    d.name = "blackwing";
    d.can_raise = true;
    d.session_address = address;
    d.system_address = address;
    d.notify = on_wake;
    d.user = &woken;
    MediaSurface* s = media_open(d, why, sizeof why);
    KV_CHECK(c, s != nullptr);
    if (!s) { KV_WARN("test", "%s", why); atomic_store(&svc.stop, true, MO_RELEASE); os_thread_join(&svc.thread); dbus_close(svc.c); daemon.close(); return; }
    MediaInfo info = media_info(s);
    KV_CHECK(c, strcmp(info.name, "org.mpris.MediaPlayer2.blackwing") == 0 && strcmp(info.backend, "mpris") == 0 && info.sleep_watched);
    const char* dest = "org.mpris.MediaPlayer2.blackwing";

    // A second one takes the name with its instance (and leaves sleep to the first).
    MediaDesc d2 = d;
    d2.watch_sleep = false;
    MediaSurface* s2 = media_open(d2, why, sizeof why);
    KV_CHECK(c, s2 && strstr(media_info(s2).name, ".instance") != nullptr);
    media_close(s2);

    // A client: what a shell reads first.
    DbusConnection* cl = dbus_open_address(address, 3.0, why, sizeof why);
    KV_CHECK(c, cl != nullptr);
    char sv[256] = {};
    KV_CHECK(c, get_string(cl, dest, "org.mpris.MediaPlayer2", "Identity", sv, sizeof sv) && strcmp(sv, "Blackwing (test)") == 0);
    KV_CHECK(c, get_string(cl, dest, "org.mpris.MediaPlayer2.Player", "PlaybackStatus", sv, sizeof sv) && strcmp(sv, "Stopped") == 0);
    KV_CHECK(c, dbus_add_match(cl, "type='signal',path='/org/mpris/MediaPlayer2'", 2.0));

    // A track, playing: the shell hears it, and reads the position moving on its own.
    MediaTrack t;
    t.id = 42;
    strcpy(t.title, "Lament");
    strcpy(t.artist, "Ünïcødé Band");
    strcpy(t.album, "Dark Side");
    strcpy(t.path, "/music/a b/01 Lament.flac");
    strcpy(t.art, "/home/x/.cache/blackwing/art/1.jpg");
    t.length_us = 240000000;
    t.track_number = 1;
    media_set_track(s, &t);
    MediaStatus st;
    st.state = MediaState::Playing;
    st.position_us = 10000000;
    st.at = time_now();
    st.rate = 1.0;
    st.volume = 0.5;
    st.can_play = st.can_pause = st.can_next = st.can_previous = st.can_seek = true;
    media_set_status(s, st);
    bool heard_meta = false, heard_playing = false;
    f64 end = time_now() + 2.0;
    while ((!heard_meta || !heard_playing) && time_now() < end) {
        DbusMessage m;
        if (!dbus_next(cl, &m)) { time_sleep(0.002); continue; }
        if (m.kind == DbusKind::Signal && strcmp(m.member, "PropertiesChanged") == 0) {
            DbusReader r = m.reader();
            const char* iface;
            r.str(&iface);
            if (r.array_begin())
                while (r.array_more()) {
                    const char* k;
                    r.entry_begin(); r.str(&k); r.variant_begin();
                    if (strcmp(k, "PlaybackStatus") == 0) { const char* v; r.str(&v); heard_playing = strcmp(v, "Playing") == 0; }
                    else if (strcmp(k, "Metadata") == 0) {
                        bool title = false, art = false, id = false, url = false;
                        if (r.array_begin())
                            while (r.array_more()) {
                                const char* mk; const char* vs = "";
                                r.entry_begin(); r.str(&mk); r.variant_begin();
                                if (strcmp(mk, "xesam:title") == 0) { r.str(&vs); title = strcmp(vs, "Lament") == 0; }
                                else if (strcmp(mk, "mpris:artUrl") == 0) { r.str(&vs); art = strcmp(vs, "file:///home/x/.cache/blackwing/art/1.jpg") == 0; }
                                else if (strcmp(mk, "mpris:trackid") == 0) { r.path(&vs); id = strcmp(vs, "/net/blackwing/track/42") == 0; }
                                else if (strcmp(mk, "xesam:url") == 0) { r.str(&vs); url = strcmp(vs, "file:///music/a%20b/01%20Lament.flac") == 0; }
                                else r.skip();
                                r.variant_end(); r.entry_end();
                            }
                        heard_meta = title && art && id && url;
                    } else r.skip();
                    r.variant_end(); r.entry_end();
                }
        }
        dbus_message_free(&m);
    }
    KV_CHECK(c, heard_meta && heard_playing);
    time_sleep(0.2);
    DbusMessage r;
    i64 pos = 0;
    KV_CHECK(c, get_prop(cl, dest, "org.mpris.MediaPlayer2.Player", "Position", &r));
    DbusReader rd = r.reader();
    KV_CHECK(c, rd.variant_begin() && rd.i64(&pos) && pos > 10150000 && pos < 11000000);
    dbus_message_free(&r);
    // GetAll, with the live Position among the rest.
    DbusWriter w;
    w.begin("s"); w.str("org.mpris.MediaPlayer2.Player");
    KV_CHECK(c, call(cl, dest, "org.freedesktop.DBus.Properties", "GetAll", &w, &r));
    u32 keys = 0;
    bool has_pos = false;
    rd = r.reader();
    if (rd.array_begin())
        while (rd.array_more()) { const char* k; rd.entry_begin(); rd.str(&k); has_pos |= strcmp(k, "Position") == 0; rd.skip(); rd.entry_end(); ++keys; }
    KV_CHECK(c, keys == 13 && has_pos);
    dbus_message_free(&r);

    // The transport's commands, each queued for the caller, and the caller woken.
    u32 woke = atomic_load(&woken.n, MO_RELAXED);
    MediaCommand cmd;
    KV_CHECK(c, call(cl, dest, "org.mpris.MediaPlayer2.Player", "PlayPause", nullptr, &r)); dbus_message_free(&r);
    KV_CHECK(c, command_within(s, &cmd, 2.0) && cmd.kind == MediaCommandKind::PlayPause);
    KV_CHECK(c, atomic_load(&woken.n, MO_RELAXED) > woke);
    KV_CHECK(c, call(cl, dest, "org.mpris.MediaPlayer2.Player", "Next", nullptr, &r)); dbus_message_free(&r);
    KV_CHECK(c, command_within(s, &cmd, 2.0) && cmd.kind == MediaCommandKind::Next);
    w.begin("x"); w.i64(-5000000);
    KV_CHECK(c, call(cl, dest, "org.mpris.MediaPlayer2.Player", "Seek", &w, &r)); dbus_message_free(&r);
    KV_CHECK(c, command_within(s, &cmd, 2.0) && cmd.kind == MediaCommandKind::SeekBy && cmd.us == -5000000);
    w.begin("ox"); w.path("/net/blackwing/track/42"); w.i64(60000000);
    KV_CHECK(c, call(cl, dest, "org.mpris.MediaPlayer2.Player", "SetPosition", &w, &r)); dbus_message_free(&r);
    KV_CHECK(c, command_within(s, &cmd, 2.0) && cmd.kind == MediaCommandKind::SeekTo && cmd.us == 60000000 && cmd.track == 42);
    w.begin("ox"); w.path("/net/blackwing/track/7"); w.i64(1);
    KV_CHECK(c, call(cl, dest, "org.mpris.MediaPlayer2.Player", "SetPosition", &w, &r)); dbus_message_free(&r);
    KV_CHECK(c, !command_within(s, &cmd, 0.1)); // another track's: ignored, as the spec says
    w.begin("ssv"); w.str("org.mpris.MediaPlayer2.Player"); w.str("Volume"); w.variant_begin("d"); w.f64(0.3); w.variant_end();
    KV_CHECK(c, call(cl, dest, "org.freedesktop.DBus.Properties", "Set", &w, &r)); dbus_message_free(&r);
    KV_CHECK(c, command_within(s, &cmd, 2.0) && cmd.kind == MediaCommandKind::Volume && cmd.volume == 0.3);
    KV_CHECK(c, call(cl, dest, "org.mpris.MediaPlayer2", "Raise", nullptr, &r)); dbus_message_free(&r);
    KV_CHECK(c, command_within(s, &cmd, 2.0) && cmd.kind == MediaCommandKind::Raise);
    KV_CHECK(c, !call(cl, dest, "org.mpris.MediaPlayer2.Player", "OpenUri", (w.begin("s"), w.str("file:///x"), &w), &r));
    dbus_message_free(&r);
    KV_CHECK(c, media_info(s).commands == 6);

    // A jump: Seeked, with the new place.
    while (dbus_next(cl, &r)) dbus_message_free(&r);
    media_seeked(s, 60000000, time_now());
    bool seeked = false;
    end = time_now() + 2.0;
    while (!seeked && time_now() < end) {
        if (!dbus_next(cl, &r)) { time_sleep(0.002); continue; }
        if (strcmp(r.member, "Seeked") == 0) { i64 at = 0; rd = r.reader(); seeked = rd.i64(&at) && at == 60000000; }
        dbus_message_free(&r);
    }
    KV_CHECK(c, seeked);
    // Only what the shell shows is sent: a status that moves the position alone is not.
    st.position_us = 61000000;
    st.at = time_now();
    media_set_status(s, st);
    time_sleep(0.1);
    u32 sent = 0;
    while (dbus_next(cl, &r)) { ++sent; dbus_message_free(&r); }
    KV_CHECK(c, sent == 0);

    // Keep-awake through the portal: Inhibit with suspend's flag, and Close to let go.
    media_keep_awake(s, true);
    end = time_now() + 2.0;
    while (!media_info(s).awake && time_now() < end) time_sleep(0.005);
    info = media_info(s);
    KV_CHECK(c, info.awake && strcmp(info.awake_how, "portal") == 0 && svc.portal_flags == 4);
    media_keep_awake(s, false);
    end = time_now() + 2.0;
    while (atomic_load(&svc.portal_closes, MO_ACQUIRE) == 0 && time_now() < end) time_sleep(0.005);
    KV_CHECK(c, atomic_load(&svc.portal_closes, MO_ACQUIRE) == 1 && !media_info(s).awake);
    // No portal: logind's idle inhibitor, blocking, let go by closing it.
    atomic_store(&svc.portal_refuses, true, MO_RELEASE);
    media_keep_awake(s, true);
    end = time_now() + 2.0;
    while ((!media_info(s).awake || atomic_load(&svc.idle_inhibits, MO_ACQUIRE) == 0) && time_now() < end) time_sleep(0.005);
    info = media_info(s);
    KV_CHECK(c, info.awake && strcmp(info.awake_how, "logind") == 0 && atomic_load(&svc.idle_inhibits, MO_ACQUIRE) == 1);
    media_keep_awake(s, false);
    KV_CHECK(c, released(svc, "idle", 2.0));

    // Sleep: a delay inhibitor held from the start; PrepareForSleep(true) is an
    // interruption, and the delay is let go once the caller says it has paused. Waking is
    // another, and a new delay is taken.
    KV_CHECK(c, atomic_load(&svc.sleep_inhibits, MO_ACQUIRE) >= 1);
    DbusWriter b;
    b.begin("b"); b.boolean(true);
    dbus_signal(svc.c, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep", &b);
    MediaInterruption in;
    KV_CHECK(c, interruption_within(s, &in, 2.0) && in.kind == MediaInterruptionKind::Sleep);
    KV_CHECK(c, !released(svc, "sleep", 0.2)); // held while the caller pauses
    media_sleep_ready(s);
    KV_CHECK(c, released(svc, "sleep", 2.0));
    u32 delays = atomic_load(&svc.sleep_inhibits, MO_ACQUIRE);
    b.begin("b"); b.boolean(false);
    dbus_signal(svc.c, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep", &b);
    KV_CHECK(c, interruption_within(s, &in, 2.0) && in.kind == MediaInterruptionKind::Wake);
    end = time_now() + 2.0;
    while (atomic_load(&svc.sleep_inhibits, MO_ACQUIRE) == delays && time_now() < end) time_sleep(0.005);
    KV_CHECK(c, atomic_load(&svc.sleep_inhibits, MO_ACQUIRE) == delays + 1);
    KV_CHECK(c, media_info(s).interruptions == 2);
    // A caller that never says: let go after the grace all the same.
    b.begin("b"); b.boolean(true);
    dbus_signal(svc.c, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep", &b);
    KV_CHECK(c, interruption_within(s, &in, 2.0) && in.kind == MediaInterruptionKind::Sleep);
    KV_CHECK(c, released(svc, "sleep", 5.0));

    w.free(); b.free();
    media_close(s);
    // Gone: the name with it.
    KV_CHECK(c, !get_string(cl, dest, "org.mpris.MediaPlayer2", "Identity", sv, sizeof sv));
    dbus_close(cl);
    atomic_store(&svc.stop, true, MO_RELEASE);
    os_thread_join(&svc.thread);
    for (u32 i = 0; i < svc.nheld; ++i) svc.held[i].close();
    dbus_close(svc.c);
    os_mutex_destroy(&svc.lock);
    daemon.close();
}
