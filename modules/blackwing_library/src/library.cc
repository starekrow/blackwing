// The library: one system on the main thread, each tick, and three threads of its own.
//
// The system takes the commands that arrived (roots added and removed, windows asked for
// and closed), hands them to the threads, takes what the threads have finished (counts,
// walks done, windows answered) and publishes it: library.roots, library.rows,
// library.scan, and the events. It never touches the index or the disk.
//
// The scan thread walks (scan.cc). The writer owns the index's one writing connection: it
// takes what the walk found in batches and commits them a few times a second, each commit
// a new generation. The reader owns a read-only connection (WAL: a long write never holds
// it) and answers windows, the newest ask for each, and asks again for open windows when
// the generation moves on.
//
// The roots are the one thing here that is not a cache: their list is a small file beside
// the index (roots.txt: an id and a path a line), written by the writer whole, under a
// temporary name and renamed. The index is deleted and made again when it is another
// version or sqlite says it is damaged, and the roots are walked again into it.
#include "korvid/blackwing_library/module.h"
#include "lib.h"
#include "korvid/core/atomic.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "vendor/sqlite3.h"
#include <new>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace lib {

constexpr f64 kCommitSeconds = 0.25; // the writer commits at most this often while batches come
constexpr f64 kRefillSeconds = 0.25; // open windows are asked again at most this often

// ---- the writer ------------------------------------------------------------------------------

void save_roots(Shared& sh) {
    // The list, whole, under a temporary name, then renamed over the old one.
    Array<char> text;
    os_mutex_lock(&sh.lock);
    for (const Shared::RootLine& r : sh.root_lines) {
        char line[kPathMax + 32];
        u32 n = (u32)format(line, sizeof line, "%u\t%s\n", r.id, r.path);
        memcpy(text.push_n(n), line, n);
    }
    char path[kPathMax];
    format(path, sizeof path, "%s", sh.roots_path);
    os_mutex_unlock(&sh.lock);
    char tmp[kPathMax + 8];
    format(tmp, sizeof tmp, "%s.new", path);
    if (!io_write_now(tmp, text.data ? text.data : "", text.len) || !io_rename(tmp, path))
        KV_ERROR("library", "the list of roots could not be saved to %s", path);
    text.free();
}

// Opens the index; one that is damaged or of another version is deleted and made again.
bool open_index(Shared& sh, Index& ix) {
    if (ix.open(sh.index_path, true)) return true;
    KV_WARN("library", "the index at %s is being made again: %s", sh.index_path, ix.error);
    Index::remove_files(sh.index_path);
    os_mutex_lock(&sh.lock);
    sh.rebuilt = true;
    os_mutex_unlock(&sh.lock);
    if (ix.open(sh.index_path, true)) return true;
    KV_ERROR("library", "the index at %s cannot be made: %s", sh.index_path, ix.error);
    return false;
}

