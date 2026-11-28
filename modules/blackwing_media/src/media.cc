// The media surface on the busses (media.h). A tick takes what the OS asked and emits it as
// the player's commands, and tells the surface what changed: the track when it is heard,
// the state when it changes, the place on a seek. Titles are the library's, through a
// window over the one path; art is found on a thread of the module's own, which wakes the
// loop when it has an answer. The surface's own thread wakes it for a command.
#include "korvid/blackwing_media/module.h"
#include "korvid/blackwing_media/media.h"
#include "korvid/blackwing_media/schema.h"
#include "korvid/blackwing_library/library.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/media_platform/media.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"
#include "korvid/core/log.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform/paths.h"
#include <math.h>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kMediaWindow = 0xD0E10001; // the library window over the playing file's path
constexpr f64 kReanchor = 5.0;           // the position given again this often while playing, quietly: the DAC's clock and the wall's drift apart

constinit MediaConfig g_config = {};

// The art, found off the tick.
struct ArtJob {
    OsThread thread;
    OsMutex  lock;
    OsEvent  event;
    Engine*  e = nullptr;
    bool     stop = false;          // under lock
    bool     asked = false;         // under lock: `item` and `path` to look for
    u64      item = 0;
    char     path[kPlayerPathMax] = "";
    bool     answered = false;      // under lock: `done_item`'s art is `art`
    u64      done_item = 0;
    char     art[1024] = "";
    char     dir[1024] = "";
};

struct Media {
    Engine*       e = nullptr;
    MediaSurface* surface = nullptr;
    MediaStatusRow row;
    ArtJob        art;
    bool          art_running = false;

    // From the handlers, for the tick.
    u64  started_item = 0;
    bool started = false;
    bool seeked = false;
    u64  seek_item = 0, seek_frame = 0;

    MediaTrack track;
    bool  has_track = false;
    bool  track_dirty = false;
    bool  asked_library = false;
    char  asked_path[kPlayerPathMax] = ""; // as the queue has it: what the library's row says
    MediaStatus last;
    bool  sent_once = false;
    f64   anchored_at = 0.0;
    f64   stopped_at = -1.0;   // when the transport last stopped playing; -1 while it plays
    bool  awake = false;
    bool  sleep_pending = false;
    u32   gain_stage = 0xFFFFFFFFu;
};

Media* media_of(Engine& e) { return engine_module_state_as<Media>(e, "media"); }

void wake_loop(void* user) { platform_wake(*(Engine*)user); }

// ---- the art's thread ---------------------------------------------------------------

void art_run(void* arg) {
    ArtJob& j = *(ArtJob*)arg;
    for (;;) {
        os_event_wait(&j.event, 3600.0);
        os_mutex_lock(&j.lock);
        bool stop = j.stop, asked = j.asked;
        u64 item = j.item;
        char path[kPlayerPathMax];
        memcpy(path, j.path, sizeof path);
        j.asked = false;
        os_mutex_unlock(&j.lock);
        if (stop) return;
        if (!asked) continue;
        char found[1024] = "";
        media_find_art(path, j.dir, found, sizeof found);
        os_mutex_lock(&j.lock);
        j.answered = true;
        j.done_item = item;
        memcpy(j.art, found, sizeof j.art);
        os_mutex_unlock(&j.lock);
        platform_wake(*j.e);
    }
}

void ask_art(Media& m, u64 item, const char* path) {
    if (!m.art_running) return;
    os_mutex_lock(&m.art.lock);
    m.art.asked = true;
    m.art.item = item;
    format(m.art.path, sizeof m.art.path, "%s", path);
    os_mutex_unlock(&m.art.lock);
    os_event_set(&m.art.event);
}

// ---- what the player says ------------------------------------------------------------

void on_started(TickContext& ctx, Span<const PlayerTrackStarted> ev) {
    Media& m = ctx.state_as<Media>();
    for (const PlayerTrackStarted& s : ev) { m.started = true; m.started_item = s.item; }
}

