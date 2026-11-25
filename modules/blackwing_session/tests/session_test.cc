// The session over the player itself, on a capture device on the wall clock, by starting
// twice: an engine is run and shut down, and another started on the same directory.
// What was queued, where it had got to and how the chain was set are there again, paused;
// a kill loses no more than the position's clock; a file that will not parse is set
// aside; nothing is written while nothing changes.
#include "korvid/blackwing_player/module.h"
#include "korvid/blackwing_session/module.h"
#include "korvid/blackwing_session/session.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/json.h"
#include "korvid/core/arena.h"
#include "korvid/core/ktable.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <string.h>

namespace {

using namespace kv;

// What the session said was kept, as it was said at a start.
struct Said { char owner[16]; char key[32]; f64 number; char text[256]; };
constinit Said g_said[16] = {};
constinit u32 g_nsaid = 0;
constinit u32 g_started = 0;
constinit u64 g_started_item = 0;
void on_kept(TickContext&, Span<const SessionKept> ev) {
    for (const SessionKept& k : ev) {
        if (g_nsaid == 16) return;
        Said& s = g_said[g_nsaid++];
        memcpy(s.owner, k.owner, sizeof s.owner); memcpy(s.key, k.key, sizeof s.key); s.number = k.number; memcpy(s.text, k.text, sizeof s.text);
    }
}
void on_started(TickContext&, Span<const PlayerTrackStarted> ev) { for (const PlayerTrackStarted& e : ev) { ++g_started; g_started_item = e.item; } }
constinit const HandlerDesc k_handlers[] = {
    {.name = ".kept", KV_ON(SessionKept, on_kept), .phase = Phase::PostSim},
    {.name = ".started", KV_ON(PlayerTrackStarted, on_started), .phase = Phase::PostSim},
};
KV_MODULE_DEFINE(k_listener) = {.name = "listener", .handlers = k_handlers};

struct Run {
    Engine e;
    bool up = false;
    u32  most_a_tick = 0; // the most rows the session enqueued in one tick, while restoring
    u32  restore_ticks = 0;
    f64  worst_tick = 0.0;

    bool start(const char* dir, bool enabled = true, f64 change = 0.3, f64 position = 0.5) {
        g_nsaid = 0; g_started = 0; g_started_item = 0;
        PlayerConfig pc;
        pc.device = "capture"; // the player's own, on the wall clock
        player_configure(pc);
        SessionConfig sc;
        sc.enabled = enabled;
        sc.dir = dir;
        sc.change_seconds = change;
        sc.position_seconds = position;
        session_configure(sc);
        CliConfig cli; cli.no_stdin = true;
        cli_configure(cli);
        PlatformMounts mounts; mounts.none = true;
        platform_configure_mounts(mounts);
        static const ModuleDesc* const list[] = {&platform_module, &cli_module, &blackwing_player_module, &blackwing_session_module, &k_listener};
        up = engine_init(e, list);
        return up;
    }
    void stop() { if (up) engine_shutdown(e); up = false; session_configure(SessionConfig{}); player_configure(PlayerConfig{}); }
    void step() {
        u32 before = state().queue_restored;
        bool restoring = state().phase == (u8)SessionPhase::Restoring;
        f64 t0 = time_now();
        platform_pump(e);
        engine_frame(e, e.clock.tick_dt);
        f64 took = time_now() - t0;
        if (restoring) {
            ++restore_ticks;
            if (state().queue_restored - before > most_a_tick) most_a_tick = state().queue_restored - before;
            if (took > worst_tick) worst_tick = took;
        }
        time_sleep(0.002);
    }
    template <class F> bool until(F done, f64 seconds = 10.0) {
        f64 end = time_now() + seconds;
        for (;;) { step(); if (done()) return true; if (time_now() > end) return false; }
    }
    void run(f64 seconds) { f64 end = time_now() + seconds; while (time_now() < end) step(); }
    const SessionStatus& state() { return *session_status(e); }
    const PlayerTransport& t() { return *player_transport(e); }
    template <class E> void say(const E& ev) { e.events.emit(ev); }
    void add(const char* path) { PlayerEnqueue q{}; format(q.path, sizeof q.path, "%s", path); say(q); }
    // The queue's row at a place; a parameter's value heard, by stage and index.
    PlayerItem row(u32 pos) {
        KTable* q = engine_ktable(e, "player.queue");
        Span<PlayerItem> rows = q->rows_of<PlayerItem>();
        for (u32 i = 0; i < rows.len; ++i) if (q->is_alive(i) && rows[i].pos == pos) return rows[i];
        return PlayerItem{};
    }
    f64 param(u32 stage, u32 index) {
        KTable* p = engine_ktable(e, "player.params");
        Span<PlayerParam> rows = p->rows_of<PlayerParam>();
        for (u32 i = 0; i < rows.len; ++i) if (p->is_alive(i) && rows[i].stage == stage && rows[i].index == index) return rows[i].value;
        return -999.0;
    }
};

const Said* said(const char* owner, const char* key) {
    for (u32 i = 0; i < g_nsaid; ++i) if (strcmp(g_said[i].owner, owner) == 0 && strcmp(g_said[i].key, key) == 0) return &g_said[i];
    return nullptr;
}

// The path of the queue's k-th row in these tests: the long tone at every hundredth place,
// a file that is not there at the rest.
void path_of(char* out, usize cap, const char* tone, u32 k) {
    if (k % 100 == 0) format(out, cap, "%s", tone);
    else format(out, cap, "/nowhere/blackwing session test/%04u not there.flac", k);
}

// How many paths a session file's queue has; ~0 when it does not parse.
u32 file_rows(const char* path) {
    IoBytes file;
    if (!io_read_file(path, &file)) return ~0u;
    u32 n = ~0u;
    {
        MemScope scope;
        JsonDoc doc;
        if (json_parse((const char*)file.data, file.size, &scope.arena->a, doc))
            if (const JsonValue* q = json_get(doc, doc.root(), "queue")) n = q->is_array() ? q->as.kids.count : ~0u;
    }
    file.free();
    return n;
}

} // namespace