void write_thread(void* arg) {
    Shared& sh = *(Shared*)arg;
    Index ix;
    bool up = open_index(sh, ix);
    os_mutex_lock(&sh.lock);
    sh.ready = true;
    os_cond_broadcast(&sh.read_wake);
    os_mutex_unlock(&sh.lock);
    Array<u32> live;                // roots added and not removed: a batch for any other is old news
    Array<WriteMsg*> work;
    Array<u32> touched;             // roots whose counts are to be said
    Array<Note> walked;             // walks whose last batch is in this commit
    for (;;) {
        os_mutex_lock(&sh.lock);
        while (!sh.quit && sh.writes.len == 0) os_cond_wait(&sh.write_wake, &sh.lock);
        if (sh.quit) { os_mutex_unlock(&sh.lock); break; }
        // A batch alone waits a little for others, so a commit carries a few of them.
        if (sh.writes.len == 1 && sh.writes[0]->kind == WriteKind::Batch) os_cond_timed_wait(&sh.write_wake, &sh.lock, kCommitSeconds);
        for (WriteMsg* m : sh.writes) work.push(m);
        sh.writes.clear();
        os_mutex_unlock(&sh.lock);

        if (up) ix.begin();
        bool save = false;
        for (WriteMsg* m : work) {
            switch (m->kind) {
            case WriteKind::AddRoot:
                live.push(m->root);
                if (up) ix.put_root(m->root, m->path);
                break;
            case WriteKind::RemoveRoot:
                for (u32 i = 0; i < live.len; ++i) if (live[i] == m->root) { live[i] = live[live.len - 1]; --live.len; break; }
                if (up) ix.remove_root(m->root);
                break;
            case WriteKind::SaveRoots: save = true; break;
            case WriteKind::Walked: {
                Note n{NoteKind::Walked};
                n.root = m->root;
                n.seconds = m->seconds;
                walked.push(n);
                break;
            }
            case WriteKind::Batch: {
                Batch& b = *m->batch;
                auto is_live = [&](u32 r) { for (u32 x : live) if (x == r) return true; return false; };
                if (up) {
                    for (const DirMsg& d : b.dirs) {
                        if (!is_live(d.root)) continue;
                        DirRec r{d.root, b.s.at(d.path), b.s.at(d.parent), b.s.at(d.name), d.state, b.s.at(d.reason)};
                        ix.put_dir(r);
                    }
                    u64 dir_id = 0;
                    u32 dir_root = 0;
                    const char* dir_path = nullptr;
                    for (const TrackMsg& t : b.tracks) {
                        if (!is_live(t.root)) continue;
                        const char* dp = b.s.at(t.dir);
                        if (!dir_path || dir_root != t.root || strcmp(dp, dir_path) != 0) { dir_id = ix.find_dir(t.root, dp); dir_root = t.root; dir_path = dp; }
                        TrackRec r;
                        r.root = t.root; r.dir = dp; r.path = b.s.at(t.path); r.name = b.s.at(t.name); r.shown = b.s.at(t.shown);
                        r.size = t.size; r.mtime = t.mtime;
                        r.title = b.s.at(t.title); r.artist = b.s.at(t.artist); r.album = b.s.at(t.album);
                        r.track = t.track; r.disc = t.disc;
                        bool fresh, changed;
                        ix.put_track(r, dir_id, &fresh, &changed);
                    }
                }
                for (const DirMsg& d : b.dirs) { bool seen = false; for (u32 x : touched) seen |= x == d.root; if (!seen) touched.push(d.root); }
                b.free();
                mem_free(m->batch);
                break;
            }
            }
            mem_free(m);
        }
        work.clear();
        if (up && !ix.commit()) KV_ERROR("library", "the index could not commit: %s", ix.error);
        if (up && ix.damaged) {
            // sqlite says the file is damaged: made again, and every root walked again into it.
            KV_WARN("library", "the index is damaged (%s) and is being made again", ix.error);
            ix.close();
            Index::remove_files(sh.index_path);
            up = open_index(sh, ix);
            os_mutex_lock(&sh.lock);
            sh.rebuilt = true;
            for (const Shared::RootLine& r : sh.root_lines) {
                if (up) ix.put_root(r.id, r.path);
                Walk w{r.id, ""};
                format(w.path, sizeof w.path, "%s", r.path);
                sh.walks.push(w);
            }
            os_cond_signal(&sh.scan_wake);
            os_mutex_unlock(&sh.lock);
        }
        u64 gen = atomic_add_fetch(&sh.generation, (u64)1, MO_RELEASE);
        if (save) save_roots(sh);

        // What changed, for the main thread.
        os_mutex_lock(&sh.lock);
        for (Note& n : walked) { bool seen = false; for (u32 x : touched) seen |= x == n.root; if (!seen) touched.push(n.root); }
        u64 total = up ? ix.count_all() : 0;
        for (u32 root : touched) {
            Note n{NoteKind::Counts};
            n.root = root;
            if (up) { n.tracks = ix.count_tracks(root); n.dirs = ix.count_dirs(root, &n.failed); }
            n.generation = gen; n.total = total;
            sh.notes.push(n);
        }
        for (Note& n : walked) { if (up) n.tracks = ix.count_tracks(n.root); n.generation = gen; n.total = total; sh.notes.push(n); }
        Note c{NoteKind::Committed};
        c.generation = gen; c.total = total;
        sh.notes.push(c);
        os_cond_signal(&sh.read_wake); // open windows may want filling again
        os_mutex_unlock(&sh.lock);
        touched.clear();
        walked.clear();
    }
    for (WriteMsg* m : work) { if (m->batch) { m->batch->free(); mem_free(m->batch); } mem_free(m); }
    work.free(); live.free(); touched.free(); walked.free();
    ix.close();
}

