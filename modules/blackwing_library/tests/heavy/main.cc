// The library against fifty thousand files: `kormake test blackwing_library --heavy`, or
// with `-- DIR` to make them somewhere else. The default is build/library-heavy in this
// checkout, on the disk the checkout is on (not /tmp, which may be memory).
//
// From LibraryAddRoot on an empty index: when a chosen track is first found by name, and
// when it is first heard through the player (into the capture device on the wall clock);
// the walk's whole time and rate; then the second start, with the index built: how soon a
// window answers, and the walk again. The files are made once and kept for the next run;
// the index is deleted before each.
#include "korvid/blackwing_library/module.h"
#include "korvid/blackwing_player/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/hifi_platform/capture.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "../gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace kv;

namespace {

constexpr u32 kFiles = 50000;

constinit u32 g_finished = 0;
constinit f64 g_walk_seconds = 0.0;
constinit u32 g_walk_tracks = 0;
constinit u32 g_started = 0;
constinit LibraryResult g_last = {};
constinit u32 g_results = 0;
void on_finished(TickContext&, Span<const LibraryScanFinished> ev) { for (auto& e : ev) { ++g_finished; g_walk_seconds = e.seconds; g_walk_tracks = e.tracks; } }
void on_started(TickContext&, Span<const PlayerTrackStarted> ev) { g_started += ev.len; }
void on_result(TickContext&, Span<const LibraryResult> ev) { for (auto& e : ev) { g_last = e; ++g_results; } }
constinit const HandlerDesc k_handlers[] = {
    {.name = ".finished", KV_ON(LibraryScanFinished, on_finished), .phase = Phase::PostSim},
    {.name = ".started",  KV_ON(PlayerTrackStarted, on_started),   .phase = Phase::PostSim},
    {.name = ".result",   KV_ON(LibraryResult, on_result),         .phase = Phase::PostSim},
};
KV_MODULE_DEFINE(k_listener) = {.name = "listener", .handlers = k_handlers};

struct Rig {
    Engine e;
    HifiCapture* cap = nullptr;
    bool up = false;
    bool start(const char* data) {
        g_finished = g_started = g_results = 0;
        HifiCaptureDesc cd;
        cd.wall_clock = true;
        cap = hifi_capture_create(cd);
        PlayerConfig pc;
        pc.device = "capture";
        pc.device_backend = cap;
        player_configure(pc);
        LibraryConfig lc;
        lc.data_dir = data;
        library_configure(lc);
        CliConfig cli; cli.no_stdin = true;
        cli_configure(cli);
        PlatformMounts mounts; mounts.none = true;
        platform_configure_mounts(mounts);
        static const ModuleDesc* const list[] = {&platform_module, &cli_module, &blackwing_player_module, &blackwing_library_module, &k_listener};
        up = engine_init(e, list);
        return up;
    }
    void stop() { if (up) engine_shutdown(e); up = false; hifi_capture_destroy(cap); cap = nullptr; }
    void step() { platform_pump(e); engine_frame(e, e.clock.tick_dt); time_sleep(0.002); }
    const LibraryRow* row(u32 query, u32 index) {
        KTable* t = engine_ktable(e, "library.rows");
        Span<LibraryRow> r = t->rows_of<LibraryRow>();
        for (u32 i = 0; i < r.len; ++i) if (t->is_alive(i) && r[i].query == query && r[i].index == index) return &r[i];
        return nullptr;
    }
};

} // namespace

