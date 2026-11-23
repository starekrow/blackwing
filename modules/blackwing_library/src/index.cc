// The index over sqlite: the schema, the writer's statements, and the reader's windows.
#include "index.h"
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
    "CREATE TABLE roots(id INTEGER PRIMARY KEY, path BLOB NOT NULL);"
    "CREATE TABLE dirs(id INTEGER PRIMARY KEY, root INTEGER NOT NULL, parent INTEGER NOT NULL, path BLOB NOT NULL,"
    "  name TEXT NOT NULL, state INTEGER NOT NULL DEFAULT 0, reason TEXT);"
    "CREATE UNIQUE INDEX dirs_path ON dirs(root, path);"
    "CREATE INDEX dirs_parent ON dirs(parent, name);"
    "CREATE TABLE tracks(id INTEGER PRIMARY KEY, root INTEGER NOT NULL, dir INTEGER NOT NULL, path BLOB NOT NULL,"
    "  name TEXT NOT NULL, size INTEGER NOT NULL, mtime INTEGER NOT NULL, missing INTEGER NOT NULL DEFAULT 0,"
    "  read_size INTEGER, read_mtime INTEGER, reason TEXT,"
    "  rate INTEGER, channels INTEGER, bits INTEGER, frames INTEGER, estimated INTEGER,"
    "  title TEXT, artist TEXT, album TEXT, albumartist TEXT, trackno INTEGER, discno INTEGER, date TEXT, genre TEXT);"
    "CREATE UNIQUE INDEX tracks_path ON tracks(root, path);"
    "CREATE INDEX tracks_dir ON tracks(dir, name);"
    "CREATE TABLE tagvals(track INTEGER NOT NULL, source INTEGER NOT NULL, field INTEGER NOT NULL, value TEXT NOT NULL);"
    "CREATE INDEX tagvals_track ON tagvals(track);"
    "CREATE VIRTUAL TABLE search USING fts5(title, artist, album, path, tokenize = 'unicode61 remove_diacritics 2', prefix = '2 3');";

// The columns a row is filled from, in this order.
constexpr const char* kTrackCols = "t.id, t.root, t.name, t.title, t.artist, t.album, t.trackno, t.discno, t.size, t.frames, t.rate, t.path, t.missing, t.reason";

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
        st_root      = prep("INSERT OR REPLACE INTO roots(id, path) VALUES(?, ?)");
        st_unroot_s  = prep("DELETE FROM search WHERE rowid IN (SELECT id FROM tracks WHERE root = ?)");
        st_unroot_t  = prep("DELETE FROM tracks WHERE root = ?");
        st_unroot_d  = prep("DELETE FROM dirs WHERE root = ?");
        st_unroot_r  = prep("DELETE FROM roots WHERE id = ?");
        st_dir_find  = prep("SELECT id FROM dirs WHERE root = ? AND path = ?");
        st_dir_put   = prep("INSERT INTO dirs(root, parent, path, name, state, reason) VALUES(?, ?, ?, ?, ?, ?)");
        st_dir_state = prep("UPDATE dirs SET state = ?, reason = ? WHERE id = ?");
        st_track_find = prep("SELECT id, size, mtime, read_size, read_mtime FROM tracks WHERE root = ? AND path = ?");
        st_track_put = prep("INSERT INTO tracks(root, dir, path, name, size, mtime, title, artist, album, trackno, discno) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        st_track_seen = prep("UPDATE tracks SET size = ?, mtime = ?, dir = ?, missing = 0 WHERE id = ?");
        st_search_put = prep("INSERT INTO search(rowid, title, artist, album, path) VALUES(?, ?, ?, ?, ?)");
        st_count_t   = prep("SELECT count(*) FROM tracks WHERE root = ? AND missing = 0");
        st_count_d   = prep("SELECT count(*), sum(state = 2) FROM dirs WHERE root = ?");
        st_count_all = prep("SELECT count(*) FROM tracks WHERE missing = 0");
        st_count_unread = prep("SELECT count(*) FROM tracks WHERE root = ? AND missing = 0 AND (read_size IS NOT size OR read_mtime IS NOT mtime)");
        st_vals_del  = prep("DELETE FROM tagvals WHERE track = ?");
        st_vals_put  = prep("INSERT INTO tagvals(track, source, field, value) VALUES(?, ?, ?, ?)");
        st_tags_put  = prep("UPDATE tracks SET read_size = ?, read_mtime = ?, reason = ?, rate = ?, channels = ?, bits = ?, frames = ?, estimated = ?,"
                            " title = ?, artist = ?, album = ?, albumartist = ?, date = ?, genre = ?, trackno = ?, discno = ? WHERE id = ?");
        st_search_del = prep("DELETE FROM search WHERE rowid = ?");
        if (!st_count_all) { damaged = true; close(); return false; }
    }
    return true;
}