// ---- the reader ------------------------------------------------------------------------------

struct RootPaths { Array<Shared::RootLine> lines; };
const char* root_path_of(void* user, u32 id) {
    RootPaths& r = *(RootPaths*)user;
    for (const Shared::RootLine& l : r.lines) if (l.id == id) return l.path;
    return "";
}

void read_thread(void* arg) {
    Shared& sh = *(Shared*)arg;
    os_mutex_lock(&sh.lock);
    while (!sh.quit && !sh.ready) os_cond_wait(&sh.read_wake, &sh.lock);
    os_mutex_unlock(&sh.lock);
    Index ix;
    bool up = false;
    RootPaths roots;
    Array<Ask> todo;
    for (;;) {
        os_mutex_lock(&sh.lock);
        while (!sh.quit && sh.asks.len == 0) os_cond_wait(&sh.read_wake, &sh.lock);
        if (sh.quit) { os_mutex_unlock(&sh.lock); break; }
        for (const Ask& a : sh.asks) todo.push(a);
        sh.asks.clear();
        roots.lines.clear();
        for (const Shared::RootLine& l : sh.root_lines) roots.lines.push(l);
        os_mutex_unlock(&sh.lock);
        if (!up) up = ix.open(sh.index_path, false);
        for (const Ask& a : todo) {
            Result r;
            r.id = a.q.id; r.seq = a.seq; r.first = a.q.first;
            r.generation = atomic_load(&sh.generation, MO_ACQUIRE);
            u32 want = a.q.count < kLibraryMaxWindow ? a.q.count : kLibraryMaxWindow;
            r.rows = (LibraryRow*)mem_alloc(sizeof(LibraryRow) * (want ? want : 1));
            if (up) r.count = ix.window(a.q, r.rows, &r.total, root_path_of, &roots);
            if (up && ix.damaged) { ix.close(); up = false; } // the writer will make it again; open it afresh next time
            os_mutex_lock(&sh.lock);
            sh.results.push(r);
            os_mutex_unlock(&sh.lock);
        }
        todo.clear();
    }
    todo.free();
    roots.lines.free();
    ix.close();
}

} // namespace lib

