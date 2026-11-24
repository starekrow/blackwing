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

// Names what someone is looking at, so it is scanned next: a directory of it not yet listed
// is listed before the rest of the walk, and its tracks' tags are read before the rest's,
// at the I/O queue's normal priority instead of the scan's. There is one slot: a newer
// call takes the place of the last. When the slot's work is done the scan goes back to
// where it was. One of: `path`, a file or a directory by the OS's path (what the player
// plays); `dir`, a directory's id (a Folders row's); `query`, an open window's tracks; or
// up to 32 `tracks` by id.
struct LibraryPrioritize {
    char path[512] = "";
    u64  dir = 0;
    u32  query = 0;
    u32  count = 0;
    u64  tracks[32] = {};
};

// Sets a field of a track (a row's `id`) for the user: `value` wins over every source, in
// every query, from the next generation on. It is kept in Blackwing's own data, never in
// the music, and outlasts the index. An empty value hides the field; `clear` takes the
// override away, so the file's own value is seen again. `field` is a blackwing_tags name:
// "title", "artist", "album", "albumartist", "tracknumber", "date", "genre" ...
struct LibraryOverride { u64 track = 0; char field[32] = ""; char value[256] = ""; u8 clear = 0; };

// What a window looks at. Albums and artists are groupings of tracks, made when asked: an
// album is its album artist and title (or, with no album artist, its title and directory);
// an artist is the album artist, or the artist where there is none.
enum class LibraryView : u8 {
    Folders,     // a directory's directories, then its tracks; `parent` is the directory (0: the roots)
    Tracks,      // every track; with `parent`, a genre's (a Genres row's id)
    Search,      // tracks whose title, artist, album or path has words beginning with `text`'s
    Artists,     // the artists
    Albums,      // the albums; with `parent`, an artist's (an Artists row's id)
    AlbumTracks, // an album's tracks, by disc and track number; `parent` is the album (an Albums row's id)
    Genres,      // the genres
    Count
};
// The order of a window's rows. Default is the view's own: names for Folders, artists and
// albums and genres by name, an album's tracks by disc and number, every track by artist,
// album, disc and number, search best first. The others are for track views. Names sort
// by a key with case folded and marks stripped ("Ångström" beside "angle").
enum class LibrarySort : u8 { Default, Title, Artist, Album, Date, Path, Count };
// Opens window `id`, or replaces what it shows: `count` rows (at most kLibraryMaxWindow)
// from `first`. A newer query with the same id takes the place of an older one: one not
// yet answered is dropped, one being answered is stopped in sqlite.
struct LibraryQuery {
    u32         id     = 0;
    LibraryView view   = LibraryView::Tracks;
    LibrarySort sort   = LibrarySort::Default;
    u8          pad[2] = {};
    u64         parent = 0;
    char        text[128] = "";
    u32         first  = 0;
    u32         count  = 0;
};
// Closes window `id`: its rows leave the table.
struct LibraryClose { u32 id; };

// ---- notifications: events the library emits ------------------------------------------------

// Window `id`'s rows are in library.rows: `count` of them from `first`, of `total` the
// query has, as the index was at `generation`; `seconds` is how long the index took.
struct LibraryResult { u32 id; u32 total; u32 first; u32 count; u64 generation; f64 seconds; };
// Something a query might show has changed: the index's generation is now this. At most a
// few times a second while a scan runs.
struct LibraryChanged { u64 generation; };
// A root's scan is over: every file under it is a row (its walk took `seconds`), and every
// one's tags have been read.
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
    u32  unread;       // tracks whose tags are still to be read
    char path[512];
    char reason[96];   // why it is offline
};

enum class LibraryRowKind : u8 { Directory, Track, Artist, Album, Genre, Count };

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
    u32  count;        // an artist's, album's or genre's tracks
    u32  albums;       // an artist's albums
    u32  pad2;
    char date[16];     // as its tags have it; an album's earliest
    char name[256];    // the file's or the directory's own name, UTF-8 (U+FFFD for what is not); a group's name
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
    u64  cancelled   = 0;  // queries stopped in sqlite by a newer one for their window
    u64  files_found = 0;  // audio files found by the walk since start-up
    u64  tags_read   = 0;  // tracks whose tags have been read since start-up
    u64  bytes_read  = 0;  // what the tags pass has read of the files, since start-up
    u64  unread      = 0;  // tracks whose tags are still to be read, across every root
    f64  names_per_second = 0.0; // files the walk found, a second, over the last second
    f64  tags_per_second  = 0.0; // tracks read, a second, over the last second
    u8   slot        = 0;  // something is in the priority slot
    u8   pad2[7]     = {};
    char dir[256]    = ""; // the directory being listed (within its root), cut to fit
};

constexpr u32 kLibraryMaxWindow = 512; // rows in one window, at most
constexpr u32 kLibraryMaxRows   = 4096; // rows across every open window

// ---- setting up ----------------------------------------------------------------------------------

struct LibraryConfig {
    // Where the index and the list of roots are kept; "" or null: the per-user data
    // directory (platform_pref_path) and "library/" under it.
    const char* data_dir = nullptr;
    f64  quiet_seconds   = 1.0;  // a changed file is looked at once it has been quiet this long
    f64  offline_seconds = 60.0; // an offline root is looked at again this often
    f64  poll_seconds    = 60.0; // a root the OS will not report on (a share) is polled this often
    bool poll_roots      = false; // poll every root, whatever the OS offers (for a share it does not know, and tests)
};
// Before engine_init.
void library_configure(const LibraryConfig& config);

// The resource, for code outside the schedule (a main loop deciding how long to sleep, a
// test). Null if the module is not running.
const LibraryScan* library_scan(Engine& e);

} // namespace kv
