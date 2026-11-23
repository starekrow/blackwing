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
constexpr u32 kMaxReads      = 48;   // the tagger's reads in the I/O queue at once
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

// Tracks that want reading, gathered by a commit for the tagger.
struct Jobs {
    TagJobs* b = nullptr;
    void add(u64 id, u32 root, const char* rel, u64 size, i64 mtime) {
        if (!b) { b = (TagJobs*)mem_alloc(sizeof(TagJobs)); new (b) TagJobs{}; }
        b->jobs.push(TagJob{id, root, b->s.add(rel), size, mtime});
    }
    static void cb(void* user, u64 id, u32 root, const char* rel, u64 size, i64 mtime) { ((Jobs*)user)->add(id, root, rel, size, mtime); }
};

// The slot, as the writer keeps it: a directory, by id or (not yet listed) by its path.
struct Slot {
    u64  dir = 0;
    u32  root = 0;
    char rel[kPathMax] = "";
    bool on = false;
};

// Which root an OS path is under, and its path within it.
bool under_root(Shared& sh, const char* path, u32* root, char* rel, u32 cap) {
    bool found = false;
    os_mutex_lock(&sh.lock);
    for (const Shared::RootLine& r : sh.root_lines) {
        usize n = strlen(r.path);
        if (strncmp(path, r.path, n) == 0 && (path[n] == 0 || path[n] == '/')) {
            *root = r.id;
            format(rel, cap, "%s", path[n] ? path + n + 1 : "");
            found = true;
            break;
        }
    }
    os_mutex_unlock(&sh.lock);
    return found;
}

