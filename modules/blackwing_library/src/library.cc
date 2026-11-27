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
#include "korvid/blackwing_library/schema.h"
#include "lib.h"
#include "overrides.h"
#include "korvid/core/atomic.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/paths.h"
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
constexpr u32 kCollectPerTick = 64;  // LibraryCollected events said in one tick

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
    if (!io_write_file(tmp, text.data ? text.data : "", text.len) || !io_rename(tmp, path))
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
    atomic_store(&sh.slot_state, 2u, MO_RELEASE);
    os_cond_signal(&sh.tag_wake);
    os_mutex_unlock(&sh.lock);
}

const char* root_path_locked(Shared& sh, u32 id, char* out, u32 cap) {
    out[0] = 0;
    os_mutex_lock(&sh.lock);
    for (const Shared::RootLine& r : sh.root_lines) if (r.id == id) format(out, cap, "%s", r.path);
    os_mutex_unlock(&sh.lock);
    return out[0] ? out : nullptr;
}

// An override file that will not parse: set aside, and the main thread told, which says
// so in LibraryOverridesSetAside and library.scan.
void set_aside(Shared& sh, u32 root, const char* path, const char* problem) {
    LibraryOverridesSetAside* m = (LibraryOverridesSetAside*)mem_alloc(sizeof(LibraryOverridesSetAside));
    new (m) LibraryOverridesSetAside{};
    m->root = root;
    format(m->path, sizeof m->path, "%s", path);
    format(m->reason, sizeof m->reason, "%s", problem);
    override_set_aside(path, problem, m->aside, sizeof m->aside);
    os_mutex_lock(&sh.lock);
    sh.set_asides.push(m);
    os_mutex_unlock(&sh.lock);
    tell_main(sh);
}

// A directory's overrides taken in: its tracks merged again with them, and the file's time
// noted, so it is not taken in again until it changes. One that will not parse is set aside.
void take_in_overrides(Shared& sh, Index& ix, u32 root, const char* rel_dir, const char* path, i64 mtime) {
    OverrideFile f;
    char problem[160];
    if (!override_load(path, &f, problem, sizeof problem)) { set_aside(sh, root, path, problem); f.clear(); }
    Array<u64> ids;
    ix.tracks_in_dir(root, rel_dir, &ids);
    for (u64 id : ids) {
        Index::TrackAt at;
        if (!ix.track_info(id, &at)) continue;
        const char* name = strrchr(at.rel, '/');
        TagValue ov[64];
        u32 n = override_values(f, name ? name + 1 : at.rel, ov, 64);
        ix.remerge(id, Span<const TagValue>(ov, n));
    }
    ix.set_override_seen(path, mtime);
    ids.free();
    f.free();
    atomic_add_fetch(&sh.overrides_epoch, 1u, MO_RELEASE);
}

// A root's overrides, at start-up or when it is added: every file that changed since it was
// last taken in (edited by hand while the program was not running, or never seen).
void scan_overrides(Shared& sh, Index& ix, u32 root, const char* root_path) {
    char base[kPathMax];
    format(base, sizeof base, "%s/overrides/%s", sh.data_dir, root_path[0] == '/' ? root_path + 1 : root_path);
    if (!io_is_dir(base)) return;
    struct Seen { Shared* sh; Index* ix; u32 root; const char* base; };
    Seen w{&sh, &ix, root, base};
    io_list_files(base, [](void* user, const char* rel) {
        Seen& x = *(Seen*)user;
        const char* name = strrchr(rel, '/');
        if (strcmp(name ? name + 1 : rel, "overrides.txt") != 0) return true;
        char path[kPathMax], dir[kPathMax];
        format(path, sizeof path, "%s/%s", x.base, rel);
        format(dir, sizeof dir, "%.*s", name ? (int)(name - rel) : 0, rel);
        IoStat st;
        if (!io_stat_file(path, &st)) return true;
        if (x.ix->override_seen(path) != st.mtime_ns) take_in_overrides(*x.sh, *x.ix, x.root, dir, path, st.mtime_ns);
        return true;
    }, &w);
}

