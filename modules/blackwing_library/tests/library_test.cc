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
    // Five tracks of one album whose titles sort by their folded keys, one in another genre.
    {
        const char* titles[] = {"Zebra", "Ångström", "apple", "Été", "angle"};
        format(p, sizeof p, "%s/Sorts", music); io_make_dirs(p);
        for (u32 i = 0; i < 5; ++i) {
            kvtest::Bytes f;
            kvtest::Tags t{titles[i], "Sorter", "Sorting", i + 1, 5, 1, "2001", i == 4 ? "Jazz" : "Electronic"};
            kvtest::flac_file(f, t);
            format(p, sizeof p, "%s/Sorts/%u.flac", music, i);
            io_write_now(p, f.b, f.n);
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
    io_write_now(ix, "this is not a database, whatever its name says", 47);
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
    LibraryPrioritize p;
    format(p.path, sizeof p.path, "%s", dir);
    r.say(p);
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
    KV_CHECK(c, io_stat(track.path, &before));
    IoBytes bytes_before;
    io_read_now(track.path, &bytes_before);
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
    KV_CHECK(c, io_read_now(ovr, &text) && strstr((const char*)text.data, "title = Night Swim Zz") && strstr((const char*)text.data, "album =\n"));
    text.free();
    IoStat after;
    IoBytes bytes_after;
    KV_CHECK(c, io_stat(track.path, &after) && after.mtime_ns == before.mtime_ns && after.size == before.size);
    KV_CHECK(c, io_read_now(track.path, &bytes_after) && bytes_after.size == bytes_before.size && memcmp(bytes_after.data, bytes_before.data, bytes_after.size) == 0);
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
        io_read_now(ovr, &b);
        char* at = strstr((char*)b.data, "Night Swim Zz");
        if (at) memcpy(at, "Night Walk Zz", 13);
        io_write_now(ovr, b.data, b.size);
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
    io_write_now(ovr, "[05 - x.flac]\nthis line is not a field\n", 40);
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
    io_write_now(path, f.b, f.n);
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