// Fills the slot: the tracks that want reading, first; a directory not listed, listed next.
void resolve_slot(Shared& sh, Index& ix, const SlotMsg& m, Slot& slot) {
    Jobs jobs;
    slot = Slot{};
    u32 root = 0;
    char rel[kPathMax] = "";
    bool is_dir = false;
    if (m.kind == SlotKind::Path && under_root(sh, m.path, &root, rel, sizeof rel)) {
        if (u64 t = ix.track_at(root, rel)) ix.unread_track(t, Jobs::cb, &jobs);
        else is_dir = true;
    } else if (m.kind == SlotKind::Dir) {
        is_dir = ix.dir_of(m.dir, &root, rel, sizeof rel);
    } else if (m.kind == SlotKind::Tracks) {
        for (u64 id : m.tracks) ix.unread_track(id, Jobs::cb, &jobs);
    }
    if (is_dir) {
        slot.on = true;
        slot.root = root;
        format(slot.rel, sizeof slot.rel, "%s", rel);
        slot.dir = ix.find_dir(root, rel);
        if (slot.dir) ix.unread_in(slot.dir, Jobs::cb, &jobs);
        if (!slot.dir || ix.dir_state(slot.dir) == 0) {
            os_mutex_lock(&sh.lock);
            sh.walk_next_root = root; // listed now, if the walk has not come to it
            format(sh.walk_next, sizeof sh.walk_next, "%s", rel);
            os_cond_signal(&sh.scan_wake);
            os_mutex_unlock(&sh.lock);
        }
    }
    if (!jobs.b) { jobs.b = (TagJobs*)mem_alloc(sizeof(TagJobs)); new (jobs.b) TagJobs{}; } // an empty slot empties it
    os_mutex_lock(&sh.lock);
    if (sh.slot_jobs) { sh.slot_jobs->free(); mem_free(sh.slot_jobs); }
    sh.slot_jobs = jobs.b;
    os_cond_signal(&sh.tag_wake);
    os_mutex_unlock(&sh.lock);
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
    Slot slot;
    Array<const char*> texts;
    Array<u8> fields, sources;
    Array<Note> notes;
    f64 last_commit = 0.0;
    for (;;) {
        os_mutex_lock(&sh.lock);
        while (!sh.quit && sh.writes.len == 0) os_cond_wait(&sh.write_wake, &sh.lock);
        if (sh.quit) { os_mutex_unlock(&sh.lock); break; }
        // Commits at most four times a second, each carrying what came meanwhile; a slot
        // goes at once, since someone is looking.
        for (;;) {
            bool slot_waiting = false;
            for (WriteMsg* m : sh.writes) slot_waiting |= m->kind == WriteKind::Slot;
            f64 left = kCommitSeconds - (time_now() - last_commit);
            if (slot_waiting || left <= 0.0 || sh.quit) break;
            os_cond_timed_wait(&sh.write_wake, &sh.lock, left);
        }
        // The walk's rows first, in a commit of their own, so a file is findable by name as
        // soon as it can be; the tags' after.
        for (WriteMsg* m : sh.writes) if (m->kind != WriteKind::Tags) work.push(m);
        u32 names_end = work.len;
        for (WriteMsg* m : sh.writes) if (m->kind == WriteKind::Tags) work.push(m);
        sh.writes.clear();
        os_mutex_unlock(&sh.lock);

        auto is_live = [&](u32 r) { for (u32 x : live) if (x == r) return true; return false; };
        auto touch = [&](u32 r) { for (u32 x : touched) if (x == r) return; touched.push(r); };
        Jobs jobs, slot_more;
        if (up) ix.begin();
        bool save = false;
        for (u32 k = 0; k < work.len; ++k) {
            WriteMsg* m = work[k];
            if (k == names_end && k > 0 && up) { ix.commit(); ix.begin(); atomic_add_fetch(&sh.generation, (u64)1, MO_RELEASE); }
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
                if (up) {
                    for (const DirMsg& d : b.dirs) {
                        if (!is_live(d.root)) continue;
                        DirRec r{d.root, b.s.at(d.path), b.s.at(d.parent), b.s.at(d.name), d.state, b.s.at(d.reason)};
                        u64 id = ix.put_dir(r);
                        if (slot.on && !slot.dir && slot.root == d.root && strcmp(slot.rel, r.path) == 0) slot.dir = id;
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
                        bool fresh, changed, unread;
                        u64 id = ix.put_track(r, dir_id, &fresh, &changed, &unread);
                        if (id && unread) {
                            // A track in the slot's directory goes with the slot's; the rest in walk order.
                            if (slot.on && slot.dir && slot.dir == dir_id) slot_more.add(id, t.root, r.path, t.size, t.mtime);
                            else jobs.add(id, t.root, r.path, t.size, t.mtime);
                        }
                    }
                }
                for (const DirMsg& d : b.dirs) touch(d.root);
                b.free();
                mem_free(m->batch);
                break;
            }
            case WriteKind::Tags: {
                TagBatch& b = *m->tags;
                for (const TagRec& r : b.recs) {
                    if (!is_live(r.root)) continue;
                    texts.clear(); fields.clear(); sources.clear();
                    for (u32 i = 0; i < r.nvals; ++i) {
                        const TagVal& v = b.vals[r.vals + i];
                        texts.push(b.s.at(v.text)); fields.push(v.field); sources.push(v.source);
                    }
                    Index::Tags t;
                    t.track = r.track; t.size = r.size; t.mtime = r.mtime;
                    t.rate = r.rate; t.channels = r.channels; t.bits = r.bits; t.frames = r.frames; t.estimated = r.estimated != 0;
                    t.reason = b.s.at(r.reason);
                    t.title = b.s.at(r.title); t.artist = b.s.at(r.artist); t.album = b.s.at(r.album); t.albumartist = b.s.at(r.albumartist);
                    t.date = b.s.at(r.date); t.genre = b.s.at(r.genre); t.trackno = r.trackno; t.discno = r.discno;
                    t.shown = b.s.at(r.shown);
                    t.titlesort = b.s.at(r.titlesort); t.artistsort = b.s.at(r.artistsort);
                    t.albumsort = b.s.at(r.albumsort); t.albumartistsort = b.s.at(r.albumartistsort);
                    t.nvals = r.nvals; t.fields = fields.data; t.sources = sources.data; t.texts = texts.data;
                    if (up) ix.put_tags(t);
                    touch(r.root);
                }
                b.free();
                mem_free(m->tags);
                break;
            }
            case WriteKind::Slot:
                if (up) resolve_slot(sh, ix, *m->slot, slot);
                m->slot->tracks.free();
                mem_free(m->slot);
                break;
            }
            mem_free(m);
        }
        work.clear();
        if (up && !ix.commit()) KV_ERROR("library", "the index could not commit: %s", ix.error);
        last_commit = time_now();
        if (up && ix.damaged) {
            // sqlite says the file is damaged: made again, and every root walked again into it.
            KV_WARN("library", "the index is damaged (%s) and is being made again", ix.error);
            ix.close();
            Index::remove_files(sh.index_path);
            up = open_index(sh, ix);
            if (jobs.b) { jobs.b->free(); mem_free(jobs.b); jobs.b = nullptr; }
            if (slot_more.b) { slot_more.b->free(); mem_free(slot_more.b); slot_more.b = nullptr; }
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

        // What the commit made readable goes to the tagger: the walk's, and the slot's more.
        if (jobs.b || slot_more.b) {
            os_mutex_lock(&sh.lock);
            if (jobs.b) sh.jobs.push(jobs.b);
            if (slot_more.b) {
                if (sh.slot_jobs) { // not yet taken: these join it
                    for (const TagJob& j : slot_more.b->jobs) sh.slot_jobs->jobs.push(TagJob{j.track, j.root, sh.slot_jobs->s.add(slot_more.b->s.at(j.rel)), j.size, j.mtime});
                    slot_more.b->free(); mem_free(slot_more.b);
                } else {
                    sh.slot_jobs = slot_more.b;
                }
            }
            os_cond_signal(&sh.tag_wake);
            os_mutex_unlock(&sh.lock);
        }

        // What changed, for the main thread: counted first, then handed over under the lock.
        for (Note& n : walked) touch(n.root);
        u64 total = up ? ix.count_all() : 0;
        for (u32 root : touched) {
            Note n{NoteKind::Counts};
            n.root = root;
            if (up) { n.tracks = ix.count_tracks(root); n.dirs = ix.count_dirs(root, &n.failed); n.unread = ix.count_unread(root); }
            n.generation = gen; n.total = total;
            notes.push(n);
        }
        for (Note& n : walked) {
            if (up) { n.tracks = ix.count_tracks(n.root); n.unread = ix.count_unread(n.root); }
            n.generation = gen; n.total = total;
            notes.push(n);
        }
        Note c{NoteKind::Committed};
        c.generation = gen; c.total = total;
        notes.push(c);
        os_mutex_lock(&sh.lock);
        for (const Note& n : notes) sh.notes.push(n);
        os_cond_signal(&sh.read_wake); // open windows may want filling again
        os_mutex_unlock(&sh.lock);
        notes.clear();
        touched.clear();
        walked.clear();
    }
    for (WriteMsg* m : work) {
        if (m->batch) { m->batch->free(); mem_free(m->batch); }
        if (m->tags) { m->tags->free(); mem_free(m->tags); }
        if (m->slot) { m->slot->tracks.free(); mem_free(m->slot); }
        mem_free(m);
    }
    work.free(); live.free(); touched.free(); walked.free(); texts.free(); fields.free(); sources.free(); notes.free();
    ix.close();
}

