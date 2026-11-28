// Linux: MPRIS on the session bus; keep-awake through the desktop portal's Inhibit, else
// logind's idle inhibitor; sleep through logind's PrepareForSleep and a delay inhibitor.
//
// The surface's thread owns both connections' traffic. It sleeps on an event that the
// connections' notify and every media_set_* set, so a still player costs nothing: the
// shell reads Position when it wants it, and the thread answers from the position given
// and its rate. What the bus asks of the player is queued for the caller's tick.
#include "korvid/media_platform/media.h"
#include "korvid/dbus/dbus.h"
#include "korvid/core/array.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/time.h"
#include "korvid/platform/net.h"
#include <new>
#include <string.h>
#include <unistd.h>

namespace kv {

namespace mpris {

const char* const kPath = "/org/mpris/MediaPlayer2";
const char* const kRoot = "org.mpris.MediaPlayer2";
const char* const kPlayer = "org.mpris.MediaPlayer2.Player";
const char* const kNoTrack = "/org/mpris/MediaPlayer2/TrackList/NoTrack";
const char* const kPortal = "org.freedesktop.portal.Desktop";
const char* const kLogin = "org.freedesktop.login1";
constexpr f64 kSleepGrace = 3.0; // how long the machine is held for a pause that does not say it is done

const DbusMethodDesc kRootMethods[] = {{"Raise"}, {"Quit"}};
const DbusPropertyDesc kRootProps[] = {
    {"CanQuit", "b"}, {"CanRaise", "b"}, {"HasTrackList", "b"}, {"Identity", "s"},
    {"DesktopEntry", "s"}, {"SupportedUriSchemes", "as"}, {"SupportedMimeTypes", "as"},
};
const DbusInterfaceDesc kRootIface = {kRoot, kRootMethods, 2, kRootProps, 7};

const DbusArg kSeekArgs[] = {{"Offset", "x"}};
const DbusArg kSetPosArgs[] = {{"TrackId", "o"}, {"Position", "x"}};
const DbusArg kUriArgs[] = {{"Uri", "s"}};
const DbusMethodDesc kPlayerMethods[] = {
    {"Next"}, {"Previous"}, {"Pause"}, {"PlayPause"}, {"Stop"}, {"Play"},
    {"Seek", "x", "", kSeekArgs, 1}, {"SetPosition", "ox", "", kSetPosArgs, 2}, {"OpenUri", "s", "", kUriArgs, 1},
};
const DbusPropertyDesc kPlayerProps[] = {
    {"PlaybackStatus", "s"}, {"Rate", "d"}, {"Metadata", "a{sv}"},
    {"Volume", "d", DbusAccess::ReadWrite},
    {"Position", "x", DbusAccess::Read, DbusEmits::None, true},
    {"MinimumRate", "d"}, {"MaximumRate", "d"},
    {"CanGoNext", "b"}, {"CanGoPrevious", "b"}, {"CanPlay", "b"}, {"CanPause", "b"}, {"CanSeek", "b"},
    {"CanControl", "b", DbusAccess::Read, DbusEmits::None},
};
const DbusArg kSeekedArgs[] = {{"Position", "x"}};
const DbusSignalDesc kPlayerSignals[] = {{"Seeked", "x", kSeekedArgs, 1}};
const DbusInterfaceDesc kPlayerIface = {kPlayer, kPlayerMethods, 9, kPlayerProps, 13, kPlayerSignals, 1};

enum class Awake : u8 { None, Portal, Logind };

} // namespace mpris

using namespace mpris;

struct MediaSurface {
    char identity[128] = "", desktop_entry[128] = "", base_name[96] = "";
    bool can_raise = false, can_quit = true, watch_sleep = true;
    void (*notify)(void*) = nullptr;
    void* user = nullptr;

    DbusConnection* session = nullptr;
    DbusConnection* system = nullptr;
    OsThread thread;
    OsEvent  event;
    OsMutex  lock;

    // Under lock: what the caller said and what it is to take.
    bool        stop = false;
    bool        has_track = false;
    MediaTrack  track;
    MediaStatus status;
    bool        dirty = true;       // properties to publish
    bool        seeked = false;
    i64         seeked_us = 0;
    bool        want_awake = false;
    bool        ready = false;      // media_sleep_ready since the last Sleep
    Array<MediaCommand>      commands;
    Array<MediaInterruption> interruptions;
    MediaInfo   info;

