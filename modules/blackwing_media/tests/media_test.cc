// The media surface over the player itself, on the capture device, through a private
// dbus-daemon: an MPRIS client plays, pauses, seeks and sets the volume, and hears the
// track (in DAC time, with the art beside the file), the state and the seeks. Stand-ins
// for the portal and logind see keep-awake held and let go, and send the machine to
// sleep while it plays. And the art in a file's tags, written once.
#include "korvid/blackwing_library/module.h"
#include "korvid/blackwing_library/library.h"
#include "korvid/blackwing_media/media.h"
#include "korvid/blackwing_media/module.h"
#include "korvid/blackwing_player/module.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/cli/module.h"
#include "korvid/dbus/dbus.h"
#include "korvid/core/atomic.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"
#include "korvid/core/log.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform/net.h"
#include "korvid/platform/process.h"
#include <math.h>
#include <string.h>

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

// The portal and logind, answered on a thread: the portal's Inhibit and Close counted,
// logind's delay inhibitor's far end kept to see it let go.
struct Services {
    DbusConnection* c = nullptr;
    OsThread  thread;
    bool      stop = false;   // atomic
    u32       inhibits = 0;   // atomic
    u32       closes = 0;     // atomic
    u32       delays = 0;     // atomic
    OsMutex   lock;
    NetSocket delay;          // the newest delay's far end
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
            atomic_add_fetch(&s.inhibits, 1u, MO_RELEASE);
            DbusWriter w;
            w.begin("o"); w.path("/org/freedesktop/portal/desktop/request/1/t");
            dbus_reply(s.c, m, &w);
            w.free();
        } else if (m.kind == DbusKind::Call && strcmp(m.interface, "org.freedesktop.portal.Request") == 0) {
            atomic_add_fetch(&s.closes, 1u, MO_RELEASE);
            dbus_reply(s.c, m, nullptr);
        } else if (m.kind == DbusKind::Call && strcmp(m.interface, "org.freedesktop.login1.Manager") == 0) {
            NetSocket a, b;
            net_socket_pair(&a, &b);
            DbusWriter w;
            w.begin("h"); w.fd(a.fd);
            dbus_reply(s.c, m, &w);
            w.free();
            time_sleep(0.02);
            a.close();
            os_mutex_lock(&s.lock);
            s.delay.close();
            s.delay.adopt(b.fd);
            b.fd = Handle{};
            os_mutex_unlock(&s.lock);
            atomic_add_fetch(&s.delays, 1u, MO_RELEASE);
        }
        dbus_message_free(&m);
    }
}

bool delay_released(Services& s) {
    os_mutex_lock(&s.lock);
    char b[4];
    bool gone = s.delay.is_open() ? s.delay.read(b, sizeof b) < 0 : false;
    os_mutex_unlock(&s.lock);
    return gone;
}

// What the module said on the busses.
constinit u32 g_raised = 0, g_interrupted = 0, g_resumed = 0;
void on_raise(TickContext&, Span<const MediaRaise> ev) { g_raised += ev.len; }
void on_interrupted(TickContext&, Span<const MediaInterrupted> ev) { g_interrupted += ev.len; }
void on_resumed(TickContext&, Span<const MediaResumed> ev) { g_resumed += ev.len; }
constinit const HandlerDesc k_handlers[] = {
    {.name = ".raise", KV_ON(MediaRaise, on_raise), .phase = Phase::PostSim},
    {.name = ".interrupted", KV_ON(MediaInterrupted, on_interrupted), .phase = Phase::PostSim},
    {.name = ".resumed", KV_ON(MediaResumed, on_resumed), .phase = Phase::PostSim},
};
KV_MODULE_DEFINE(k_listener) = {.name = "listener", .handlers = k_handlers};

