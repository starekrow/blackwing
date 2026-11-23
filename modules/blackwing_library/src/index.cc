// The index over sqlite: the schema, the writer's statements, and the reader's windows.
#include "index.h"
#include "overrides.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "vendor/sqlite3.h"
#include <initializer_list>
#include <string.h>

namespace kv::lib {

namespace {

constexpr const char* kSchemaSql =
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value) WITHOUT ROWID;"
    "CREATE TABLE roots(id INTEGER PRIMARY KEY, path BLOB NOT NULL, volume INTEGER NOT NULL DEFAULT 0, walks INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE dirs(id INTEGER PRIMARY KEY, root INTEGER NOT NULL, parent INTEGER NOT NULL, path BLOB NOT NULL,"
    "  name TEXT NOT NULL, state INTEGER NOT NULL DEFAULT 0, reason TEXT);"
    "CREATE UNIQUE INDEX dirs_path ON dirs(root, path);"
    "CREATE INDEX dirs_parent ON dirs(parent, name);"
    "CREATE TABLE tracks(id INTEGER PRIMARY KEY, root INTEGER NOT NULL, dir INTEGER NOT NULL, path BLOB NOT NULL,"
    "  name TEXT NOT NULL, size INTEGER NOT NULL, mtime INTEGER NOT NULL, missing INTEGER NOT NULL DEFAULT 0,"
    "  read_size INTEGER, read_mtime INTEGER, reason TEXT,"
    "  rate INTEGER, channels INTEGER, bits INTEGER, frames INTEGER, estimated INTEGER,"
    "  title TEXT, artist TEXT, album TEXT, albumartist TEXT, trackno INTEGER, discno INTEGER, date TEXT, genre TEXT,"
    "  sort_title TEXT NOT NULL DEFAULT '', sort_artist TEXT NOT NULL DEFAULT '', sort_album TEXT NOT NULL DEFAULT '',"
    "  sort_genre TEXT NOT NULL DEFAULT '', artist_hash INTEGER NOT NULL DEFAULT 0, album_hash INTEGER NOT NULL DEFAULT 0,"
    "  genre_hash INTEGER NOT NULL DEFAULT 0, shown_artist TEXT NOT NULL DEFAULT '',"
    "  seen INTEGER NOT NULL DEFAULT 0, born INTEGER NOT NULL DEFAULT 0, missing_since INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX tracks_moved ON tracks(missing, root, name, size, mtime);"
    "CREATE INDEX tracks_seen ON tracks(root, missing, seen);"
    "CREATE UNIQUE INDEX tracks_path ON tracks(root, path);"
    // An index per order a window takes, matching its ORDER BY to the last column, so the
    // ids of a window anywhere in fifty thousand rows are a walk of the index, not a sort;
    // the rows are then fetched by id.
    "CREATE INDEX tracks_dir ON tracks(dir, missing, name);"
    "CREATE INDEX tracks_by_artist ON tracks(missing, sort_artist, sort_album, album_hash, discno, trackno, sort_title);"
    "CREATE INDEX tracks_by_title ON tracks(missing, sort_title, sort_artist);"
    "CREATE INDEX tracks_by_album ON tracks(missing, sort_album, album_hash, discno, trackno);"
    "CREATE INDEX tracks_by_date ON tracks(missing, date, sort_artist, sort_album);"
    "CREATE INDEX tracks_by_path ON tracks(missing, root, path);"
    "CREATE INDEX tracks_album ON tracks(album_hash, missing, discno, trackno, sort_title);"
    "CREATE INDEX tracks_genre ON tracks(genre_hash, missing, sort_artist, sort_album, album_hash, discno, trackno, sort_title);"
    // And one covering each grouping, so it reads no table rows.
    "CREATE INDEX groups_artist ON tracks(missing, sort_artist, album_hash, artist_hash, shown_artist);"
    "CREATE INDEX groups_album ON tracks(missing, sort_album, album_hash, artist_hash, album, shown_artist, date);"
    "CREATE INDEX groups_genre ON tracks(missing, sort_genre, genre_hash, genre);"
    "CREATE TABLE overrides(path BLOB PRIMARY KEY, mtime INTEGER NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE tagvals(track INTEGER NOT NULL, source INTEGER NOT NULL, field INTEGER NOT NULL, value TEXT NOT NULL);"
    "CREATE INDEX tagvals_track ON tagvals(track);"
    "CREATE VIRTUAL TABLE search USING fts5(title, artist, album, path, tokenize = 'unicode61 remove_diacritics 2', prefix = '2 3');";

// The columns a row is filled from, in this order.
constexpr const char* kTrackCols = "t.id, t.root, t.name, t.title, t.artist, t.album, t.trackno, t.discno, t.size, t.frames, t.rate, t.path, t.missing, t.reason, t.date";

void text_into(char* out, u32 cap, const unsigned char* s) { format(out, cap, "%s", s ? (const char*)s : ""); }

} // namespace

bool Index::ok(int rc) {
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) return true;
    if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) damaged = true;
    format(error, sizeof error, "%s", db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return false;
}

bool Index::exec(const char* sql) {
    char* msg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
    if (msg) { format(error, sizeof error, "%s", msg); sqlite3_free(msg); }
    if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) damaged = true;
    return rc == SQLITE_OK;
}

sqlite3_stmt* Index::prep(const char* sql) {
    sqlite3_stmt* s = nullptr;
    if (!ok(sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, nullptr))) return nullptr;
    return s;
}

void Index::remove_files(const char* path) {
    char p[2048];
    io_remove(path);
    format(p, sizeof p, "%s-wal", path); io_remove(p);
    format(p, sizeof p, "%s-shm", path); io_remove(p);
}