namespace {

using namespace lib;

constinit LibraryConfig g_config = {};
constinit u32 g_roots_ktable = 0;
constinit u32 g_rows_ktable = 0;

struct RootInfo {
    u32    id = 0;
    Handle row;
    char   path[kPathMax] = "";
    bool   walked = false;
};
struct Window {
    u32    id = 0;
    LibraryQuery q;
    u32    seq = 0;           // the newest ask sent for it
    bool   open = false;
    u64    shown_generation = 0;
    Array<Handle> rows;
};

struct Library {
    Shared*  sh = nullptr;
    OsThread scan, writer, reader;
    bool     threads = false;
    Array<RootInfo> roots;
    Array<Window>   windows;
    u32      next_root = 1;
    u32      seq = 0;
    LibraryScan scan_res;
    u64      said_generation = 0;
    f64      last_refill = 0.0;
    f64      rate_at = 0.0;
    u64      rate_files = 0;
    char     data_dir[kPathMax] = "";
};

Library* state(Engine& e) { return engine_module_state_as<Library>(e, "library"); }

LibraryRoot& root_row(KTable* t, const RootInfo& r) { return t->rows_of<LibraryRoot>()[r.row.index()]; }

void send_write(Library& L, WriteKind k, u32 root, const char* path = "") {
    WriteMsg* m = (WriteMsg*)mem_alloc(sizeof(WriteMsg));
    new (m) WriteMsg{};
    m->kind = k;
    m->root = root;
    format(m->path, sizeof m->path, "%s", path);
    os_mutex_lock(&L.sh->lock);
    L.sh->writes.push(m);
    os_cond_signal(&L.sh->write_wake);
    os_mutex_unlock(&L.sh->lock);
}

// A root's path as the library keeps it: absolute, '/' between names, no '/' at the end.
bool normal_path(const char* in, char* out, u32 cap) {
    char buf[kPathMax];
    if (in[0] == '/') format(buf, sizeof buf, "%s", in);
    else {
        char cwd[kPathMax];
        if (!io_current_dir(cwd, sizeof cwd)) return false;
        format(buf, sizeof buf, "%s/%s", cwd, in);
    }
    // "." and "" names out, ".." takes the one before it.
    u32 n = 0;
    out[0] = 0;
    const char* p = buf;
    while (*p) {
        while (*p == '/') ++p;
        const char* b = p;
        while (*p && *p != '/') ++p;
        u32 len = (u32)(p - b);
        if (len == 0 || (len == 1 && b[0] == '.')) continue;
        if (len == 2 && b[0] == '.' && b[1] == '.') { while (n > 0 && out[n - 1] != '/') --n; if (n > 0) --n; out[n] = 0; continue; }
        if (n + len + 2 >= cap) return false;
        out[n++] = '/';
        memcpy(out + n, b, len);
        n += len;
        out[n] = 0;
    }
    if (n == 0) { out[0] = '/'; out[1] = 0; }
    return true;
}

void add_root(Library& L, KTable* table, u32 id, const char* path, bool save) {
    RootInfo r;
    r.id = id;
    format(r.path, sizeof r.path, "%s", path);
    LibraryRoot row{};
    row.id = id;
    row.state = (u8)LibraryRootState::Walking;
    format(row.path, sizeof row.path, "%s", path);
    if (table) r.row = table->spawn(row);
    L.roots.push(r);
    if (id >= L.next_root) L.next_root = id + 1;
    os_mutex_lock(&L.sh->lock);
    Shared::RootLine line{id, ""};
    format(line.path, sizeof line.path, "%s", path);
    L.sh->root_lines.push(line);
    os_mutex_unlock(&L.sh->lock);
    send_write(L, WriteKind::AddRoot, id, path);
    if (save) send_write(L, WriteKind::SaveRoots, 0);
    os_mutex_lock(&L.sh->lock);
    Walk w{id, ""};
    format(w.path, sizeof w.path, "%s", path);
    L.sh->walks.push(w);
    os_cond_signal(&L.sh->scan_wake);
    os_mutex_unlock(&L.sh->lock);
}

void ask(Library& L, Window& w) {
    w.seq = ++L.seq;
    os_mutex_lock(&L.sh->lock);
    bool replaced = false;
    for (Ask& a : L.sh->asks) if (a.q.id == w.id) { a.q = w.q; a.seq = w.seq; replaced = true; } // the older ask is dropped unanswered
    if (!replaced) L.sh->asks.push(Ask{w.q, w.seq});
    os_cond_signal(&L.sh->read_wake);
    os_mutex_unlock(&L.sh->lock);
}

Window* window(Library& L, u32 id) {
    for (Window& w : L.windows) if (w.id == id) return &w;
    return nullptr;
}

void clear_rows(KTable* rows, Window& w) {
    for (Handle h : w.rows) if (rows) rows->destroy(h);
    w.rows.clear();
}

void commands(TickContext& ctx, Library& L, KTable* roots, KTable* rows) {
    for (const LibraryAddRoot& a : ctx.received<LibraryAddRoot>()) {
        char path[kPathMax];
        if (!normal_path(a.path, path, sizeof path)) { KV_WARN("library", "%s is not a path the library can keep", a.path); continue; }
        bool have = false;
        for (const RootInfo& r : L.roots) have |= strcmp(r.path, path) == 0;
        if (have) continue;
        add_root(L, roots, L.next_root, path, true);
    }
    for (const LibraryRemoveRoot& a : ctx.received<LibraryRemoveRoot>()) {
        for (u32 i = 0; i < L.roots.len; ++i) {
            if (L.roots[i].id != a.root) continue;
            if (roots) roots->destroy(L.roots[i].row);
            L.roots[i] = L.roots[L.roots.len - 1];
            --L.roots.len;
            os_mutex_lock(&L.sh->lock);
            for (u32 k = 0; k < L.sh->root_lines.len; ++k)
                if (L.sh->root_lines[k].id == a.root) { L.sh->root_lines[k] = L.sh->root_lines[L.sh->root_lines.len - 1]; --L.sh->root_lines.len; break; }
            L.sh->drops.push(a.root);
            os_cond_signal(&L.sh->scan_wake);
            os_mutex_unlock(&L.sh->lock);
            send_write(L, WriteKind::RemoveRoot, a.root);
            send_write(L, WriteKind::SaveRoots, 0);
            break;
        }
    }
    for (const LibraryQuery& q : ctx.received<LibraryQuery>()) {
        Window* w = window(L, q.id);
        if (!w) { L.windows.push(Window{}); w = &L.windows.last(); w->id = q.id; }
        w->q = q;
        w->open = true;
        ask(L, *w);
    }
    for (const LibraryClose& c : ctx.received<LibraryClose>()) {
        for (u32 i = 0; i < L.windows.len; ++i) {
            if (L.windows[i].id != c.id) continue;
            clear_rows(rows, L.windows[i]);
            L.windows[i].rows.free();
            L.windows[i] = L.windows[L.windows.len - 1];
            --L.windows.len;
            break;
        }
    }
}

void from_threads(TickContext& ctx, Library& L, KTable* roots, KTable* rows) {
    Array<Note> notes;
    Array<Result> results;
    os_mutex_lock(&L.sh->lock);
    for (const Note& n : L.sh->notes) notes.push(n);
    L.sh->notes.clear();
    for (const Result& r : L.sh->results) results.push(r);
    L.sh->results.clear();
    L.scan_res.rebuilt = L.sh->rebuilt;
    format(L.scan_res.dir, sizeof L.scan_res.dir, "%s", L.sh->walking_dir);
    os_mutex_unlock(&L.sh->lock);

    for (const Note& n : notes) {
        RootInfo* r = nullptr;
        for (RootInfo& x : L.roots) if (x.id == n.root) r = &x;
        if (n.kind == NoteKind::Committed) { L.scan_res.generation = n.generation; L.scan_res.tracks = n.total; continue; }
        if (!r || !roots) continue;
        LibraryRoot& row = root_row(roots, *r);
        switch (n.kind) {
        case NoteKind::Counts:
            row.tracks = n.tracks; row.dirs = n.dirs; row.failed = n.failed;
            break;
        case NoteKind::Walked:
            r->walked = true;
            row.tracks = n.tracks;
            if (row.state != (u8)LibraryRootState::Offline) row.state = (u8)LibraryRootState::Idle;
            ctx.emit(LibraryScanFinished{n.root, n.tracks, n.seconds});
            break;
        case NoteKind::Offline:
            row.state = (u8)LibraryRootState::Offline;
            format(row.reason, sizeof row.reason, "%s", n.reason);
            break;
        case NoteKind::Committed: break;
        }
    }
    for (Result& res : results) {
        Window* w = window(L, res.id);
        if (w && res.seq == w->seq && rows) {
            clear_rows(rows, *w);
            for (u32 i = 0; i < res.count; ++i) {
                Handle h = rows->spawn(res.rows[i]);
                if (!h.is_null()) w->rows.push(h);
            }
            w->shown_generation = res.generation;
            ctx.emit(LibraryResult{res.id, res.total, res.first, res.count, res.generation});
        }
        mem_free(res.rows);
    }
    notes.free();
    results.free();
}

// Phase PreSim, main thread, every tick.
void library_pump(TickContext& ctx) {
    Library& L = ctx.state_as<Library>();
    KTable* roots = ctx.write_ktable(g_roots_ktable);
    KTable* rows = ctx.write_ktable(g_rows_ktable);
    commands(ctx, L, roots, rows);
    from_threads(ctx, L, roots, rows);

    // The generation, said when it moves; open windows asked again, a few times a second.
    f64 now = time_now();
    if (L.scan_res.generation != L.said_generation) {
        L.said_generation = L.scan_res.generation;
        ctx.emit(LibraryChanged{L.scan_res.generation});
    }
    if (now - L.last_refill >= kRefillSeconds) {
        for (Window& w : L.windows)
            if (w.open && w.shown_generation != L.scan_res.generation && w.seq) { ask(L, w); w.shown_generation = L.scan_res.generation; }
        L.last_refill = now;
    }

    LibraryScan& s = L.scan_res;
    s.walking = atomic_load(&L.sh->walking, MO_RELAXED);
    s.dirs_listed = atomic_load(&L.sh->dirs_listed, MO_RELAXED);
    s.files_found = atomic_load(&L.sh->files_found, MO_RELAXED);
    bool walking = false;
    for (const RootInfo& r : L.roots) walking |= !r.walked;
    s.active = walking ? 1 : 0;
    if (now - L.rate_at >= 1.0) {
        s.names_per_second = L.rate_at > 0.0 ? (f64)(s.files_found - L.rate_files) / (now - L.rate_at) : 0.0;
        L.rate_at = now;
        L.rate_files = s.files_found;
    }
}

// The list of roots from roots.txt: an id, a tab, a path, a line.
void load_roots(Library& L, KTable* table) {
    IoBytes b;
    if (!io_read_now(L.sh->roots_path, &b)) return;
    const char* p = (const char*)b.data;
    const char* end = p + b.size;
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', (usize)(end - p));
        if (!nl) nl = end;
        const char* tab = (const char*)memchr(p, '\t', (usize)(nl - p));
        if (tab) {
            u32 id = (u32)strtoul(p, nullptr, 10);
            char path[kPathMax];
            format(path, sizeof path, "%.*s", (int)(nl - tab - 1), tab + 1);
            if (id && path[0] == '/') add_root(L, table, id, path, false);
        }
        p = nl + 1;
    }
    b.free();
}

