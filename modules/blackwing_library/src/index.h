// The index: sqlite, in Blackwing's own data, in WAL mode. A cache: everything in it can be
// made again from the roots (and, from milestone 6, the overrides). One connection writes,
// on the writer's thread; one reads, on the reader's. Neither is the main thread's.
#pragma once
#include "korvid/blackwing_library/library.h"
#include "korvid/blackwing_tags/tags.h"
#include "korvid/core/array.h"
#include "korvid/core/types.h"

struct sqlite3;
struct sqlite3_stmt;

namespace kv::lib {

constexpr u32 kSchema = 5; // a file with another version is deleted and made again

// Sort keys and group identities (keys.cc).
u32 sort_key(const char* s, char* out, u32 cap);
u64 key_hash(const char* a, const char* b);
struct GroupIn {
    const char* title = ""; const char* artist = ""; const char* album = ""; const char* albumartist = ""; const char* genre = "";
    const char* titlesort = ""; const char* artistsort = ""; const char* albumsort = ""; const char* albumartistsort = "";
    u64 dir = 0;
};
struct GroupOut {
    char sort_title[256], sort_artist[256], sort_album[256], sort_genre[128];
    u64 artist_hash = 0, album_hash = 0, genre_hash = 0;
};
void group_keys(const GroupIn& in, GroupOut* out);

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
    // *fresh says which; *changed whether its size or time differ from the last look;
    // *unread whether its tags want reading (new, changed, or never read at this size and time).
    u64  put_track(const TrackRec& t, u64 dir, bool* fresh, bool* changed, bool* unread);
    // What a read of a track's tags found: every source's values, the merge, the audio.
    struct Tags {
        u64 track = 0, size = 0; i64 mtime = 0;
        u32 rate = 0; u16 channels = 0, bits = 0; u64 frames = 0; bool estimated = false;
        const char* reason = "";
        const char* title = ""; const char* artist = ""; const char* album = ""; const char* albumartist = "";
        const char* date = ""; const char* genre = ""; u32 trackno = 0, discno = 0;
        const char* shown = "";  // the path within its root as UTF-8, searched beside the merge
        const char* titlesort = ""; const char* artistsort = ""; const char* albumsort = ""; const char* albumartistsort = "";
        u32 nvals = 0;
        const u8* fields = nullptr; const u8* sources = nullptr; const char* const* texts = nullptr;
    };
    bool put_tags(const Tags& t);
    bool put_tags_merge_only(const Tags& t); // the track's columns and the search, not its values
    u32  count_tracks(u32 root);
    u32  count_unread(u32 root);
    // For the slot: a directory's root and path; the tracks of a directory, or of a root
    // and path, that want reading (id, path within the root, size, time), by callback.
    bool dir_of(u64 dir, u32* root, char* path, u32 cap);
    u8   dir_state(u64 dir); // 0 not listed yet, 1 listed, 2 could not be
    u64  track_at(u32 root, const char* path);
    void unread_in(u64 dir, void (*fn)(void*, u64 id, u32 root, const char* rel, u64 size, i64 mtime), void* user);
    void unread_track(u64 id, void (*fn)(void*, u64 id, u32 root, const char* rel, u64 size, i64 mtime), void* user);
    // Every track that wants reading, for the start-up pass over what an earlier run left.
    void unread_all(void (*fn)(void*, u64 id, u32 root, const char* rel, u64 size, i64 mtime), void* user);
    u32  count_dirs(u32 root, u32* failed);
    u64  count_all();

    // ---- the reader's ----
    // A window of a view: up to `count` rows from `first` into `out`, with query `id` and
    // their indexes set; the view's whole length in *total. Paths are joined to their
    // root's, which `root_path` says.
    // `generation` is the index's as the reader knows it: what it has kept of a search or a
    // grouping (the whole ordered list, so a window anywhere is a slice) lasts that long.
    u32  window(const LibraryQuery& q, u64 generation, LibraryRow* out, u32* total, const char* (*root_path)(void*, u32), void* user);
    // The reader's: a query is stopped when this says so (checked as sqlite works).
    void stop_when(int (*fn)(void*), void* user);

    // Overrides: when an override file was last taken in (its time; -1 never), and the
    // re-merge of a track with new ones: every other source's values from `tagvals`, the
    // user's in their place, the merge in the track's columns and the search.
    i64  override_seen(const char* path);
    void set_override_seen(const char* path, i64 mtime);
    struct TrackAt { u64 id = 0, dir = 0; u32 root = 0; char rel[4200] = ""; };
    bool track_info(u64 id, TrackAt* out);
    void tracks_in_dir(u32 root, const char* rel_dir, Array<u64>* out);
    bool remerge(u64 track, Span<const TagValue> overrides);

    // The reader's keeping: a search's ordered ids, and a grouping's rows.
    struct Kept {
        u64 generation = ~0ull;
        LibraryView view = LibraryView::Count;
        LibrarySort sort = LibrarySort::Count;
        u64 parent = 0;
        char text[128] = "";
        Array<u64> ids;          // a search's
        Array<LibraryRow> groups; // a grouping's (name, counts, id)
    };
    Kept kept;
    void free_kept() { kept.ids.free(); kept.groups.free(); kept = Kept{}; }

private:
    bool exec(const char* sql);
    bool ok(int rc);  // notes a damaged file
    sqlite3_stmt* prep(const char* sql);
    sqlite3_stmt* st_root = nullptr, *st_unroot_t = nullptr, *st_unroot_d = nullptr, *st_unroot_s = nullptr, *st_unroot_r = nullptr;
    sqlite3_stmt* st_dir_find = nullptr, *st_dir_put = nullptr, *st_dir_state = nullptr;
    sqlite3_stmt* st_track_find = nullptr, *st_track_put = nullptr, *st_track_seen = nullptr, *st_search_put = nullptr;
    sqlite3_stmt* st_count_t = nullptr, *st_count_d = nullptr, *st_count_all = nullptr, *st_count_unread = nullptr;
    sqlite3_stmt* st_vals_del = nullptr, *st_vals_put = nullptr, *st_tags_put = nullptr, *st_search_del = nullptr, *st_dir_of_track = nullptr;
    sqlite3_stmt* st_row = nullptr; // the reader's: a track's row by id
};

// The FTS5 expression for words typed: each word a prefix, all of them wanted.
u32 search_expression(const char* text, char* out, u32 cap);

} // namespace kv::lib