bool Index::open(const char* path, bool writer) {
    damaged = false;
    error[0] = 0;
    int flags = writer ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
    if (!ok(sqlite3_open_v2(path, &db, flags | SQLITE_OPEN_NOMUTEX, nullptr))) { close(); return false; }
    sqlite3_busy_timeout(db, 2000);
    if (writer) {
        // A new file has no tables; one of another version, or not a database, is refused.
        if (!exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;")) { if (!error[0]) format(error, sizeof error, "the index will not open"); damaged = true; close(); return false; }
        sqlite3_stmt* s = nullptr;
        int rc = sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = 'schema'", -1, &s, nullptr);
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(s);
            int version = rc == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
            sqlite3_finalize(s);
            if (version != (int)kSchema) {
                format(error, sizeof error, "the index is version %d, and this build's is %u", version, kSchema);
                damaged = true; close(); return false;
            }
        } else {
            // No meta table: a new file, if it has no tables at all.
            sqlite3_stmt* t = nullptr;
            int n = -1;
            if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master", -1, &t, nullptr) == SQLITE_OK && sqlite3_step(t) == SQLITE_ROW) n = sqlite3_column_int(t, 0);
            sqlite3_finalize(t);
            if (n != 0) { format(error, sizeof error, "the index is not one this build made"); damaged = true; close(); return false; }
            char sql[128];
            format(sql, sizeof sql, "INSERT INTO meta VALUES('schema', %u);", kSchema);
            if (!exec("BEGIN;") || !exec(kSchemaSql) || !exec(sql) || !exec("COMMIT;")) { damaged = true; close(); return false; }
        }
        st_root      = prep("INSERT INTO roots(id, path) VALUES(?, ?) ON CONFLICT(id) DO UPDATE SET path = excluded.path");
        st_unroot_s  = prep("DELETE FROM search WHERE rowid IN (SELECT id FROM tracks WHERE root = ?)");
        st_unroot_t  = prep("DELETE FROM tracks WHERE root = ?");
        st_unroot_d  = prep("DELETE FROM dirs WHERE root = ?");
        st_unroot_r  = prep("DELETE FROM roots WHERE id = ?");
        st_dir_find  = prep("SELECT id FROM dirs WHERE root = ? AND path = ?");
        st_dir_put   = prep("INSERT INTO dirs(root, parent, path, name, state, reason) VALUES(?, ?, ?, ?, ?, ?)");
        st_dir_state = prep("UPDATE dirs SET state = ?, reason = ? WHERE id = ?");
        st_track_find = prep("SELECT id, size, mtime, read_size, read_mtime, missing FROM tracks WHERE root = ? AND path = ?");
        st_track_put = prep("INSERT INTO tracks(root, dir, path, name, size, mtime, title, artist, album, trackno, discno,"
                            " sort_title, sort_artist, sort_album, artist_hash, album_hash, shown_artist, seen, born)"
                            " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        st_track_seen = prep("UPDATE tracks SET size = ?, mtime = ?, dir = ?, missing = 0, seen = ? WHERE id = ?");
        st_search_put = prep("INSERT INTO search(rowid, title, artist, album, path) VALUES(?, ?, ?, ?, ?)");
        st_count_t   = prep("SELECT count(*) FROM tracks WHERE root = ? AND missing = 0");
        st_count_d   = prep("SELECT count(*), sum(state = 2) FROM dirs WHERE root = ?");
        st_count_all = prep("SELECT count(*) FROM tracks WHERE missing = 0");
        st_count_unread = prep("SELECT count(*) FROM tracks WHERE root = ? AND missing = 0 AND (read_size IS NOT size OR read_mtime IS NOT mtime)");
        st_vals_del  = prep("DELETE FROM tagvals WHERE track = ?");
        st_vals_put  = prep("INSERT INTO tagvals(track, source, field, value) VALUES(?, ?, ?, ?)");
        st_tags_put  = prep("UPDATE tracks SET read_size = ?, read_mtime = ?, reason = ?, rate = ?, channels = ?, bits = ?, frames = ?, estimated = ?,"
                            " title = ?, artist = ?, album = ?, albumartist = ?, date = ?, genre = ?, trackno = ?, discno = ?,"
                            " sort_title = ?, sort_artist = ?, sort_album = ?, sort_genre = ?, artist_hash = ?, album_hash = ?, genre_hash = ?,"
                            " shown_artist = ? WHERE id = ?");
        st_dir_of_track = prep("SELECT dir FROM tracks WHERE id = ?");
        st_search_del = prep("DELETE FROM search WHERE rowid = ?");
        if (!st_count_all) { damaged = true; close(); return false; }
    }
    return true;
}

void Index::close() {
    sqlite3_stmt** all[] = {&st_root, &st_unroot_t, &st_unroot_d, &st_unroot_s, &st_unroot_r, &st_dir_find, &st_dir_put, &st_dir_state,
                            &st_track_find, &st_track_put, &st_track_seen, &st_search_put, &st_count_t, &st_count_d, &st_count_all,
                            &st_count_unread, &st_vals_del, &st_vals_put, &st_tags_put, &st_search_del, &st_dir_of_track, &st_row};
    for (sqlite3_stmt** s : all) { if (*s) sqlite3_finalize(*s); *s = nullptr; }
    if (db) sqlite3_close_v2(db);
    db = nullptr;
    free_kept();
}

bool Index::begin()  { return exec("BEGIN;"); }
bool Index::commit() { return exec("COMMIT;"); }

bool Index::put_root(u32 id, const char* path) {
    sqlite3_reset(st_root);
    sqlite3_bind_int64(st_root, 1, id);
    sqlite3_bind_blob(st_root, 2, path, (int)strlen(path), SQLITE_TRANSIENT);
    return ok(sqlite3_step(st_root));
}

bool Index::remove_root(u32 id) {
    bool good = true;
    for (sqlite3_stmt* s : {st_unroot_s, st_unroot_t, st_unroot_d, st_unroot_r}) {
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, id);
        good = ok(sqlite3_step(s)) && good;
    }
    return good;
}

// Every prepared statement is reset once its answer is read: one left on a row holds a
// read open, which pins the connection's view of the index and stops the WAL's checkpoints.
template <class F> auto once(sqlite3_stmt* st, F read) { auto v = read(); sqlite3_reset(st); return v; }

u64 Index::find_dir(u32 root, const char* p) {
    sqlite3_reset(st_dir_find);
    sqlite3_bind_int64(st_dir_find, 1, root);
    sqlite3_bind_blob(st_dir_find, 2, p, (int)strlen(p), SQLITE_TRANSIENT);
    return once(st_dir_find, [&] { return sqlite3_step(st_dir_find) == SQLITE_ROW ? (u64)sqlite3_column_int64(st_dir_find, 0) : (u64)0; });
}

u64 Index::put_dir(const DirRec& d) {
    auto find = [&](const char* p) -> u64 { return find_dir(d.root, p); };
    u64 id = find(d.path);
    if (id) {
        sqlite3_reset(st_dir_state);
        sqlite3_bind_int(st_dir_state, 1, d.state);
        sqlite3_bind_text(st_dir_state, 2, d.reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st_dir_state, 3, (i64)id);
        ok(sqlite3_step(st_dir_state));
        return id;
    }
    u64 parent = d.path[0] ? find(d.parent) : 0;
    sqlite3_reset(st_dir_put);
    sqlite3_bind_int64(st_dir_put, 1, d.root);
    sqlite3_bind_int64(st_dir_put, 2, (i64)parent);
    sqlite3_bind_blob(st_dir_put, 3, d.path, (int)strlen(d.path), SQLITE_TRANSIENT);
    sqlite3_bind_text(st_dir_put, 4, d.name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st_dir_put, 5, d.state);
    sqlite3_bind_text(st_dir_put, 6, d.reason, -1, SQLITE_TRANSIENT);
    if (!ok(sqlite3_step(st_dir_put))) return 0;
    return (u64)sqlite3_last_insert_rowid(db);
}