bool library_startup(Engine& e, ModuleInstance& self) {
    Library* L = arena_push<Library>(self.arena);
    self.set_state(L);
    if (g_config.data_dir && g_config.data_dir[0]) format(L->data_dir, sizeof L->data_dir, "%s", g_config.data_dir);
    else {
        const char* pref = platform_pref_path(e, "blackwing", "blackwing");
        if (!pref[0]) { KV_ERROR("library", "there is no per-user data directory to keep the index in"); return false; }
        format(L->data_dir, sizeof L->data_dir, "%slibrary", pref);
    }
    if (!io_make_dirs(L->data_dir)) { KV_ERROR("library", "%s cannot be made", L->data_dir); return false; }
    if (sqlite3_initialize() != SQLITE_OK) { KV_ERROR("library", "sqlite will not start"); return false; }
    L->sh = (Shared*)mem_alloc(sizeof(Shared));
    new (L->sh) Shared();
    Shared& sh = *L->sh;
    os_mutex_init(&sh.lock);
    os_cond_init(&sh.scan_wake); os_cond_init(&sh.write_wake); os_cond_init(&sh.read_wake);
    format(sh.index_path, sizeof sh.index_path, "%s/index.sqlite", L->data_dir);
    format(sh.roots_path, sizeof sh.roots_path, "%s/roots.txt", L->data_dir);
    if (!os_thread_start(&L->writer, write_thread, &sh) || !os_thread_start(&L->reader, read_thread, &sh) ||
        !os_thread_start(&L->scan, scan_thread, &sh)) {
        KV_ERROR("library", "the library's threads will not start");
        return false; // shutdown joins what started
    }
    L->threads = true;
    // Every root is walked at start-up: things change while the program is not running.
    load_roots(*L, engine_ktable(e, g_roots_ktable));
    return engine_publish(e, self, ".scan", &L->scan_res);
}