void on_seeked(TickContext& ctx, Span<const PlayerSeeked> ev) {
    Media& m = ctx.state_as<Media>();
    for (const PlayerSeeked& s : ev) { m.seeked = true; m.seek_item = s.item; m.seek_frame = s.frame; }
}

const PlayerItem* queue_row(const KTable* q, u64 id) {
    if (!q || !id) return nullptr;
    Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
    for (u32 i = 0; i < rows.len; ++i)
        if (q->is_alive(i) && rows[i].id == id) return &rows[i];
    return nullptr;
}

// A file's name without its directories or its extension: a title until the library's.
void stem_of(const char* path, char* out, usize cap) {
    const char* s = strrchr(path, '/');
    s = s ? s + 1 : path;
    const char* dot = strrchr(s, '.');
    format(out, cap, "%.*s", (int)(dot && dot > s ? dot - s : (i64)strlen(s)), s);
}

f64 gain_db(TickContext& ctx, Media& m) {
    if (m.gain_stage == 0xFFFFFFFFu) {
        if (const KTable* chain = ctx.read_ktable("player.chain")) {
            Span<const PlayerStage> rows = chain->rows_of<PlayerStage>();
            for (u32 i = 0; i < rows.len; ++i)
                if (chain->is_alive(i) && strcmp(rows[i].name, "gain") == 0) m.gain_stage = rows[i].index;
        }
    }
    if (const KTable* params = ctx.read_ktable("player.params")) {
        Span<const PlayerParam> rows = params->rows_of<PlayerParam>();
        for (u32 i = 0; i < rows.len; ++i)
            if (params->is_alive(i) && rows[i].stage == m.gain_stage && rows[i].index == 0) return rows[i].value;
    }
    return 1.0; // none: there is no volume to show
}

void take_commands(TickContext& ctx, Media& m, const PlayerTransport* t) {
    MediaCommand c;
    while (media_next_command(m.surface, &c)) {
        bool playing = t && (t->state == (u8)PlayerState::Playing || t->state == (u8)PlayerState::Starting);
        switch (c.kind) {
        case MediaCommandKind::Play:      ctx.emit(PlayerPlay{}); break;
        case MediaCommandKind::Pause:     ctx.emit(PlayerPause{}); break;
        case MediaCommandKind::PlayPause: if (playing) ctx.emit(PlayerPause{}); else ctx.emit(PlayerPlay{}); break;
        case MediaCommandKind::Stop:      ctx.emit(PlayerStop{}); break;
        case MediaCommandKind::Next:      ctx.emit(PlayerNext{}); break;
        case MediaCommandKind::Previous:  ctx.emit(PlayerPrevious{}); break;
        case MediaCommandKind::SeekBy:    ctx.emit(PlayerSeekBy{(f64)c.us / 1e6}); break;
        case MediaCommandKind::SeekTo:
            if (t && t->item == c.track && t->rate) ctx.emit(PlayerSeek{c.track, (u64)((f64)c.us * t->rate / 1e6)});
            break;
        case MediaCommandKind::Volume:
            if (m.gain_stage != 0xFFFFFFFFu) {
                f64 db = c.volume <= 0.0 ? -100.0 : 20.0 * log10(c.volume);
                if (db < -100.0) db = -100.0;
                if (db > 0.0) db = 0.0;
                ctx.emit(PlayerSetParam{m.gain_stage, 0, db});
            }
            break;
        case MediaCommandKind::Raise: ctx.emit(MediaRaise{}); break;
        case MediaCommandKind::Quit:  engine_request_quit(*ctx.engine); break;
        default: break;
        }
    }
}