namespace {
// The search's row for a track, from its columns and its path.
void search_again(Index& ix, sqlite3* db, sqlite3_stmt* put, u64 id) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT title, artist, album, path FROM tracks WHERE id = ?", -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (i64)id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        char rel[4200], shown[4200];
        format(rel, sizeof rel, "%.*s", sqlite3_column_bytes(st, 3), (const char*)sqlite3_column_blob(st, 3));
        shown_utf8(rel, shown, sizeof shown);
        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM search WHERE rowid = ?", -1, &d, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(d, 1, (i64)id);
            sqlite3_step(d);
            sqlite3_finalize(d);
        }
        sqlite3_reset(put);
        sqlite3_bind_int64(put, 1, (i64)id);
        for (int k = 0; k < 3; ++k) sqlite3_bind_text(put, 2 + k, (const char*)sqlite3_column_text(st, k), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(put, 5, shown, -1, SQLITE_TRANSIENT);
        sqlite3_step(put);
    }
    sqlite3_finalize(st);
    (void)ix;
}
void unsearch(sqlite3* db, u64 id) {
    sqlite3_stmt* d = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM search WHERE rowid = ?", -1, &d, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(d, 1, (i64)id);
        sqlite3_step(d);
        sqlite3_finalize(d);
    }
}
void exec_id(sqlite3* db, const char* sql, i64 a, i64 b = 0, bool two = false) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, a);
    if (two) sqlite3_bind_int64(st, 2, b);
    sqlite3_step(st);
    sqlite3_finalize(st);
}
} // namespace

u64 Index::put_track(const TrackRec& t, u64 dir, u32 walk, bool* fresh, bool* changed, bool* unread, char* moved_from, u32 cap) {
    *fresh = *changed = *unread = false;
    if (moved_from && cap) moved_from[0] = 0;
    sqlite3_reset(st_track_find);
    sqlite3_bind_int64(st_track_find, 1, t.root);
    sqlite3_bind_blob(st_track_find, 2, t.path, (int)strlen(t.path), SQLITE_TRANSIENT);
    int found_rc = sqlite3_step(st_track_find);
    if (found_rc != SQLITE_ROW) sqlite3_reset(st_track_find);
    if (found_rc == SQLITE_ROW) {
        u64 id = (u64)sqlite3_column_int64(st_track_find, 0);
        *changed = (u64)sqlite3_column_int64(st_track_find, 1) != t.size || sqlite3_column_int64(st_track_find, 2) != t.mtime;
        *unread = *changed || sqlite3_column_type(st_track_find, 3) == SQLITE_NULL ||
                  (u64)sqlite3_column_int64(st_track_find, 3) != t.size || sqlite3_column_int64(st_track_find, 4) != t.mtime;
        bool was_missing = sqlite3_column_int(st_track_find, 5) != 0;
        sqlite3_reset(st_track_find);
        sqlite3_reset(st_track_seen);
        sqlite3_bind_int64(st_track_seen, 1, (i64)t.size);
        sqlite3_bind_int64(st_track_seen, 2, t.mtime);
        sqlite3_bind_int64(st_track_seen, 3, (i64)dir);
        sqlite3_bind_int64(st_track_seen, 4, walk);
        sqlite3_bind_int64(st_track_seen, 5, (i64)id);
        ok(sqlite3_step(st_track_seen));
        if (was_missing) search_again(*this, db, st_search_put, id); // back: searchable again
        return id;
    }
    // A missing track of this name, size and time, in this root: this is it, moved.
    {
        sqlite3_stmt* st = nullptr;
        u64 found = 0;
        u32 n = 0;
        char from[4200] = "";
        if (ok(sqlite3_prepare_v2(db, "SELECT id, path FROM tracks WHERE missing = 1 AND root = ? AND name = ? AND size = ? AND mtime = ? LIMIT 2", -1, &st, nullptr))) {
            sqlite3_bind_int64(st, 1, t.root);
            sqlite3_bind_text(st, 2, t.name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 3, (i64)t.size);
            sqlite3_bind_int64(st, 4, t.mtime);
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (n++ == 0) { found = (u64)sqlite3_column_int64(st, 0); format(from, sizeof from, "%.*s", sqlite3_column_bytes(st, 1), (const char*)sqlite3_column_blob(st, 1)); }
            }
            sqlite3_finalize(st);
        }
        if (n == 1) { // exactly one: otherwise which of them it is cannot be told, and it is a new track
            if (ok(sqlite3_prepare_v2(db, "UPDATE tracks SET path = ?, dir = ?, missing = 0, seen = ? WHERE id = ?", -1, &st, nullptr))) {
                sqlite3_bind_blob(st, 1, t.path, (int)strlen(t.path), SQLITE_TRANSIENT);
                sqlite3_bind_int64(st, 2, (i64)dir);
                sqlite3_bind_int64(st, 3, walk);
                sqlite3_bind_int64(st, 4, (i64)found);
                ok(sqlite3_step(st));
                sqlite3_finalize(st);
            }
            search_again(*this, db, st_search_put, found);
            if (moved_from && cap) format(moved_from, cap, "%s", from);
            return found;
        }
    }
    sqlite3_stmt* s = st_track_put;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, t.root);
    sqlite3_bind_int64(s, 2, (i64)dir);
    sqlite3_bind_blob(s, 3, t.path, (int)strlen(t.path), SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, t.name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 5, (i64)t.size);
    sqlite3_bind_int64(s, 6, t.mtime);
    sqlite3_bind_text(s, 7, t.title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 8, t.artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 9, t.album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 10, (int)t.track);
    sqlite3_bind_int(s, 11, (int)t.disc);
    GroupIn gi;
    gi.title = t.title; gi.artist = t.artist; gi.album = t.album; gi.dir = dir;
    GroupOut g;
    group_keys(gi, &g);
    sqlite3_bind_text(s, 12, g.sort_title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 13, g.sort_artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 14, g.sort_album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 15, (i64)g.artist_hash);
    sqlite3_bind_int64(s, 16, (i64)g.album_hash);
    sqlite3_bind_text(s, 17, t.artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 18, walk);
    sqlite3_bind_int64(s, 19, walk);
    if (!ok(sqlite3_step(s))) return 0;
    u64 id = (u64)sqlite3_last_insert_rowid(db);
    *fresh = *unread = true;
    // Searchable by the guess and by its path (as text: U+FFFD for what is not UTF-8).
    sqlite3_reset(st_search_put);
    sqlite3_bind_int64(st_search_put, 1, (i64)id);
    sqlite3_bind_text(st_search_put, 2, t.title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st_search_put, 3, t.artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st_search_put, 4, t.album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st_search_put, 5, t.shown, -1, SQLITE_TRANSIENT);
    ok(sqlite3_step(st_search_put));
    return id;
}

bool Index::put_tags(const Tags& t) {
    sqlite3_reset(st_vals_del);
    sqlite3_bind_int64(st_vals_del, 1, (i64)t.track);
    bool good = ok(sqlite3_step(st_vals_del));
    for (u32 i = 0; i < t.nvals; ++i) {
        sqlite3_reset(st_vals_put);
        sqlite3_bind_int64(st_vals_put, 1, (i64)t.track);
        sqlite3_bind_int(st_vals_put, 2, t.sources[i]);
        sqlite3_bind_int(st_vals_put, 3, t.fields[i]);
        sqlite3_bind_text(st_vals_put, 4, t.texts[i], -1, SQLITE_TRANSIENT);
        good = ok(sqlite3_step(st_vals_put)) && good;
    }
    return put_tags_merge_only(t) && good;
}