void library_shutdown(Engine& e, ModuleInstance&) {
    Library* L = state(e);
    if (!L || !L->sh) return;
    Shared& sh = *L->sh;
    os_mutex_lock(&sh.lock);
    sh.quit = true;
    os_cond_broadcast(&sh.scan_wake); os_cond_broadcast(&sh.write_wake); os_cond_broadcast(&sh.read_wake);
    os_mutex_unlock(&sh.lock);
    if (L->scan.handle) os_thread_join(&L->scan);
    if (L->writer.handle) os_thread_join(&L->writer);
    if (L->reader.handle) os_thread_join(&L->reader);
    for (WriteMsg* m : sh.writes) { if (m->batch) { m->batch->free(); mem_free(m->batch); } mem_free(m); }
    for (Result& r : sh.results) mem_free(r.rows);
    sh.walks.free(); sh.drops.free(); sh.writes.free(); sh.asks.free(); sh.notes.free(); sh.results.free(); sh.root_lines.free();
    os_cond_destroy(&sh.scan_wake); os_cond_destroy(&sh.write_wake); os_cond_destroy(&sh.read_wake);
    os_mutex_destroy(&sh.lock);
    mem_free(L->sh);
    L->sh = nullptr;
    for (Window& w : L->windows) w.rows.free();
    L->windows.free();
    L->roots.free();
    sqlite3_shutdown();
}