void take_interruptions(TickContext& ctx, Media& m, const PlayerTransport* t) {
    MediaInterruption in;
    while (media_next_interruption(m.surface, &in)) {
        if (in.kind == MediaInterruptionKind::Sleep) {
            bool playing = t && (t->state == (u8)PlayerState::Playing || t->state == (u8)PlayerState::Starting);
            if (playing) ctx.emit(PlayerPause{});
            m.sleep_pending = true;
            MediaInterrupted e{};
            format(e.reason, sizeof e.reason, "%s", in.reason);
            ctx.emit(e);
        } else {
            MediaResumed e{};
            format(e.reason, sizeof e.reason, "%s", in.reason);
            ctx.emit(e);
        }
    }
    // The pause has landed (or there was nothing to pause): the machine may sleep.
    if (m.sleep_pending && !(t && (t->state == (u8)PlayerState::Playing || t->state == (u8)PlayerState::Starting))) {
        media_sleep_ready(m.surface);
        m.sleep_pending = false;
    }
}

void media_tick(TickContext& ctx) {
    Media& m = ctx.state_as<Media>();
    if (!m.surface) return;
    const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
    const KTable* q = ctx.read_ktable("player.queue");
    f64 now = time_now();
    f64 vol_db = gain_db(ctx, m);
    take_commands(ctx, m, t);
    take_interruptions(ctx, m, t);
    if (!t) return;

    // The track, when it is heard.
    if (m.started) {
        m.started = false;
        if (const PlayerItem* it = queue_row(q, m.started_item)) {
            m.track = MediaTrack{};
            m.track.id = it->id;
            // The OS is given a URL, which is absolute: a path typed relative is the current
            // directory's.
            char cwd[1024];
            bool relative = it->path[0] != '/' && !(it->path[0] && it->path[1] == ':');
            if (relative && io_current_dir(cwd, sizeof cwd)) format(m.track.path, sizeof m.track.path, "%s/%s", cwd, it->path);
            else format(m.track.path, sizeof m.track.path, "%s", it->path);
            stem_of(it->path, m.track.title, sizeof m.track.title);
            if (it->rate && it->frames) m.track.length_us = (i64)((f64)it->frames * 1e6 / it->rate);
            m.has_track = true;
            m.track_dirty = true;
            LibraryPath p;
            p.id = kMediaWindow;
            p.index = 0;
            format(p.path, sizeof p.path, "%s", it->path);
            format(m.asked_path, sizeof m.asked_path, "%s", it->path);
            ctx.emit(p);
            m.asked_library = true;
            ask_art(m, it->id, it->path);
        }
    }
    // Nothing to be heard any more: nothing playing.
    if (t->state == (u8)PlayerState::Stopped && t->item == 0 && t->place == 0 && m.has_track) {
        m.has_track = false;
        m.track_dirty = true;
    }
    // The library's titles for it.
    if (m.has_track && m.asked_library)
        if (const KTable* lib = ctx.read_ktable("library.rows")) {
            Span<const LibraryRow> rows = lib->rows_of<LibraryRow>();
            for (u32 i = 0; i < rows.len; ++i) {
                const LibraryRow& r = rows[i];
                if (!lib->is_alive(i) || r.query != kMediaWindow || r.index != 0 || strcmp(r.path, m.asked_path) != 0) continue;
                if (r.title[0]) format(m.track.title, sizeof m.track.title, "%s", r.title);
                format(m.track.artist, sizeof m.track.artist, "%s", r.artist);
                format(m.track.album, sizeof m.track.album, "%s", r.album);
                m.track.track_number = r.track_number;
                if (!m.track.length_us && r.frames && r.rate) m.track.length_us = (i64)((f64)r.frames * 1e6 / r.rate);
                m.asked_library = false;
                m.track_dirty = true;
            }
        }
    // The art's answer.
    if (m.art_running) {
        os_mutex_lock(&m.art.lock);
        if (m.art.answered) {
            m.art.answered = false;
            if (m.has_track && m.art.done_item == m.track.id) {
                format(m.track.art, sizeof m.track.art, "%s", m.art.art);
                m.track_dirty = true;
            }
        }
        os_mutex_unlock(&m.art.lock);
    }
    if (m.track_dirty) {
        m.track_dirty = false;
        media_set_track(m.surface, m.has_track ? &m.track : nullptr);
        format(m.row.title, sizeof m.row.title, "%s", m.has_track ? m.track.title : "");
        format(m.row.art, sizeof m.row.art, "%s", m.has_track ? m.track.art : "");
    }

    // The state, and the place with its rate.
    bool playing = t->state == (u8)PlayerState::Playing;
    bool moving = playing || t->state == (u8)PlayerState::Starting;
    MediaStatus st;
    st.state = moving ? MediaState::Playing : t->state == (u8)PlayerState::Paused ? MediaState::Paused : MediaState::Stopped;
    st.position_us = t->rate ? (i64)((f64)t->position * 1e6 / t->rate) : 0;
    st.at = now;
    st.rate = playing ? 1.0 : 0.0;
    st.volume = m.gain_stage == 0xFFFFFFFFu ? -1.0 : vol_db <= -100.0 ? 0.0 : pow(10.0, vol_db / 20.0);
    st.can_play = t->queued > 0;
    st.can_pause = moving;
    st.can_next = t->place_pos < t->queued;
    st.can_previous = t->queued > 0;
    st.can_seek = t->frames > 0;
    bool changed = !m.sent_once || st.state != m.last.state || st.rate != m.last.rate || st.volume != m.last.volume ||
                   st.can_play != m.last.can_play || st.can_pause != m.last.can_pause || st.can_next != m.last.can_next ||
                   st.can_previous != m.last.can_previous || st.can_seek != m.last.can_seek || m.seeked;
    if (changed || (playing && now - m.anchored_at > kReanchor)) {
        media_set_status(m.surface, st);
        if (m.seeked && t->item == m.seek_item && t->rate) media_seeked(m.surface, (i64)((f64)m.seek_frame * 1e6 / t->rate), now);
        m.seeked = false;
        m.last = st;
        m.sent_once = true;
        m.anchored_at = now;
    }
    m.row.playing = moving;

    // Held awake while it plays, and a little after.
    if (moving) m.stopped_at = -1.0;
    else if (m.stopped_at < 0) m.stopped_at = now;
    bool want = moving || (m.stopped_at >= 0 && now - m.stopped_at < g_config.awake_linger);
    if (want != m.awake) { media_keep_awake(m.surface, want); m.awake = want; }
    if (!moving && want) engine_want_ticks(*ctx.engine); // to let go when the linger is up

    MediaInfo info = media_info(m.surface);
    m.row.awake = info.awake;
    m.row.sleep_watched = info.sleep_watched;
    m.row.commands = info.commands;
    m.row.interruptions = info.interruptions;
    m.row.clients_asked = info.clients_asked;
    format(m.row.awake_how, sizeof m.row.awake_how, "%s", info.awake_how);
    format(m.row.last_command, sizeof m.row.last_command, "%s", info.last_command);
    if (moving && !info.awake && info.awake_why[0]) format(m.row.why, sizeof m.row.why, "%s", info.awake_why);
}

