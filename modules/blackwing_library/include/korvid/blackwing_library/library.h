// Korvid blackwing_library: the library's whole API, which is data, as the player's is.
// Events it consumes, events it emits, two tables and a resource. Nothing here is a
// function a face calls.
//
//   ctx.emit(LibraryAddRoot{"/music"});                       // aim it at a directory
//   ctx.emit(LibraryQuery{.id = 1, .view = LibraryView::Search, .text = "blue", .count = 50});
//   ... a tick or two later, LibraryResult{1, ...}: the window's rows are in library.rows
//   for (const LibraryRow& r : ctx.read_ktable("library.rows")->rows_of<LibraryRow>())
//       if (r.query == 1) ... r.title, r.path (what PlayerEnqueue takes)
//
// The work is done off the main thread: a walk of each root, a directory at a time; an
// index in sqlite, written by one thread and read by another. What comes back arrives in
// a later tick. modules/blackwing_library/README.md has the model.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

// ---- commands: events the library consumes ------------------------------------------------

// Adds a directory to the library, and walks it. The path is the OS's, absolute or relative
// to the working directory; one already a root is not added twice.
struct LibraryAddRoot    { char path[512]; };
// Takes a root out of the library: its rows leave the index. The user's overrides for it
// stay where they are.
struct LibraryRemoveRoot { u32 root; };

// What a window looks at.
enum class LibraryView : u8 {
    Folders, // a directory's directories, then its tracks; `parent` is the directory (0: the roots)
    Tracks,  // every track
    Search,  // tracks whose title, artist, album or path has words beginning with `text`'s
    Count
};
// Opens window `id`, or replaces what it shows: `count` rows (at most kLibraryMaxWindow)
// from `first`. A newer query with the same id takes the place of an older one not yet
// answered.
struct LibraryQuery {
    u32         id     = 0;
    LibraryView view   = LibraryView::Tracks;
    u8          pad[3] = {};
    u64         parent = 0;
    char        text[128] = "";
    u32         first  = 0;
    u32         count  = 0;
};
// Closes window `id`: its rows leave the table.
struct LibraryClose { u32 id; };

// ---- notifications: events the library emits ------------------------------------------------

// Window `id`'s rows are in library.rows: `count` of them from `first`, of `total` the
// query has, as the index was at `generation`.
struct LibraryResult { u32 id; u32 total; u32 first; u32 count; u64 generation; };
// Something a query might show has changed: the index's generation is now this. At most a
// few times a second while a scan runs.
struct LibraryChanged { u64 generation; };
// A root's walk is over: every file under it is a row.
struct LibraryScanFinished { u32 root; u32 tracks; f64 seconds; };

// ---- state: what anyone may read ---------------------------------------------------------------

enum class LibraryRootState : u8 {
    Walking, // its names are being listed
    Reading, // listed; its files' tags are being read
    Idle,    // everything under it is in the index
    Offline, // its directory is gone, cannot be listed, or is empty where it held tracks
    Count
};
// "walking", "reading", "idle", "offline".
const char* library_root_state_name(LibraryRootState s);

// A row of the ktable "library.roots": the component LibraryRoot, one per root.
struct LibraryRoot {
    u32  id;           // from 1; it stays for the root's life, across runs
    u8   state;        // a LibraryRootState
    u8   pad[3];
    u32  dirs;         // directories listed
    u32  tracks;       // tracks in the index under it
    u32  failed;       // directories that could not be listed
    char path[512];
    char reason[96];   // why it is offline
};

enum class LibraryRowKind : u8 { Directory, Track, Count };

// A row of the ktable "library.rows": the component LibraryRow, one per row of an open
// window. `index` is its place in the query's whole list, from 0.
struct LibraryRow {
    u32  query;
    u32  index;
    u64  id;           // the track's or the directory's, in the index
    u8   kind;         // a LibraryRowKind
    u8   missing;      // the file has gone (its row is kept a while)
    u8   offline;      // its root is offline: playing it fails
    u8   pad;
    u32  root;
    u32  track_number; // 0 when nothing says
    u32  disc_number;
    u64  size;
    u64  frames;       // 0 until its tags have been read, or when the file does not say
    u32  rate;
    u32  pad2;
    char name[256];    // the file's or the directory's own name, UTF-8 (U+FFFD for what is not)
    char title[256];
    char artist[256];
    char album[256];
    char path[512];    // what PlayerEnqueue takes: the root's path and the file's within it; "" when longer than that holds
    char reason[96];   // why its tags could not be read, or why it cannot be played
};

// The resource "library.scan": what the library is doing.
struct LibraryScan {
    u8   active      = 0;  // a walk or a read is under way
    u8   rebuilt     = 0;  // the index was deleted and made again at start-up (a version that did not match, or a damaged file)
    u8   pad[2]      = {};
    u32  walking     = 0;  // the root being walked; 0 when none is
    u64  generation  = 0;  // the index's, as of the last commit
    u64  tracks      = 0;  // in the index, across every root
    u64  dirs_listed = 0;  // directories listed since start-up
    u64  files_found = 0;  // audio files found by the walk since start-up
    f64  names_per_second = 0.0; // files the walk found, a second, over the last second
    char dir[256]    = ""; // the directory being listed (within its root), cut to fit
};

constexpr u32 kLibraryMaxWindow = 512; // rows in one window, at most
constexpr u32 kLibraryMaxRows   = 4096; // rows across every open window

// ---- setting up ----------------------------------------------------------------------------------

struct LibraryConfig {
    // Where the index and the list of roots are kept; "" or null: the per-user data
    // directory (platform_pref_path) and "library/" under it.
    const char* data_dir = nullptr;
};
// Before engine_init.
void library_configure(const LibraryConfig& config);

// The resource, for code outside the schedule (a main loop deciding how long to sleep, a
// test). Null if the module is not running.
const LibraryScan* library_scan(Engine& e);

} // namespace kv