// ---- the command line -----------------------------------------------------------------------

void cmd_add(CliContext& ctx, Span<const char* const> args) {
    if (args.len == 0) { ctx.fail("library add <directory>..."); return; }
    for (const char* a : args) {
        LibraryAddRoot r{};
        if (strlen(a) >= sizeof r.path) { ctx.fail("the path is too long: %s", a); return; }
        format(r.path, sizeof r.path, "%s", a);
        ctx.engine->events.emit(r);
    }
}

void cmd_remove(CliContext& ctx, Span<const char* const> args) {
    if (args.len != 1) { ctx.fail("library remove <root>"); return; }
    ctx.engine->events.emit(LibraryRemoveRoot{(u32)strtoul(args[0], nullptr, 10)});
}

void cmd_status(CliContext& ctx, Span<const char* const>) {
    Library* L = state(*ctx.engine);
    if (!L) { ctx.fail("the library is not running"); return; }
    const LibraryScan& s = L->scan_res;
    ctx.print("index: %llu tracks, generation %llu%s; %s", (unsigned long long)s.tracks, (unsigned long long)s.generation,
              s.rebuilt ? ", made again at start-up" : "", s.active ? "walking" : "idle");
    if (s.active) ctx.print("walking root %u: %s (%.0f files a second)", s.walking, s.dir, s.names_per_second);
    KTable* t = engine_ktable(*ctx.engine, g_roots_ktable);
    for (const RootInfo& r : L->roots) {
        const LibraryRoot& row = t->rows_of<LibraryRoot>()[r.row.index()];
        ctx.print("%3u %-8s %6u tracks %5u dirs%s %s%s%s", row.id, library_root_state_name((LibraryRootState)row.state), row.tracks, row.dirs,
                  row.failed ? " (some unreadable)" : "", row.path, row.reason[0] ? ": " : "", row.reason);
    }
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "library add",    .help = "<directory>... adds directories to the library, and walks them", .fn = cmd_add},
    {.name = "library remove", .help = "<root> takes a root out of the library", .fn = cmd_remove},
    {.name = "library status", .help = "the roots and the scan", .fn = cmd_status},
};