// ---- the reader ------------------------------------------------------------------------------

struct RootPaths { Array<Shared::RootLine> lines; };
const char* root_path_of(void* user, u32 id) {
    RootPaths& r = *(RootPaths*)user;
    for (const Shared::RootLine& l : r.lines) if (l.id == id) return l.path;
    return "";
}

// sqlite asks this as it works: a newer ask for the window being answered stops it.
struct StopCheck { Shared* sh; u32 epoch; };
int stop_check(void* user) {
    StopCheck& c = *(StopCheck*)user;
    return atomic_load(&c.sh->stop_epoch, MO_RELAXED) != c.epoch ? 1 : 0;
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
            // Which window this is, so a newer ask for it can stop it in sqlite.
            os_mutex_lock(&sh.lock);
            bool newer = false;
            for (const Ask& b : sh.asks) newer |= b.q.id == a.q.id;
            sh.reading_window = a.q.id;
            StopCheck check{&sh, atomic_load(&sh.stop_epoch, MO_RELAXED)};
            os_mutex_unlock(&sh.lock);
            f64 t0 = time_now();
            if (newer) r.stopped = true; // already asked again: not worth starting
            else if (up) {
                ix.stop_when(stop_check, &check);
                r.count = ix.window(a.q, r.generation, r.rows, &r.total, root_path_of, &roots);
                ix.stop_when(nullptr, nullptr);
                if (stop_check(&check)) { r.stopped = true; atomic_add_fetch(&sh.cancelled, (u64)1, MO_RELAXED); }
            }
            r.seconds = time_now() - t0;
            if (up && ix.damaged) { ix.close(); up = false; } // the writer will make it again; open it afresh next time
            os_mutex_lock(&sh.lock);
            sh.reading_window = 0;
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
    bool   finished = false;   // LibraryScanFinished said, since the last walk
    f64    walk_seconds = 0.0;
};
// A read of the tagger's in the I/O queue.
struct Reading { Handle request; ReadAsk ask; };
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
    u64      rate_files = 0, rate_tags = 0;
    char     data_dir[kPathMax] = "";
    OsThread tagger;
    Array<Reading> reading;
    Array<ReadAsk> waiting;       // asked, not yet in the queue (it was full)
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
    if (L.sh->reading_window == w.id) atomic_add_fetch(&L.sh->stop_epoch, 1u, MO_RELAXED); // the one being answered stops
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
    for (const LibraryPrioritize& p : ctx.received<LibraryPrioritize>()) {
        SlotMsg* m = (SlotMsg*)mem_alloc(sizeof(SlotMsg));
        new (m) SlotMsg{};
        if (p.path[0]) { m->kind = SlotKind::Path; normal_path(p.path, m->path, sizeof m->path); }
        else if (p.dir) { m->kind = SlotKind::Dir; m->dir = p.dir; }
        else if (p.query) {
            // An open window's tracks, as its rows are now.
            m->kind = SlotKind::Tracks;
            if (Window* w = window(L, p.query))
                for (Handle h : w->rows) if (rows && rows->valid(h)) { const LibraryRow& r = rows->rows_of<LibraryRow>()[h.index()]; if (r.kind == (u8)LibraryRowKind::Track) m->tracks.push(r.id); }
        } else {
            m->kind = SlotKind::Tracks;
            for (u32 i = 0; i < p.count && i < 32; ++i) m->tracks.push(p.tracks[i]);
        }
        WriteMsg* w = (WriteMsg*)mem_alloc(sizeof(WriteMsg));
        new (w) WriteMsg{};
        w->kind = WriteKind::Slot;
        w->slot = m;
        os_mutex_lock(&L.sh->lock);
        L.sh->writes.push(w);
        os_cond_signal(&L.sh->write_wake);
        os_mutex_unlock(&L.sh->lock);
        L.scan_res.slot = m->kind != SlotKind::None ? 1 : 0;
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
            row.tracks = n.tracks; row.dirs = n.dirs; row.failed = n.failed; row.unread = n.unread;
            break;
        case NoteKind::Walked:
            r->walked = true;
            r->walk_seconds = n.seconds;
            row.tracks = n.tracks; row.unread = n.unread;
            break;
        case NoteKind::Offline:
            row.state = (u8)LibraryRootState::Offline;
            format(row.reason, sizeof row.reason, "%s", n.reason);
            break;
        case NoteKind::Committed: break;
        }
    }
    // A root's state from its counts: walking until its walk is in; reading while tracks
    // want reading; then idle, and the scan's end said once.
    for (RootInfo& r : L.roots) {
        if (!roots) break;
        LibraryRoot& row = root_row(roots, r);
        if (row.state == (u8)LibraryRootState::Offline) { if (r.walked && !r.finished) { r.finished = true; ctx.emit(LibraryScanFinished{r.id, row.tracks, r.walk_seconds}); } continue; }
        if (!r.walked) row.state = (u8)LibraryRootState::Walking;
        else if (row.unread) row.state = (u8)LibraryRootState::Reading;
        else {
            row.state = (u8)LibraryRootState::Idle;
            if (!r.finished) { r.finished = true; ctx.emit(LibraryScanFinished{r.id, row.tracks, r.walk_seconds}); }
        }
    }
    for (Result& res : results) {
        Window* w = window(L, res.id);
        if (w && res.seq == w->seq && rows && !res.stopped) {
            clear_rows(rows, *w);
            for (u32 i = 0; i < res.count; ++i) {
                Handle h = rows->spawn(res.rows[i]);
                if (!h.is_null()) w->rows.push(h);
            }
            w->shown_generation = res.generation;
            ctx.emit(LibraryResult{res.id, res.total, res.first, res.count, res.generation, res.seconds});
        }
        mem_free(res.rows);
    }
    notes.free();
    results.free();
}