    // The thread's own.
    Awake  awake = Awake::None;
    char   portal_handle[256] = "";
    Handle logind_fd;
    f64    awake_retry_at = 0.0;
    Handle delay_fd;
    bool   sleeping = false;
    f64    sleep_at = 0.0;
};

namespace mpris {

void wake(void* user) { os_event_set(&((MediaSurface*)user)->event); }

void tell(MediaSurface* s) { if (s->notify) s->notify(s->user); }

void queue_command(MediaSurface* s, MediaCommandKind k, i64 us = 0, u64 track = 0, f64 volume = 0.0) {
    MediaCommand c;
    c.kind = k;
    c.us = us;
    c.track = track;
    c.volume = volume;
    os_mutex_lock(&s->lock);
    s->commands.push(c);
    ++s->info.commands;
    kv::format(s->info.last_command, sizeof s->info.last_command, "%s", media_command_name(k));
    os_mutex_unlock(&s->lock);
    tell(s);
}

void queue_interruption(MediaSurface* s, MediaInterruptionKind k, const char* reason) {
    MediaInterruption i;
    i.kind = k;
    kv::format(i.reason, sizeof i.reason, "%s", reason);
    os_mutex_lock(&s->lock);
    s->interruptions.push(i);
    ++s->info.interruptions;
    os_mutex_unlock(&s->lock);
    tell(s);
}

// file:// and a path, with what a URI cannot carry escaped.
void file_uri(const char* path, char* out, u32 cap) {
    static const char* hex = "0123456789ABCDEF";
    u32 n = 0;
    const char* pre = "file://";
    for (const char* p = pre; *p && n + 1 < cap; ++p) out[n++] = *p;
    for (const u8* p = (const u8*)path; *p && n + 4 < cap; ++p) {
        u8 ch = *p;
        bool keep = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || strchr("/-._~", ch);
        if (keep) out[n++] = (char)ch;
        else { out[n++] = '%'; out[n++] = hex[ch >> 4]; out[n++] = hex[ch & 15]; }
    }
    out[n] = 0;
}

void track_path(u64 id, char* out, u32 cap) { kv::format(out, cap, "/net/blackwing/track/%llu", (unsigned long long)id); }

i64 position_now(const MediaSurface* s) {
    const MediaStatus& st = s->status;
    f64 us = (f64)st.position_us + st.rate * (time_now() - st.at) * 1e6;
    if (us < 0) us = 0;
    if (s->has_track && s->track.length_us > 0 && us > (f64)s->track.length_us) us = (f64)s->track.length_us;
    return (i64)us;
}

const char* status_name(MediaState st) {
    return st == MediaState::Playing ? "Playing" : st == MediaState::Paused ? "Paused" : "Stopped";
}

void put_entry_s(DbusWriter& w, const char* key, const char* v) {
    if (!v[0]) return;
    w.entry_begin(); w.str(key); w.variant_begin("s"); w.str(v); w.variant_end(); w.entry_end();
}
void put_entry_as(DbusWriter& w, const char* key, const char* v) {
    if (!v[0]) return;
    w.entry_begin(); w.str(key); w.variant_begin("as"); w.array_begin(); w.str(v); w.array_end(); w.variant_end(); w.entry_end();
}

// The Player's properties as the caller left them; under lock.
void publish(MediaSurface* s) {
    const MediaStatus& st = s->status;
    DbusWriter status, rate, meta, vol, next, prev, play, pause, seek, ctrl, minr, maxr;
    status.begin("s"); status.str(status_name(st.state));
    rate.begin("d"); rate.f64(1.0);
    minr.begin("d"); minr.f64(1.0);
    maxr.begin("d"); maxr.f64(1.0);
    meta.begin("a{sv}");
    meta.array_begin();
    char buf[1100];
    if (s->has_track) {
        const MediaTrack& t = s->track;
        track_path(t.id, buf, sizeof buf);
        meta.entry_begin(); meta.str("mpris:trackid"); meta.variant_begin("o"); meta.path(buf); meta.variant_end(); meta.entry_end();
        if (t.length_us > 0) { meta.entry_begin(); meta.str("mpris:length"); meta.variant_begin("x"); meta.i64(t.length_us); meta.variant_end(); meta.entry_end(); }
        put_entry_s(meta, "xesam:title", t.title);
        put_entry_as(meta, "xesam:artist", t.artist);
        put_entry_s(meta, "xesam:album", t.album);
        put_entry_as(meta, "xesam:albumArtist", t.album_artist);
        if (t.track_number) { meta.entry_begin(); meta.str("xesam:trackNumber"); meta.variant_begin("i"); meta.i32((i32)t.track_number); meta.variant_end(); meta.entry_end(); }
        if (t.path[0]) { file_uri(t.path, buf, sizeof buf); put_entry_s(meta, "xesam:url", buf); }
        if (t.art[0]) { file_uri(t.art, buf, sizeof buf); put_entry_s(meta, "mpris:artUrl", buf); }
    } else {
        meta.entry_begin(); meta.str("mpris:trackid"); meta.variant_begin("o"); meta.path(kNoTrack); meta.variant_end(); meta.entry_end();
    }
    meta.array_end();
    vol.begin("d"); vol.f64(st.volume < 0 ? 1.0 : st.volume);
    next.begin("b"); next.boolean(st.can_next);
    prev.begin("b"); prev.boolean(st.can_previous);
    play.begin("b"); play.boolean(st.can_play);
    pause.begin("b"); pause.boolean(st.can_pause);
    seek.begin("b"); seek.boolean(st.can_seek && s->has_track);
    ctrl.begin("b"); ctrl.boolean(true);
    DbusPropertyValue v[] = {
        {"PlaybackStatus", &status}, {"Rate", &rate}, {"Metadata", &meta}, {"Volume", &vol},
        {"MinimumRate", &minr}, {"MaximumRate", &maxr}, {"CanGoNext", &next}, {"CanGoPrevious", &prev},
        {"CanPlay", &play}, {"CanPause", &pause}, {"CanSeek", &seek}, {"CanControl", &ctrl},
    };
    dbus_properties_set(s->session, kPath, kPlayer, v, KV_COUNTOF(v));
    for (DbusPropertyValue& x : v) ((DbusWriter*)x.value)->free();
}

void publish_root(MediaSurface* s) {
    DbusWriter quit, raise, list, id, entry, schemes, mimes;
    quit.begin("b"); quit.boolean(s->can_quit);
    raise.begin("b"); raise.boolean(s->can_raise);
    list.begin("b"); list.boolean(false);
    id.begin("s"); id.str(s->identity);
    entry.begin("s"); entry.str(s->desktop_entry);
    schemes.begin("as"); schemes.array_begin(); schemes.str("file"); schemes.array_end();
    mimes.begin("as"); mimes.array_begin(); mimes.str("audio/flac"); mimes.str("audio/x-flac"); mimes.str("audio/mpeg"); mimes.array_end();
    DbusPropertyValue v[] = {
        {"CanQuit", &quit}, {"CanRaise", &raise}, {"HasTrackList", &list}, {"Identity", &id},
        {"DesktopEntry", &entry}, {"SupportedUriSchemes", &schemes}, {"SupportedMimeTypes", &mimes},
    };
    dbus_properties_set(s->session, kPath, kRoot, v, KV_COUNTOF(v), true);
    for (DbusPropertyValue& x : v) ((DbusWriter*)x.value)->free();
}

void answer_session(MediaSurface* s, DbusMessage& m) {
    if (m.kind != DbusKind::Call) return; // the bus's own news (NameAcquired) and nothing else
    if (strcmp(m.interface, "org.freedesktop.DBus.Properties") == 0) {
        os_mutex_lock(&s->lock);
        ++s->info.clients_asked;
        os_mutex_unlock(&s->lock);
        DbusReader r = m.reader();
        const char* iface = "";
        const char* name = "";
        r.str(&iface);
        if (strcmp(m.member, "Set") == 0) {
            r.str(&name);
            f64 v = 0;
            const char* sig = "";
            if (strcmp(name, "Volume") == 0 && r.variant_begin(&sig) && r.f64(&v)) {
                dbus_reply(s->session, m, nullptr);
                queue_command(s, MediaCommandKind::Volume, 0, 0, v < 0 ? 0 : v > 1 ? 1 : v);
            } else {
                dbus_reply_error(s->session, m, "org.freedesktop.DBus.Error.InvalidArgs", "only Volume can be set");
            }
            return;
        }
        os_mutex_lock(&s->lock);
        i64 pos = position_now(s);
        os_mutex_unlock(&s->lock);
        DbusWriter p;
        p.begin("x");
        p.i64(pos);
        if (strcmp(m.member, "GetAll") == 0) {
            DbusPropertyValue live{"Position", &p};
            dbus_reply_get_all(s->session, m, &live, 1);
        } else { // Get of Position, the only live property
            DbusWriter v;
            v.begin("v"); v.variant_begin("x"); v.i64(pos); v.variant_end();
            dbus_reply(s->session, m, &v);
            v.free();
        }
        p.free();
        return;
    }
    const char* member = m.member;
    MediaCommandKind k = MediaCommandKind::Count;
    i64 us = 0;
    u64 track = 0;
    if (strcmp(m.interface, kRoot) == 0) {
        if (strcmp(member, "Raise") == 0 && s->can_raise) k = MediaCommandKind::Raise;
        else if (strcmp(member, "Quit") == 0 && s->can_quit) k = MediaCommandKind::Quit;
    } else {
        if (strcmp(member, "Play") == 0) k = MediaCommandKind::Play;
        else if (strcmp(member, "Pause") == 0) k = MediaCommandKind::Pause;
        else if (strcmp(member, "PlayPause") == 0) k = MediaCommandKind::PlayPause;
        else if (strcmp(member, "Stop") == 0) k = MediaCommandKind::Stop;
        else if (strcmp(member, "Next") == 0) k = MediaCommandKind::Next;
        else if (strcmp(member, "Previous") == 0) k = MediaCommandKind::Previous;
        else if (strcmp(member, "Seek") == 0) {
            DbusReader r = m.reader();
            if (r.i64(&us)) k = MediaCommandKind::SeekBy;
        } else if (strcmp(member, "SetPosition") == 0) {
            DbusReader r = m.reader();
            const char* id = "";
            r.path(&id);
            r.i64(&us);
            char want[64];
            os_mutex_lock(&s->lock);
            bool have = s->has_track;
            u64 cur = s->track.id;
            os_mutex_unlock(&s->lock);
            track_path(cur, want, sizeof want);
            // The spec: a position for a track not playing, or past its length, is ignored.
            if (have && strcmp(id, want) == 0 && us >= 0) { k = MediaCommandKind::SeekTo; track = cur; }
            else { dbus_reply(s->session, m, nullptr); return; }
        } else if (strcmp(member, "OpenUri") == 0) {
            dbus_reply_error(s->session, m, "org.freedesktop.DBus.Error.NotSupported", "Blackwing opens files from its own library");
            return;
        }
    }
    dbus_reply(s->session, m, nullptr);
    if (k != MediaCommandKind::Count) queue_command(s, k, us, track);
}

// ---- keep-awake ---------------------------------------------------------------------

bool portal_take(MediaSurface* s, char* why, u32 cap) {
    DbusHeader h;
    h.destination = kPortal;
    h.path = "/org/freedesktop/portal/desktop";
    h.interface = "org.freedesktop.portal.Inhibit";
    h.member = "Inhibit";
    DbusWriter w;
    w.begin("sua{sv}");
    w.str("");
    w.u32(4); // suspend: the machine's idle sleep, not the screen's
    w.array_begin();
    w.entry_begin(); w.str("reason"); w.variant_begin("s"); w.str("Playing music"); w.variant_end(); w.entry_end();
    w.array_end();
    DbusMessage reply;
    bool ok = dbus_call(s->session, h, &w, 2.0, &reply, why, cap);
    w.free();
    if (!ok) return false;
    const char* handle = "";
    DbusReader r = reply.reader();
    ok = reply.kind == DbusKind::Return && r.path(&handle);
    if (ok) kv::format(s->portal_handle, sizeof s->portal_handle, "%s", handle);
    else kv::format(why, cap, "the portal said %s: %s", reply.error_name, reply.error_text());
    dbus_message_free(&reply);
    return ok;
}

void portal_release(MediaSurface* s) {
    DbusHeader h;
    h.destination = kPortal;
    h.path = s->portal_handle;
    h.interface = "org.freedesktop.portal.Request";
    h.member = "Close";
    DbusMessage reply;
    if (dbus_call(s->session, h, nullptr, 2.0, &reply)) dbus_message_free(&reply);
    s->portal_handle[0] = 0;
}

// logind's Inhibit: a descriptor that holds it until closed.
bool logind_inhibit(MediaSurface* s, const char* what, const char* why_text, const char* mode, Handle* out, char* why, u32 cap) {
    if (!s->system) { kv::format(why, cap, "no system bus"); return false; }
    DbusHeader h;
    h.destination = kLogin;
    h.path = "/org/freedesktop/login1";
    h.interface = "org.freedesktop.login1.Manager";
    h.member = "Inhibit";
    DbusWriter w;
    w.begin("ssss");
    w.str(what); w.str("Blackwing"); w.str(why_text); w.str(mode);
    DbusMessage reply;
    bool ok = dbus_call(s->system, h, &w, 2.0, &reply, why, cap);
    w.free();
    if (!ok) return false;
    u32 idx = 0;
    DbusReader r = reply.reader();
    ok = reply.kind == DbusKind::Return && r.fd(&idx);
    if (ok) *out = dbus_message_take_fd(&reply, idx);
    else kv::format(why, cap, "logind said %s: %s", reply.error_name, reply.error_text());
    dbus_message_free(&reply);
    return ok;
}

void reconcile_awake(MediaSurface* s) {
    os_mutex_lock(&s->lock);
    bool want = s->want_awake;
    os_mutex_unlock(&s->lock);
    bool have = s->awake != Awake::None;
    if (want == have) return;
    char why[160] = "";
    if (want) {
        if (time_now() < s->awake_retry_at) return;
        const char* how = "";
        char pwhy[160] = "";
        if (portal_take(s, pwhy, sizeof pwhy)) { s->awake = Awake::Portal; how = "portal"; }
        else if (logind_inhibit(s, "idle", "Playing music", "block", &s->logind_fd, why, sizeof why)) { s->awake = Awake::Logind; how = "logind"; }
        else s->awake_retry_at = time_now() + 30.0;
        os_mutex_lock(&s->lock);
        s->info.awake = s->awake != Awake::None;
        kv::format(s->info.awake_how, sizeof s->info.awake_how, "%s", how);
        if (s->awake == Awake::None) kv::format(s->info.awake_why, sizeof s->info.awake_why, "no portal (%.40s), no logind (%.40s)", pwhy, why);
        else s->info.awake_why[0] = 0;
        os_mutex_unlock(&s->lock);
    } else {
        if (s->awake == Awake::Portal) portal_release(s);
        if (s->awake == Awake::Logind) { net_close(s->logind_fd); s->logind_fd = Handle{}; }
        s->awake = Awake::None;
        os_mutex_lock(&s->lock);
        s->info.awake = false;
        s->info.awake_how[0] = 0;
        os_mutex_unlock(&s->lock);
    }
}

// ---- sleep --------------------------------------------------------------------------

void take_delay(MediaSurface* s) {
    char why[160] = "";
    Handle fd;
    if (logind_inhibit(s, "sleep", "Pause the music before the machine sleeps", "delay", &fd, why, sizeof why)) s->delay_fd = fd;
    else KV_INFO("media", "no delay before sleep: %s", why);
}

void answer_system(MediaSurface* s, DbusMessage& m) {
    if (m.kind != DbusKind::Signal || strcmp(m.member, "PrepareForSleep") != 0 ||
        strcmp(m.interface, "org.freedesktop.login1.Manager") != 0)
        return;
    bool going = false;
    DbusReader r = m.reader();
    if (!r.boolean(&going)) return;
    if (going) {
        os_mutex_lock(&s->lock);
        s->ready = false;
        os_mutex_unlock(&s->lock);
        s->sleeping = true;
        s->sleep_at = time_now();
        queue_interruption(s, MediaInterruptionKind::Sleep, "the machine is going to sleep");
    } else {
        if (s->sleeping && os_handle_open(s->delay_fd)) { net_close(s->delay_fd); s->delay_fd = Handle{}; }
        s->sleeping = false;
        take_delay(s);
        queue_interruption(s, MediaInterruptionKind::Wake, "the machine has woken");
    }
}

void run(void* arg) {
    MediaSurface* s = (MediaSurface*)arg;
    if (s->system && s->watch_sleep) take_delay(s);
    for (;;) {
        f64 wait = 3600.0;
        if (s->sleeping) wait = 0.1;
        os_mutex_lock(&s->lock);
        bool awake_pending = s->want_awake != (s->awake != Awake::None);
        os_mutex_unlock(&s->lock);
        if (awake_pending && s->awake_retry_at > time_now()) wait = s->awake_retry_at - time_now();
        os_event_wait(&s->event, wait);

        os_mutex_lock(&s->lock);
        bool stop = s->stop;
        bool dirty = s->dirty;
        bool seeked = s->seeked;
        i64 seeked_us = s->seeked_us;
        s->dirty = s->seeked = false;
        if (dirty) publish(s);
        bool ready = s->ready;
        os_mutex_unlock(&s->lock);
        if (stop) break;
        if (seeked) {
            DbusWriter w;
            w.begin("x");
            w.i64(seeked_us);
            dbus_signal(s->session, kPath, kPlayer, "Seeked", &w);
            w.free();
        }
        DbusMessage m;
        while (dbus_next(s->session, &m)) { answer_session(s, m); dbus_message_free(&m); }
        if (s->system)
            while (dbus_next(s->system, &m)) { answer_system(s, m); dbus_message_free(&m); }
        reconcile_awake(s);
        if (s->sleeping && os_handle_open(s->delay_fd) && (ready || time_now() - s->sleep_at > kSleepGrace)) {
            net_close(s->delay_fd); // the machine may sleep now
            s->delay_fd = Handle{};
        }
    }
    os_mutex_lock(&s->lock);
    s->want_awake = false;
    os_mutex_unlock(&s->lock);
    reconcile_awake(s);
    if (os_handle_open(s->delay_fd)) net_close(s->delay_fd);
}

} // namespace mpris

MediaSurface* media_open(const MediaDesc& d, char* why, u32 cap) {
    char w[200] = "";
    DbusConnection* session = d.session_address ? dbus_open_address(d.session_address, 3.0, w, sizeof w)
                                                : dbus_open(DbusBus::Session, 3.0, w, sizeof w);
    if (!session) { kv::format(why, cap, "no session bus: %s", w); return nullptr; }
    MediaSurface* s = (MediaSurface*)mem_alloc(sizeof(MediaSurface));
    new (s) MediaSurface();
    kv::format(s->identity, sizeof s->identity, "%s", d.identity ? d.identity : "Blackwing");
    kv::format(s->desktop_entry, sizeof s->desktop_entry, "%s", d.desktop_entry ? d.desktop_entry : "");
    kv::format(s->base_name, sizeof s->base_name, "%s", d.name ? d.name : "blackwing");
    s->can_raise = d.can_raise;
    s->can_quit = d.can_quit;
    s->watch_sleep = d.watch_sleep;
    s->notify = d.notify;
    s->user = d.user;
    s->session = session;
    os_mutex_init(&s->lock);
    os_event_init(&s->event);
    kv::format(s->info.backend, sizeof s->info.backend, "mpris");

    // The object first, with its values, so a shell that hears the name appear reads them.
    const DbusInterfaceDesc* ifaces[] = {&kRootIface, &kPlayerIface};
    dbus_export(session, kPath, ifaces, 2);
    publish_root(s);
    os_mutex_lock(&s->lock);
    publish(s);
    s->dirty = false;
    os_mutex_unlock(&s->lock);
    char name[160];
    kv::format(name, sizeof name, "org.mpris.MediaPlayer2.%s", s->base_name);
    DbusNameReply got = dbus_request_name(session, name, kDbusNameDoNotQueue, 3.0);
    if (got == DbusNameReply::Exists) {
        kv::format(name, sizeof name, "org.mpris.MediaPlayer2.%s.instance%d", s->base_name, (int)getpid());
        got = dbus_request_name(session, name, kDbusNameDoNotQueue, 3.0);
    }
    if (got != DbusNameReply::PrimaryOwner && got != DbusNameReply::AlreadyOwner) {
        kv::format(why, cap, "the session bus would not give %s", name);
        dbus_close(session);
        os_event_destroy(&s->event);
        os_mutex_destroy(&s->lock);
        s->~MediaSurface();
        mem_free(s);
        return nullptr;
    }
    kv::format(s->info.name, sizeof s->info.name, "%s", name);

    if (d.watch_sleep) {
        s->system = d.system_address ? dbus_open_address(d.system_address, 3.0, w, sizeof w) : dbus_open(DbusBus::System, 3.0, w, sizeof w);
        if (s->system &&
            !dbus_add_match(s->system, "type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'", 3.0)) {
            kv::format(w, sizeof w, "logind's PrepareForSleep could not be asked for");
            dbus_close(s->system);
            s->system = nullptr;
        }
        s->info.sleep_watched = s->system != nullptr;
        if (!s->system) kv::format(s->info.sleep_why, sizeof s->info.sleep_why, "%.95s", w);
    } else {
        kv::format(s->info.sleep_why, sizeof s->info.sleep_why, "not asked to");
    }
    dbus_set_notify(session, wake, s);
    if (s->system) dbus_set_notify(s->system, wake, s);
    if (!os_thread_start(&s->thread, run, s)) {
        kv::format(why, cap, "no thread for the media surface");
        if (s->system) dbus_close(s->system);
        dbus_close(session);
        os_event_destroy(&s->event);
        os_mutex_destroy(&s->lock);
        s->~MediaSurface();
        mem_free(s);
        return nullptr;
    }
    os_event_set(&s->event); // anything that came before the notify was set
    return s;
}

void media_close(MediaSurface* s) {
    if (!s) return;
    os_mutex_lock(&s->lock);
    s->stop = true;
    os_mutex_unlock(&s->lock);
    os_event_set(&s->event);
    os_thread_join(&s->thread);
    dbus_set_notify(s->session, nullptr, nullptr);
    if (s->system) { dbus_set_notify(s->system, nullptr, nullptr); dbus_close(s->system); }
    dbus_close(s->session);
    s->commands.free();
    s->interruptions.free();
    os_event_destroy(&s->event);
    os_mutex_destroy(&s->lock);
    s->~MediaSurface();
    mem_free(s);
}

void media_set_track(MediaSurface* s, const MediaTrack* t) {
    os_mutex_lock(&s->lock);
    s->has_track = t != nullptr;
    if (t) s->track = *t;
    s->dirty = true;
    os_mutex_unlock(&s->lock);
    os_event_set(&s->event);
}

void media_set_status(MediaSurface* s, const MediaStatus& st) {
    os_mutex_lock(&s->lock);
    const MediaStatus& o = s->status;
    // Only what the shell shows wakes the thread; the position is read when asked.
    bool shown = o.state != st.state || o.volume != st.volume || o.can_play != st.can_play || o.can_pause != st.can_pause ||
                 o.can_next != st.can_next || o.can_previous != st.can_previous || o.can_seek != st.can_seek;
    s->status = st;
    if (shown) s->dirty = true;
    os_mutex_unlock(&s->lock);
    if (shown) os_event_set(&s->event);
}

void media_seeked(MediaSurface* s, i64 position_us, f64 at) {
    os_mutex_lock(&s->lock);
    s->status.position_us = position_us;
    s->status.at = at;
    s->seeked = true;
    s->seeked_us = position_us;
    os_mutex_unlock(&s->lock);
    os_event_set(&s->event);
}

bool media_next_command(MediaSurface* s, MediaCommand* out) {
    os_mutex_lock(&s->lock);
    bool have = s->commands.len != 0;
    if (have) {
        *out = s->commands.data[0];
        memmove(s->commands.data, s->commands.data + 1, sizeof(MediaCommand) * (s->commands.len - 1));
        s->commands.len--;
    }
    os_mutex_unlock(&s->lock);
    return have;
}

bool media_next_interruption(MediaSurface* s, MediaInterruption* out) {
    os_mutex_lock(&s->lock);
    bool have = s->interruptions.len != 0;
    if (have) {
        *out = s->interruptions.data[0];
        memmove(s->interruptions.data, s->interruptions.data + 1, sizeof(MediaInterruption) * (s->interruptions.len - 1));
        s->interruptions.len--;
    }
    os_mutex_unlock(&s->lock);
    return have;
}

void media_keep_awake(MediaSurface* s, bool on) {
    os_mutex_lock(&s->lock);
    bool change = s->want_awake != on;
    s->want_awake = on;
    if (change) s->awake_retry_at = 0.0;
    os_mutex_unlock(&s->lock);
    if (change) os_event_set(&s->event);
}

void media_sleep_ready(MediaSurface* s) {
    os_mutex_lock(&s->lock);
    s->ready = true;
    os_mutex_unlock(&s->lock);
    os_event_set(&s->event);
}

MediaInfo media_info(MediaSurface* s) {
    os_mutex_lock(&s->lock);
    MediaInfo i = s->info;
    os_mutex_unlock(&s->lock);
    return i;
}

} // namespace kv