constinit const FieldDesc k_root_fields[] = {KV_FIELDS(LibraryRoot, id, state, dirs, tracks, failed, path, reason)};
constinit const FieldDesc k_row_fields[] = {KV_FIELDS(LibraryRow, query, index, id, kind, missing, offline, root, track_number, disc_number, size, frames, rate, name, title, artist), // sixteen at most a list
                                            KV_FIELDS(LibraryRow, album, path, reason)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(LibraryRoot, k_root_fields, CompCosmetic), KV_COMPONENT(LibraryRow, k_row_fields, CompCosmetic)};
constinit const KTableDesc k_ktables[] = {
    {.name = ".roots", .components = "LibraryRoot", .capacity = 256, .id = &g_roots_ktable, .cosmetic = true},
    {.name = ".rows", .components = "LibraryRow", .capacity = kLibraryMaxRows, .id = &g_rows_ktable, .cosmetic = true},
};
constinit const FieldDesc k_f_add[] = {KV_FIELDS(LibraryAddRoot, path)};
constinit const FieldDesc k_f_remove[] = {KV_FIELDS(LibraryRemoveRoot, root)};
constinit const FieldDesc k_f_query[] = {KV_FIELDS(LibraryQuery, id, view, parent, text, first, count)};
constinit const FieldDesc k_f_close[] = {KV_FIELDS(LibraryClose, id)};
constinit const FieldDesc k_f_result[] = {KV_FIELDS(LibraryResult, id, total, first, count, generation)};
constinit const FieldDesc k_f_changed[] = {KV_FIELDS(LibraryChanged, generation)};
constinit const FieldDesc k_f_finished[] = {KV_FIELDS(LibraryScanFinished, root, tracks, seconds)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(LibraryAddRoot, k_f_add, 0),
    KV_EVENT_EX(LibraryRemoveRoot, k_f_remove, 0),
    KV_EVENT_EX(LibraryQuery, k_f_query, 0),
    KV_EVENT_EX(LibraryClose, k_f_close, 0),
    KV_EVENT_EX(LibraryResult, k_f_result, 0),
    KV_EVENT_EX(LibraryChanged, k_f_changed, 0),
    KV_EVENT_EX(LibraryScanFinished, k_f_finished, 0),
};
constinit const ResourceDesc k_resources[] = {{".scan"}};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = library_pump,
     .reads = "LibraryAddRoot, LibraryRemoveRoot, LibraryQuery, LibraryClose",
     .writes = ".roots, .rows, .scan",
     .emits = "LibraryResult, LibraryChanged, LibraryScanFinished",
     .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(blackwing_library_module) = {
    .name       = "library",
    .events     = k_events,
    .components = k_components,
    .ktables    = k_ktables,
    .resources  = k_resources,
    .systems    = k_systems,
    .commands   = k_commands,
    .startup    = library_startup,
    .shutdown   = library_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

void library_configure(const LibraryConfig& config) { g_config = config; }

const LibraryScan* library_scan(Engine& e) { Library* L = state(e); return L ? &L->scan_res : nullptr; }

const char* library_root_state_name(LibraryRootState s) {
    switch (s) {
    case LibraryRootState::Walking: return "walking";
    case LibraryRootState::Reading: return "reading";
    case LibraryRootState::Idle:    return "idle";
    case LibraryRootState::Offline: return "offline";
    case LibraryRootState::Count:   break;
    }
    return "?";
}

} // namespace kv