// ---- the command line ---------------------------------------------------------------

void cmd_status(CliContext& ctx, Span<const char* const>) {
    Media* m = media_of(*ctx.engine);
    if (!m || !m->row.open) { ctx.print("no media surface%s%s", m && m->row.why[0] ? ": " : "", m ? m->row.why : ""); return; }
    const MediaStatusRow& r = m->row;
    ctx.print("%s as %s; %s; %s", r.backend, r.name, r.playing ? "playing" : "not playing",
              r.awake ? (r.awake_how[0] ? r.awake_how : "awake") : "not held awake");
    ctx.print("sleep %s; %u commands from the OS (last: %s), %u reads, %u interruptions", r.sleep_watched ? "heard" : "not heard",
              r.commands, r.last_command[0] ? r.last_command : "none", r.clients_asked, r.interruptions);
    if (r.title[0]) ctx.print("shown: %s%s%s", r.title, r.art[0] ? ", art " : "", r.art);
}

// ---- the module ---------------------------------------------------------------------

bool media_startup(Engine& e, ModuleInstance& self) {
    Media* m = arena_push<Media>(self.arena);
    if (!m) return false;
    new (m) Media();
    self.set_state(m);
    m->e = &e;
    if (!engine_publish(e, self, ".status", &m->row)) return false;
    if (!g_config.enabled) { format(m->row.why, sizeof m->row.why, "not asked for in this run"); return true; }
    MediaDesc d;
    d.identity = "Blackwing";
    d.desktop_entry = "blackwing";
    d.name = g_config.name ? g_config.name : "blackwing";
    d.can_raise = g_config.can_raise;
    d.can_quit = true;
    d.session_address = g_config.session_address;
    d.system_address = g_config.system_address;
    d.watch_sleep = g_config.watch_sleep;
    d.notify = wake_loop;
    d.user = &e;
    char why[200] = "";
    m->surface = media_open(d, why, sizeof why);
    if (!m->surface) {
        KV_INFO("media", "no media surface: %s", why);
        format(m->row.why, sizeof m->row.why, "%.95s", why);
        return true; // a player with no surface still plays
    }
    MediaInfo info = media_info(m->surface);
    m->row.open = 1;
    format(m->row.backend, sizeof m->row.backend, "%s", info.backend);
    format(m->row.name, sizeof m->row.name, "%s", info.name);
    // The art's thread, and where it writes.
    if (g_config.art_dir) format(m->art.dir, sizeof m->art.dir, "%s", g_config.art_dir);
    else if (const char* cache = platform_user_dir(e, UserDir::Cache, "blackwing", "blackwing")) format(m->art.dir, sizeof m->art.dir, "%s/art", cache);
    m->art.e = &e;
    os_mutex_init(&m->art.lock);
    os_event_init(&m->art.event);
    m->art_running = os_thread_start(&m->art.thread, art_run, &m->art);
    if (!m->art_running) { os_event_destroy(&m->art.event); os_mutex_destroy(&m->art.lock); }
    return true;
}