int main(int argc, char** argv) {
    TestCtx c;
    c.begin("library heavy");
    log_set_level(LogLevel::Warn);
    char dir[2048];
    if (argc > 1) format(dir, sizeof dir, "%s", argv[1]);
    else {
        const char* here = __FILE__; // .../modules/blackwing_library/tests/heavy/main.cc
        const char* m = strstr(here, "/modules/");
        format(dir, sizeof dir, "%.*s/build/library-heavy", m ? (int)(m - here) : 0, here);
    }
    char music[2100], data[2100], marker[2200];
    format(music, sizeof music, "%s/music", dir);
    format(data, sizeof data, "%s/data", dir);
    format(marker, sizeof marker, "%s/made-%u", dir, kFiles);
    if (!io_is_file(marker)) {
        io_remove_tree(music);
        io_make_dirs(music);
        f64 t = time_now();
        kvtest::Made m = kvtest::make_library(music, kFiles);
        printf("made %llu files (%.1f MB) in %llu albums in %.1f s under %s\n", (unsigned long long)m.files, (f64)m.bytes / 1e6,
               (unsigned long long)m.dirs, time_now() - t, music);
        io_write_now(marker, "", 0);
    }
    io_remove_tree(data);

    // The chosen tracks, by a word only their titles have (five digits: no other title's
    // number starts with them). The walk is breadth first, so a file is a row once its own
    // directory is listed, wherever that is in the walk.
    const u32 targets[] = {10123, kFiles - 7};
    Rig r;
    KV_CHECK(c, r.start(data));
    f64 t0 = time_now();
    LibraryAddRoot a{};
    format(a.path, sizeof a.path, "%s", music);
    r.e.events.emit(a);
    for (u32 k = 0; k < 2; ++k) {
        char word[32];
        format(word, sizeof word, "zq%u", targets[k]);
        f64 found = 0.0, heard = 0.0, asked = 0.0;
        u32 started = g_started;
        f64 give_up = time_now() + 60.0;
        while (time_now() < give_up && !heard) {
            r.step();
            if (!found && time_now() - asked > 0.05) {
                LibraryQuery q;
                q.id = 1; q.view = LibraryView::Search; q.count = 5;
                format(q.text, sizeof q.text, "%s", word);
                r.e.events.emit(q);
                asked = time_now();
            }
            const LibraryRow* hit = found ? nullptr : r.row(1, 0);
            if (!found && hit && g_last.id == 1 && g_last.total == 1 && strstr(hit->title, word + 2)) {
                found = time_now() - t0;
                PlayerEnqueue q{};
                memcpy(q.path, hit->path, sizeof q.path);
                r.e.events.emit(q);
                r.e.events.emit(PlayerPlay{});
            }
            if (found && g_started > started) heard = time_now() - t0;
        }
        printf("track %u: found by name %.3f s after the root was added, heard at %.3f s (the walk %s)\n", targets[k], found, heard,
               g_finished ? "was over" : "was not");
        if (k == 0) KV_CHECK(c, heard > 0.0 && heard < 3.0); // the criterion: playing a chosen one within three seconds
        KV_CHECK(c, found > 0.0 && heard > 0.0);
    }
    while (!g_finished && time_now() - t0 < 300.0) r.step();
    KV_CHECK(c, g_finished == 1 && g_walk_tracks == kFiles);
    printf("names pass: %u files in %.2f s, %.0f files a second\n", g_walk_tracks, g_walk_seconds, g_walk_tracks / g_walk_seconds);
    r.stop();

    u64 index_bytes = 0;
    {
        char ix[2200];
        format(ix, sizeof ix, "%s/index.sqlite", data);
        IoStat st;
        if (io_stat(ix, &st)) index_bytes = st.size;
        format(ix, sizeof ix, "%s/index.sqlite-wal", data);
        if (io_stat(ix, &st)) index_bytes += st.size;
    }
    printf("index: %.1f MB for %u tracks\n", (f64)index_bytes / 1e6, kFiles);

    // The second start: a window answers, and the walk again finds nothing new to read.
    Rig s;
    f64 t1 = time_now();
    KV_CHECK(c, s.start(data));
    LibraryQuery q;
    q.id = 2; q.view = LibraryView::Tracks; q.first = 25000; q.count = 100;
    s.e.events.emit(q);
    f64 answered = 0.0;
    while (time_now() - t1 < 30.0 && !answered) { s.step(); if (g_results && g_last.id == 2) answered = time_now() - t1; }
    printf("second start: a window of 100 at row 25000 of %u answered %.3f s after start-up\n", g_last.total, answered);
    KV_CHECK(c, answered > 0.0 && answered < 1.0 && g_last.total == kFiles);
    while (!g_finished && time_now() - t1 < 300.0) s.step();
    printf("second start: the walk again in %.2f s\n", g_walk_seconds);
    s.stop();

    names_shutdown();
    mem_shutdown();
    return test_report(c, "blackwing_library_heavy");
}