// A track moved from one place in its root to another: its overrides go with it, from one
// directory's file to the other's.
void move_overrides(Shared& sh, u32 root, const char* from, const char* to) {
    char rp[kPathMax];
    if (!root_path_locked(sh, root, rp, sizeof rp)) return;
    auto split = [](const char* rel, char* dir, u32 cap) { const char* s = strrchr(rel, '/'); format(dir, cap, "%.*s", s ? (int)(s - rel) : 0, rel); return s ? s + 1 : rel; };
    char fd[kPathMax], td[kPathMax], fp[kPathMax], tp[kPathMax], problem[160];
    const char* fname = split(from, fd, sizeof fd);
    const char* tname = split(to, td, sizeof td);
    override_path(fp, sizeof fp, sh.data_dir, rp, fd);
    override_path(tp, sizeof tp, sh.data_dir, rp, td);
    OverrideFile a;
    if (!override_load(fp, &a, problem, sizeof problem) || !a.entries.len) { a.free(); return; }
    TagValue v[64];
    u32 n = override_values(a, fname, v, 64);
    if (!n) { a.free(); return; }
    OverrideFile b;
    if (strcmp(fp, tp) != 0 && !override_load(tp, &b, problem, sizeof problem)) { set_aside(sh, root, tp, problem); b.clear(); }
    OverrideFile& dst = strcmp(fp, tp) == 0 ? a : b;
    // Copied before the source's lines go (they point into it).
    Strings keep;
    u32 at[64];
    for (u32 i = 0; i < n; ++i) at[i] = keep.add(v[i].text, v[i].len);
    for (u32 i = 0; i < n; ++i) override_set(a, fname, v[i].field, "", true);
    bool first[(u32)TagField::Count];
    for (bool& f : first) f = true;
    for (u32 i = 0; i < n; ++i) {
        // A field given twice is two values: the first replaces, the others join it.
        if (first[(u32)v[i].field]) { override_set(dst, tname, v[i].field, keep.at(at[i]), false); first[(u32)v[i].field] = false; }
        else dst.entries.push(OverrideFile::Entry{dst.s.add(tname), (u8)v[i].field, dst.s.add(keep.at(at[i]))});
    }
    override_save(fp, a);
    if (&dst != &a) override_save(tp, dst);
    atomic_add_fetch(&sh.overrides_epoch, 1u, MO_RELEASE);
    a.free(); b.free(); keep.free();
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
    struct WalkNo { u32 root, walk; };
    Array<WalkNo> walk_no;          // each root's walk under way, or its last
    auto walk_of = [&](u32 root) -> u32 {
        for (const WalkNo& x : walk_no) if (x.root == root) return x.walk;
        u32 n = up ? ix.current_walk(root) : 0;
        walk_no.push(WalkNo{root, n});
        return n;
    };
    if (up) ix.forget_missing(30); // a month missing is forgotten
    Array<char> moves;
    for (;;) {
        // At the end, what was already given is still done (the list of roots saved), then it stops.
        os_mutex_lock(&sh.lock);
        while (!sh.quit && sh.writes.len == 0) os_cond_wait(&sh.write_wake, &sh.lock);
        if (sh.quit && sh.writes.len == 0) { os_mutex_unlock(&sh.lock); break; }
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
                if (up) { ix.put_root(m->root, m->path); scan_overrides(sh, ix, m->root, m->path); }
                break;
            case WriteKind::Override: {
                // The user's: into the directory's file (written whole), then the track merged again.
                Index::TrackAt at;
                char rp[kPathMax];
                if (!up || !ix.track_info(m->track, &at) || !root_path_locked(sh, at.root, rp, sizeof rp)) {
                    KV_WARN("library", "an override for track %llu, which the library does not have", (unsigned long long)m->track);
                    break;
                }
                const char* slash = strrchr(at.rel, '/');
                char dir[kPathMax], path[kPathMax];
                format(dir, sizeof dir, "%.*s", slash ? (int)(slash - at.rel) : 0, at.rel);
                override_path(path, sizeof path, sh.data_dir, rp, dir);
                OverrideFile f;
                char problem[160];
                if (!override_load(path, &f, problem, sizeof problem)) { set_aside(sh, at.root, path, problem); f.clear(); }
                const char* name = slash ? slash + 1 : at.rel;
                override_set(f, name, (TagField)m->field, m->value, m->clear);
                if (!override_save(path, f)) KV_ERROR("library", "the override could not be saved to %s", path);
                IoStat st;
                ix.set_override_seen(path, io_stat_file(path, &st) ? st.mtime_ns : -2);
                atomic_add_fetch(&sh.overrides_epoch, 1u, MO_RELEASE);
                TagValue ov[64];
                u32 n = override_values(f, name, ov, 64);
                ix.remerge(m->track, Span<const TagValue>(ov, n));
                touch(at.root);
                f.free();
                break;
            }
            case WriteKind::RemoveRoot:
                for (u32 i = 0; i < live.len; ++i) if (live[i] == m->root) { live[i] = live[live.len - 1]; --live.len; break; }
                if (up) ix.remove_root(m->root);
                break;
            case WriteKind::SaveRoots: save = true; break;
            case WriteKind::WalkStart: {
                u32 n = up ? ix.next_walk(m->root) : 0;
                bool set = false;
                for (WalkNo& x : walk_no) if (x.root == m->root) { x.walk = n; set = true; }
                if (!set) walk_no.push(WalkNo{m->root, n});
                Note s{NoteKind::WalkStarted};
                s.root = m->root;
                notes.push(s);
                break;
            }
            case WriteKind::Walked: {
                if (!up || !is_live(m->root)) break;
                Note n{NoteKind::Walked};
                n.root = m->root;
                n.seconds = m->seconds;
                u32 had = ix.count_tracks(m->root);
                if (m->root_failed) {
                    // Gone, or cannot be listed: offline; nothing marked missing.
                    Note o{NoteKind::Offline};
                    o.root = m->root;
                    format(o.reason, sizeof o.reason, "%s", m->value);
                    notes.push(o);
                } else if (m->files == 0 && had > 0) {
                    // Empty where it held tracks: an unmounted disk leaves its mount point so. Offline,
                    // and nothing under it forgotten.
                    Note o{NoteKind::Offline};
                    o.root = m->root;
                    u64 was = ix.root_volume(m->root);
                    if (was && m->volume && was != m->volume) format(o.reason, sizeof o.reason, "the disk it was on is not there: its directory is empty, on another disk");
                    else format(o.reason, sizeof o.reason, "it is empty, where it held %u tracks", had);
                    notes.push(o);
                } else {
                    // What the walk did not see is missing; what it found that is one of those, moved.
                    moves.clear();
                    ix.after_walk(m->root, walk_of(m->root), &moves);
                    for (u32 at = 0; at < moves.len;) {
                        const char* from = moves.data + at;
                        const char* to = from + strlen(from) + 1;
                        move_overrides(sh, m->root, from, to);
                        at += (u32)(strlen(from) + strlen(to) + 2);
                    }
                    if (m->volume) ix.set_root_volume(m->root, m->volume);
                }
                walked.push(n);
                break;
            }
            case WriteKind::TakeIn: {
                // An override file the watcher saw change: whose, and taken in if it is new to us.
                if (!up) break;
                char base[kPathMax];
                format(base, sizeof base, "%s/overrides/", sh.data_dir);
                usize bn = strlen(base);
                if (strncmp(m->path, base, bn) != 0) break;
                const char* under = m->path + bn; // "home/me/Music/Artist/Album/overrides.txt"
                os_mutex_lock(&sh.lock);
                u32 root = 0;
                char rel_dir[kPathMax] = "";
                for (const Shared::RootLine& r : sh.root_lines) {
                    const char* rp = r.path[0] == '/' ? r.path + 1 : r.path;
                    usize n = strlen(rp);
                    if (strncmp(under, rp, n) == 0 && under[n] == '/') {
                        root = r.id;
                        const char* rest = under + n + 1;
                        const char* last = strrchr(rest, '/');
                        format(rel_dir, sizeof rel_dir, "%.*s", last ? (int)(last - rest) : 0, rest);
                    }
                }
                os_mutex_unlock(&sh.lock);
                IoStat st;
                if (root && io_stat_file(m->path, &st) && ix.override_seen(m->path) != st.mtime_ns) {
                    take_in_overrides(sh, ix, root, rel_dir, m->path, st.mtime_ns);
                    touch(root);
                }
                break;
            }
            case WriteKind::Batch: {
                Batch& b = *m->batch;
                // What went first: a rename is a removal and an addition, and the addition is
                // known for the track moved by finding it missing.
                if (up) for (const GoneMsg& g : b.gone) {
                    if (!is_live(g.root)) continue;
                    if (g.dir) ix.mark_missing_under(g.root, b.s.at(g.path));
                    else ix.mark_missing(g.root, b.s.at(g.path));
                    touch(g.root);
                }
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
                        if (!dir_path || dir_root != t.root || strcmp(dp, dir_path) != 0) {
                            dir_id = ix.find_dir(t.root, dp);
                            if (!dir_id) dir_id = ix.ensure_dir(t.root, dp); // a watcher's file in a directory not yet a row
                            dir_root = t.root; dir_path = dp;
                        }
                        TrackRec r;
                        r.root = t.root; r.dir = dp; r.path = b.s.at(t.path); r.name = b.s.at(t.name); r.shown = b.s.at(t.shown);
                        r.size = t.size; r.mtime = t.mtime;
                        r.title = b.s.at(t.title); r.artist = b.s.at(t.artist); r.album = b.s.at(t.album);
                        r.track = t.track; r.disc = t.disc;
                        bool fresh, changed, unread;
                        char moved[kPathMax];
                        u64 id = ix.put_track(r, dir_id, walk_of(t.root), &fresh, &changed, &unread, moved, sizeof moved);
                        if (moved[0]) move_overrides(sh, t.root, moved, r.path);
                        if (id && unread) {
                            // A track in the slot's directory goes with the slot's; the rest in walk order.
                            if (slot.on && slot.dir && slot.dir == dir_id) slot_more.add(id, t.root, r.path, t.size, t.mtime);
                            else jobs.add(id, t.root, r.path, t.size, t.mtime);
                        }
                    }
                }
                for (const DirMsg& d : b.dirs) touch(d.root);
                for (const TrackMsg& t : b.tracks) touch(t.root);
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
                atomic_store(&sh.slot_state, 2u, MO_RELEASE);
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
        tell_main(sh);
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
    walk_no.free(); moves.free();
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

char* keep_text(const char* s) {
    usize n = strlen(s);
    char* out = (char*)mem_alloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}
void free_ask(Ask& a) {
    for (u32 i = 0; i < a.npaths; ++i) mem_free(a.paths[i].path);
    if (a.paths) mem_free(a.paths);
    a.paths = nullptr;
    a.npaths = 0;
}
void free_collected(Collected* c) {
    for (char* p : c->paths) mem_free(p);
    c->paths.free();
    mem_free(c);
}

// A window over paths, answered: each path's root is the root whose path it begins with
// (the longest, should roots nest), and its row the track there. A path under no root, or
// with no track at it, is a row all the same: the file's name, and why.
void answer_paths(Index& ix, const Ask& a, const RootPaths& roots, Result& r) {
    r.total = a.npaths;
    for (u32 i = 0; i < a.npaths; ++i) {
        const char* path = a.paths[i].path;
        LibraryRow& row = r.rows[r.count++];
        const Shared::RootLine* best = nullptr;
        usize best_len = 0;
        for (const Shared::RootLine& l : roots.lines) {
            usize n = strlen(l.path);
            if (n >= best_len && strncmp(path, l.path, n) == 0 && path[n] == '/') { best = &l; best_len = n; }
        }
        bool have = best && ix.row_at(best->id, path + best_len + 1, a.q.id, &row, root_path_of, (void*)&roots);
        if (!have) {
            row = LibraryRow{};
            row.query = a.q.id;
            row.kind = (u8)LibraryRowKind::Track;
            row.root = best ? best->id : 0;
            const char* name = path;
            for (const char* p = path; *p; ++p) if (*p == '/' && p[1]) name = p + 1;
            format(row.name, sizeof row.name, "%s", name);
            format(row.path, sizeof row.path, "%s", path);
            format(row.reason, sizeof row.reason, "%s", best ? "the library has no track at this path" : "not under a root of the library");
        }
        row.index = a.paths[i].index;
    }
}

// A LibraryCollect, answered with the windows any face would open: every window of the
// view, a kLibraryMaxWindow at a time, until its total is reached.
struct Collector {
    Index* ix; const RootPaths* roots; u64 generation; Collected* out; LibraryRow* rows; u32 depth = 0;
    bool full() const { return out->paths.len >= kLibraryMaxCollect; }
    void add(const LibraryRow& r) {
        if (r.kind == (u8)LibraryRowKind::Track && r.path[0] && !r.missing && !full()) out->paths.push(keep_text(r.path));
    }
    // Every row of a view, to fn; directories and groups as well as tracks.
    template <class F> void each(LibraryView view, u64 parent, const char* text, LibrarySort sort, F fn) {
        for (u32 first = 0; !full();) {
            LibraryQuery q;
            q.id = 0; q.view = view; q.sort = sort; q.parent = parent; q.first = first; q.count = kLibraryMaxWindow;
            format(q.text, sizeof q.text, "%s", text);
            u32 total = 0;
            u32 n = ix->window(q, generation, rows, &total, root_path_of, (void*)roots);
            // A window's rows are read into the one buffer: taken before the next overwrites them.
            for (u32 i = 0; i < n; ++i) fn(rows[i]);
            first += n;
            if (n == 0 || first >= total) break;
        }
    }
    void tracks(LibraryView view, u64 parent, const char* text, LibrarySort sort) { each(view, parent, text, sort, [&](const LibraryRow& r) { add(r); }); }
    // A directory: its directories in turn, each with everything under it, then its own
    // tracks, as the Folders view shows them.
    void directory(u64 dir) {
        if (++depth > 256) { --depth; return; }
        Array<u64> dirs;
        Array<char*> own;
        each(LibraryView::Folders, dir, "", LibrarySort::Default, [&](const LibraryRow& r) {
            if (r.kind == (u8)LibraryRowKind::Directory) dirs.push(r.id);
            else if (r.kind == (u8)LibraryRowKind::Track && r.path[0] && !r.missing) own.push(keep_text(r.path));
        });
        for (u64 d : dirs) directory(d);
        for (char* p : own) { if (full()) mem_free(p); else out->paths.push(p); }
        dirs.free();
        own.free();
        --depth;
    }
};

void answer_collect(Index& ix, const LibraryCollect& c, const RootPaths& roots, u64 generation, Collected* out) {
    LibraryRow* rows = (LibraryRow*)mem_alloc(sizeof(LibraryRow) * kLibraryMaxWindow);
    Collector k{&ix, &roots, generation, out, rows};
    if (c.text[0]) {
        k.tracks(LibraryView::Search, 0, c.text, c.sort);
    } else switch ((LibraryRowKind)c.kind) {
    case LibraryRowKind::Directory: k.directory(c.row); break;
    case LibraryRowKind::Album:     k.tracks(LibraryView::AlbumTracks, c.row, "", LibrarySort::Default); break;
    case LibraryRowKind::Genre:     k.tracks(LibraryView::Tracks, c.row, "", c.sort); break;
    case LibraryRowKind::Artist: {
        Array<u64> albums;
        k.each(LibraryView::Albums, c.row, "", LibrarySort::Default, [&](const LibraryRow& r) { if (r.kind == (u8)LibraryRowKind::Album) albums.push(r.id); });
        for (u64 a : albums) k.tracks(LibraryView::AlbumTracks, a, "", LibrarySort::Default);
        albums.free();
    } break;
    case LibraryRowKind::Track: {
        Index::TrackAt at;
        if (ix.track_info(c.row, &at)) {
            char path[kPathMax + 4200];
            format(path, sizeof path, "%s/%s", root_path_of((void*)&roots, at.root), at.rel);
            if (strlen(path) < kLibraryPathMax) out->paths.push(keep_text(path));
        }
    } break;
    default: break;
    }
    mem_free(rows);
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
    Array<LibraryCollect> collects;
    for (;;) {
        os_mutex_lock(&sh.lock);
        while (!sh.quit && sh.asks.len == 0 && sh.collects.len == 0) os_cond_wait(&sh.read_wake, &sh.lock);
        if (sh.quit) { os_mutex_unlock(&sh.lock); break; }
        for (const Ask& a : sh.asks) todo.push(a);
        sh.asks.clear();
        for (const LibraryCollect& c : sh.collects) collects.push(c);
        sh.collects.clear();
        roots.lines.clear();
        for (const Shared::RootLine& l : sh.root_lines) roots.lines.push(l);
        os_mutex_unlock(&sh.lock);
        if (!up) up = ix.open(sh.index_path, false);
        for (Ask& a : todo) {
            Result r;
            r.id = a.q.id; r.seq = a.seq; r.first = a.q.first;
            r.generation = atomic_load(&sh.generation, MO_ACQUIRE);
            bool by_path = a.q.view == LibraryView::Paths;
            u32 want = by_path ? a.npaths : a.q.count < kLibraryMaxWindow ? a.q.count : kLibraryMaxWindow;
            if (by_path) r.first = 0;
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
            else if (up && by_path) answer_paths(ix, a, roots, r);
            else if (up) {
                ix.stop_when(stop_check, &check);
                r.count = ix.window(a.q, r.generation, r.rows, &r.total, root_path_of, &roots);
                ix.stop_when(nullptr, nullptr);
                if (stop_check(&check)) { r.stopped = true; atomic_add_fetch(&sh.cancelled, (u64)1, MO_RELAXED); }
            }
            r.seconds = time_now() - t0;
            for (u32 i = 0; i < r.count; ++i)
                for (const Shared::RootLine& l : roots.lines) if (l.id == r.rows[i].root && l.offline) r.rows[i].offline = 1;
            if (up && ix.damaged) { ix.close(); up = false; } // the writer will make it again; open it afresh next time
            os_mutex_lock(&sh.lock);
            sh.reading_window = 0;
            sh.results.push(r);
            os_mutex_unlock(&sh.lock);
            tell_main(sh);
            free_ask(a);
        }
        todo.clear();
        for (const LibraryCollect& c : collects) {
            Collected* out = (Collected*)mem_alloc(sizeof(Collected));
            new (out) Collected{};
            out->id = c.id;
            if (up) answer_collect(ix, c, roots, atomic_load(&sh.generation, MO_ACQUIRE), out);
            if (up && ix.damaged) { ix.close(); up = false; }
            os_mutex_lock(&sh.lock);
            sh.collected.push(out);
            os_mutex_unlock(&sh.lock);
            tell_main(sh);
        }
        collects.clear();
    }
    for (Ask& a : todo) free_ask(a);
    todo.free();
    collects.free();
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
    u32    answered = 0;      // the newest ask a result has come for
    Array<PathAsk> paths;     // a window over paths: its paths, each from mem_alloc
    bool   dirty = false;     // its paths changed this tick: asked once when all are in
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
    OsThread tagger, watcher;
    Array<Reading> reading;
    Array<ReadAsk> waiting;       // asked, not yet in the queue (it was full)
    u32      collects_out = 0;    // LibraryCollects sent to the reader and not yet answered
    Array<Collected*> sending;    // answered: their paths said a few dozen a tick, in order
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
    // Its walk starts once the watcher is watching it (the watch thread asks for it), so
    // nothing that changes after the walk has passed a directory goes unheard.
    os_mutex_lock(&L.sh->lock);
    os_cond_signal(&L.sh->watch_wake);
    os_mutex_unlock(&L.sh->lock);
}

void ask(Library& L, Window& w) {
    w.seq = ++L.seq;
    Ask fresh{w.q, w.seq};
    if (w.q.view == LibraryView::Paths && w.paths.len) {
        // The reader gets a copy: the window's own may change before it has answered.
        fresh.npaths = w.paths.len;
        fresh.paths = (PathAsk*)mem_alloc(sizeof(PathAsk) * w.paths.len);
        for (u32 i = 0; i < w.paths.len; ++i) fresh.paths[i] = PathAsk{w.paths[i].index, keep_text(w.paths[i].path)};
    }
    os_mutex_lock(&L.sh->lock);
    if (L.sh->reading_window == w.id) atomic_add_fetch(&L.sh->stop_epoch, 1u, MO_RELAXED); // the one being answered stops
    bool replaced = false;
    for (Ask& a : L.sh->asks) if (a.q.id == w.id) { free_ask(a); a = fresh; replaced = true; } // the older ask is dropped unanswered
    if (!replaced) L.sh->asks.push(fresh);
    os_cond_signal(&L.sh->read_wake);
    os_mutex_unlock(&L.sh->lock);
}

Window* window(Library& L, u32 id) {
    for (Window& w : L.windows) if (w.id == id) return &w;
    return nullptr;
}

void free_paths(Window& w) {
    for (PathAsk& p : w.paths) mem_free(p.path);
    w.paths.free();
    w.paths = Array<PathAsk>{};
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
        if (q.view == LibraryView::Paths) continue; // a window over paths is made of LibraryPath, not asked for
        free_paths(*w);
        w->q = q;
        w->open = true;
        ask(L, *w);
    }
    for (const LibraryPath& p : ctx.received<LibraryPath>()) {
        Window* w = window(L, p.id);
        if (!w) { L.windows.push(Window{}); w = &L.windows.last(); w->id = p.id; }
        if (!w->open || w->q.view != LibraryView::Paths) {
            free_paths(*w);
            w->q = LibraryQuery{};
            w->q.id = p.id;
            w->q.view = LibraryView::Paths;
            w->open = true;
        }
        u32 at = 0;
        while (at < w->paths.len && w->paths[at].index != p.index) ++at;
        if (at < w->paths.len) { mem_free(w->paths[at].path); w->paths[at] = w->paths[w->paths.len - 1]; --w->paths.len; }
        char path[kPathMax];
        if (p.path[0] && normal_path(p.path, path, sizeof path)) {
            if (w->paths.len < kLibraryMaxWindow) w->paths.push(PathAsk{p.index, keep_text(path)});
            else KV_WARN("library", "window %u has %u paths already: no more are taken", p.id, kLibraryMaxWindow);
        }
        w->dirty = true;
    }
    for (Window& w : L.windows) if (w.dirty) { w.dirty = false; ask(L, w); }
    for (const LibraryCollect& c : ctx.received<LibraryCollect>()) {
        os_mutex_lock(&L.sh->lock);
        L.sh->collects.push(c);
        os_cond_signal(&L.sh->read_wake);
        os_mutex_unlock(&L.sh->lock);
        ++L.collects_out;
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
        atomic_store(&L.sh->slot_state, m->kind != SlotKind::None ? 1u : 0u, MO_RELEASE);
    }
    for (const LibraryOverride& o : ctx.received<LibraryOverride>()) {
        TagField f;
        if (!tag_field_from_name(o.field, &f)) { KV_WARN("library", "an override of '%s', which is not a field", o.field); continue; }
        WriteMsg* w = (WriteMsg*)mem_alloc(sizeof(WriteMsg));
        new (w) WriteMsg{};
        w->kind = WriteKind::Override;
        w->track = o.track;
        w->field = (u8)f;
        w->clear = o.clear != 0;
        format(w->value, sizeof w->value, "%s", o.value);
        os_mutex_lock(&L.sh->lock);
        L.sh->writes.push(w);
        os_cond_signal(&L.sh->write_wake);
        os_mutex_unlock(&L.sh->lock);
    }
    for (const LibraryClose& c : ctx.received<LibraryClose>()) {
        for (u32 i = 0; i < L.windows.len; ++i) {
            if (L.windows[i].id != c.id) continue;
            clear_rows(rows, L.windows[i]);
            L.windows[i].rows.free();
            free_paths(L.windows[i]);
            L.windows[i] = L.windows[L.windows.len - 1];
            --L.windows.len;
            break;
        }
    }
}