// The tagger's reads, through the engine's I/O queue: below the player's (Low), or at Normal
// for the slot's; at most kMaxReads at once, within the queue's own budget.
void io_pump(TickContext& ctx, Library& L) {
    IoQueue* io = platform_io(*ctx.engine);
    os_mutex_lock(&L.sh->lock);
    for (const ReadAsk& a : L.sh->read_asks) L.waiting.push(a);
    L.sh->read_asks.clear();
    os_mutex_unlock(&L.sh->lock);
    Array<ReadDone> done;
    // What has come.
    for (u32 i = 0; i < L.reading.len;) {
        Reading& r = L.reading[i];
        const IoCompletion* c = nullptr;
        if (io) for (const IoCompletion& x : io->completions()) if (x.request == r.request) { c = &x; break; }
        if (!c && io) { ++i; continue; }
        ReadDone d;
        d.job = r.ask.job; d.gen = r.ask.gen; d.at = r.ask.at;
        if (c) {
            d.ok = c->status == IoStatus::Done;
            d.data = c->data; d.size = c->size; d.err = c->error;
            io->reap(r.request);
        }
        done.push(d);
        L.reading[i] = L.reading[L.reading.len - 1];
        --L.reading.len;
    }
    // What can go: the slot's first.
    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < L.waiting.len && L.reading.len < kMaxReads;) {
            const ReadAsk& a = L.waiting[i];
            if ((pass == 0) != a.slot) { ++i; continue; }
            IoRequest rq;
            rq.path = a.path;
            rq.priority = a.slot ? IoPriority::Normal : IoPriority::Low;
            rq.offset = a.at;
            rq.length = (usize)a.len;
            Handle h = io ? io->read_file(rq) : Handle{};
            if (h.is_null()) {
                if (io) break; // the queue is full: next tick
                ReadDone d; d.job = a.job; d.gen = a.gen; d.at = a.at; d.err = 5; done.push(d); // no queue at all
            } else {
                L.reading.push(Reading{h, a});
            }
            L.waiting[i] = L.waiting[L.waiting.len - 1];
            --L.waiting.len;
        }
    }
    if (done.len) {
        os_mutex_lock(&L.sh->lock);
        for (const ReadDone& d : done) L.sh->read_dones.push(d);
        os_cond_signal(&L.sh->tag_wake);
        os_mutex_unlock(&L.sh->lock);
    }
    done.free();
}