void test_session(kv::TestCtx& c) {
    using namespace kv;
    char tmp[1024], dir[1100], file[1200], tone[1100], path[kPlayerPathMax];
    KV_CHECK(c, io_make_temp_dir("bw_session", tmp, sizeof tmp));
    const char* here = __FILE__;
    const char* slash = strrchr(here, '/');
    format(tone, sizeof tone, "%.*s/../../hifi_codec/tests/data/tone_16_2ch_8000_long.flac", slash ? (int)(slash - here) : 0, here);

    // Quit in the middle of a track with a thousand rows queued, and start again.
    c.begin("session comes back");
    format(dir, sizeof dir, "%s/one", tmp);
    session_file_in(dir, file, sizeof file);
    u64 left_at = 0;
    {
        Run a;
        KV_CHECK(c, a.start(dir));
        KV_CHECK(c, a.until([&] { return a.state().phase == (u8)SessionPhase::Ready; }) && a.state().queue_total == 0 && !a.state().set_aside);
        for (u32 k = 1; k <= 1000; ++k) { path_of(path, sizeof path, tone, k); a.add(path); }
        KV_CHECK(c, a.until([&] { return a.t().queued == 1000; }));
        a.say(PlayerJump{500});
        a.say(PlayerSetParam{0, 0, -12.0});
        a.say(PlayerSetParam{0, 1, 1.0});
        a.say(PlayerSetParam{1, 2, 3.5});
        SessionKeep k1; format(k1.owner, sizeof k1.owner, "a face"); format(k1.key, sizeof k1.key, "tab"); format(k1.text, sizeof k1.text, "events");
        SessionKeep k2; format(k2.owner, sizeof k2.owner, "a face"); format(k2.key, sizeof k2.key, "w"); k2.number = 1234.0;
        a.say(k1); a.say(k2);
        k1.number = 7.0; format(k1.text, sizeof k1.text, "tables"); // the newest for a key replaces the one before
        a.say(k1);
        KV_CHECK(c, a.until([&] { return a.t().state == (u8)PlayerState::Playing && a.t().position > 8000 && a.param(0, 0) == -12.0 && a.param(1, 2) == 3.5; }));
        KV_CHECK(c, a.t().place_pos == 500 && a.t().item == 500);
        left_at = a.t().position;
        a.stop(); // quit: saved at exit
    }
    KV_CHECK(c, file_rows(file) == 1000);
    { IoStat st; if (io_stat_file(file, &st)) KV_INFO("test", "session: the file for a thousand rows is %llu bytes", (unsigned long long)st.size); }
    {
        Run b;
        KV_CHECK(c, b.start(dir));
        KV_CHECK(c, b.state().phase == (u8)SessionPhase::Restoring && b.state().queue_total == 1000);
        // It answers while the queue comes back: ticks go on, a few dozen rows each.
        KV_CHECK(c, b.until([&] { return b.state().phase == (u8)SessionPhase::Ready; }));
        KV_INFO("test", "session: a thousand rows back in %u ticks, %u a tick at most, the longest tick %.2f ms", b.restore_ticks, b.most_a_tick, b.worst_tick * 1e3);
        KV_CHECK(c, b.most_a_tick == 64 && b.restore_ticks >= 16 && b.state().queue_restored == 1000);
        // The same queue in the same order; a row whose file has gone is a row all the same.
        KV_CHECK(c, b.t().queued == 1000);
        bool same = true;
        for (u32 k = 1; k <= 1000 && same; ++k) { path_of(path, sizeof path, tone, k); same = strcmp(b.row(k).path, path) == 0; }
        KV_CHECK(c, same && b.row(499).state == (u8)PlayerItemState::Idle);
        // The place at the same row, the position where it was, paused; nothing heard.
        KV_CHECK(c, b.t().state == (u8)PlayerState::Paused && b.t().place_pos == 500 && b.t().item == b.row(500).id && g_started == 0);
        KV_CHECK(c, b.t().position == left_at && b.t().rate == 8000 && b.t().frames == 32000);
        // The volume, the mute and the band as they were.
        KV_CHECK(c, b.until([&] { return b.param(0, 0) == -12.0 && b.param(0, 1) == 1.0 && b.param(1, 2) == 3.5; }, 3.0));
        // What was kept for others, said once each: the newest of each key.
        KV_CHECK(c, g_nsaid == 2 && said("a face", "tab") && strcmp(said("a face", "tab")->text, "tables") == 0 && said("a face", "tab")->number == 7.0 && said("a face", "w") && said("a face", "w")->number == 1234.0);
        // Back as it was left, and nothing has changed: nothing is written, and nothing is asked.
        b.run(1.0);
        KV_CHECK(c, b.state().saves == 0 && !b.state().dirty && !b.e.ticks_wanted);
        // One press plays from where it was.
        b.say(PlayerPlay{});
        KV_CHECK(c, b.until([&] { return g_started == 1; }, 4.0) && g_started_item == b.row(500).id && b.t().position >= left_at);
        b.stop();
    }

    // Killed while playing: the next start is within the position's clock of where it was.
    c.begin("session killed");
    format(dir, sizeof dir, "%s/two", tmp);
    session_file_in(dir, file, sizeof file);
    {
        Run a;
        KV_CHECK(c, a.start(dir, true, 0.3, 0.5));
        a.add(tone); a.add(tone);
        a.say(PlayerPlay{});
        // The position is saved on its clock while it plays: the file as a kill would leave it.
        KV_CHECK(c, a.until([&] { return a.state().saves >= 3; }, 8.0));
        u32 saves = a.state().saves;
        a.run(0.35); // and it plays on a while, as it would before the kill
        u64 killed_at = a.t().position;
        u64 killed_item = a.t().item;
        IoBytes as_killed;
        KV_CHECK(c, io_read_file(file, &as_killed) && a.state().saves <= saves + 1);
        KV_INFO("test", "session: while playing, %u saves; the last %llu bytes, in place %.1f ms after it was asked for", a.state().saves,
                (unsigned long long)a.state().save_bytes, a.state().save_seconds * 1e3);
        a.stop();
        io_write_file(file, as_killed.data, as_killed.size); // the clean exit's save undone: what a kill left
        as_killed.free();
        Run b;
        KV_CHECK(c, b.start(dir));
        KV_CHECK(c, b.until([&] { return b.state().phase == (u8)SessionPhase::Ready; }));
        u64 lost = killed_at > b.t().position ? killed_at - b.t().position : 0;
        KV_INFO("test", "session: killed at %.2f s, back at %.2f s", (f64)killed_at / 8000.0, (f64)b.t().position / 8000.0);
        KV_CHECK(c, b.t().queued == 2 && b.t().state == (u8)PlayerState::Paused && b.t().place_pos == (u32)killed_item && b.t().position <= killed_at);
        KV_CHECK(c, lost <= (u64)(8000 * (0.5 + 0.25 + 0.35 + 0.2))); // the clock, a look, the while it played on, and slack
        b.stop();
    }

    // A session that cannot be read is set aside, and the start is an empty one. An older
    // one, and one with things in it nobody knows, are read for what they have.
    c.begin("session unreadable");
    format(dir, sizeof dir, "%s/three", tmp);
    session_file_in(dir, file, sizeof file);
    io_make_dirs(dir);
    {
        static const char k_junk[] = "{ \"blackwing_session\": 1, \"queue\": [ \"/a.flac\", ";
        KV_CHECK(c, io_write_file(file, k_junk, sizeof k_junk - 1));
        Run a;
        KV_CHECK(c, a.start(dir));
        KV_CHECK(c, a.until([&] { return a.state().phase == (u8)SessionPhase::Ready; }));
        KV_CHECK(c, a.state().set_aside == 1 && a.state().reason[0] && a.t().queued == 0 && !io_is_file(file)); // not overwritten: moved
        // And the next thing to keep is kept, in a new file.
        a.add(tone);
        KV_CHECK(c, a.until([&] { return a.state().saves == 1; }, 5.0) && file_rows(file) == 1);
        a.stop();
    }
    {
        static const char k_old[] = "{ \"blackwing_session\": 0, \"queue\": [ \"/music/a.flac\", \"/music/b.flac\" ], \"place\": 2,\n"
                                    "  \"someday\": { \"shuffle\": true }, \"params\": [ { \"stage\": \"gain\", \"name\": \"gain_db\", \"value\": -3.0 },\n"
                                    "  { \"stage\": \"reverb\", \"name\": \"room\", \"value\": 0.4 }, { \"stage\": \"gain\" } ] }";
        KV_CHECK(c, io_write_file(file, k_old, sizeof k_old - 1));
        Run a;
        KV_CHECK(c, a.start(dir));
        KV_CHECK(c, a.until([&] { return a.state().phase == (u8)SessionPhase::Ready; }));
        KV_CHECK(c, !a.state().set_aside && a.t().queued == 2 && strcmp(a.row(2).path, "/music/b.flac") == 0);
        KV_CHECK(c, a.t().state == (u8)PlayerState::Paused && a.t().place_pos == 2 && a.t().position == 0); // what it lacks keeps its default
        KV_CHECK(c, a.until([&] { return a.param(0, 0) == -3.0; }, 3.0));                                    // what it has is used; a stage nobody has is passed over
        a.stop();
    }

    // A drag is one write; still is free; off is off.
    c.begin("session still");
    format(dir, sizeof dir, "%s/four", tmp);
    session_file_in(dir, file, sizeof file);
    {
        Run a;
        KV_CHECK(c, a.start(dir, true, 0.4, 10.0));
        KV_CHECK(c, a.until([&] { return a.state().phase == (u8)SessionPhase::Ready; }));
        a.run(0.6);
        KV_CHECK(c, a.state().saves == 0 && !io_is_file(file)); // nothing to keep yet: nothing written
        for (u32 i = 0; i < 40; ++i) { a.say(PlayerSetParam{0, 0, -1.0 - (f64)i * 0.5}); a.step(); a.step(); } // a slider dragged
        KV_CHECK(c, a.until([&] { return a.state().saves >= 1; }, 5.0));
        a.run(1.2);
        KV_CHECK(c, a.state().saves == 1 && !a.state().dirty && a.param(0, 0) == -20.5);
        u32 saves = a.state().saves;
        a.run(1.0);
        KV_CHECK(c, a.state().saves == saves && !a.e.ticks_wanted);
        a.stop();
    }
    format(dir, sizeof dir, "%s/five", tmp);
    session_file_in(dir, file, sizeof file);
    {
        Run a;
        KV_CHECK(c, a.start(dir, false));
        a.add(tone);
        SessionKeep k; format(k.owner, sizeof k.owner, "x"); format(k.key, sizeof k.key, "y");
        a.say(k);
        a.run(0.8);
        KV_CHECK(c, a.state().phase == (u8)SessionPhase::Off && a.state().saves == 0);
        a.stop();
        KV_CHECK(c, !io_is_file(file));
    }
    // A number someone kept, asked of the file with no engine: what a window's size is read with.
    format(dir, sizeof dir, "%s/one", tmp);
    KV_CHECK(c, session_peek(dir, "a face", "w", 0.0) == 1234.0 && session_peek(dir, "a face", "h", 800.0) == 800.0 && session_peek("/nowhere", "a", "b", 5.0) == 5.0);
    io_remove_tree(tmp);
}
