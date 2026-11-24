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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace kv;

namespace {

constexpr u32 kFiles = 50000;
constexpr u32 kPerAlbum = 20;

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
    LibraryRoot root(u32 id) {
        KTable* t = engine_ktable(e, "library.roots");
        Span<LibraryRoot> r = t->rows_of<LibraryRoot>();
        for (u32 i = 0; i < r.len; ++i) if (t->is_alive(i) && r[i].id == id) return r[i];
        return LibraryRoot{};
    }
    const LibraryRow* row(u32 query, u32 index) {
        KTable* t = engine_ktable(e, "library.rows");
        Span<LibraryRow> r = t->rows_of<LibraryRow>();
        for (u32 i = 0; i < r.len; ++i) if (t->is_alive(i) && r[i].query == query && r[i].index == index) return &r[i];
        return nullptr;
    }
};

// What this process holds, and the most it has: Linux's own account, in MB.
void memory(const char* when) {
    // Read as a stream: /proc files say they are 0 bytes long, so a read by size gets nothing.
    char text[8192];
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;
    usize n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    const char* rss = strstr(text, "VmRSS:");
    const char* hwm = strstr(text, "VmHWM:");
    printf("memory %s: %.0f MB held, %.0f MB at most\n", when, rss ? atof(rss + 6) / 1024.0 : 0.0, hwm ? atof(hwm + 6) / 1024.0 : 0.0);
}

// Every file under `root` out of the page cache (opened to read, and only asked to be
// forgotten), so the next read of it comes from the disk. How many.
u32 evict(const char* root) {
    struct W { const char* root; u32 n; } w{root, 0};
    io_list_files(root, [](void* u, const char* rel) {
        W& x = *(W*)u;
        char full[4200];
        format(full, sizeof full, "%s/%s", x.root, rel);
        int fd = open(full, O_RDONLY);
        if (fd >= 0) { posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); close(fd); ++x.n; }
        return true;
    }, &w);
    return w.n;
}

// A real library, read only: walked and read into an index of its own, and what came of it.
int real(const char* music, const char* data) {
    TestCtx c;
    c.begin("library real");
    io_remove_tree(data);
    u32 cold = evict(music); // read only: nothing under it is written
    printf("real library: %u files out of the cache first\n", cold);
    Rig r;
    if (!r.start(data)) return 1;
    f64 t0 = time_now();
    LibraryAddRoot a{};
    format(a.path, sizeof a.path, "%s", music);
    r.e.events.emit(a);
    f64 walked = 0.0;
    while (!g_finished && time_now() - t0 < 1800.0) {
        r.step();
        if (!walked && r.root(1).state != (u8)LibraryRootState::Walking) walked = time_now() - t0;
    }
    f64 all = time_now() - t0;
    LibraryRoot rt = r.root(1);
    const LibraryScan& sc = *library_scan(r.e);
    printf("real library %s: %u tracks in %u directories (%u unreadable); walked in %.1f s (%.0f files a second), read in %.1f s more (%.0f a second), %.1f MB read\n",
           music, rt.tracks, rt.dirs, rt.failed, walked, rt.tracks / (walked > 0 ? walked : 1), all - walked, rt.tracks / (all - walked > 0 ? all - walked : 1),
           sc.bytes_read / 1e6);
    // What could not be read, by reason.
    struct Why { char text[96]; u32 n; };
    Why why[32];
    u32 nwhy = 0, noaudio = 0, estimated = 0;
    for (u32 first = 0;; first += kLibraryMaxWindow) {
        LibraryQuery q; q.id = 9; q.view = LibraryView::Tracks; q.first = first; q.count = kLibraryMaxWindow;
        u32 before = g_results;
        r.e.events.emit(q);
        f64 t = time_now();
        while (time_now() - t < 10.0) { r.step(); if (g_results > before && g_last.id == 9) break; }
        u32 got = 0;
        for (u32 i = first; i < first + kLibraryMaxWindow; ++i) {
            const LibraryRow* x = r.row(9, i);
            if (!x) continue;
            ++got;
            if (x->rate == 0) ++noaudio;
            if (!x->reason[0]) continue;
            u32 k = 0;
            while (k < nwhy && strcmp(why[k].text, x->reason) != 0) ++k;
            if (k == nwhy && nwhy < 32) { format(why[nwhy].text, sizeof why[nwhy].text, "%s", x->reason); why[nwhy++].n = 0; }
            if (k < nwhy) ++why[k].n;
        }
        if (got < kLibraryMaxWindow) break;
    }
    (void)estimated;
    printf("  %u without audio; the reasons given:\n", noaudio);
    for (u32 k = 0; k < nwhy; ++k) printf("  %5u  %s\n", why[k].n, why[k].text);
    LibraryQuery q; q.id = 8; q.view = LibraryView::Albums; q.count = 100;
    u32 before = g_results;
    r.e.events.emit(q);
    while (g_results == before) r.step();
    printf("  %u albums, %u-row window in %.1f ms\n", g_last.total, g_last.count, g_last.seconds * 1e3);
    q.view = LibraryView::Artists;
    before = g_results;
    r.e.events.emit(q);
    while (g_results == before) r.step();
    printf("  %u artists\n", g_last.total);
    memory("after the real library");
    r.stop();
    names_shutdown();
    mem_shutdown();
    return test_report(c, "blackwing_library_real");
}

} // namespace