void media_shutdown(Engine& e, ModuleInstance&) {
    Media* m = media_of(e);
    if (!m) return;
    if (m->art_running) {
        os_mutex_lock(&m->art.lock);
        m->art.stop = true;
        os_mutex_unlock(&m->art.lock);
        os_event_set(&m->art.event);
        os_thread_join(&m->art.thread);
        os_event_destroy(&m->art.event);
        os_mutex_destroy(&m->art.lock);
        m->art_running = false;
    }
    media_close(m->surface);
    m->surface = nullptr;
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".started", KV_ON(PlayerTrackStarted, on_started), .phase = Phase::PreSim},
    {.name = ".seeked", KV_ON(PlayerSeeked, on_seeked), .phase = Phase::PreSim},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::PostSim, .fn = media_tick, .reads = "player.transport, player.queue, player.chain, player.params, library.rows",
     .writes = ".status",
     .emits = "PlayerPlay, PlayerPause, PlayerStop, PlayerNext, PlayerPrevious, PlayerSeek, PlayerSeekBy, PlayerSetParam, LibraryPath, MediaRaise, MediaInterrupted, MediaResumed",
     .main_thread = true, .runs = RunWhen::Always},
};
constinit const CliCommandDesc k_commands[] = {
    {.name = "media status", .help = "the OS's media surface: the name owned, what it shows, keep-awake, sleep, the commands it took", .fn = cmd_status},
};

} // namespace

void media_configure(const MediaConfig& config) { g_config = config; }

const MediaStatusRow* media_status(Engine& e) {
    Media* m = media_of(e);
    return m ? &m->row : nullptr;
}

KV_MODULE_DEFINE(blackwing_media_module) = {
    .name      = "media",
    .events    = k_media_events,
    .resources = k_media_resources,
    .systems   = k_systems,
    .handlers  = k_handlers,
    .commands  = k_commands,
    .startup   = media_startup,
    .shutdown  = media_shutdown,
    .memory    = {.reserve = 64 * 1024},
};

} // namespace kv