struct Run {
    Engine e;
    bool up = false;
    bool start(const char* data, const char* address, const char* art) {
        PlayerConfig pc;
        pc.device = "capture";
        player_configure(pc);
        LibraryConfig lc;
        lc.data_dir = data;
        library_configure(lc);
        MediaConfig mc;
        mc.enabled = true;
        mc.can_raise = true;
        mc.session_address = address;
        mc.system_address = address;
        mc.awake_linger = 0.3;
        mc.art_dir = art;
        media_configure(mc);
        CliConfig cli; cli.no_stdin = true;
        cli_configure(cli);
        PlatformMounts mounts; mounts.none = true;
        platform_configure_mounts(mounts);
        static const ModuleDesc* const list[] = {&platform_module, &cli_module, &blackwing_player_module, &blackwing_library_module,
                                                 &blackwing_media_module, &k_listener};
        up = engine_init(e, list);
        return up;
    }
    void stop() {
        if (up) engine_shutdown(e);
        up = false;
        media_configure(MediaConfig{});
        player_configure(PlayerConfig{});
        library_configure(LibraryConfig{});
    }
    void step() { platform_pump(e); engine_frame(e, e.clock.tick_dt); time_sleep(0.002); }
    void run_for(f64 seconds) { f64 end = time_now() + seconds; while (time_now() < end) step(); }
    template <class F> bool until(F done, f64 seconds = 5.0) {
        f64 end = time_now() + seconds;
        for (;;) { step(); if (done()) return true; if (time_now() > end) return false; }
    }
    const PlayerTransport& t() { return *player_transport(e); }
    const MediaStatusRow& m() { return *media_status(e); }
};

bool mpris(DbusConnection* c, const char* iface, const char* member, DbusWriter* body) {
    DbusHeader h;
    h.destination = "org.mpris.MediaPlayer2.blackwing";
    h.path = "/org/mpris/MediaPlayer2";
    h.interface = iface;
    h.member = member;
    return dbus_send(c, h, body) != 0;
}

// The next of the player's signals with this member, and what it says.
struct Heard { bool playing = false; bool paused = false; char title[256] = ""; char art[1100] = ""; i64 seeked = -1; };
void listen(DbusConnection* c, Heard& h) {
    DbusMessage m;
    while (dbus_next(c, &m)) {
        if (m.kind == DbusKind::Signal && strcmp(m.member, "Seeked") == 0) { DbusReader r = m.reader(); r.i64(&h.seeked); }
        if (m.kind == DbusKind::Signal && strcmp(m.member, "PropertiesChanged") == 0) {
            DbusReader r = m.reader();
            const char* iface;
            r.str(&iface);
            if (r.array_begin())
                while (r.array_more()) {
                    const char* k;
                    r.entry_begin(); r.str(&k); r.variant_begin();
                    if (strcmp(k, "PlaybackStatus") == 0) { const char* v; r.str(&v); h.playing = strcmp(v, "Playing") == 0; h.paused = strcmp(v, "Paused") == 0; }
                    else if (strcmp(k, "Metadata") == 0 && r.array_begin()) {
                        while (r.array_more()) {
                            const char* mk; const char* vs = "";
                            r.entry_begin(); r.str(&mk); r.variant_begin();
                            if (strcmp(mk, "xesam:title") == 0) { r.str(&vs); format(h.title, sizeof h.title, "%s", vs); }
                            else if (strcmp(mk, "mpris:artUrl") == 0) { r.str(&vs); format(h.art, sizeof h.art, "%s", vs); }
                            else r.skip();
                            r.variant_end(); r.entry_end();
                        }
                    } else r.skip();
                    r.variant_end(); r.entry_end();
                }
        }
        dbus_message_free(&m);
    }
}

f64 gain(Engine& e) {
    KTable* p = engine_ktable(e, "player.params");
    Span<PlayerParam> rows = p->rows_of<PlayerParam>();
    for (u32 i = 0; i < rows.len; ++i) if (p->is_alive(i) && rows[i].stage == 0 && rows[i].index == 0) return rows[i].value;
    return 999.0;
}

} // namespace