bool Index::put_tags_merge_only(const Tags& t) {
    bool good = true;
    sqlite3_stmt* s = st_tags_put;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, (i64)t.size);
    sqlite3_bind_int64(s, 2, t.mtime);
    sqlite3_bind_text(s, 3, t.reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 4, (int)t.rate);
    sqlite3_bind_int(s, 5, t.channels);
    sqlite3_bind_int(s, 6, t.bits);
    sqlite3_bind_int64(s, 7, (i64)t.frames);
    sqlite3_bind_int(s, 8, t.estimated ? 1 : 0);
    sqlite3_bind_text(s, 9, t.title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 10, t.artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 11, t.album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 12, t.albumartist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 13, t.date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 14, t.genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 15, (int)t.trackno);
    sqlite3_bind_int(s, 16, (int)t.discno);
    sqlite3_reset(st_dir_of_track);
    sqlite3_bind_int64(st_dir_of_track, 1, (i64)t.track);
    GroupIn gi;
    gi.title = t.title; gi.artist = t.artist; gi.album = t.album; gi.albumartist = t.albumartist; gi.genre = t.genre;
    gi.titlesort = t.titlesort; gi.artistsort = t.artistsort; gi.albumsort = t.albumsort; gi.albumartistsort = t.albumartistsort;
    gi.dir = sqlite3_step(st_dir_of_track) == SQLITE_ROW ? (u64)sqlite3_column_int64(st_dir_of_track, 0) : 0;
    sqlite3_reset(st_dir_of_track);
    GroupOut g;
    group_keys(gi, &g);
    sqlite3_bind_text(s, 17, g.sort_title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 18, g.sort_artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 19, g.sort_album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 20, g.sort_genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 21, (i64)g.artist_hash);
    sqlite3_bind_int64(s, 22, (i64)g.album_hash);
    sqlite3_bind_int64(s, 23, (i64)g.genre_hash);
    sqlite3_bind_text(s, 24, t.albumartist[0] ? t.albumartist : t.artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 25, (i64)t.track);
    good = ok(sqlite3_step(s)) && good;
    if (sqlite3_changes(db) == 0) return good; // the track has gone meanwhile
    // Search by the merge now, and still by the path.
    sqlite3_reset(st_search_del);
    sqlite3_bind_int64(st_search_del, 1, (i64)t.track);
    ok(sqlite3_step(st_search_del));
    sqlite3_reset(st_search_put);
    sqlite3_bind_int64(st_search_put, 1, (i64)t.track);
    sqlite3_bind_text(st_search_put, 2, t.title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st_search_put, 3, t.artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st_search_put, 4, t.album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st_search_put, 5, t.shown, -1, SQLITE_TRANSIENT);
    return ok(sqlite3_step(st_search_put)) && good;
}

u32 Index::count_unread(u32 root) {
    sqlite3_reset(st_count_unread);
    sqlite3_bind_int64(st_count_unread, 1, root);
    return once(st_count_unread, [&] { return sqlite3_step(st_count_unread) == SQLITE_ROW ? (u32)sqlite3_column_int64(st_count_unread, 0) : 0u; });
}

bool Index::dir_of(u64 dir, u32* root, char* path, u32 cap) {
    sqlite3_stmt* s = nullptr;
    bool found = false;
    if (ok(sqlite3_prepare_v2(db, "SELECT root, path FROM dirs WHERE id = ?", -1, &s, nullptr))) {
        sqlite3_bind_int64(s, 1, (i64)dir);
        if (sqlite3_step(s) == SQLITE_ROW) {
            *root = (u32)sqlite3_column_int64(s, 0);
            format(path, cap, "%.*s", sqlite3_column_bytes(s, 1), (const char*)sqlite3_column_blob(s, 1));
            found = true;
        }
        sqlite3_finalize(s);
    }
    return found;
}