void set_offline(Library& L, u32 root, bool offline) {
    os_mutex_lock(&L.sh->lock);
    for (Shared::RootLine& l : L.sh->root_lines) if (l.id == root) l.offline = offline;
    os_cond_signal(&L.sh->watch_wake);
    os_mutex_unlock(&L.sh->lock);
}

void from_threads(TickContext& ctx, Library& L, KTable* roots, KTable* rows) {
    Array<Note> notes;
    Array<Result> results;
    os_mutex_lock(&L.sh->lock);
    for (const Note& n : L.sh->notes) notes.push(n);
    L.sh->notes.clear();
    for (const Result& r : L.sh->results) results.push(r);
    L.sh->results.clear();
    for (Collected* c : L.sh->collected) { L.sending.push(c); if (L.collects_out) --L.collects_out; }
    L.sh->collected.clear();
    Array<LibraryOverridesSetAside*> asides;
    for (LibraryOverridesSetAside* m : L.sh->set_asides) asides.push(m);
    L.sh->set_asides.clear();
    L.scan_res.rebuilt = L.sh->rebuilt;
    format(L.scan_res.dir, sizeof L.scan_res.dir, "%s", L.sh->walking_dir);
    os_mutex_unlock(&L.sh->lock);

    for (LibraryOverridesSetAside* m : asides) {
        ctx.emit(*m);
        ++L.scan_res.set_aside;
        mem_free(m);
    }
    asides.free();

    for (const Note& n : notes) {
        RootInfo* r = nullptr;
        for (RootInfo& x : L.roots) if (x.id == n.root) r = &x;
        if (n.kind == NoteKind::Committed) { L.scan_res.generation = n.generation; L.scan_res.tracks = n.total; continue; }
        if (!r || !roots) continue;
        LibraryRoot& row = root_row(roots, *r);
        switch (n.kind) {
        case NoteKind::Counts:
            row.tracks = n.tracks; row.dirs = n.dirs; row.failed = n.failed; row.unread = n.unread;
            os_mutex_lock(&L.sh->lock);
            for (Shared::RootLine& l : L.sh->root_lines) if (l.id == r->id) l.tracks = n.tracks;
            os_mutex_unlock(&L.sh->lock);
            break;
        case NoteKind::Walked:
            r->walked = true;
            r->walk_seconds = n.seconds;
            row.tracks = n.tracks; row.unread = n.unread;
            break;
        case NoteKind::Offline:
            row.state = (u8)LibraryRootState::Offline;
            format(row.reason, sizeof row.reason, "%s", n.reason);
            set_offline(L, r->id, true);
            break;
        case NoteKind::WalkStarted:
            // Walked again (at start-up, back from offline, events lost): its state from the walk.
            r->walked = false;
            r->finished = false;
            row.state = (u8)LibraryRootState::Walking;
            row.reason[0] = 0;
            set_offline(L, r->id, false);
            break;
        case NoteKind::Committed: break;
        }
    }
    // A root's state from its counts: walking until its walk is in; reading while tracks
    // want reading; then idle, and the scan's end said once.
    for (RootInfo& r : L.roots) {
        if (!roots) break;
        LibraryRoot& row = root_row(roots, r);
        if (row.state == (u8)LibraryRootState::Offline) {
            if (r.walked && !r.finished && !atomic_load(&L.sh->tags_active, MO_RELAXED)) { r.finished = true; ctx.emit(LibraryScanFinished{r.id, row.tracks, r.walk_seconds}); }
            continue;
        }
        if (!r.walked) row.state = (u8)LibraryRootState::Walking;
        else if (row.unread) row.state = (u8)LibraryRootState::Reading;
        else {
            row.state = (u8)LibraryRootState::Idle;
            if (!r.finished) { r.finished = true; ctx.emit(LibraryScanFinished{r.id, row.tracks, r.walk_seconds}); }
        }
    }
    for (Result& res : results) {
        Window* w = window(L, res.id);
        if (w && res.seq == w->seq) w->answered = res.seq;
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
        IoCompletion c;
        if (io && !io->take(r.request, &c)) { ++i; continue; } // still on its way
        ReadDone d;
        d.job = r.ask.job; d.gen = r.ask.gen; d.at = r.ask.at;
        if (io) {
            d.ok = c.status == IoStatus::Done;
            d.bytes = c.bytes; d.err = c.error;
            atomic_add_fetch(&L.sh->bytes_read, (u64)c.bytes.size, MO_RELAXED);
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
            rq.owner = &L; // the library's own list: drained at shutdown
            Handle h = io ? io->read_file(rq) : Handle{};
            if (h.is_null()) {
                if (io) break; // the queue is full: next tick
                ReadDone d; d.job = a.job; d.gen = a.gen; d.at = a.at; d.err = Err::Io; done.push(d); // no queue at all
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

    // What was collected, said in order, the oldest ask's first: a few dozen paths a tick,
    // so that ten thousand of them are not one tick's work for whoever takes them.
    for (u32 said = 0; L.sending.len && said < kCollectPerTick;) {
        Collected* c = L.sending[0];
        if (c->paths.len == 0) {
            LibraryCollected none{};
            none.id = c->id;
            ctx.emit(none);
            ++said;
        }
        for (; c->sent < c->paths.len && said < kCollectPerTick; ++c->sent, ++said) {
            LibraryCollected e{};
            e.id = c->id; e.index = c->sent; e.total = c->paths.len;
            format(e.path, sizeof e.path, "%s", c->paths[c->sent]);
            ctx.emit(e);
        }
        if (c->sent < c->paths.len) break;
        free_collected(c);
        for (u32 i = 1; i < L.sending.len; ++i) L.sending[i - 1] = L.sending[i];
        --L.sending.len;
    }

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
    s.bytes_read = atomic_load(&L.sh->bytes_read, MO_RELAXED);
    bool busy = false;
    u64 unread = 0;
    for (const RootInfo& r : L.roots) {
        busy |= !r.finished;
        if (roots) unread += root_row(roots, r).unread;
    }
    s.unread = unread;
    s.active = busy || atomic_load(&L.sh->tags_active, MO_RELAXED) ? 1 : 0;
    s.slot = atomic_load(&L.sh->slot_state, MO_ACQUIRE) != 0; // until the tagger has read the last of what was put in it
    // The next tick at the clock's rate (engine_want_ticks) while a scan runs, a read is
    // out, or a window has been asked for and not answered: an answer waits for a tick.
    bool owed = s.active || L.reading.len || L.waiting.len || L.collects_out || L.sending.len;
    for (const Window& w : L.windows) owed = owed || (w.open && w.seq != w.answered);
    if (owed) engine_want_ticks(*ctx.engine);
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
    if (!io_read_file(L.sh->roots_path, &b)) return;
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

// Before slice 5 the index was in the data directory, beside the roots. It is moved to
// the cache once, with its WAL; if it cannot be, it is left and a new one is built from
// the roots, which costs a scan and nothing else.
void move_old_index(const char* data_dir, const char* index_path) {
    char from[kPathMax], to[kPathMax];
    format(from, sizeof from, "%s/index.sqlite", data_dir);
    if (strcmp(from, index_path) == 0) return;
    IoStat st;
    if (!io_stat_file(from, &st) || io_stat_file(index_path, &st)) return;
    static const char* const kParts[] = {"", "-wal", "-shm"};
    for (const char* part : kParts) {
        format(from, sizeof from, "%s/index.sqlite%s", data_dir, part);
        format(to, sizeof to, "%s%s", index_path, part);
        if (!io_stat_file(from, &st)) continue;
        i32 err = 0;
        if (!io_rename(from, to, &err)) {
            KV_WARN("library", "the index at %s could not be moved to %s (%s): it is built again", from, to, os_error_text(err));
            return;
        }
    }
    KV_INFO("library", "the index moved to %s", index_path);
}

bool library_startup(Engine& e, ModuleInstance& self) {
    Library* L = arena_push<Library>(self.arena);
    self.set_state(L);
    // The roots and the overrides are the user's, in the data directory; the index can be
    // made again from them, so it is a cache. A data directory named in the config holds
    // both (the tests, `--data`).
    char cache_dir[kPathMax];
    if (g_config.data_dir && g_config.data_dir[0]) {
        format(L->data_dir, sizeof L->data_dir, "%s", g_config.data_dir);
        format(cache_dir, sizeof cache_dir, "%s", g_config.data_dir);
    } else {
        const char* pref = platform_user_dir(e, UserDir::Data, "blackwing", "blackwing");
        if (!pref[0]) { KV_ERROR("library", "there is no per-user data directory to keep the roots in"); return false; }
        format(L->data_dir, sizeof L->data_dir, "%slibrary", pref);
        const char* cache = platform_user_dir(e, UserDir::Cache, "blackwing", "blackwing");
        if (cache[0]) format(cache_dir, sizeof cache_dir, "%slibrary", cache);
        else format(cache_dir, sizeof cache_dir, "%s", L->data_dir);
    }
    if (!io_make_dirs(L->data_dir)) { KV_ERROR("library", "%s cannot be made", L->data_dir); return false; }
    if (!io_make_dirs(cache_dir)) { KV_ERROR("library", "%s cannot be made", cache_dir); return false; }
    if (sqlite3_initialize() != SQLITE_OK) { KV_ERROR("library", "sqlite will not start"); return false; }
    L->sh = (Shared*)mem_alloc(sizeof(Shared));
    new (L->sh) Shared();
    Shared& sh = *L->sh;
    sh.engine = &e;
    os_mutex_init(&sh.lock);
    os_cond_init(&sh.scan_wake); os_cond_init(&sh.write_wake); os_cond_init(&sh.read_wake); os_cond_init(&sh.tag_wake); os_cond_init(&sh.watch_wake);
    sh.quiet_seconds = g_config.quiet_seconds;
    sh.offline_seconds = g_config.offline_seconds;
    sh.poll_seconds = g_config.poll_seconds;
    sh.poll_roots = g_config.poll_roots;
    format(sh.index_path, sizeof sh.index_path, "%s/index.sqlite", cache_dir);
    move_old_index(L->data_dir, sh.index_path);
    format(sh.data_dir, sizeof sh.data_dir, "%s", L->data_dir);
    format(sh.roots_path, sizeof sh.roots_path, "%s/roots.txt", L->data_dir);
    if (!os_thread_start(&L->writer, write_thread, &sh) || !os_thread_start(&L->reader, read_thread, &sh) ||
        !os_thread_start(&L->scan, scan_thread, &sh) || !os_thread_start(&L->tagger, tag_thread, &sh) ||
        !os_thread_start(&L->watcher, watch_thread, &sh)) {
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
    os_cond_broadcast(&sh.watch_wake);
    os_mutex_unlock(&sh.lock);
    if (L->watcher.handle) os_thread_join(&L->watcher);
    if (L->tagger.handle) os_thread_join(&L->tagger);
    // Reads still in the queue: cancelled and waited for, and their bytes freed if they came anyway.
    if (IoQueue* io = platform_io(e)) io->drain(L);
    L->reading.free();
    L->waiting.free();
    if (L->scan.handle) os_thread_join(&L->scan);
    if (L->writer.handle) os_thread_join(&L->writer);
    if (L->reader.handle) os_thread_join(&L->reader);
    for (WriteMsg* m : sh.writes) { if (m->batch) { m->batch->free(); mem_free(m->batch); } mem_free(m); }
    for (Result& r : sh.results) mem_free(r.rows);
    for (Ask& a : sh.asks) free_ask(a);
    for (Collected* c : sh.collected) free_collected(c);
    for (Collected* c : L->sending) free_collected(c);
    sh.collects.free(); sh.collected.free(); L->sending.free();
    for (LibraryOverridesSetAside* m : sh.set_asides) mem_free(m);
    sh.set_asides.free();
    for (Window& w : L->windows) free_paths(w);
    for (WriteMsg* m : sh.writes) { if (m->tags) { m->tags->free(); mem_free(m->tags); } if (m->slot) { m->slot->tracks.free(); mem_free(m->slot); } }
    for (TagJobs* b : sh.jobs) { b->free(); mem_free(b); }
    if (sh.slot_jobs) { sh.slot_jobs->free(); mem_free(sh.slot_jobs); }
    sh.walks.free(); sh.drops.free(); sh.writes.free(); sh.asks.free(); sh.notes.free(); sh.results.free(); sh.root_lines.free();
    sh.jobs.free(); sh.read_asks.free(); sh.read_dones.free();
    os_cond_destroy(&sh.scan_wake); os_cond_destroy(&sh.write_wake); os_cond_destroy(&sh.read_wake); os_cond_destroy(&sh.tag_wake);
    os_cond_destroy(&sh.watch_wake);
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

constinit const KTableDesc k_ktables[] = {
    {.name = ".roots", .components = "LibraryRoot", .capacity = 256, .id = &g_roots_ktable, .cosmetic = true},
    {.name = ".rows", .components = "LibraryRow", .capacity = kLibraryMaxRows, .id = &g_rows_ktable, .cosmetic = true},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = library_pump,
     .reads = "LibraryAddRoot, LibraryRemoveRoot, LibraryQuery, LibraryClose, LibraryPrioritize, LibraryOverride, LibraryPath, LibraryCollect",
     .writes = ".roots, .rows, .scan",
     .emits = "LibraryResult, LibraryChanged, LibraryScanFinished, LibraryCollected, LibraryOverridesSetAside",
     .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(blackwing_library_module) = {
    .name       = "library",
    .events     = k_library_events,
    .components = k_library_components,
    .ktables    = k_ktables,
    .resources  = k_library_resources,
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