void Index::close() {
    sqlite3_stmt** all[] = {&st_root, &st_unroot_t, &st_unroot_d, &st_unroot_s, &st_unroot_r, &st_dir_find, &st_dir_put, &st_dir_state,
                            &st_track_find, &st_track_put, &st_track_seen, &st_search_put, &st_count_t, &st_count_d, &st_count_all,
                            &st_count_unread, &st_vals_del, &st_vals_put, &st_tags_put, &st_search_del};
    for (sqlite3_stmt** s : all) { if (*s) sqlite3_finalize(*s); *s = nullptr; }
    if (db) sqlite3_close_v2(db);
    db = nullptr;
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

u64 Index::find_dir(u32 root, const char* p) {
    sqlite3_reset(st_dir_find);
    sqlite3_bind_int64(st_dir_find, 1, root);
    sqlite3_bind_blob(st_dir_find, 2, p, (int)strlen(p), SQLITE_TRANSIENT);
    return sqlite3_step(st_dir_find) == SQLITE_ROW ? (u64)sqlite3_column_int64(st_dir_find, 0) : 0;
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

u64 Index::put_track(const TrackRec& t, u64 dir, bool* fresh, bool* changed, bool* unread) {
    *fresh = *changed = *unread = false;
    sqlite3_reset(st_track_find);
    sqlite3_bind_int64(st_track_find, 1, t.root);
    sqlite3_bind_blob(st_track_find, 2, t.path, (int)strlen(t.path), SQLITE_TRANSIENT);
    if (sqlite3_step(st_track_find) == SQLITE_ROW) {
        u64 id = (u64)sqlite3_column_int64(st_track_find, 0);
        *changed = (u64)sqlite3_column_int64(st_track_find, 1) != t.size || sqlite3_column_int64(st_track_find, 2) != t.mtime;
        *unread = *changed || sqlite3_column_type(st_track_find, 3) == SQLITE_NULL ||
                  (u64)sqlite3_column_int64(st_track_find, 3) != t.size || sqlite3_column_int64(st_track_find, 4) != t.mtime;
        sqlite3_reset(st_track_seen);
        sqlite3_bind_int64(st_track_seen, 1, (i64)t.size);
        sqlite3_bind_int64(st_track_seen, 2, t.mtime);
        sqlite3_bind_int64(st_track_seen, 3, (i64)dir);
        sqlite3_bind_int64(st_track_seen, 4, (i64)id);
        ok(sqlite3_step(st_track_seen));
        return id;
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
    sqlite3_bind_int64(s, 17, (i64)t.track);
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
    return sqlite3_step(st_count_unread) == SQLITE_ROW ? (u32)sqlite3_column_int64(st_count_unread, 0) : 0;
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
    return sqlite3_step(st_track_find) == SQLITE_ROW ? (u64)sqlite3_column_int64(st_track_find, 0) : 0;
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

u32 Index::count_tracks(u32 root) {
    sqlite3_reset(st_count_t);
    sqlite3_bind_int64(st_count_t, 1, root);
    return sqlite3_step(st_count_t) == SQLITE_ROW ? (u32)sqlite3_column_int64(st_count_t, 0) : 0;
}

u32 Index::count_dirs(u32 root, u32* failed) {
    sqlite3_reset(st_count_d);
    sqlite3_bind_int64(st_count_d, 1, root);
    *failed = 0;
    if (sqlite3_step(st_count_d) != SQLITE_ROW) return 0;
    *failed = (u32)sqlite3_column_int64(st_count_d, 1);
    return (u32)sqlite3_column_int64(st_count_d, 0);
}

u64 Index::count_all() {
    sqlite3_reset(st_count_all);
    return sqlite3_step(st_count_all) == SQLITE_ROW ? (u64)sqlite3_column_int64(st_count_all, 0) : 0;
}

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

u32 Index::window(const LibraryQuery& q, LibraryRow* out, u32* total, const char* (*root_path)(void*, u32), void* user) {
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
    };
    auto run_tracks = [&](sqlite3_stmt* s, u32 index) {
        while (got < want && sqlite3_step(s) == SQLITE_ROW) {
            fill_track(s, out[got]);
            out[got].index = index++;
            ++got;
        }
    };
    char sql[512];
    switch (q.view) {
    case LibraryView::Folders: {
        u32 ndirs = count("SELECT count(*) FROM dirs WHERE parent = ?", [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, (i64)q.parent); });
        u32 ntracks = q.parent ? count("SELECT count(*) FROM tracks WHERE dir = ? AND missing = 0", [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, (i64)q.parent); }) : 0;
        *total = ndirs + ntracks;
        if (q.first < ndirs) {
            sqlite3_stmt* s = nullptr;
            if (ok(sqlite3_prepare_v2(db, "SELECT id, root, name FROM dirs WHERE parent = ? ORDER BY name LIMIT ? OFFSET ?", -1, &s, nullptr))) {
                sqlite3_bind_int64(s, 1, (i64)q.parent);
                sqlite3_bind_int64(s, 2, want);
                sqlite3_bind_int64(s, 3, q.first);
                u32 index = q.first;
                while (got < want && sqlite3_step(s) == SQLITE_ROW) {
                    LibraryRow& r = out[got++];
                    r = LibraryRow{};
                    r.query = q.id; r.index = index++;
                    r.kind = (u8)LibraryRowKind::Directory;
                    r.id = (u64)sqlite3_column_int64(s, 0);
                    r.root = (u32)sqlite3_column_int64(s, 1);
                    text_into(r.name, sizeof r.name, sqlite3_column_text(s, 2));
                    if (!q.parent) format(r.name, sizeof r.name, "%s", root_path(user, r.root)); // a root shows as its path
                }
                sqlite3_finalize(s);
            }
        }
        if (got < want && ntracks) {
            u32 from = q.first > ndirs ? q.first - ndirs : 0;
            sqlite3_stmt* s = nullptr;
            format(sql, sizeof sql, "SELECT %s FROM tracks t WHERE t.dir = ? AND t.missing = 0 ORDER BY t.name LIMIT ? OFFSET ?", kTrackCols);
            if (ok(sqlite3_prepare_v2(db, sql, -1, &s, nullptr))) {
                sqlite3_bind_int64(s, 1, (i64)q.parent);
                sqlite3_bind_int64(s, 2, want - got);
                sqlite3_bind_int64(s, 3, from);
                run_tracks(s, ndirs + from);
                sqlite3_finalize(s);
            }
        }
        break;
    }
    case LibraryView::Tracks: {
        *total = count("SELECT count(*) FROM tracks WHERE missing = 0", [](sqlite3_stmt*) {});
        sqlite3_stmt* s = nullptr;
        format(sql, sizeof sql, "SELECT %s FROM tracks t WHERE t.missing = 0 ORDER BY t.root, t.path LIMIT ? OFFSET ?", kTrackCols);
        if (ok(sqlite3_prepare_v2(db, sql, -1, &s, nullptr))) {
            sqlite3_bind_int64(s, 1, want);
            sqlite3_bind_int64(s, 2, q.first);
            run_tracks(s, q.first);
            sqlite3_finalize(s);
        }
        break;
    }
    case LibraryView::Search: {
        char expr[512];
        if (!search_expression(q.text, expr, sizeof expr)) break;
        *total = count("SELECT count(*) FROM search WHERE search MATCH ?", [&](sqlite3_stmt* s) { sqlite3_bind_text(s, 1, expr, -1, SQLITE_TRANSIENT); });
        sqlite3_stmt* s = nullptr;
        format(sql, sizeof sql, "SELECT %s FROM search JOIN tracks t ON t.id = search.rowid WHERE search MATCH ? AND t.missing = 0 ORDER BY rank, t.name LIMIT ? OFFSET ?", kTrackCols);
        if (ok(sqlite3_prepare_v2(db, sql, -1, &s, nullptr))) {
            sqlite3_bind_text(s, 1, expr, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, want);
            sqlite3_bind_int64(s, 3, q.first);
            run_tracks(s, q.first);
            sqlite3_finalize(s);
        }
        break;
    }
    case LibraryView::Count: break;
    }
    return got;
}

} // namespace kv::lib
