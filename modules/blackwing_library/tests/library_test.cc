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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "korvid/core/os_thread.h"
#include "korvid/core/random.h"

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
constinit u32 g_set_aside = 0;          // LibraryOverridesSetAside seen
constinit char g_aside[2048] = "";      // the last one's aside, and its reason
constinit char g_aside_reason[160] = "";
void on_set_aside(TickContext&, Span<const LibraryOverridesSetAside> ev) {
    for (auto& e : ev) { ++g_set_aside; format(g_aside, sizeof g_aside, "%s", e.aside); format(g_aside_reason, sizeof g_aside_reason, "%s", e.reason); }
}
void on_result(TickContext&, Span<const LibraryResult> ev) { for (auto& e : ev) { if (e.id < 16) g_result[e.id] = e; ++g_results; } }
// What a LibraryCollect answered with, by the asker's id: the paths in the order they came.
struct Got { u32 id; u32 index; u32 total; char path[600]; };
constinit Got g_got[512] = {};
constinit u32 g_ngot = 0;
void on_collected(TickContext&, Span<const LibraryCollected> ev) {
    for (auto& e : ev) {
        if (g_ngot == 512) return;
        Got& g = g_got[g_ngot++];
        g.id = e.id; g.index = e.index; g.total = e.total;
        format(g.path, sizeof g.path, "%s", e.path);
    }
}
constinit const HandlerDesc k_handlers[] = {
    {.name = ".collected", KV_ON(LibraryCollected, on_collected), .phase = Phase::PostSim},
    {.name = ".finished", KV_ON(LibraryScanFinished, on_finished), .phase = Phase::PostSim},
    {.name = ".changed",  KV_ON(LibraryChanged, on_changed),       .phase = Phase::PostSim},
    {.name = ".aside",    KV_ON(LibraryOverridesSetAside, on_set_aside), .phase = Phase::PostSim},
    {.name = ".result",   KV_ON(LibraryResult, on_result),         .phase = Phase::PostSim},
};
KV_MODULE_DEFINE(k_listener) = {.name = "listener", .handlers = k_handlers};

// ---- the rig ------------------------------------------------------------------------------------

struct Rig {
    Engine e;
    bool up = false;
    char data[1024] = "";

