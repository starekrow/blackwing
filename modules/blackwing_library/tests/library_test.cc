// The library in a real engine, driven only by events and read only through its tables
// and resource: roots walked into the index, windows answered, the index found again at
// the next start and made again when it is wrong.
//
// Each test starts an engine with platform, cli, the library and a small module that writes
// down what the library emits, over a tree made in a temporary directory. A step is: pump
// the platform, run one tick, and nap, since the work is on the library's own threads.
#include "korvid/blackwing_library/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "gen.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace kv;

namespace {

// ---- the listener ------------------------------------------------------------------------------

constinit u32 g_finished = 0;           // LibraryScanFinished seen
constinit u32 g_finished_tracks = 0;
constinit u32 g_changed = 0;
constinit LibraryResult g_result[16] = {};
constinit u32 g_results = 0;            // LibraryResult seen
void on_finished(TickContext&, Span<const LibraryScanFinished> ev) { for (auto& e : ev) { ++g_finished; g_finished_tracks = e.tracks; } }
void on_changed(TickContext&, Span<const LibraryChanged> ev) { g_changed += ev.len; }
void on_result(TickContext&, Span<const LibraryResult> ev) { for (auto& e : ev) { if (e.id < 16) g_result[e.id] = e; ++g_results; } }
constinit const HandlerDesc k_handlers[] = {
    {.name = ".finished", KV_ON(LibraryScanFinished, on_finished), .phase = Phase::PostSim},
    {.name = ".changed",  KV_ON(LibraryChanged, on_changed),       .phase = Phase::PostSim},
    {.name = ".result",   KV_ON(LibraryResult, on_result),         .phase = Phase::PostSim},
};
KV_MODULE_DEFINE(k_listener) = {.name = "listener", .handlers = k_handlers};

// ---- the rig ------------------------------------------------------------------------------------

struct Rig {
    Engine e;
    bool up = false;
    char data[1024] = "";

    bool start(const char* data_dir) {
        g_finished = g_changed = g_results = 0;
        memset(g_result, 0, sizeof g_result);
        format(data, sizeof data, "%s", data_dir);
        LibraryConfig lc;
        lc.data_dir = data;
        library_configure(lc);
        CliConfig cli; cli.no_stdin = true;
        cli_configure(cli);
        PlatformMounts mounts; mounts.none = true;
        platform_configure_mounts(mounts);
        static const ModuleDesc* const list[] = {&platform_module, &cli_module, &blackwing_library_module, &k_listener};
        up = engine_init(e, list);
        return up;
    }
    void stop() { if (up) engine_shutdown(e); up = false; }
    void step() { platform_pump(e); engine_frame(e, e.clock.tick_dt); time_sleep(0.001); }
    template <class E> void say(const E& ev) { e.events.emit(ev); }
    const LibraryScan& scan() { return *library_scan(e); }

    template <class F> bool until(F done, f64 seconds = 10.0) {
        f64 end = time_now() + seconds;
        for (;;) { step(); if (done()) return true; if (time_now() > end) return false; }
    }
    void add(const char* path) { LibraryAddRoot a{}; format(a.path, sizeof a.path, "%s", path); say(a); }
    // Asks window `id` and waits for its answer.
    bool query(u32 id, LibraryView view, u64 parent, const char* text, u32 first = 0, u32 count = 100) {
        LibraryQuery q;
        q.id = id; q.view = view; q.parent = parent; q.first = first; q.count = count;
        format(q.text, sizeof q.text, "%s", text);
        u32 before = g_results;
        say(q);
        return until([&] { return g_results > before && g_result[id].id == id; });
    }
    Array<LibraryRow> rows(u32 query) {
        Array<LibraryRow> out;
        KTable* t = engine_ktable(e, "library.rows");
        Span<LibraryRow> r = t->rows_of<LibraryRow>();
        for (u32 i = 0; i < r.len; ++i) if (t->is_alive(i) && r[i].query == query) out.push(r[i]);
        for (u32 i = 1; i < out.len; ++i) for (u32 j = i; j > 0 && out[j - 1].index > out[j].index; --j) { LibraryRow x = out[j]; out[j] = out[j - 1]; out[j - 1] = x; }
        return out;
    }
    LibraryRoot root(u32 id) {
        KTable* t = engine_ktable(e, "library.roots");
        Span<LibraryRoot> r = t->rows_of<LibraryRoot>();
        for (u32 i = 0; i < r.len; ++i) if (t->is_alive(i) && r[i].id == id) return r[i];
        return LibraryRoot{};
    }
};

void write_file(const char* path, const char* text = "x") { io_write_now(path, text, strlen(text)); }

} // namespace