void test_media(TestCtx& c) {
    c.begin("blackwing_media over the player");
    Process daemon;
    char address[512] = {};
    if (!start_daemon(daemon, address, sizeof address)) {
        KV_INFO("test", "skipped: dbus-daemon would not start (%s)", daemon.error);
        daemon.close();
        return;
    }
    char tmp[1024], data[1100], art[1100], song[1200], cover[1200], tone[1100];
    KV_CHECK(c, io_make_temp_dir("bw_media", tmp, sizeof tmp));
    format(data, sizeof data, "%s/data", tmp);
    format(art, sizeof art, "%s/art", tmp);
    io_make_dirs(data);
    io_make_dirs(art);
    const char* here = __FILE__;
    const char* slash = strrchr(here, '/');
    format(tone, sizeof tone, "%.*s/../../hifi_codec/tests/data/tone_16_2ch_8000_long.flac", slash ? (int)(slash - here) : 0, here);
    // The tone in a directory of its own, with a cover beside it.
    char album[1100];
    format(album, sizeof album, "%s/Some Album", tmp);
    io_make_dirs(album);
    format(song, sizeof song, "%s/01 The Tone.flac", album);
    KV_CHECK(c, io_copy_file(tone, song));
    format(cover, sizeof cover, "%s/Cover.JPG", album);
    KV_CHECK(c, io_write_file(cover, "\xFF\xD8\xFF\xE0 not really a jpeg", 22));

    char why[200];
    Services svc;
    os_mutex_init(&svc.lock);
    svc.c = dbus_open_address(address, 3.0, why, sizeof why);
    dbus_request_name(svc.c, "org.freedesktop.portal.Desktop", kDbusNameDoNotQueue, 2.0);
    dbus_request_name(svc.c, "org.freedesktop.login1", kDbusNameDoNotQueue, 2.0);
    const DbusInterfaceDesc* p1[] = {&kPortalIface};
    const DbusInterfaceDesc* p2[] = {&kRequestIface};
    const DbusInterfaceDesc* p3[] = {&kLoginIface};
    dbus_export(svc.c, "/org/freedesktop/portal/desktop", p1, 1);
    dbus_export(svc.c, "/org/freedesktop/portal/desktop/request/1/t", p2, 1);
    dbus_export(svc.c, "/org/freedesktop/login1", p3, 1);
    os_thread_start(&svc.thread, services_run, &svc);

    Run run;
    KV_CHECK(c, run.start(data, address, art));
    KV_CHECK(c, run.m().open && strcmp(run.m().name, "org.mpris.MediaPlayer2.blackwing") == 0);
    DbusConnection* cl = dbus_open_address(address, 3.0, why, sizeof why);
    dbus_add_match(cl, "type='signal',path='/org/mpris/MediaPlayer2'", 2.0);
    Heard h;

    // Queued, then played from the OS: heard, its title and the cover beside it shown.
    PlayerEnqueue q{};
    format(q.path, sizeof q.path, "%s", song);
    run.e.events.emit(q);
    run.until([&] { return run.t().queued == 1; });
    KV_CHECK(c, mpris(cl, "org.mpris.MediaPlayer2.Player", "PlayPause", nullptr));
    KV_CHECK(c, run.until([&] { listen(cl, h); return h.playing && h.title[0] && h.art[0]; }));
    KV_CHECK(c, run.t().state == (u8)PlayerState::Playing);
    char want_art[1300];
    format(want_art, sizeof want_art, "file://%s/Some%%20Album/Cover.JPG", tmp);
    KV_CHECK(c, strcmp(h.title, "01 The Tone") == 0 || strcmp(h.title, "The Tone") == 0);
    KV_CHECK(c, strcmp(h.art, want_art) == 0);
    if (strcmp(h.art, want_art) != 0) KV_WARN("test", "art: %s", h.art);
    // Held awake while it plays.
    KV_CHECK(c, run.until([&] { return run.m().awake && atomic_load(&svc.inhibits, MO_ACQUIRE) == 1; }));

    // A seek from the OS: Seeked, at the place heard.
    DbusWriter w;
    w.begin("x"); w.i64(2000000);
    mpris(cl, "org.mpris.MediaPlayer2.Player", "Seek", &w);
    KV_CHECK(c, run.until([&] { listen(cl, h); return h.seeked >= 0; }));
    KV_CHECK(c, h.seeked > 1500000 && h.seeked < 6000000);
    // The volume, as the gain stage's decibels.
    w.begin("ssv"); w.str("org.mpris.MediaPlayer2.Player"); w.str("Volume"); w.variant_begin("d"); w.f64(0.5); w.variant_end();
    DbusHeader sh;
    sh.destination = "org.mpris.MediaPlayer2.blackwing";
    sh.path = "/org/mpris/MediaPlayer2";
    sh.interface = "org.freedesktop.DBus.Properties";
    sh.member = "Set";
    dbus_send(cl, sh, &w);
    KV_CHECK(c, run.until([&] { return fabs(gain(run.e) - 20.0 * log10(0.5)) < 0.01; }));

    // Paused from the OS: shown paused, and let go of keep-awake after the linger.
    mpris(cl, "org.mpris.MediaPlayer2.Player", "Pause", nullptr);
    KV_CHECK(c, run.until([&] { listen(cl, h); return h.paused && run.t().state == (u8)PlayerState::Paused; }));
    KV_CHECK(c, run.until([&] { return !run.m().awake && atomic_load(&svc.closes, MO_ACQUIRE) == 1; }));

    // Playing, the machine goes to sleep: paused before it does, and then let sleep.
    mpris(cl, "org.mpris.MediaPlayer2.Player", "Play", nullptr);
    KV_CHECK(c, run.until([&] { return run.t().state == (u8)PlayerState::Playing; }));
    KV_CHECK(c, run.until([&] { return atomic_load(&svc.delays, MO_ACQUIRE) >= 1; }));
    DbusWriter b;
    b.begin("b"); b.boolean(true);
    dbus_signal(svc.c, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep", &b);
    KV_CHECK(c, run.until([&] { return run.t().state == (u8)PlayerState::Paused && g_interrupted == 1; }));
    KV_CHECK(c, run.until([&] { return delay_released(svc); }));
    b.begin("b"); b.boolean(false);
    dbus_signal(svc.c, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep", &b);
    KV_CHECK(c, run.until([&] { return g_resumed == 1; }));
    run.run_for(0.3);
    KV_CHECK(c, run.t().state == (u8)PlayerState::Paused); // nothing resumes on its own

    // Raise is for the window; Quit ends the run.
    mpris(cl, "org.mpris.MediaPlayer2", "Raise", nullptr);
    KV_CHECK(c, run.until([&] { return g_raised == 1; }));
    mpris(cl, "org.mpris.MediaPlayer2", "Quit", nullptr);
    KV_CHECK(c, run.until([&] { return run.e.quit_requested; }));
    KV_CHECK(c, run.m().commands >= 7);
    w.free(); b.free();
    run.stop();
    dbus_close(cl);
    atomic_store(&svc.stop, true, MO_RELEASE);
    os_thread_join(&svc.thread);
    svc.delay.close();
    dbus_close(svc.c);
    os_mutex_destroy(&svc.lock);
    daemon.close();
    io_remove_tree(tmp);
}

void test_art(TestCtx& c) {
    c.begin("blackwing_media, art in the tags");
    char tmp[1024], file[1100], dir[1100], found[1200], again[1200];
    KV_CHECK(c, io_make_temp_dir("bw_art", tmp, sizeof tmp));
    format(file, sizeof file, "%s/song.mp3", tmp);
    format(dir, sizeof dir, "%s/art", tmp);
    // ID3v2.4 with a front cover (a PNG's first bytes and some), then something that is not audio.
    u8 buf[512];
    u32 n = 0;
    const u8 pic[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    u8 body[64];
    u32 bn = 0;
    body[bn++] = 0; // Latin-1
    memcpy(body + bn, "image/png", 10); bn += 10;
    body[bn++] = 3; // front cover
    body[bn++] = 0; // no description
    memcpy(body + bn, pic, sizeof pic); bn += sizeof pic;
    u32 frame = 10 + bn;
    memcpy(buf + n, "ID3\x04\x00\x00", 6); n += 6;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = (u8)(frame >> 7); buf[n++] = (u8)(frame & 0x7F);
    memcpy(buf + n, "APIC", 4); n += 4;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = (u8)(bn >> 7); buf[n++] = (u8)(bn & 0x7F);
    buf[n++] = 0; buf[n++] = 0;
    memcpy(buf + n, body, bn); n += bn;
    memset(buf + n, 0x55, 100); n += 100;
    KV_CHECK(c, io_write_file(file, buf, n));
    KV_CHECK(c, media_find_art(file, dir, found, sizeof found) && strstr(found, ".png"));
    IoBytes got;
    KV_CHECK(c, io_read_file(found, &got) && got.size == sizeof pic && memcmp(got.data, pic, sizeof pic) == 0);
    got.free();
    IoStat st1, st2;
    io_stat_file(found, &st1);
    time_sleep(0.02);
    KV_CHECK(c, media_find_art(file, dir, again, sizeof again) && strcmp(found, again) == 0);
    io_stat_file(again, &st2);
    KV_CHECK(c, st1.mtime_ns == st2.mtime_ns); // written once
    // No art anywhere: none.
    format(file, sizeof file, "%s/plain.mp3", tmp);
    io_write_file(file, "\xFF\xFB not much", 11);
    KV_CHECK(c, !media_find_art(file, dir, found, sizeof found) && found[0] == 0);
    io_remove_tree(tmp);
}