    bool start(const char* data_dir, f64 quiet = 1.0, f64 offline = 60.0, bool poll = false, f64 poll_seconds = 60.0) {
        g_finished = g_changed = g_results = 0;
        memset(g_result, 0, sizeof g_result);
        format(data, sizeof data, "%s", data_dir);
        LibraryConfig lc;
        lc.data_dir = data;
        lc.quiet_seconds = quiet;
        lc.offline_seconds = offline;
        lc.poll_roots = poll;
        lc.poll_seconds = poll_seconds;
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

void write_file(const char* path, const char* text = "x") { io_write_file(path, text, strlen(text)); }

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
    // Five tracks of one album whose titles sort by their folded keys, one in another genre.
    {
        const char* titles[] = {"Zebra", "Ångström", "apple", "Été", "angle"};
        format(p, sizeof p, "%s/Sorts", music); io_make_dirs(p);
        for (u32 i = 0; i < 5; ++i) {
            kvtest::Bytes f;
            kvtest::Tags t{titles[i], "Sorter", "Sorting", i + 1, 5, 1, "2001", i == 4 ? "Jazz" : "Electronic"};
            kvtest::flac_file(f, t);
            format(p, sizeof p, "%s/Sorts/%u.flac", music, i);
            io_write_file(p, f.b, f.n);
        }
    }

    Rig r;
    KV_CHECK(c, r.start(data));
    f64 t0 = time_now();
    r.add(music);
    KV_CHECK(c, r.until([] { return g_finished == 1; }));
    f64 walked = time_now() - t0;
    LibraryRoot root = r.root(1);
    u32 expect = (u32)made.files + 2 + 5 + (root_user ? 1 : 0); // and SHOUTING.MP3, the Latin-1 name, Sorts
    KV_CHECK(c, root.id == 1 && root.state == (u8)LibraryRootState::Idle && root.tracks == expect && g_finished_tracks == expect);
    KV_CHECK(c, root_user || root.failed == 1);
    KV_CHECK(c, r.scan().tracks == expect && r.scan().active == 0 && g_changed > 0 && root.unread == 0 && r.scan().unread == 0);
    KV_CHECK(c, r.scan().tags_read == expect);
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
    // Files that are not audio, whatever their names: rows, with the reason.
    KV_CHECK(c, in_odd.len == 4 && in_odd[2].rate == 0 && strstr(in_odd[2].reason, "too short to be audio") && in_odd[3].reason[0]);
    in_odd.free();

    // Found by a word of its title: exactly one, playable by its path.
    KV_CHECK(c, r.query(4, LibraryView::Search, 0, "zq37"));
    Array<LibraryRow> hit = r.rows(4);
    KV_CHECK(c, hit.len == 1 && g_result[4].total == 1 && strstr(hit[0].title, "Zq37") && hit[0].track_number > 0);
    // Its tags read: the title is the Vorbis comment's (the file name has its number in front
    // and its extension after), and the decoder said its rate and length.
    char want[64];
    format(want, sizeof want, "%s Track Zq37", kvtest::kWords[(37 * 7) % kvtest::kNWords]);
    KV_CHECK(c, hit.len == 1 && strcmp(hit[0].title, want) == 0 && hit[0].rate == 44100 && hit[0].frames == 4096 && !hit[0].reason[0]);
    KV_CHECK(c, hit.len == 1 && hit[0].artist[0] && hit[0].album[0] && strstr(hit[0].path, hit[0].album));
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
    // The groupings. Artists: the generator's (nine albums each, so one here), Sorter, and the
    // files with no artist at all (the Odd ones, guessed from nothing).
    KV_CHECK(c, r.query(8, LibraryView::Artists, 0, ""));
    Array<LibraryRow> artists = r.rows(8);
    u64 sorter = 0;
    u32 artist_tracks = 0;
    for (const LibraryRow& a : artists) {
        KV_CHECK(c, a.kind == (u8)LibraryRowKind::Artist && a.count > 0);
        artist_tracks += a.count;
        if (!strcmp(a.name, "Sorter")) { sorter = a.id; KV_CHECK(c, a.count == 5 && a.albums == 1); }
    }
    KV_CHECK(c, sorter && artist_tracks == expect && g_result[8].total == artists.len);
    artists.free();
    // Its albums, then the album's tracks by number.
    KV_CHECK(c, r.query(9, LibraryView::Albums, sorter, ""));
    Array<LibraryRow> albums = r.rows(9);
    KV_CHECK(c, albums.len == 1 && strcmp(albums[0].name, "Sorting") == 0 && strcmp(albums[0].artist, "Sorter") == 0 && albums[0].count == 5);
    KV_CHECK(c, albums.len == 1 && strcmp(albums[0].date, "2001") == 0);
    u64 album = albums.len ? albums[0].id : 0;
    albums.free();
    KV_CHECK(c, r.query(10, LibraryView::AlbumTracks, album, ""));
    Array<LibraryRow> at = r.rows(10);
    bool numbered = at.len == 5;
    for (u32 i = 0; i < at.len; ++i) numbered &= at[i].track_number == i + 1;
    KV_CHECK(c, numbered);
    at.free();
    // Every album: the generator's five, Sorting, and the guessed ones of the Odd files.
    KV_CHECK(c, r.query(11, LibraryView::Albums, 0, ""));
    KV_CHECK(c, g_result[11].total >= 6);
    // Genres: Electronic for all but one, Jazz for it; the files with no tags have none.
    KV_CHECK(c, r.query(12, LibraryView::Genres, 0, ""));
    Array<LibraryRow> genres = r.rows(12);
    KV_CHECK(c, genres.len == 2 && strcmp(genres[0].name, "Electronic") == 0 && strcmp(genres[1].name, "Jazz") == 0 && genres[1].count == 1);
    u64 jazz = genres.len == 2 ? genres[1].id : 0;
    genres.free();
    KV_CHECK(c, r.query(13, LibraryView::Tracks, jazz, ""));
    Array<LibraryRow> jz = r.rows(13);
    KV_CHECK(c, jz.len == 1 && strcmp(jz[0].title, "angle") == 0);
    jz.free();
    // Sorted by title, case folded and marks stripped: angle, Ångström, apple, Été, Zebra.
    {
        LibraryQuery q;
        q.id = 14; q.view = LibraryView::AlbumTracks; q.parent = album; q.sort = LibrarySort::Title; q.count = 10;
        u32 before = g_results;
        r.say(q);
        KV_CHECK(c, r.until([&] { return g_results > before && g_result[14].id == 14; }));
        Array<LibraryRow> st = r.rows(14);
        const char* want_order[] = {"angle", "Ångström", "apple", "Été", "Zebra"};
        bool sorted = st.len == 5;
        for (u32 i = 0; i < st.len && sorted; ++i) sorted = strcmp(st[i].title, want_order[i]) == 0;
        KV_CHECK(c, sorted);
        st.free();
    }
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
    KV_CHECK(c, s.scan().tags_read == 0); // every file's size and time unchanged: none is read again
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
    io_write_file(ix, "this is not a database, whatever its name says", 47);
    Rig d;
    KV_CHECK(c, d.start(data));
    KV_CHECK(c, d.until([] { return g_finished == 1; }));
    if (!(d.scan().rebuilt == 1 && d.root(2).tracks == 5 && d.root(1).id == 0))
        KV_ERROR("test", "rebuilt %u, root 2 has %u tracks (state %u), root 1 is %u, %u finished", d.scan().rebuilt, d.root(2).tracks, d.root(2).state, d.root(1).id, g_finished);
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

// The slot: with a scan of eight thousand files under way, a directory named by its path
// (one the walk comes to near its end) has its tags read before most of the rest; then the
// scan goes on. "Before most": the read counter when its row is first seen read, which is
// a commit and a window's answer after the read itself.
void test_library_slot(TestCtx& c) {
    c.begin("library slot");
    char tmp[1024], music[1100], data[1100];
    KV_CHECK(c, io_make_temp_dir("bw_slot", tmp, sizeof tmp));
    format(music, sizeof music, "%s/music", tmp);
    format(data, sizeof data, "%s/data", tmp);
    kvtest::make_library(music, 8000, 10);
    Rig r;
    KV_CHECK(c, r.start(data));
    r.add(music);
    // A late album's directory, by the path the generator gave it (album 290: layout 2).
    kvtest::Album al;
    kvtest::album(music, 290, 1, &al);
    const char* dir = al.dir;
    KV_CHECK(c, io_is_dir(dir));
    KV_CHECK(c, r.scan().slot == 0);
    LibraryPrioritize p;
    format(p.path, sizeof p.path, "%s", dir);
    r.say(p);
    r.step();
    r.step();
    KV_CHECK(c, r.scan().slot == 1); // something is in it
    // Its ten tracks read while most of the library is not.
    u64 dir_id = 0;
    u32 unread_then = 0;
    f64 asked = 0.0;
    bool read = r.until([&] {
        if (!dir_id && time_now() - asked > 0.02) {
            asked = time_now();
            LibraryQuery q; q.id = 9; q.view = LibraryView::Search; q.count = 20;
            format(q.text, sizeof q.text, "zq2900");
            r.say(q);
        }
        Array<LibraryRow> rows = r.rows(9);
        bool all = rows.len == 1 && rows[0].rate == 44100;
        rows.free();
        if (all) { unread_then = 8000 - (u32)r.scan().tags_read; dir_id = 1; }
        return all;
    }, 20.0);
    KV_CHECK(c, read);
    KV_INFO("test", "library slot: the slot's track seen read with %u of 8000 still to read", unread_then);
    KV_CHECK(c, unread_then > 2000); // without the slot it would be among the last few hundred
    KV_CHECK(c, r.until([] { return g_finished == 1; }, 60.0));
    KV_CHECK(c, r.root(1).unread == 0 && r.root(1).tracks == 8000);
    // Read, the slot is empty again (it said "something" for the rest of the run until slice 4's face showed it).
    KV_CHECK(c, r.until([&] { return r.scan().slot == 0; }, 5.0));
    // Asked for with nothing to read in it: taken, found empty, and said empty.
    r.say(p);
    r.step();
    KV_CHECK(c, r.until([&] { return r.scan().slot == 0; }, 5.0));
    r.stop();
    io_remove_tree(tmp);
}

// A query replaced by a newer one for its window: only the newer is shown, whether the
// older was dropped before it began or stopped in sqlite.
void test_library_cancel(TestCtx& c) {
    c.begin("library cancel");
    char tmp[1024], music[1100], data[1100];
    KV_CHECK(c, io_make_temp_dir("bw_cancel", tmp, sizeof tmp));
    format(music, sizeof music, "%s/music", tmp);
    format(data, sizeof data, "%s/data", tmp);
    kvtest::make_library(music, 4000, 10);
    Rig r;
    KV_CHECK(c, r.start(data));
    r.add(music);
    KV_CHECK(c, r.until([] { return g_finished == 1; }, 60.0));
    u32 shown_wrong = 0, rounds = 0;
    for (u32 k = 0; k < 40; ++k) {
        // A wide one (every title has "Track"), then at once a narrow one in its place.
        LibraryQuery wide; wide.id = 1; wide.view = LibraryView::Search; wide.sort = LibrarySort::Artist; wide.count = 512; wide.first = 3000;
        format(wide.text, sizeof wide.text, "track");
        r.say(wide);
        r.step();
        LibraryQuery narrow; narrow.id = 1; narrow.view = LibraryView::Search; narrow.count = 10;
        format(narrow.text, sizeof narrow.text, "zq%u", 1000 + k);
        u32 before = g_results;
        r.say(narrow);
        KV_CHECK(c, r.until([&] { return g_results > before && g_result[1].total == 1; }));
        Array<LibraryRow> rows = r.rows(1);
        char want[16];
        format(want, sizeof want, "Zq%u", 1000 + k);
        if (rows.len != 1 || !strstr(rows[0].title, want)) ++shown_wrong;
        rows.free();
        ++rounds;
    }
    KV_CHECK(c, shown_wrong == 0);
    KV_INFO("test", "library cancel: %u rounds, %llu stopped in sqlite", rounds, (unsigned long long)r.scan().cancelled);
    r.stop();
    io_remove_tree(tmp);
}

// Overrides: written into Blackwing's data (never the music), shown in every query, kept
// through the index's deletion, taken in when edited by hand while stopped, set aside when
// they will not parse, and kept for a root that is removed and added again.
void test_library_overrides(TestCtx& c) {
    c.begin("library overrides");
    char tmp[1024], music[1100], data[1100];
    KV_CHECK(c, io_make_temp_dir("bw_override", tmp, sizeof tmp));
    format(music, sizeof music, "%s/music", tmp);
    format(data, sizeof data, "%s/data", tmp);
    kvtest::make_library(music, 24, 12);
    Rig r;
    KV_CHECK(c, r.start(data));
    r.add(music);
    KV_CHECK(c, r.until([] { return g_finished == 1; }));
    KV_CHECK(c, r.query(1, LibraryView::Search, 0, "zq5"));
    Array<LibraryRow> hit = r.rows(1);
    KV_CHECK(c, hit.len == 1);
    if (hit.len != 1) { r.stop(); return; }
    LibraryRow track = hit[0];
    hit.free();
    IoStat before;
    KV_CHECK(c, io_stat_file(track.path, &before));
    IoBytes bytes_before;
    io_read_file(track.path, &bytes_before);
    auto over = [&](const char* field, const char* value, bool clear = false) {
        LibraryOverride o;
        o.track = track.id;
        format(o.field, sizeof o.field, "%s", field);
        format(o.value, sizeof o.value, "%s", value);
        o.clear = clear;
        u64 gen = r.scan().generation;
        r.say(o);
        r.until([&] { return r.scan().generation > gen; });
    };
    auto title_of = [&](Rig& x, const char* word, char* out) {
        out[0] = 0;
        if (!x.query(1, LibraryView::Search, 0, word)) return 0u;
        Array<LibraryRow> rows = x.rows(1);
        if (rows.len) format(out, 256, "%s", rows[0].title);
        u32 n = rows.len;
        rows.free();
        return n;
    };
    over("title", "Night Swim Zz");
    over("album", "");      // hidden
    char t[256];
    KV_CHECK(c, title_of(r, "swim", t) == 1 && strcmp(t, "Night Swim Zz") == 0);
    KV_CHECK(c, title_of(r, "zq5", t) == 1 && strcmp(t, "Night Swim Zz") == 0); // still found by its path, showing the override
    KV_CHECK(c, r.query(2, LibraryView::Albums, 0, ""));
    u32 album_tracks = 0;
    { Array<LibraryRow> al = r.rows(2); for (const LibraryRow& a : al) album_tracks += a.count; al.free(); }
    KV_CHECK(c, album_tracks == 23); // the one whose album is hidden is in none
    // The file in Blackwing's data, readable, and the music untouched.
    char ovr[2048], dir[2048];
    format(dir, sizeof dir, "%s", track.path + strlen(music) + 1);
    *strrchr(dir, '/') = 0;
    format(ovr, sizeof ovr, "%s/overrides%s/%s/overrides.txt", data, music, dir);
    IoBytes text;
    KV_CHECK(c, io_read_file(ovr, &text) && strstr((const char*)text.data, "title = Night Swim Zz") && strstr((const char*)text.data, "album =\n"));
    text.free();
    IoStat after;
    IoBytes bytes_after;
    KV_CHECK(c, io_stat_file(track.path, &after) && after.mtime_ns == before.mtime_ns && after.size == before.size);
    KV_CHECK(c, io_read_file(track.path, &bytes_after) && bytes_after.size == bytes_before.size && memcmp(bytes_after.data, bytes_before.data, bytes_after.size) == 0);
    bytes_after.free();
    r.stop();

    // The index deleted: made again from the roots and the overrides.
    char ix[1200];
    format(ix, sizeof ix, "%s/index.sqlite", data);
    io_remove(ix);
    Rig s;
    KV_CHECK(c, s.start(data));
    KV_CHECK(c, s.until([] { return g_finished == 1; }));
    KV_CHECK(c, title_of(s, "swim", t) == 1 && strcmp(t, "Night Swim Zz") == 0);
    s.stop();

    // Edited by hand while stopped: taken in at the next start.
    {
        IoBytes b;
        io_read_file(ovr, &b);
        char* at = strstr((char*)b.data, "Night Swim Zz");
        if (at) memcpy(at, "Night Walk Zz", 13);
        io_write_file(ovr, b.data, b.size);
        b.free();
    }
    Rig e;
    KV_CHECK(c, e.start(data));
    KV_CHECK(c, e.until([] { return g_finished == 1; }));
    KV_CHECK(c, title_of(e, "walk", t) == 1 && strcmp(t, "Night Walk Zz") == 0);
    // The root taken out and added again (a new id): its overrides are by its path, and stay.
    e.say(LibraryRemoveRoot{1});
    KV_CHECK(c, e.until([&] { return e.scan().tracks == 0; }));
    e.add(music);
    KV_CHECK(c, e.until([] { return g_finished == 2; }));
    KV_CHECK(c, e.root(2).tracks == 24 && title_of(e, "walk", t) == 1 && strcmp(t, "Night Walk Zz") == 0);
    // Cleared: the file's own title again.
    {
        LibraryOverride o; o.track = track.id; format(o.field, sizeof o.field, "title"); o.clear = 1;
        // the track's id may differ after the rebuild: find it again
        if (e.query(1, LibraryView::Search, 0, "walk")) { Array<LibraryRow> rr = e.rows(1); if (rr.len) o.track = rr[0].id; rr.free(); }
        u64 gen = e.scan().generation;
        e.say(o);
        e.until([&] { return e.scan().generation > gen; });
    }
    KV_CHECK(c, title_of(e, "zq5", t) == 1 && strstr(t, "Zq5"));
    e.stop();

    // A file that will not parse: set aside, its overrides not used.
    io_write_file(ovr, "[05 - x.flac]\nthis line is not a field\n", 40);
    Rig b;
    KV_CHECK(c, b.start(data));
    KV_CHECK(c, b.until([] { return g_finished == 1; }));
    KV_CHECK(c, !io_is_file(ovr));
    bool aside = false;
    {
        char d[2048];
        format(d, sizeof d, "%s/overrides%s/%s", data, music, dir);
        io_list_files(d, [](void* u, const char* rel) { if (strstr(rel, "overrides.txt.broken-")) *(bool*)u = true; return true; }, &aside);
    }
    KV_CHECK(c, aside);
    // And said: an event with where it went and why, and a count in library.scan.
    KV_CHECK(c, b.until([] { return g_set_aside > 0; }));
    KV_CHECK(c, strstr(g_aside, "overrides.txt.broken-") && io_is_file(g_aside));
    KV_CHECK(c, g_aside_reason[0] != 0);
    KV_CHECK(c, b.scan().set_aside >= 1);
    b.stop();
    bytes_before.free();
    io_remove_tree(tmp);
}

namespace {
// Writes a generated track with these tags at `path` (made with its directories).
void put_track_file(const char* path, const char* title, u32 n) {
    char dir[2048];
    format(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    io_make_dirs(dir);
    kvtest::Bytes f;
    kvtest::Tags t{title, "Watcher", "Changes", n, 9, 1, "2020", "Ambient"};
    kvtest::flac_file(f, t);
    io_write_file(path, f.b, f.n);
}
}

// Changes under a root, heard by the watcher: a file added, changed, renamed, moved (with
// its override), removed and put back, a directory removed; changes made while stopped;
// a root emptied (offline, its rows kept and marked) and filled again (back, nothing read
// again); a root that is polled.
void test_library_changes(TestCtx& c) {
    c.begin("library changes");
    char tmp[1024], music[1100], data[1100], p[2048], q[2048];
    KV_CHECK(c, io_make_temp_dir("bw_changes", tmp, sizeof tmp));
    format(music, sizeof music, "%s/music", tmp);
    format(data, sizeof data, "%s/data", tmp);
    kvtest::make_library(music, 24, 12);
    Rig r;
    KV_CHECK(c, r.start(data, 0.3, 0.5));
    r.add(music);
    KV_CHECK(c, r.until([] { return g_finished == 1; }));
    auto find = [&](Rig& x, const char* word, LibraryRow* out) -> u32 {
        if (!x.query(1, LibraryView::Search, 0, word)) return 0;
        Array<LibraryRow> rows = x.rows(1);
        u32 n = rows.len;
        if (n && out) *out = rows[0];
        rows.free();
        return n;
    };
    // Waits for a search to have `want` rows; how long it took, or -1.
    auto until_found = [&](Rig& x, const char* word, u32 want, LibraryRow* out = nullptr) -> f64 {
        f64 t0 = time_now();
        while (time_now() - t0 < 6.0) {
            for (u32 i = 0; i < 20; ++i) x.step();
            if (find(x, word, out) == want) return time_now() - t0;
        }
        return -1.0;
    };
    // Added.
    format(p, sizeof p, "%s/New Album/01 - Fresh.flac", music);
    put_track_file(p, "Fresh Arrival Qx1", 1);
    f64 added = until_found(r, "qx1", 1);
    KV_CHECK(c, added >= 0.0 && added < 2.0);
    // Changed: its title rewritten.
    put_track_file(p, "Fresher Arrival Qx2", 1);
    f64 changed = until_found(r, "qx2", 1);
    KV_CHECK(c, changed >= 0.0 && changed < 2.0 && find(r, "qx1", nullptr) == 0);
    // An override on it, then it is moved (rename keeps its size and time): the same track,
    // with its override, at its new path.
    LibraryRow before{};
    KV_CHECK(c, find(r, "qx2", &before) == 1);
    LibraryOverride o; o.track = before.id; format(o.field, sizeof o.field, "genre"); format(o.value, sizeof o.value, "Moved Genre");
    u64 gen = r.scan().generation;
    r.say(o);
    r.until([&] { return r.scan().generation > gen; });
    format(q, sizeof q, "%s/Elsewhere/01 - Fresh.flac", music);
    io_make_dirs(q); io_remove(q); // the directory, then the place for the file
    KV_CHECK(c, rename(p, q) == 0);
    LibraryRow after{};
    f64 moved = -1.0;
    {
        f64 t0 = time_now();
        while (time_now() - t0 < 6.0) {
            for (u32 i = 0; i < 20; ++i) r.step();
            if (find(r, "qx2", &after) == 1 && strstr(after.path, "Elsewhere")) { moved = time_now() - t0; break; }
        }
    }
    KV_CHECK(c, moved >= 0.0 && moved < 2.0 && after.id == before.id);
    KV_CHECK(c, r.query(2, LibraryView::Genres, 0, ""));
    { Array<LibraryRow> g = r.rows(2); bool kept = false; for (const LibraryRow& x : g) kept |= strcmp(x.name, "Moved Genre") == 0; KV_CHECK(c, kept); g.free(); }
    // Removed: gone from every view; put back: the same track.
    io_remove(q);
    f64 removed = until_found(r, "qx2", 0);
    KV_CHECK(c, removed >= 0.0 && removed < 2.0);
    put_track_file(q, "Fresher Arrival Qx2", 1);
    LibraryRow back{};
    KV_CHECK(c, until_found(r, "qx2", 1, &back) >= 0.0 && back.id == before.id);
    // A directory removed: its tracks leave the views, and so does it.
    format(p, sizeof p, "%s/Gone Dir", music);
    for (u32 i = 0; i < 3; ++i) { format(q, sizeof q, "%s/Gone Dir/%02u.flac", music, i); put_track_file(q, "Short Lived Qx3", i + 1); }
    KV_CHECK(c, until_found(r, "qx3", 3) >= 0.0);
    io_remove_tree(p);
    f64 dir_gone = until_found(r, "qx3", 0);
    KV_CHECK(c, dir_gone >= 0.0 && dir_gone < 2.0);
    KV_INFO("test", "library changes: added %.2f s, changed %.2f s, moved %.2f s, removed %.2f s, a directory removed %.2f s",
            added, changed, moved, removed, dir_gone);
    u32 tracks_now = r.root(1).tracks;
    r.stop();

    // While stopped: one removed, one added. The start-up walk finds both.
    format(p, sizeof p, "%s/Elsewhere/01 - Fresh.flac", music);
    io_remove(p);
    format(p, sizeof p, "%s/Later/01 - Late.flac", music);
    put_track_file(p, "Late Comer Qx4", 1);
    Rig s;
    KV_CHECK(c, s.start(data, 0.3, 0.5));
    KV_CHECK(c, s.until([] { return g_finished == 1; }));
    KV_CHECK(c, find(s, "qx4", nullptr) == 1 && find(s, "qx2", nullptr) == 0 && s.root(1).tracks == tracks_now);
    // The root emptied, as an unplugged disk leaves it: offline, its rows kept and marked.
    char away[1100];
    format(away, sizeof away, "%s/away", tmp);
    KV_CHECK(c, rename(music, away) == 0 && io_make_dirs(music));
    bool offline = s.until([&] { return s.root(1).state == (u8)LibraryRootState::Offline; }, 6.0);
    KV_CHECK(c, offline && strstr(s.root(1).reason, "empty"));
    LibraryRow marked{};
    KV_CHECK(c, find(s, "qx4", &marked) == 1 && marked.offline == 1);
    u64 read_before = s.scan().tags_read;
    // Back: walked again, nothing read again, nothing missing.
    io_remove(music);
    KV_CHECK(c, rename(away, music) == 0);
    bool back_on = s.until([&] { return s.root(1).state == (u8)LibraryRootState::Idle; }, 10.0);
    KV_CHECK(c, back_on && s.root(1).tracks == tracks_now && s.scan().tags_read == read_before);
    KV_CHECK(c, find(s, "qx4", &marked) == 1 && marked.offline == 0);
    s.stop();

    // A root that is polled (as a share is): an added file is found by the next walk of it.
    Rig w;
    KV_CHECK(c, w.start(data, 0.3, 0.5, true, 0.5));
    KV_CHECK(c, w.until([] { return g_finished == 1; }));
    format(p, sizeof p, "%s/Polled/01 - Polled.flac", music);
    put_track_file(p, "Polled In Qx5", 1);
    f64 polled = until_found(w, "qx5", 1);
    KV_INFO("test", "library changes: a polled root's new file found in %.2f s (polled every 0.5 s)", polled);
    KV_CHECK(c, polled >= 0.0 && polled < 3.0);
    w.stop();
    io_remove_tree(tmp);
}

namespace {
bool audio_like(const char* rel) {
    const char* dot = strrchr(rel, '.');
    return dot && (strcmp(dot, ".flac") == 0 || strcmp(dot, ".mp3") == 0);
}
// Grows a file a piece at a time while the scan runs: "a file that changes while it is read".
struct Grower { char path[2048]; u32 stop = 0; };
void grow(void* arg) {
    Grower& g = *(Grower*)arg;
    kvtest::Bytes f;
    kvtest::Tags t{"Growing", "Writer", "Being Written", 1, 1, 1, "2024", "Noise"};
    kvtest::flac_file(f, t);
    for (u32 i = 0; i < 60 && !atomic_load(&g.stop, MO_RELAXED); ++i) {
        int fd = open(g.path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { ssize_t w = write(fd, f.b, f.n); (void)w; close(fd); }
        time_sleep(0.02);
    }
}
void copy_file(const char* from, const char* to) {
    IoBytes b;
    if (io_read_file(from, &b)) { io_write_file(to, b.data, b.size); b.free(); }
}
// An ID3v2.4 tag of `frames` text frames (all the same field) in front of `audio`.
void tagged(const char* path, const char* id, u32 frames, const char* text, const IoBytes& audio) {
    Array<u8> t;
    for (u32 i = 0; i < frames; ++i) {
        u32 n = (u32)strlen(text) + 1;
        u8 h[10] = {(u8)id[0], (u8)id[1], (u8)id[2], (u8)id[3], (u8)((n >> 21) & 0x7F), (u8)((n >> 14) & 0x7F), (u8)((n >> 7) & 0x7F), (u8)(n & 0x7F), 0, 0};
        memcpy(t.push_n(10), h, 10);
        t.push(3);
        memcpy(t.push_n(n - 1), text, n - 1);
    }
    u32 n = t.len;
    u8 head[10] = {'I', 'D', '3', 4, 0, 0, (u8)((n >> 21) & 0x7F), (u8)((n >> 14) & 0x7F), (u8)((n >> 7) & 0x7F), (u8)(n & 0x7F)};
    Array<u8> all;
    memcpy(all.push_n(10), head, 10);
    memcpy(all.push_n(t.len), t.data, t.len);
    memcpy(all.push_n((u32)audio.size), audio.data, audio.size);
    io_write_file(path, all.data, all.len);
    t.free(); all.free();
}
}

// The corpus, whole, scanned in one go: the codec's good, broken and spoiled files; hostile
// tags, a hundred megabytes of art among them; and the odd tree. Every audio file ends as a
// row, with its tags or a reason; nothing hangs the scan, crashes it, or makes it read a
// file whole.
void test_library_corpus(TestCtx& c) {
    c.begin("library corpus");
    char tmp[1024], root[1100], data[1100], p[4200], q[4200];
    KV_CHECK(c, io_make_temp_dir("bw_corpus", tmp, sizeof tmp));
    format(root, sizeof root, "%s/corpus", tmp);
    format(data, sizeof data, "%s/data", tmp);
    u32 audio = 0;
    // The codec's files, good and broken, as they are.
    char codec[2048];
    {
        const char* here = __FILE__;
        const char* m = strstr(here, "/modules/");
        format(codec, sizeof codec, "%.*s/modules/hifi_codec/tests/data", m ? (int)(m - here) : 0, here);
    }
    struct Cp { const char* from; const char* to; u32* n; };
    Cp cp{codec, root, &audio};
    io_list_files(codec, [](void* u, const char* rel) {
        Cp& x = *(Cp*)u;
        if (!audio_like(rel)) return true;
        char from[4200], to[4200], dir[4200];
        format(from, sizeof from, "%s/%s", x.from, rel);
        format(to, sizeof to, "%s/codec/%s", x.to, rel);
        format(dir, sizeof dir, "%s", to); *strrchr(dir, '/') = 0;
        io_make_dirs(dir);
        copy_file(from, to);
        ++*x.n;
        return true;
    }, &cp);
    // Spoiled copies by the hundred.
    format(p, sizeof p, "%s/spoiled", root); io_make_dirs(p);
    Rng rng = Rng::seeded(20261123, 31);
    const char* bases[] = {"tone_16_2ch_44100.flac", "mp3/vbr_44100_2ch.mp3"};
    for (u32 k = 0; k < 2; ++k) {
        format(p, sizeof p, "%s/%s", codec, bases[k]);
        IoBytes b;
        if (!io_read_file(p, &b)) continue;
        for (u32 i = 0; i < 150; ++i) {
            usize len = i % 3 == 2 ? rng.next() % (b.size + 1) : b.size;
            Array<u8> w;
            memcpy(w.push_n((u32)len), b.data, len);
            if (i % 3 != 2) for (u32 f = 0, flips = 1 + rng.next() % 10; f < flips && len; ++f) w[rng.next() % (u32)len] ^= (u8)(1u << (rng.next() % 8));
            format(q, sizeof q, "%s/spoiled/%u_%s", root, i, k ? "x.mp3" : "x.flac");
            io_write_file(q, w.data, w.len);
            w.free();
            ++audio;
        }
        b.free();
    }
    // Hostile tags.
    {
        format(p, sizeof p, "%s/mp3/cbr128_44100_2ch.mp3", codec);
        IoBytes mp3;
        io_read_file(p, &mp3);
        format(p, sizeof p, "%s/tags", root); io_make_dirs(p);
        format(q, sizeof q, "%s/tags/thousand artists.mp3", root); tagged(q, "TPE1", 1000, "Again and again", mp3); ++audio;
        format(q, sizeof q, "%s/tags/odd text.mp3", root); tagged(q, "TIT2", 3, "\xff\xfe broken \xc3", mp3); ++audio;
        // A hundred megabytes of art: a tag that says so, the file sparse where the art is,
        // and the audio after it.
        format(q, sizeof q, "%s/tags/huge art.mp3", root);
        {
            const u32 art = 100u << 20;
            u8 head[64] = {'I', 'D', '3', 4, 0, 0};
            u32 body = 10 + 11 + 10 + art; // TIT2, APIC header, art
            head[6] = (u8)((body >> 21) & 0x7F); head[7] = (u8)((body >> 14) & 0x7F); head[8] = (u8)((body >> 7) & 0x7F); head[9] = (u8)(body & 0x7F);
            u8 f1[21] = {'T', 'I', 'T', '2', 0, 0, 0, 11, 0, 0, 3, 'H', 'u', 'g', 'e', ' ', 'A', 'r', 't', '!', '!'};
            u32 an = art;
            u8 f2[10] = {'A', 'P', 'I', 'C', (u8)((an >> 21) & 0x7F), (u8)((an >> 14) & 0x7F), (u8)((an >> 7) & 0x7F), (u8)(an & 0x7F), 0, 0};
            int fd = open(q, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                ssize_t w = write(fd, head, 10); w = write(fd, f1, 21); w = write(fd, f2, 10);
                u8 apic[16] = {0, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 0, 0, 0};
                w = write(fd, apic, 16);
                if (ftruncate(fd, (off_t)(10 + body)) != 0) {}
                if (lseek(fd, 0, SEEK_END) >= 0) w = write(fd, mp3.data, mp3.size);
                (void)w;
                close(fd);
            }
            ++audio;
        }
        mp3.free();
    }
    // The odd tree: a path past a thousand bytes, ten thousand files in one directory, a
    // loop, a directory that may not be read, a name that is not UTF-8, a file being written.
    {
        char longp[4200];
        u32 n = (u32)format(longp, sizeof longp, "%s/tree", root);
        for (u32 d = 0; d < 5; ++d) {
            longp[n++] = '/';
            for (u32 k = 0; k < 220; ++k) longp[n++] = (char)('a' + (k + d) % 26);
            longp[n] = 0;
        }
        KV_CHECK(c, io_make_dirs(longp)); // past a thousand bytes, in one call (KR15)
        format(q, sizeof q, "%s/deep.flac", longp);
        kvtest::Bytes f;
        kvtest::Tags t{"Deep Down", "Long Path", "Nesting", 1, 1, 1, "2000", "Ambient"};
        kvtest::flac_file(f, t);
        io_write_file(q, f.b, f.n);
        ++audio;
        format(p, sizeof p, "%s/tree/ten thousand", root); io_make_dirs(p);
        for (u32 i = 0; i < 10000; ++i) {
            format(q, sizeof q, "%s/tree/ten thousand/%05u.flac", root, i);
            io_write_file(q, f.b, f.n);
            ++audio;
        }
        format(p, sizeof p, "%s/tree/loop", root); KV_CHECK(c, symlink(root, p) == 0);
        format(p, sizeof p, "%s/tree/locked", root); io_make_dirs(p);
        format(q, sizeof q, "%s/tree/locked/hidden away.flac", root); io_write_file(q, f.b, f.n);
        chmod(p, 0);
        if (geteuid() == 0) ++audio;
        format(q, sizeof q, "%s/tree/caf\xe9.flac", root); io_write_file(q, f.b, f.n); ++audio;
    }
    Grower g;
    format(g.path, sizeof g.path, "%s/tree/growing.flac", root);
    io_write_file(g.path, "", 0);
    ++audio;
    OsThread gt;
    os_thread_start(&gt, grow, &g);

    Rig r;
    KV_CHECK(c, r.start(data, 0.3, 60.0));
    f64 t0 = time_now();
    r.add(root);
    bool done = r.until([] { return g_finished == 1; }, 180.0);
    f64 took = time_now() - t0;
    atomic_store(&g.stop, 1u, MO_RELAXED);
    os_thread_join(&gt);
    KV_CHECK(c, done);
    // The growing file's last changes in, and the index still, before its rows are counted a
    // window at a time (a row that moves in the order between windows would count twice).
    {
        u64 gen = r.scan().generation;
        f64 still = time_now();
        while (time_now() - still < 1.0) { r.step(); if (r.scan().generation != gen || r.scan().active) { gen = r.scan().generation; still = time_now(); } }
    }
    // Every audio file a row; every row its tags, or a reason.
    LibraryRoot rt = r.root(1);
    u32 rows = 0, no_audio = 0, unexplained = 0, too_long = 0, past_511 = 0;
    Array<u64> ids;
    for (u32 first = 0;; first += kLibraryMaxWindow) {
        if (!r.query(3, LibraryView::Tracks, 0, "", first, kLibraryMaxWindow)) break;
        Array<LibraryRow> w = r.rows(3);
        for (const LibraryRow& x : w) {
            for (u64 id : ids) if (id == x.id) KV_ERROR("test", "twice: %llu at %u: %s (%s)", (unsigned long long)x.id, x.index, x.name, x.title);
            if (strstr(x.path, "/loop/")) KV_ERROR("test", "a row through the loop: %s", x.path);
            ids.push(x.id);
            ++rows;
            if (x.rate == 0) { ++no_audio; if (!x.reason[0]) ++unexplained; }
            if (!x.path[0] && strstr(x.reason, "longer than the player takes")) ++too_long;
            if (strlen(x.path) > 511) { ++past_511; IoStat st; if (!io_stat_file(x.path, &st) || !st.is_file()) KV_ERROR("test", "a long path that is not a file: %s", x.path); }
        }
        u32 n = w.len;
        w.free();
        if (n < kLibraryMaxWindow) break;
    }
    ids.free();
    u64 read = r.scan().bytes_read;
    if (rows != audio) KV_ERROR("test", "root says %u tracks", rt.tracks);
    KV_INFO("test", "library corpus: %u audio files, %u rows, %u without audio (each with its reason), %u unexplained, %u too long to play, %u past 511 bytes; "
            "%.1f MB read, the scan %.1f s", audio, rows, no_audio, unexplained, too_long, past_511, read / 1e6, took);
    // A path of any length the OS allows is a row with its path (slice 4: it was refused past 511 bytes).
    KV_CHECK(c, rt.tracks == audio && rows == audio && unexplained == 0 && too_long == 0 && past_511 == 1);
    KV_CHECK(c, read < (40u << 20)); // a hundred megabytes of art among it, not read
    KV_CHECK(c, r.query(4, LibraryView::Search, 0, "huge"));
    { Array<LibraryRow> h = r.rows(4); KV_CHECK(c, h.len == 1 && strstr(h[0].title, "Huge Art") && h[0].rate == 44100); h.free(); }
    r.stop();
    format(p, sizeof p, "%s/tree/locked", root); chmod(p, 0755);
    io_remove_tree(tmp);
}

// What slice 4's face needs of the library: a window over paths (a queue's rows, by what
// the library knows of them), and the paths of every track of a row of any kind, in its
// own order, from one command.
void test_library_paths(TestCtx& c) {
    c.begin("library paths");
    char tmp[1024], music[1100], data[1100], p[2600];
    KV_CHECK(c, io_make_temp_dir("bw_paths", tmp, sizeof tmp));
    format(music, sizeof music, "%s/music", tmp);
    format(data, sizeof data, "%s/data", tmp);
    kvtest::make_library(music, 60, 12); // five albums of twelve
    Rig r;
    KV_CHECK(c, r.start(data));
    r.add(music);
    KV_CHECK(c, r.until([&] { return g_finished == 1 && r.scan().unread == 0 && !r.scan().active; }, 30.0));
    KV_CHECK(c, r.query(1, LibraryView::Tracks, 0, "", 0, 10));
    Array<LibraryRow> tracks = r.rows(1);
    KV_CHECK(c, tracks.len == 10 && tracks[0].path[0] && tracks[2].title[0]);

    // A window over paths: a row for each, at the index given, the library's or not.
    auto path = [&](u32 id, u32 index, const char* at) { LibraryPath lp; lp.id = id; lp.index = index; format(lp.path, sizeof lp.path, "%s", at); r.say(lp); };
    u32 before = g_results;
    path(7, 40, tracks[2].path);
    path(7, 41, tracks[0].path);
    path(7, 42, "/nowhere/at all/x.flac");
    format(p, sizeof p, "%s/not there.flac", music);
    path(7, 43, p);
    KV_CHECK(c, r.until([&] { return g_results > before && g_result[7].id == 7; }));
    Array<LibraryRow> rows = r.rows(7);
    KV_CHECK(c, rows.len == 4 && g_result[7].count == 4 && g_result[7].total == 4);
    if (rows.len == 4) {
        KV_CHECK(c, rows[0].index == 40 && rows[0].id == tracks[2].id && strcmp(rows[0].title, tracks[2].title) == 0 && strcmp(rows[0].path, tracks[2].path) == 0);
        KV_CHECK(c, rows[1].index == 41 && rows[1].id == tracks[0].id && strcmp(rows[1].artist, tracks[0].artist) == 0 && rows[1].rate == 44100);
        KV_CHECK(c, rows[2].index == 42 && rows[2].id == 0 && strcmp(rows[2].name, "x.flac") == 0 && strstr(rows[2].reason, "not under a root") && strcmp(rows[2].path, "/nowhere/at all/x.flac") == 0);
        KV_CHECK(c, rows[3].index == 43 && rows[3].id == 0 && strcmp(rows[3].name, "not there.flac") == 0 && strstr(rows[3].reason, "no track at this path") && rows[3].root == 1);
    }
    rows.free();
    // An index given another path; one taken out with an empty one.
    before = g_results;
    path(7, 40, tracks[5].path);
    path(7, 42, "");
    KV_CHECK(c, r.until([&] { return g_results > before && g_result[7].count == 3; }));
    rows = r.rows(7);
    KV_CHECK(c, rows.len == 3 && rows[0].index == 40 && rows[0].id == tracks[5].id && rows[1].index == 41 && rows[2].index == 43);
    rows.free();
    // An override shows in it, as in any window, when the index moves on.
    LibraryOverride o;
    o.track = tracks[5].id;
    format(o.field, sizeof o.field, "title");
    format(o.value, sizeof o.value, "A Title By Hand");
    r.say(o);
    KV_CHECK(c, r.until([&] { Array<LibraryRow> w = r.rows(7); bool ok = w.len == 3 && strcmp(w[0].title, "A Title By Hand") == 0; w.free(); return ok; }));
    // Closed: its rows go. A query cannot open one.
    r.say(LibraryClose{7});
    KV_CHECK(c, r.until([&] { Array<LibraryRow> w = r.rows(7); bool none = w.len == 0; w.free(); return none; }));

    // The tracks of a row of any kind, from one command, in the order its own view has.
    auto collect = [&](u32 id, LibraryRowKind kind, u64 row, const char* text = "") {
        g_ngot = 0;
        LibraryCollect lc;
        lc.id = id; lc.kind = (u8)kind; lc.row = row;
        format(lc.text, sizeof lc.text, "%s", text);
        r.say(lc);
        return r.until([&] { return g_ngot && (g_got[g_ngot - 1].total == 0 || g_got[g_ngot - 1].index + 1 == g_got[g_ngot - 1].total); });
    };
    auto in_order = [&](u32 id) {
        for (u32 i = 0; i < g_ngot; ++i) if (g_got[i].id != id || g_got[i].index != i || g_got[i].total != g_ngot) return false;
        return g_ngot > 0;
    };
    KV_CHECK(c, r.query(2, LibraryView::Albums, 0, ""));
    Array<LibraryRow> albums = r.rows(2);
    KV_CHECK(c, albums.len == 5 && albums[1].kind == (u8)LibraryRowKind::Album && albums[1].count == 12);
    // An album: its tracks by number.
    KV_CHECK(c, r.query(3, LibraryView::AlbumTracks, albums[1].id, ""));
    Array<LibraryRow> of_album = r.rows(3);
    KV_CHECK(c, of_album.len == 12 && collect(21, LibraryRowKind::Album, albums[1].id) && g_ngot == 12 && in_order(21));
    bool same = g_ngot == 12;
    for (u32 i = 0; i < 12 && same; ++i) same = strcmp(g_got[i].path, of_album[i].path) == 0;
    KV_CHECK(c, same);
    // An artist: every album's, an album at a time.
    KV_CHECK(c, r.query(4, LibraryView::Artists, 0, ""));
    Array<LibraryRow> artists = r.rows(4);
    // (The generator's fourth layout is a compilation, which the library files under an artist of its own.)
    KV_CHECK(c, artists.len == 2 && artists[0].count == 48 && artists[0].albums == 4 && artists[1].count == 12);
    KV_CHECK(c, collect(22, LibraryRowKind::Artist, artists[0].id) && g_ngot == 48 && in_order(22));
    bool by_album = g_ngot == 48;
    for (u32 i = 0; i + 1 < g_ngot && by_album; ++i) {
        // Twelve in a row share a directory, and the next twelve another.
        const char* a = strrchr(g_got[i].path, '/');
        const char* b = strrchr(g_got[i + 1].path, '/');
        bool same_dir = a && b && (a - g_got[i].path) == (b - g_got[i + 1].path) && strncmp(g_got[i].path, g_got[i + 1].path, (usize)(a - g_got[i].path)) == 0;
        by_album = same_dir == ((i + 1) % 12 != 0);
    }
    KV_CHECK(c, by_album);
    // A folder, with everything under it: the root's.
    KV_CHECK(c, r.query(5, LibraryView::Folders, 0, ""));
    Array<LibraryRow> top = r.rows(5);
    KV_CHECK(c, top.len == 1 && top[0].kind == (u8)LibraryRowKind::Directory);
    KV_CHECK(c, collect(23, LibraryRowKind::Directory, top[0].id) && g_ngot == 60 && in_order(23));
    u32 real = 0;
    for (u32 i = 0; i < g_ngot; ++i) { IoStat st; real += io_stat_file(g_got[i].path, &st) && st.is_file(); }
    KV_CHECK(c, real == 60);
    // One track; a search; a genre; and nothing, which says so once.
    KV_CHECK(c, collect(24, LibraryRowKind::Track, tracks[3].id) && g_ngot == 1 && strcmp(g_got[0].path, tracks[3].path) == 0 && g_got[0].total == 1);
    KV_CHECK(c, collect(25, LibraryRowKind::Track, 0, "zq37") && g_ngot == 1 && strstr(g_got[0].path, "Zq37"));
    KV_CHECK(c, r.query(6, LibraryView::Genres, 0, ""));
    Array<LibraryRow> genres = r.rows(6);
    KV_CHECK(c, genres.len == 1 && genres[0].count == 60 && collect(26, LibraryRowKind::Genre, genres[0].id) && g_ngot == 60 && in_order(26));
    KV_CHECK(c, collect(27, LibraryRowKind::Album, 123456789) && g_ngot == 1 && g_got[0].total == 0 && g_got[0].path[0] == 0 && g_got[0].id == 27);
    genres.free(); top.free(); artists.free(); of_album.free(); albums.free(); tracks.free();
    r.stop();
    io_remove_tree(tmp);
}