// Phase PreSim, main thread, every tick.
void library_pump(TickContext& ctx) {
    Library& L = ctx.state_as<Library>();
    KTable* roots = ctx.write_ktable(g_roots_ktable);
    KTable* rows = ctx.write_ktable(g_rows_ktable);
    commands(ctx, L, roots, rows);
    io_pump(ctx, L);
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
    s.tags_read = atomic_load(&L.sh->tags_read, MO_RELAXED);
    s.cancelled = atomic_load(&L.sh->cancelled, MO_RELAXED);
    bool busy = false;
    u64 unread = 0;
    for (const RootInfo& r : L.roots) {
        busy |= !r.finished;
        if (roots) unread += root_row(roots, r).unread;
    }
    s.unread = unread;
    s.active = busy || atomic_load(&L.sh->tags_active, MO_RELAXED) ? 1 : 0;
    if (now - L.rate_at >= 1.0) {
        s.names_per_second = L.rate_at > 0.0 ? (f64)(s.files_found - L.rate_files) / (now - L.rate_at) : 0.0;
        s.tags_per_second = L.rate_at > 0.0 ? (f64)(s.tags_read - L.rate_tags) / (now - L.rate_at) : 0.0;
        L.rate_at = now;
        L.rate_files = s.files_found;
        L.rate_tags = s.tags_read;
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
    os_cond_init(&sh.scan_wake); os_cond_init(&sh.write_wake); os_cond_init(&sh.read_wake); os_cond_init(&sh.tag_wake);
    format(sh.index_path, sizeof sh.index_path, "%s/index.sqlite", L->data_dir);
    format(sh.roots_path, sizeof sh.roots_path, "%s/roots.txt", L->data_dir);
    if (!os_thread_start(&L->writer, write_thread, &sh) || !os_thread_start(&L->reader, read_thread, &sh) ||
        !os_thread_start(&L->scan, scan_thread, &sh) || !os_thread_start(&L->tagger, tag_thread, &sh)) {
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
    os_cond_broadcast(&sh.scan_wake); os_cond_broadcast(&sh.write_wake); os_cond_broadcast(&sh.read_wake); os_cond_broadcast(&sh.tag_wake);
    os_mutex_unlock(&sh.lock);
    if (L->tagger.handle) os_thread_join(&L->tagger);
    // Reads still in the queue: cancelled, and their bytes freed if they came anyway.
    if (IoQueue* io = platform_io(e)) {
        for (Reading& r : L->reading) {
            io->cancel(r.request);
            for (const IoCompletion& c : io->completions())
                if (c.request == r.request) { if (c.data) free_in(mem_general(), c.data, c.size ? c.size : 1); io->reap(r.request); break; }
        }
    }
    L->reading.free();
    L->waiting.free();
    if (L->scan.handle) os_thread_join(&L->scan);
    if (L->writer.handle) os_thread_join(&L->writer);
    if (L->reader.handle) os_thread_join(&L->reader);
    for (WriteMsg* m : sh.writes) { if (m->batch) { m->batch->free(); mem_free(m->batch); } mem_free(m); }
    for (Result& r : sh.results) mem_free(r.rows);
    for (WriteMsg* m : sh.writes) { if (m->tags) { m->tags->free(); mem_free(m->tags); } if (m->slot) { m->slot->tracks.free(); mem_free(m->slot); } }
    for (TagJobs* b : sh.jobs) { b->free(); mem_free(b); }
    if (sh.slot_jobs) { sh.slot_jobs->free(); mem_free(sh.slot_jobs); }
    sh.walks.free(); sh.drops.free(); sh.writes.free(); sh.asks.free(); sh.notes.free(); sh.results.free(); sh.root_lines.free();
    sh.jobs.free(); sh.read_asks.free(); sh.read_dones.free();
    os_cond_destroy(&sh.scan_wake); os_cond_destroy(&sh.write_wake); os_cond_destroy(&sh.read_wake); os_cond_destroy(&sh.tag_wake);
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
    if (s.walking) ctx.print("walking root %u: %s (%.0f files a second)", s.walking, s.dir, s.names_per_second);
    if (s.unread) ctx.print("reading tags: %llu to go (%.0f a second)", (unsigned long long)s.unread, s.tags_per_second);
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

constinit const FieldDesc k_root_fields[] = {KV_FIELDS(LibraryRoot, id, state, dirs, tracks, failed, unread, path, reason)};
constinit const FieldDesc k_row_fields[] = {KV_FIELDS(LibraryRow, query, index, id, kind, missing, offline, root, track_number, disc_number, size, frames, rate, name, title, artist), // sixteen at most a list
                                            KV_FIELDS(LibraryRow, album, path, reason, count, albums, date)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(LibraryRoot, k_root_fields, CompCosmetic), KV_COMPONENT(LibraryRow, k_row_fields, CompCosmetic)};
constinit const KTableDesc k_ktables[] = {
    {.name = ".roots", .components = "LibraryRoot", .capacity = 256, .id = &g_roots_ktable, .cosmetic = true},
    {.name = ".rows", .components = "LibraryRow", .capacity = kLibraryMaxRows, .id = &g_rows_ktable, .cosmetic = true},
};
constinit const FieldDesc k_f_add[] = {KV_FIELDS(LibraryAddRoot, path)};
constinit const FieldDesc k_f_remove[] = {KV_FIELDS(LibraryRemoveRoot, root)};
constinit const FieldDesc k_f_query[] = {KV_FIELDS(LibraryQuery, id, view, sort, parent, text, first, count)};
constinit const FieldDesc k_f_close[] = {KV_FIELDS(LibraryClose, id)};
constinit const FieldDesc k_f_prio[] = {KV_FIELDS(LibraryPrioritize, path, dir, query, count, tracks)};
constinit const FieldDesc k_f_result[] = {KV_FIELDS(LibraryResult, id, total, first, count, generation, seconds)};
constinit const FieldDesc k_f_changed[] = {KV_FIELDS(LibraryChanged, generation)};
constinit const FieldDesc k_f_finished[] = {KV_FIELDS(LibraryScanFinished, root, tracks, seconds)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(LibraryAddRoot, k_f_add, 0),
    KV_EVENT_EX(LibraryRemoveRoot, k_f_remove, 0),
    KV_EVENT_EX(LibraryQuery, k_f_query, 0),
    KV_EVENT_EX(LibraryClose, k_f_close, 0),
    KV_EVENT_EX(LibraryPrioritize, k_f_prio, 0),
    KV_EVENT_EX(LibraryResult, k_f_result, 0),
    KV_EVENT_EX(LibraryChanged, k_f_changed, 0),
    KV_EVENT_EX(LibraryScanFinished, k_f_finished, 0),
};
constinit const ResourceDesc k_resources[] = {{".scan"}};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = library_pump,
     .reads = "LibraryAddRoot, LibraryRemoveRoot, LibraryQuery, LibraryClose, LibraryPrioritize",
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
