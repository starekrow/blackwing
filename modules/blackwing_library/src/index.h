// The index: sqlite, in Blackwing's own data, in WAL mode. A cache: everything in it can be
// made again from the roots (and, from milestone 6, the overrides). One connection writes,
// on the writer's thread; one reads, on the reader's. Neither is the main thread's.
#pragma once
#include "korvid/blackwing_library/library.h"
#include "korvid/core/types.h"

struct sqlite3;
struct sqlite3_stmt;

namespace kv::lib {

constexpr u32 kSchema = 1; // a file with another version is deleted and made again

// What the walk found of one directory, and of one file in it, as the writer takes them.
struct DirRec {
    u32 root = 0;
    const char* path = "";   // within the root: "" for the root itself, "a/b" below it
    const char* parent = ""; // its parent's path (unused for the root)
    const char* name = "";   // its own name, as bytes
    u8  state = 0;           // 1 listed, 2 could not be listed
    const char* reason = "";
};
struct TrackRec {
    u32 root = 0;
    const char* dir = "";    // its directory's path within the root
    const char* path = "";   // its own, within the root
    const char* name = "";
    const char* shown = "";  // its path as UTF-8 (U+FFFD for what is not), for search
    u64 size = 0;
    i64 mtime = 0;
    // The guess from its path (blackwing_tags), the lowest rank, until its tags are read.
    const char* title = "";
    const char* artist = "";
    const char* album = "";
    u32 track = 0, disc = 0;
};

struct Index {
    sqlite3* db = nullptr;
    bool     damaged = false;  // sqlite said the file is not a database, or is corrupt
    char     error[256] = "";

    // Opens `path`. The writer makes the schema when the file is new, and says false (with
    // `damaged`) when it is another version or not a database: the caller deletes it and
    // opens again. The reader opens what the writer made, read-only.
    bool open(const char* path, bool writer);
    void close();
    // Deletes the file and its WAL and shared-memory files.
    static void remove_files(const char* path);

    // ---- the writer's ----
    bool begin();
    bool commit();
    bool put_root(u32 id, const char* path);
    bool remove_root(u32 id);
    // The directory's id, made if need be; 0 on failure.
    u64  put_dir(const DirRec& d);
    // A directory's id by its path within its root; 0 when it has none.
    u64  find_dir(u32 root, const char* path);
    // The track's id: a new row, or the old one with its size and time brought up to date.
    // *fresh says which; *changed whether its size or time differ from the last look.
    u64  put_track(const TrackRec& t, u64 dir, bool* fresh, bool* changed);
    u32  count_tracks(u32 root);
    u32  count_dirs(u32 root, u32* failed);
    u64  count_all();

    // ---- the reader's ----
    // A window of a view: up to `count` rows from `first` into `out`, with query `id` and
    // their indexes set; the view's whole length in *total. Paths are joined to their
    // root's, which `root_path` says.
    u32  window(const LibraryQuery& q, LibraryRow* out, u32* total, const char* (*root_path)(void*, u32), void* user);

private:
    bool exec(const char* sql);
    bool ok(int rc);  // notes a damaged file
    sqlite3_stmt* prep(const char* sql);
    sqlite3_stmt* st_root = nullptr, *st_unroot_t = nullptr, *st_unroot_d = nullptr, *st_unroot_s = nullptr, *st_unroot_r = nullptr;
    sqlite3_stmt* st_dir_find = nullptr, *st_dir_put = nullptr, *st_dir_state = nullptr;
    sqlite3_stmt* st_track_find = nullptr, *st_track_put = nullptr, *st_track_seen = nullptr, *st_search_put = nullptr;
    sqlite3_stmt* st_count_t = nullptr, *st_count_d = nullptr, *st_count_all = nullptr;
};

// The FTS5 expression for words typed: each word a prefix, all of them wanted.
u32 search_expression(const char* text, char* out, u32 cap);

} // namespace kv::lib