// A small tree, walked: every audio file a row, by folder, with a guess from its path; the
// directories that cannot be listed marked; links not followed; found by name.
void test_library_walk(TestCtx& c) {
    c.begin("library walk");
    char tmp[1024], music[1100], data[1100], p[2048];
    KV_CHECK(c, io_make_temp_dir("bw_library", tmp, sizeof tmp));
    format(music, sizeof music, "%s/music", tmp);
    format(data, sizeof data, "%s/data", tmp);
    kvtest::Made made = kvtest::make_library(music, 60, 12);
    // Odd things beside the generated albums.
    format(p, sizeof p, "%s/Odd/Not audio", music); io_make_dirs(p);
    format(p, sizeof p, "%s/Odd/readme.txt", music); write_file(p);
    format(p, sizeof p, "%s/Odd/SHOUTING.MP3", music); write_file(p);
    format(p, sizeof p, "%s/Odd/.hidden.flac", music); write_file(p);
    format(p, sizeof p, "%s/Odd/caf\xe9 latin1.flac", music); write_file(p); // a name that is not UTF-8
    format(p, sizeof p, "%s/Odd/loop", music); KV_CHECK(c, symlink(music, p) == 0);  // a link back up: not followed
    format(p, sizeof p, "%s/Odd/Locked", music); io_make_dirs(p);
    format(p, sizeof p, "%s/Odd/Locked/inside.flac", music); write_file(p);
    chmod(p, 0644);
    format(p, sizeof p, "%s/Odd/Locked", music); chmod(p, 0);
    bool root_user = geteuid() == 0;

    Rig r;
    KV_CHECK(c, r.start(data));
    f64 t0 = time_now();
    r.add(music);
    KV_CHECK(c, r.until([] { return g_finished == 1; }));
    f64 walked = time_now() - t0;
    LibraryRoot root = r.root(1);
    u32 expect = (u32)made.files + 2 + (root_user ? 1 : 0); // and SHOUTING.MP3, the Latin-1 name
    KV_CHECK(c, root.id == 1 && root.state == (u8)LibraryRootState::Idle && root.tracks == expect && g_finished_tracks == expect);
    KV_CHECK(c, root_user || root.failed == 1);
    KV_CHECK(c, r.scan().tracks == expect && r.scan().active == 0 && g_changed > 0);
    KV_INFO("test", "library walk: %u tracks in %.0f ms, %u dirs", root.tracks, walked * 1e3, root.dirs);

    // The roots, then the root's own directories, then one album's tracks with their guesses.
    KV_CHECK(c, r.query(1, LibraryView::Folders, 0, ""));
    Array<LibraryRow> top = r.rows(1);
    KV_CHECK(c, top.len == 1 && top[0].kind == (u8)LibraryRowKind::Directory && strcmp(top[0].name, music) == 0);
    u64 root_dir = top.len ? top[0].id : 0;
    top.free();
    KV_CHECK(c, r.query(2, LibraryView::Folders, root_dir, ""));
    Array<LibraryRow> dirs = r.rows(2);
    KV_CHECK(c, dirs.len == g_result[2].count && dirs.len > 2);
    u64 odd = 0;
    for (const LibraryRow& d : dirs) if (!strcmp(d.name, "Odd")) odd = d.id;
    dirs.free();
    KV_CHECK(c, odd != 0 && r.query(3, LibraryView::Folders, odd, ""));
    Array<LibraryRow> in_odd = r.rows(3);
    // Locked and Not audio (directories, first), then the two files, by name; no loop, no text, no hidden file.
    KV_CHECK(c, in_odd.len == 4 && in_odd[0].kind == (u8)LibraryRowKind::Directory && in_odd[1].kind == (u8)LibraryRowKind::Directory);
    KV_CHECK(c, in_odd.len == 4 && strcmp(in_odd[2].name, "SHOUTING.MP3") == 0 && strcmp(in_odd[3].name, "caf\xef\xbf\xbd latin1.flac") == 0);
    KV_CHECK(c, in_odd.len == 4 && in_odd[3].path[strlen(in_odd[3].path) - 13] == '\xe9'); // the path the player opens is the bytes
    in_odd.free();

    // Found by a word of its title: exactly one, playable by its path.
    KV_CHECK(c, r.query(4, LibraryView::Search, 0, "zq37"));
    Array<LibraryRow> hit = r.rows(4);
    KV_CHECK(c, hit.len == 1 && g_result[4].total == 1 && strstr(hit[0].title, "Zq37") && hit[0].track_number > 0);
    KV_CHECK(c, hit.len == 1 && io_is_file(hit[0].path));
    hit.free();
    // By a word that is not ASCII, with its marks left off: "Moskva" is not, but "москва" is "Москва".
    KV_CHECK(c, r.query(5, LibraryView::Search, 0, "москва"));
    KV_CHECK(c, g_result[5].total > 0);
    KV_CHECK(c, r.query(6, LibraryView::Search, 0, "cafe"));
    KV_CHECK(c, g_result[6].total > 0);
    // Every track, in windows.
    KV_CHECK(c, r.query(7, LibraryView::Tracks, 0, "", 10, 25));
    Array<LibraryRow> w = r.rows(7);
    KV_CHECK(c, g_result[7].total == expect && w.len == 25 && w[0].index == 10 && w[24].index == 34);
    w.free();
    // A window closed: its rows go.
    r.say(LibraryClose{7});
    r.step(); r.step();
    KV_CHECK(c, r.rows(7).len == 0);
    r.stop();

    // The next start: the roots are there, walked again, every track keeps its id.
    Rig s;
    KV_CHECK(c, s.start(data));
    KV_CHECK(c, s.root(1).id == 1 && s.until([] { return g_finished == 1; }));
    KV_CHECK(c, s.root(1).tracks == expect && !s.scan().rebuilt);
    KV_CHECK(c, s.query(4, LibraryView::Search, 0, "zq37"));
    Array<LibraryRow> again = s.rows(4);
    KV_CHECK(c, again.len == 1);
    again.free();
    // A second root, then the first taken out.
    char more[1100];
    format(more, sizeof more, "%s/more", tmp);
    kvtest::make_library(more, 5, 5, 9);
    s.add(more);
    s.add(music); // already a root: not added twice
    KV_CHECK(c, s.until([] { return g_finished == 2; }));
    KV_CHECK(c, s.root(2).tracks == 5 && s.scan().tracks == expect + 5 && s.root(3).id == 0);
    s.say(LibraryRemoveRoot{1});
    KV_CHECK(c, s.until([&] { return s.scan().tracks == 5; }));
    KV_CHECK(c, s.root(1).id == 0);
    s.stop();

    // The index damaged: made again, with a message, from the roots.
    char ix[1200];
    format(ix, sizeof ix, "%s/index.sqlite", data);
    io_write_now(ix, "this is not a database, whatever its name says", 47);
    Rig d;
    KV_CHECK(c, d.start(data));
    KV_CHECK(c, d.until([] { return g_finished == 1; }));
    KV_CHECK(c, d.scan().rebuilt == 1 && d.root(2).tracks == 5 && d.root(1).id == 0);
    d.stop();

    // A root that is not there: offline, with the reason.
    Rig o;
    char gone[1100];
    format(gone, sizeof gone, "%s/gone", tmp);
    KV_CHECK(c, o.start(data));
    o.add(gone);
    KV_CHECK(c, o.until([] { return g_finished == 2; }));
    LibraryRoot g = o.root(3);
    KV_CHECK(c, g.state == (u8)LibraryRootState::Offline && strstr(g.reason, "not there"));
    o.stop();

    format(p, sizeof p, "%s/Odd/Locked", music); chmod(p, 0755);
    io_remove_tree(tmp);
}