u8 Index::dir_state(u64 dir) {
    sqlite3_stmt* s = nullptr;
    u8 state = 0;
    if (ok(sqlite3_prepare_v2(db, "SELECT state FROM dirs WHERE id = ?", -1, &s, nullptr))) {
        sqlite3_bind_int64(s, 1, (i64)dir);
        if (sqlite3_step(s) == SQLITE_ROW) state = (u8)sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return state;
}

u64 Index::track_at(u32 root, const char* path) {
    sqlite3_reset(st_track_find);
    sqlite3_bind_int64(st_track_find, 1, root);
    sqlite3_bind_blob(st_track_find, 2, path, (int)strlen(path), SQLITE_TRANSIENT);
    return once(st_track_find, [&] { return sqlite3_step(st_track_find) == SQLITE_ROW ? (u64)sqlite3_column_int64(st_track_find, 0) : (u64)0; });
}

namespace {
void each_unread(Index& ix, sqlite3* db, const char* where, i64 arg, bool has_arg,
                 void (*fn)(void*, u64, u32, const char*, u64, i64), void* user) {
    char sql[320];
    format(sql, sizeof sql, "SELECT id, root, path, size, mtime FROM tracks WHERE %s missing = 0 AND (read_size IS NOT size OR read_mtime IS NOT mtime) ORDER BY root, path", where);
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return;
    if (has_arg) sqlite3_bind_int64(s, 1, arg);
    char rel[4200];
    while (sqlite3_step(s) == SQLITE_ROW) {
        format(rel, sizeof rel, "%.*s", sqlite3_column_bytes(s, 2), (const char*)sqlite3_column_blob(s, 2));
        fn(user, (u64)sqlite3_column_int64(s, 0), (u32)sqlite3_column_int64(s, 1), rel, (u64)sqlite3_column_int64(s, 3), sqlite3_column_int64(s, 4));
    }
    sqlite3_finalize(s);
    (void)ix;
}
} // namespace

void Index::unread_in(u64 dir, void (*fn)(void*, u64, u32, const char*, u64, i64), void* user) { each_unread(*this, db, "dir = ? AND", (i64)dir, true, fn, user); }
void Index::unread_track(u64 id, void (*fn)(void*, u64, u32, const char*, u64, i64), void* user) { each_unread(*this, db, "id = ? AND", (i64)id, true, fn, user); }
void Index::unread_all(void (*fn)(void*, u64, u32, const char*, u64, i64), void* user) { each_unread(*this, db, "", 0, false, fn, user); }

void Index::mark_missing(u32 root, const char* rel) {
    sqlite3_stmt* st = nullptr;
    u64 id = 0;
    if (ok(sqlite3_prepare_v2(db, "SELECT id FROM tracks WHERE root = ? AND path = ? AND missing = 0", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, root);
        sqlite3_bind_blob(st, 2, rel, (int)strlen(rel), SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) id = (u64)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (!id) return;
    exec_id(db, "UPDATE tracks SET missing = 1, missing_since = unixepoch() WHERE id = ?", (i64)id);
    unsearch(db, id);
}

void Index::mark_missing_under(u32 root, const char* rel_dir) {
    // Paths under rel_dir/ are those from "rel_dir/" up to "rel_dir0" ('0' is '/' + 1).
    char lo[4200], hi[4200];
    u32 n = (u32)format(lo, sizeof lo, "%s/", rel_dir);
    format(hi, sizeof hi, "%s0", rel_dir);
    Array<u64> ids;
    sqlite3_stmt* st = nullptr;
    if (ok(sqlite3_prepare_v2(db, "SELECT id FROM tracks WHERE root = ? AND missing = 0 AND path >= ? AND path < ?", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, root);
        sqlite3_bind_blob(st, 2, lo, (int)n, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 3, hi, (int)strlen(hi), SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) ids.push((u64)sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    }
    for (u64 id : ids) { exec_id(db, "UPDATE tracks SET missing = 1, missing_since = unixepoch() WHERE id = ?", (i64)id); unsearch(db, id); }
    ids.free();
    // Its directories, itself included, are gone (state 3) and leave the folder views.
    if (ok(sqlite3_prepare_v2(db, "UPDATE dirs SET state = 3 WHERE root = ? AND (path = ? OR (path >= ? AND path < ?))", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, root);
        sqlite3_bind_blob(st, 2, rel_dir, (int)strlen(rel_dir), SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 3, lo, (int)n, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 4, hi, (int)strlen(hi), SQLITE_TRANSIENT);
        ok(sqlite3_step(st));
        sqlite3_finalize(st);
    }
}

void Index::after_walk(u32 root, u32 walk, Array<char>* moves) {
    // What the walk did not see is missing.
    Array<u64> gone;
    sqlite3_stmt* st = nullptr;
    if (ok(sqlite3_prepare_v2(db, "SELECT id FROM tracks WHERE root = ? AND missing = 0 AND seen != ?", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, root);
        sqlite3_bind_int64(st, 2, walk);
        while (sqlite3_step(st) == SQLITE_ROW) gone.push((u64)sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    }
    for (u64 id : gone) { exec_id(db, "UPDATE tracks SET missing = 1, missing_since = unixepoch() WHERE id = ?", (i64)id); unsearch(db, id); }
    gone.free();
    // Tracks new in this walk that are one just gone missing, moved: the old row takes the
    // new one's path and the new one goes.
    struct Pair { u64 fresh, old; char from[2048], to[2048]; u64 dir; };
    Array<Pair> pairs;
    const char* sql =
        "SELECT f.id, o.id, o.path, f.path, f.dir FROM tracks f JOIN tracks o ON o.root = f.root AND o.name = f.name AND o.size = f.size"
        " AND o.mtime = f.mtime AND o.missing = 1 WHERE f.root = ? AND f.born = ? AND f.missing = 0";
    if (ok(sqlite3_prepare_v2(db, sql, -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, root);
        sqlite3_bind_int64(st, 2, walk);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Pair p;
            p.fresh = (u64)sqlite3_column_int64(st, 0);
            p.old = (u64)sqlite3_column_int64(st, 1);
            format(p.from, sizeof p.from, "%.*s", sqlite3_column_bytes(st, 2), (const char*)sqlite3_column_blob(st, 2));
            format(p.to, sizeof p.to, "%.*s", sqlite3_column_bytes(st, 3), (const char*)sqlite3_column_blob(st, 3));
            p.dir = (u64)sqlite3_column_int64(st, 4);
            bool twice = false;
            for (const Pair& q : pairs) twice |= q.fresh == p.fresh || q.old == p.old;
            if (!twice) pairs.push(p);
        }
        sqlite3_finalize(st);
    }
    for (const Pair& p : pairs) {
        exec_id(db, "DELETE FROM tagvals WHERE track = ?", (i64)p.fresh);
        unsearch(db, p.fresh);
        exec_id(db, "DELETE FROM tracks WHERE id = ?", (i64)p.fresh);
        if (ok(sqlite3_prepare_v2(db, "UPDATE tracks SET path = ?, dir = ?, missing = 0, seen = ? WHERE id = ?", -1, &st, nullptr))) {
            sqlite3_bind_blob(st, 1, p.to, (int)strlen(p.to), SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, (i64)p.dir);
            sqlite3_bind_int64(st, 3, walk);
            sqlite3_bind_int64(st, 4, (i64)p.old);
            ok(sqlite3_step(st));
            sqlite3_finalize(st);
        }
        search_again(*this, db, st_search_put, p.old);
        if (moves) {
            u32 a = (u32)strlen(p.from) + 1, b = (u32)strlen(p.to) + 1;
            memcpy(moves->push_n(a), p.from, a);
            memcpy(moves->push_n(b), p.to, b);
        }
    }
    pairs.free();
}

void Index::forget_missing(u32 days) {
    char sql[256];
    format(sql, sizeof sql, "DELETE FROM tagvals WHERE track IN (SELECT id FROM tracks WHERE missing = 1 AND missing_since < unixepoch() - %u)", days * 86400u);
    exec(sql);
    format(sql, sizeof sql, "DELETE FROM tracks WHERE missing = 1 AND missing_since < unixepoch() - %u", days * 86400u);
    exec(sql);
}

u64 Index::ensure_dir(u32 root, const char* rel) {
    if (u64 id = find_dir(root, rel)) return id;
    char parent[4200];
    const char* slash = strrchr(rel, '/');
    format(parent, sizeof parent, "%.*s", slash ? (int)(slash - rel) : 0, rel);
    if (rel[0]) ensure_dir(root, parent);
    char name[512];
    shown_utf8(slash ? slash + 1 : rel, name, sizeof name);
    DirRec d{root, rel, parent, name, 1, ""};
    return put_dir(d);
}

u32 Index::next_walk(u32 root) {
    exec_id(db, "UPDATE roots SET walks = walks + 1 WHERE id = ?", root);
    return current_walk(root);
}

u32 Index::current_walk(u32 root) {
    sqlite3_stmt* st = nullptr;
    u32 v = 0;
    if (ok(sqlite3_prepare_v2(db, "SELECT walks FROM roots WHERE id = ?", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, root);
        if (sqlite3_step(st) == SQLITE_ROW) v = (u32)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

u64 Index::root_volume(u32 root) {
    sqlite3_stmt* st = nullptr;
    u64 v = 0;
    if (ok(sqlite3_prepare_v2(db, "SELECT volume FROM roots WHERE id = ?", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, root);
        if (sqlite3_step(st) == SQLITE_ROW) v = (u64)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

void Index::set_root_volume(u32 root, u64 volume) { exec_id(db, "UPDATE roots SET volume = ? WHERE id = ?", (i64)volume, root, true); }

u32 Index::count_tracks(u32 root) {
    sqlite3_reset(st_count_t);
    sqlite3_bind_int64(st_count_t, 1, root);
    return once(st_count_t, [&] { return sqlite3_step(st_count_t) == SQLITE_ROW ? (u32)sqlite3_column_int64(st_count_t, 0) : 0u; });
}

u32 Index::count_dirs(u32 root, u32* failed) {
    sqlite3_reset(st_count_d);
    sqlite3_bind_int64(st_count_d, 1, root);
    *failed = 0;
    if (sqlite3_step(st_count_d) != SQLITE_ROW) { sqlite3_reset(st_count_d); return 0; }
    *failed = (u32)sqlite3_column_int64(st_count_d, 1);
    u32 n = (u32)sqlite3_column_int64(st_count_d, 0);
    sqlite3_reset(st_count_d);
    return n;
}

u64 Index::count_all() {
    sqlite3_reset(st_count_all);
    return once(st_count_all, [&] { return sqlite3_step(st_count_all) == SQLITE_ROW ? (u64)sqlite3_column_int64(st_count_all, 0) : (u64)0; });
}

i64 Index::override_seen(const char* path) {
    sqlite3_stmt* st = nullptr;
    i64 t = -1;
    if (ok(sqlite3_prepare_v2(db, "SELECT mtime FROM overrides WHERE path = ?", -1, &st, nullptr))) {
        sqlite3_bind_blob(st, 1, path, (int)strlen(path), SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) t = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return t;
}

void Index::set_override_seen(const char* path, i64 mtime) {
    sqlite3_stmt* st = nullptr;
    if (ok(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO overrides(path, mtime) VALUES(?, ?)", -1, &st, nullptr))) {
        sqlite3_bind_blob(st, 1, path, (int)strlen(path), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, mtime);
        ok(sqlite3_step(st));
        sqlite3_finalize(st);
    }
}

bool Index::track_info(u64 id, TrackAt* out) {
    sqlite3_stmt* st = nullptr;
    bool found = false;
    if (ok(sqlite3_prepare_v2(db, "SELECT root, dir, path FROM tracks WHERE id = ?", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, (i64)id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            out->id = id;
            out->root = (u32)sqlite3_column_int64(st, 0);
            out->dir = (u64)sqlite3_column_int64(st, 1);
            format(out->rel, sizeof out->rel, "%.*s", sqlite3_column_bytes(st, 2), (const char*)sqlite3_column_blob(st, 2));
            found = true;
        }
        sqlite3_finalize(st);
    }
    return found;
}

void Index::tracks_in_dir(u32 root, const char* rel_dir, Array<u64>* out) {
    u64 dir = find_dir(root, rel_dir);
    if (!dir) return;
    sqlite3_stmt* st = nullptr;
    if (ok(sqlite3_prepare_v2(db, "SELECT id FROM tracks WHERE dir = ?", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, (i64)dir);
        while (sqlite3_step(st) == SQLITE_ROW) out->push((u64)sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    }
}

bool Index::remerge(u64 track, Span<const TagValue> overrides) {
    // The other sources' values, as the last read left them.
    Strings text;
    struct V { u8 field, source; u32 at; };
    Array<V> vals;
    sqlite3_stmt* st = nullptr;
    if (!ok(sqlite3_prepare_v2(db, "SELECT field, source, value FROM tagvals WHERE track = ? AND source != 0", -1, &st, nullptr))) return false;
    sqlite3_bind_int64(st, 1, (i64)track);
    while (sqlite3_step(st) == SQLITE_ROW)
        vals.push(V{(u8)sqlite3_column_int(st, 0), (u8)sqlite3_column_int(st, 1), text.add((const char*)sqlite3_column_text(st, 2))});
    sqlite3_finalize(st);
    Array<TagValue> all;
    for (const V& v : vals) all.push(TagValue{(TagField)v.field, (TagSource)v.source, (u32)strlen(text.at(v.at)), text.at(v.at)});
    for (const TagValue& v : overrides) all.push(v);
    Merged m;
    merge(all.span(), &m);
    // The user's values in tagvals, and the rest of the row as put_tags writes it.
    bool good = true;
    if (ok(sqlite3_prepare_v2(db, "DELETE FROM tagvals WHERE track = ? AND source = 0", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, (i64)track);
        good = ok(sqlite3_step(st));
        sqlite3_finalize(st);
    }
    for (const TagValue& v : overrides) {
        sqlite3_reset(st_vals_put);
        sqlite3_bind_int64(st_vals_put, 1, (i64)track);
        sqlite3_bind_int(st_vals_put, 2, 0);
        sqlite3_bind_int(st_vals_put, 3, (int)v.field);
        sqlite3_bind_text(st_vals_put, 4, v.text, (int)v.len, SQLITE_TRANSIENT);
        good = ok(sqlite3_step(st_vals_put)) && good;
    }
    TrackAt at;
    char shown[4200] = "";
    if (track_info(track, &at)) shown_utf8(at.rel, shown, sizeof shown);
    u64 size = 0; i64 mtime = 0; u32 rate = 0, channels = 0, bits = 0; u64 frames = 0; int est = 0;
    char reason[256] = "";
    if (ok(sqlite3_prepare_v2(db, "SELECT read_size, read_mtime, rate, channels, bits, frames, estimated, reason FROM tracks WHERE id = ?", -1, &st, nullptr))) {
        sqlite3_bind_int64(st, 1, (i64)track);
        if (sqlite3_step(st) == SQLITE_ROW) {
            size = (u64)sqlite3_column_int64(st, 0); mtime = sqlite3_column_int64(st, 1);
            rate = (u32)sqlite3_column_int(st, 2); channels = (u32)sqlite3_column_int(st, 3); bits = (u32)sqlite3_column_int(st, 4);
            frames = (u64)sqlite3_column_int64(st, 5); est = sqlite3_column_int(st, 6);
            format(reason, sizeof reason, "%s", sqlite3_column_text(st, 7) ? (const char*)sqlite3_column_text(st, 7) : "");
        }
        sqlite3_finalize(st);
    }
    Tags t;
    t.track = track; t.size = size; t.mtime = mtime; t.rate = rate; t.channels = (u16)channels; t.bits = (u16)bits; t.frames = frames;
    t.estimated = est != 0; t.reason = reason;
    t.title = m.title; t.artist = m.artist; t.album = m.album; t.albumartist = m.albumartist; t.date = m.date; t.genre = m.genre;
    t.trackno = m.trackno; t.discno = m.discno; t.shown = shown;
    t.titlesort = m.titlesort; t.artistsort = m.artistsort; t.albumsort = m.albumsort; t.albumartistsort = m.albumartistsort;
    // put_tags writes every value given and the merge; here the values are already in place.
    const u8* none = nullptr;
    t.nvals = 0; t.fields = none; t.sources = none; t.texts = nullptr;
    good = put_tags_merge_only(t) && good;
    text.free(); vals.free(); all.free();
    return good;
}

void Index::stop_when(int (*fn)(void*), void* user) { sqlite3_progress_handler(db, fn ? 1000 : 0, fn, user); }

u32 search_expression(const char* text, char* out, u32 cap) {
    u32 n = 0, words = 0;
    out[0] = 0;
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* b = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (n + (u32)(p - b) * 2 + 16 >= cap) break;
        if (words++) { memcpy(out + n, " AND ", 5); n += 5; }
        out[n++] = '"';
        for (const char* q = b; q < p; ++q) { if (*q == '"') out[n++] = '"'; out[n++] = *q; }
        out[n++] = '"';
        out[n++] = '*';
        out[n] = 0;
    }
    return words;
}

u32 Index::window(const LibraryQuery& q, u64 generation, LibraryRow* out, u32* total, const char* (*root_path)(void*, u32), void* user) {
    *total = 0;
    u32 want = q.count < kLibraryMaxWindow ? q.count : kLibraryMaxWindow;
    u32 got = 0;
    auto count = [&](const char* sql, auto bind) -> u32 {
        sqlite3_stmt* s = nullptr;
        if (!ok(sqlite3_prepare_v2(db, sql, -1, &s, nullptr))) return 0;
        bind(s);
        u32 n = sqlite3_step(s) == SQLITE_ROW ? (u32)sqlite3_column_int64(s, 0) : 0;
        sqlite3_finalize(s);
        return n;
    };
    auto fill_track = [&](sqlite3_stmt* s, LibraryRow& r) {
        r = LibraryRow{};
        r.query = q.id;
        r.kind = (u8)LibraryRowKind::Track;
        r.id = (u64)sqlite3_column_int64(s, 0);
        r.root = (u32)sqlite3_column_int64(s, 1);
        text_into(r.name, sizeof r.name, sqlite3_column_text(s, 2));
        text_into(r.title, sizeof r.title, sqlite3_column_text(s, 3));
        text_into(r.artist, sizeof r.artist, sqlite3_column_text(s, 4));
        text_into(r.album, sizeof r.album, sqlite3_column_text(s, 5));
        r.track_number = (u32)sqlite3_column_int(s, 6);
        r.disc_number = (u32)sqlite3_column_int(s, 7);
        r.size = (u64)sqlite3_column_int64(s, 8);
        r.frames = (u64)sqlite3_column_int64(s, 9);
        r.rate = (u32)sqlite3_column_int(s, 10);
        const char* rp = root_path(user, r.root);
        const void* rel = sqlite3_column_blob(s, 11);
        int rn = sqlite3_column_bytes(s, 11);
        usize need = strlen(rp) + 1 + (usize)rn;
        if (need < sizeof r.path) format(r.path, sizeof r.path, "%s/%.*s", rp, rn, (const char*)rel);
        else format(r.reason, sizeof r.reason, "its path is %zu bytes, longer than the player takes", need);
        r.missing = (u8)sqlite3_column_int(s, 12);
        if (!r.reason[0]) text_into(r.reason, sizeof r.reason, sqlite3_column_text(s, 13));
        text_into(r.date, sizeof r.date, sqlite3_column_text(s, 14));
    };
    auto run_tracks = [&](sqlite3_stmt* s, u32 index) {
        while (got < want && sqlite3_step(s) == SQLITE_ROW) {
            fill_track(s, out[got]);
            out[got].index = index++;
            ++got;
        }
    };
    char sql[1024];
    // A track view's ORDER BY, by the sort asked for or the view's own.
    auto order = [&](const char* fallback) -> const char* {
        switch (q.sort) {
        case LibrarySort::Title:  return "t.sort_title, t.sort_artist";
        case LibrarySort::Artist: return "t.sort_artist, t.sort_album, t.album_hash, t.discno, t.trackno, t.sort_title";
        case LibrarySort::Album:  return "t.sort_album, t.album_hash, t.discno, t.trackno";
        case LibrarySort::Date:   return "t.date, t.sort_artist, t.sort_album";
        case LibrarySort::Path:   return "t.root, t.path";
        default:                  return fallback;
        }
    };
    // A track's row by id, into out[got].
    auto by_id = [&](u64 id, u32 index) {
        if (!st_row) {
            char rs[512];
            format(rs, sizeof rs, "SELECT %s FROM tracks t WHERE t.id = ?", kTrackCols);
            if (!ok(sqlite3_prepare_v3(db, rs, -1, SQLITE_PREPARE_PERSISTENT, &st_row, nullptr))) return;
        }
        sqlite3_reset(st_row);
        sqlite3_bind_int64(st_row, 1, (i64)id);
        if (sqlite3_step(st_row) != SQLITE_ROW) { sqlite3_reset(st_row); return; }
        fill_track(st_row, out[got]);
        out[got].index = index;
        ++got;
        sqlite3_reset(st_row); // a statement left on a row holds its read open: the reader would see an old index
    };
    // A window of tracks: `where` with its one bound value (or none), in `ord`. The window's
    // ids first, by the index that is that order; then each row by its id.
    auto tracks = [&](const char* from, const char* where, i64 arg, bool has_arg, const char* ord) {
        format(sql, sizeof sql, "SELECT count(*) FROM %s WHERE %s", from, where);
        *total = count(sql, [&](sqlite3_stmt* st) { if (has_arg) sqlite3_bind_int64(st, 1, arg); });
        sqlite3_stmt* st = nullptr;
        format(sql, sizeof sql, "SELECT t.id FROM %s WHERE %s ORDER BY %s LIMIT ? OFFSET ?", from, where, ord);
        if (!ok(sqlite3_prepare_v2(db, sql, -1, &st, nullptr))) return;
        int k = 1;
        if (has_arg) sqlite3_bind_int64(st, k++, arg);
        sqlite3_bind_int64(st, k++, want);
        sqlite3_bind_int64(st, k++, q.first);
        u64 ids[kLibraryMaxWindow];
        u32 n = 0;
        while (n < want && sqlite3_step(st) == SQLITE_ROW) ids[n++] = (u64)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        for (u32 i = 0; i < n; ++i) by_id(ids[i], q.first + i);
    };
    // What is kept for this query, if it is still good; otherwise it is made again.
    auto keeping = [&]() {
        bool same = kept.generation == generation && kept.view == q.view && kept.sort == q.sort && kept.parent == q.parent &&
                    strcmp(kept.text, q.text) == 0;
        if (same) return true;
        kept.ids.clear(); kept.groups.clear();
        kept.generation = generation; kept.view = q.view; kept.sort = q.sort; kept.parent = q.parent;
        format(kept.text, sizeof kept.text, "%s", q.text);
        return false;
    };
    // A window of groups: the whole grouping is made once (`select` over its covering
    // index, grouped by `group_by`) and kept, and a window is a slice of it.
    auto groups = [&](LibraryRowKind kind, const char* where, i64 arg, bool has_arg, const char* group_by, const char* select) {
        if (!keeping()) {
            sqlite3_stmt* st = nullptr;
            format(sql, sizeof sql, "SELECT %s FROM tracks t WHERE %s GROUP BY %s ORDER BY %s", select, where, group_by, group_by);
            if (!ok(sqlite3_prepare_v2(db, sql, -1, &st, nullptr))) return;
            if (has_arg) sqlite3_bind_int64(st, 1, arg);
            while (sqlite3_step(st) == SQLITE_ROW) {
                LibraryRow r{};
                r.kind = (u8)kind;
                r.id = (u64)sqlite3_column_int64(st, 0);
                text_into(r.name, sizeof r.name, sqlite3_column_text(st, 1));
                r.count = (u32)sqlite3_column_int64(st, 2);
                if (kind == LibraryRowKind::Artist) r.albums = (u32)sqlite3_column_int64(st, 3);
                if (kind == LibraryRowKind::Album) {
                    text_into(r.artist, sizeof r.artist, sqlite3_column_text(st, 3));
                    text_into(r.date, sizeof r.date, sqlite3_column_text(st, 4));
                    text_into(r.album, sizeof r.album, sqlite3_column_text(st, 1));
                }
                kept.groups.push(r);
            }
            sqlite3_finalize(st);
        }
        *total = kept.groups.len;
        for (u32 i = q.first; i < kept.groups.len && got < want; ++i) {
            out[got] = kept.groups[i];
            out[got].query = q.id;
            out[got].index = i;
            ++got;
        }
    };
    constexpr const char* kShownArtist = "t.shown_artist";
    switch (q.view) {
    case LibraryView::Folders: {
        u32 ndirs = count("SELECT count(*) FROM dirs WHERE parent = ? AND state != 3", [&](sqlite3_stmt* st) { sqlite3_bind_int64(st, 1, (i64)q.parent); });
        u32 ntracks = q.parent ? count("SELECT count(*) FROM tracks WHERE dir = ? AND missing = 0", [&](sqlite3_stmt* st) { sqlite3_bind_int64(st, 1, (i64)q.parent); }) : 0;
        *total = ndirs + ntracks;
        if (q.first < ndirs) {
            sqlite3_stmt* st = nullptr;
            if (ok(sqlite3_prepare_v2(db, "SELECT id, root, name FROM dirs WHERE parent = ? AND state != 3 ORDER BY name LIMIT ? OFFSET ?", -1, &st, nullptr))) {
                sqlite3_bind_int64(st, 1, (i64)q.parent);
                sqlite3_bind_int64(st, 2, want);
                sqlite3_bind_int64(st, 3, q.first);
                u32 index = q.first;
                while (got < want && sqlite3_step(st) == SQLITE_ROW) {
                    LibraryRow& r = out[got++];
                    r = LibraryRow{};
                    r.query = q.id; r.index = index++;
                    r.kind = (u8)LibraryRowKind::Directory;
                    r.id = (u64)sqlite3_column_int64(st, 0);
                    r.root = (u32)sqlite3_column_int64(st, 1);
                    text_into(r.name, sizeof r.name, sqlite3_column_text(st, 2));
                    if (!q.parent) format(r.name, sizeof r.name, "%s", root_path(user, r.root)); // a root shows as its path
                }
                sqlite3_finalize(st);
            }
        }
        if (got < want && ntracks) {
            u32 from = q.first > ndirs ? q.first - ndirs : 0;
            sqlite3_stmt* st = nullptr;
            format(sql, sizeof sql, "SELECT %s FROM tracks t WHERE t.dir = ? AND t.missing = 0 ORDER BY t.name LIMIT ? OFFSET ?", kTrackCols);
            if (ok(sqlite3_prepare_v2(db, sql, -1, &st, nullptr))) {
                sqlite3_bind_int64(st, 1, (i64)q.parent);
                sqlite3_bind_int64(st, 2, want - got);
                sqlite3_bind_int64(st, 3, from);
                run_tracks(st, ndirs + from);
                sqlite3_finalize(st);
            }
        }
        break;
    }
    case LibraryView::Tracks:
        if (q.parent) tracks("tracks t", "t.genre_hash = ? AND t.missing = 0", (i64)q.parent, true,
                             order("t.sort_artist, t.sort_album, t.album_hash, t.discno, t.trackno, t.sort_title"));
        else tracks("tracks t", "t.missing = 0", 0, false, order("t.sort_artist, t.sort_album, t.album_hash, t.discno, t.trackno, t.sort_title"));
        break;
    case LibraryView::AlbumTracks:
        tracks("tracks t", "t.album_hash = ? AND t.missing = 0", (i64)q.parent, true, order("t.discno, t.trackno, t.sort_title"));
        break;
    case LibraryView::Search: {
        // Every match's id in order, kept: best first (FTS5's rank) or by a track order.
        if (!keeping()) {
            char expr[512];
            if (search_expression(q.text, expr, sizeof expr)) {
                if (q.sort == LibrarySort::Default) format(sql, sizeof sql, "SELECT rowid FROM search WHERE search MATCH ? ORDER BY rank");
                else format(sql, sizeof sql, "SELECT t.id FROM search JOIN tracks t ON t.id = search.rowid WHERE search MATCH ? AND t.missing = 0 ORDER BY %s", order(""));
                sqlite3_stmt* st = nullptr;
                if (ok(sqlite3_prepare_v2(db, sql, -1, &st, nullptr))) {
                    sqlite3_bind_text(st, 1, expr, -1, SQLITE_TRANSIENT);
                    while (sqlite3_step(st) == SQLITE_ROW) kept.ids.push((u64)sqlite3_column_int64(st, 0));
                    sqlite3_finalize(st);
                }
            }
        }
        *total = kept.ids.len;
        for (u32 i = q.first; i < kept.ids.len && got < want; ++i) by_id(kept.ids[i], i);
        break;
    }
    case LibraryView::Artists: {
        char sel[256];
        format(sel, sizeof sel, "MIN(t.artist_hash), MIN(%s), count(*), count(DISTINCT t.album_hash)", kShownArtist);
        groups(LibraryRowKind::Artist, "t.missing = 0", 0, false, "t.sort_artist", sel);
        break;
    }
    case LibraryView::Albums: {
        char sel[512];
        format(sel, sizeof sel, "t.album_hash, MIN(t.album), count(*), CASE WHEN MIN(%s) = MAX(%s) THEN MIN(%s) ELSE 'Various Artists' END, MIN(t.date)",
               kShownArtist, kShownArtist, kShownArtist);
        if (q.parent) groups(LibraryRowKind::Album, "t.artist_hash = ? AND t.missing = 0 AND t.album_hash != 0", (i64)q.parent, true, "t.sort_album, t.album_hash", sel);
        else groups(LibraryRowKind::Album, "t.missing = 0 AND t.album_hash != 0", 0, false, "t.sort_album, t.album_hash", sel);
        break;
    }
    case LibraryView::Genres:
        groups(LibraryRowKind::Genre, "t.missing = 0 AND t.genre_hash != 0", 0, false, "t.sort_genre", "MIN(t.genre_hash), MIN(t.genre), count(*)");
        break;
    case LibraryView::Count: break;
    }
    return got;
}

} // namespace kv::lib