int main(int argc, char** argv) {
    log_set_level(LogLevel::Warn);
    if (argc > 3 && strcmp(argv[1], "--real") == 0) return real(argv[2], argv[3]);
    TestCtx c;
    c.begin("library heavy");
    char dir[2048];
    bool built_in = argc <= 1; // the criteria's times are the built-in disk's; elsewhere they are measured, not checked
    if (argc > 1) format(dir, sizeof dir, "%s", argv[1]);
    else {
        const char* here = __FILE__; // .../modules/blackwing_library/tests/heavy/main.cc
        const char* m = strstr(here, "/modules/");
        format(dir, sizeof dir, "%.*s/build/library-heavy", m ? (int)(m - here) : 0, here);
    }
    char music[2100], data[2100], marker[2200];
    format(music, sizeof music, "%s/music", dir);
    format(data, sizeof data, "%s/data", dir);
    if (argc > 2) format(data, sizeof data, "%s", argv[2]); // the index elsewhere: music on a USB disk, Blackwing's data on the system's
    format(marker, sizeof marker, "%s/made-%u-%u", dir, kFiles, kPerAlbum);
    if (!io_is_file(marker)) {
        io_remove_tree(music);
        io_make_dirs(music);
        f64 t = time_now();
        kvtest::Made m = kvtest::make_library(music, kFiles, kPerAlbum);
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
        if (k == 0 && built_in) KV_CHECK(c, heard > 0.0 && heard < 3.0); // the criterion: playing a chosen one within three seconds, on the built-in disk
        KV_CHECK(c, found > 0.0 && heard > 0.0);
    }
    // The slot: once the walk is in, a late album of twenty, by its directory.
    while (r.root(1).state == (u8)LibraryRootState::Walking && time_now() - t0 < 300.0) r.step();
    f64 walked_at = time_now() - t0;
    u32 unread_at_slot = r.root(1).unread;
    kvtest::Album al;
    kvtest::album(music, 2400, 1, &al);
    LibraryPrioritize p;
    format(p.path, sizeof p.path, "%s", al.dir);
    f64 ts = time_now();
    r.e.events.emit(p);
    f64 slot_read = 0.0, asked = 0.0;
    while (!slot_read && time_now() - ts < 30.0) {
        r.step();
        if (time_now() - asked > 0.02) {
            LibraryQuery q;
            q.id = 3; q.view = LibraryView::Search; q.count = 100;
            format(q.text, sizeof q.text, "zq480");
            r.e.events.emit(q);
            asked = time_now();
        }
        u32 ready = 0;
        for (u32 i = 0; i < 100; ++i) {
            const LibraryRow* row = r.row(3, i);
            if (row && row->rate && strstr(row->title, "Zq480") && atoi(strstr(row->title, "Zq480") + 2) < 48020) ++ready;
        }
        if (ready == kPerAlbum) slot_read = time_now() - ts;
    }
    printf("slot: the walk in at %.2f s with %u tracks to read; a folder of %u prioritized then had its tags in %.3f s\n", walked_at,
           unread_at_slot, kPerAlbum, slot_read);
    if (built_in) KV_CHECK(c, slot_read > 0.0 && slot_read < 1.0); // within a second on the built-in disk; another's is a number for the notes
    while (!g_finished && time_now() - t0 < 600.0) r.step();
    f64 finished_at = time_now() - t0;
    KV_CHECK(c, g_finished == 1 && g_walk_tracks == kFiles && r.root(1).unread == 0);
    printf("names pass: %u files in %.2f s, %.0f files a second\n", g_walk_tracks, g_walk_seconds, g_walk_tracks / g_walk_seconds);
    printf("tags pass: done %.2f s after the root was added, %.0f files a second after the walk\n", finished_at,
           kFiles / (finished_at - walked_at));
    memory("after the scan of 50,000");
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
    printf("second start: the walk again in %.2f s, %llu files read again\n", g_walk_seconds, (unsigned long long)library_scan(s.e)->tags_read);
    memory("at the second start");
    KV_CHECK(c, library_scan(s.e)->tags_read == 0);

    // Windows of 100 anywhere in every view and order: the index's time, and the round trip
    // from the query to its result in the table.
    {
        auto ask = [&](LibraryView v, LibrarySort so, u64 parent, const char* text, u32 first, f64* rt) -> LibraryResult {
            LibraryQuery w;
            w.id = 7; w.view = v; w.sort = so; w.parent = parent; w.first = first; w.count = 100;
            format(w.text, sizeof w.text, "%s", text);
            u32 before = g_results;
            f64 t = time_now();
            s.e.events.emit(w);
            while (time_now() - t < 10.0) { s.step(); if (g_results > before && g_last.id == 7) break; }
            *rt = time_now() - t;
            return g_last;
        };
        f64 rt;
        LibraryResult ar = ask(LibraryView::Artists, LibrarySort::Default, 0, "", 0, &rt);
        u64 an_artist = s.row(7, 3) ? s.row(7, 3)->id : 0;
        ask(LibraryView::Albums, LibrarySort::Default, 0, "", 1000, &rt);
        u64 an_album = s.row(7, 1000) ? s.row(7, 1000)->id : 0;
        ask(LibraryView::Genres, LibrarySort::Default, 0, "", 0, &rt);
        u64 a_genre = s.row(7, 0) ? s.row(7, 0)->id : 0;
        ask(LibraryView::Folders, LibrarySort::Default, 0, "", 0, &rt);
        u64 top = s.row(7, 0) ? s.row(7, 0)->id : 0;
        (void)ar;
        struct V { const char* name; LibraryView v; LibrarySort so; u64 parent; const char* text; };
        const V views[] = {
            {"tracks", LibraryView::Tracks, LibrarySort::Default, 0, ""},
            {"tracks by title", LibraryView::Tracks, LibrarySort::Title, 0, ""},
            {"tracks by album", LibraryView::Tracks, LibrarySort::Album, 0, ""},
            {"tracks by date", LibraryView::Tracks, LibrarySort::Date, 0, ""},
            {"tracks by path", LibraryView::Tracks, LibrarySort::Path, 0, ""},
            {"a genre's tracks", LibraryView::Tracks, LibrarySort::Default, a_genre, ""},
            {"artists", LibraryView::Artists, LibrarySort::Default, 0, ""},
            {"albums", LibraryView::Albums, LibrarySort::Default, 0, ""},
            {"an artist's albums", LibraryView::Albums, LibrarySort::Default, an_artist, ""},
            {"an album's tracks", LibraryView::AlbumTracks, LibrarySort::Default, an_album, ""},
            {"genres", LibraryView::Genres, LibrarySort::Default, 0, ""},
            {"folders: the root's", LibraryView::Folders, LibrarySort::Default, top, ""},
            {"search \"track\"", LibraryView::Search, LibrarySort::Default, 0, "track"},
            {"search \"track\" by artist", LibraryView::Search, LibrarySort::Artist, 0, "track"},
            {"search \"zq4\"", LibraryView::Search, LibrarySort::Default, 0, "zq4"},
        };
        f64 worst = 0.0, worst_every = 0.0; // the second: a search that matches every track
        printf("windows of 100 (the index's time, then the round trip to the table), at the start, the middle and the end:\n");
        for (const V& v : views) {
            LibraryResult first = ask(v.v, v.so, v.parent, v.text, 0, &rt);
            u32 total = first.total;
            f64 t_first = first.seconds, r_first = rt;
            LibraryResult mid = ask(v.v, v.so, v.parent, v.text, total / 2, &rt);
            f64 r_mid = rt;
            LibraryResult end = ask(v.v, v.so, v.parent, v.text, total > 100 ? total - 100 : 0, &rt);
            printf("  %-26s %6u rows: %5.1f %5.1f   %5.1f %5.1f   %5.1f %5.1f ms\n", v.name, total, t_first * 1e3, r_first * 1e3,
                   mid.seconds * 1e3, r_mid * 1e3, end.seconds * 1e3, rt * 1e3);
            f64 m = t_first > mid.seconds ? t_first : mid.seconds;
            if (end.seconds > m) m = end.seconds;
            bool every = strstr(v.name, "\"track\"") != nullptr;
            f64& w = every ? worst_every : worst;
            if (m > w) w = m;
        }
        printf("windows: the slowest took the index %.1f ms; a search matching every track, %.1f ms for its first window\n",
               worst * 1e3, worst_every * 1e3);
        KV_CHECK(c, worst < 0.020);
        KV_CHECK(c, worst_every < 0.060); // ranking fifty thousand matches; its later windows are a slice of what was kept
    }
    s.stop();

    // The disk run: the files out of the page cache, the index gone, and a long track
    // playing from the same disk through the whole scan. No underrun.
    {
        u32 evicted = 0;
        evicted = evict(music);
        io_remove_tree(data);
        char song[2100];
        const char* here = __FILE__;
        const char* m = strstr(here, "/modules/");
        format(song, sizeof song, "%.*s/modules/hifi_codec/tests/data/mp3/long_44100_2ch.mp3", m ? (int)(m - here) : 0, here);
        int fd = open(song, O_RDONLY);
        if (fd >= 0) { posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); close(fd); }
        Rig d;
        KV_CHECK(c, d.start(data));
        for (u32 i = 0; i < 8; ++i) { PlayerEnqueue pe{}; format(pe.path, sizeof pe.path, "%s", song); d.e.events.emit(pe); }
        d.e.events.emit(PlayerPlay{});
        f64 t2 = time_now();
        while (!g_started && time_now() - t2 < 10.0) d.step();
        LibraryAddRoot a2{};
        format(a2.path, sizeof a2.path, "%s", music);
        d.e.events.emit(a2);
        while (!g_finished && time_now() - t2 < 600.0) d.step();
        f64 scan = time_now() - t2;
        // And on to the end of the queue's first half, past the scan.
        while (time_now() - t2 < scan + 5.0) d.step();
        const PlayerOutput* o = player_output(d.e);
        printf("disk run: %u files evicted from the cache; a cold scan of the disk took %.1f s (walk %.2f s) while playing from it: %u underruns\n",
               evicted, scan, g_walk_seconds, o ? o->underruns : 999);
        KV_CHECK(c, o && o->underruns == 0);
        d.stop();
    }

    names_shutdown();
    mem_shutdown();
    return test_report(c, "blackwing_library_heavy");
}
