// The names pass: each root walked a directory at a time, breadth first, on the scan
// thread. Each directory's own listing (io_list_dir, with sizes and times) gives its
// subdirectories, which join the end of the walk, and its audio files, which become rows
// at once with a guess from their paths. What is found goes to the writer in batches, a
// few times a second. A directory that cannot be listed is marked, and the walk goes on.
// Links to directories are not followed, so a walk ends whatever the links do.
#include "lib.h"
#include "korvid/blackwing_tags/tags.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/core/utf8.h"
#include "korvid/platform/io.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

namespace kv::lib {

namespace {

constexpr u32 kBatchRecords = 2000;  // a batch goes to the writer at this many records,
constexpr f64 kBatchSeconds = 0.1;   // or this long after its first

// One root's walk: the directories still to list, first in first out.
struct RootWalk {
    u32     root = 0;
    char    path[kPathMax] = "";
    Strings queue;              // paths within the root, NUL-separated, from `head`
    u32     head = 0;
    f64     started = 0.0;
    u32     files = 0;
    bool    root_failed = false;    // its own directory could not be listed
    bool    again = false;          // asked for again while it was walked: walked again when it is done
    char    reason[96] = "";
    Strings early;              // directories listed ahead of the walk, for the slot: not listed again
    bool    has_more() const { return head < queue.bytes.len; }
};

const char* errno_words(i32 err) {
    switch (err) {
    case ENOENT:  return "it is not there";
    case ENOTDIR: return "it is not a directory";
    case EACCES:  return "it may not be read (no permission)";
    case EIO:     return "the disk could not read it (an I/O error)";
    default:      return "it could not be listed";
    }
}

} // namespace

// The name as UTF-8, for showing: what is not becomes U+FFFD.
u32 shown_utf8(const char* s, char* out, u32 cap) {
    u32 n = 0;
    for (usize at = 0, len = strlen(s); at < len;) {
        u32 step = 0;
        u32 cp = utf8_decode(s + at, len - at, &step);
        char b[KV_UTF8_MAX_BYTES];
        u32 k = utf8_encode(cp, b);
        if (n + k + 1 > cap) break;
        memcpy(out + n, b, k);
        n += k;
        at += step ? step : 1;
    }
    out[n] = 0;
    return n;
}

namespace {

u32 shown(const char* s, char* out, u32 cap) { return shown_utf8(s, out, cap); }

struct Lister {
    RootWalk*  w = nullptr;
    Batch*     b = nullptr;
    const char* rel = "";
    u8*        tag_mem = nullptr;
    u32        found = 0;
};

u32 parse_u32(const char* s) {
    u32 v = 0;
    for (; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (u32)(*s - '0');
    return v;
}

bool on_entry(void* user, const IoDirEntry& e) {
    Lister& l = *(Lister*)user;
    char rel[kPathMax];
    u32 n = (u32)format(rel, sizeof rel, l.rel[0] ? "%s/%s" : "%s%s", l.rel, e.name);
    if (n >= sizeof rel) return true; // a path longer than the OS takes: nothing to open
    char name[512];
    shown(e.name, name, sizeof name);
    if (e.st.is_dir) {
        if (e.is_link) return true; // not followed: a loop cannot form, nor a walk leave its root
        l.w->queue.add(rel, n);
        DirMsg d{l.w->root, l.b->s.add(rel, n), l.b->s.add(l.rel), l.b->s.add(name), l.b->s.add(""), 0};
        l.b->dirs.push(d);
        return true;
    }
    if (!e.st.is_file || !audio_name(e.name)) return true;
    add_track(*l.b, l.w->root, rel, n, e.st.size, e.st.mtime_ns, l.tag_mem);
    ++l.found;
    return true;
}

void send(Shared& sh, Batch*& b, f64* since) {
    if (!b || (b->dirs.len == 0 && b->tracks.len == 0)) return;
    WriteMsg* m = (WriteMsg*)mem_alloc(sizeof(WriteMsg));
    new (m) WriteMsg{};
    m->kind = WriteKind::Batch;
    m->batch = b;
    os_mutex_lock(&sh.lock);
    sh.writes.push(m);
    os_cond_signal(&sh.write_wake);
    os_mutex_unlock(&sh.lock);
    b = nullptr;
    *since = 0.0;
}

void send_walk(Shared& sh, WriteKind kind, u32 root, f64 seconds = 0.0, u32 files = 0, bool failed = false, u64 volume = 0, const char* reason = "") {
    WriteMsg* m = (WriteMsg*)mem_alloc(sizeof(WriteMsg));
    new (m) WriteMsg{};
    m->kind = kind;
    m->root = root;
    m->seconds = seconds;
    m->files = files;
    m->root_failed = failed;
    m->volume = volume;
    format(m->value, sizeof m->value, "%s", reason);
    os_mutex_lock(&sh.lock);
    sh.writes.push(m);
    os_cond_signal(&sh.write_wake);
    os_mutex_unlock(&sh.lock);
}

} // namespace

void add_track(Batch& b, u32 root, const char* rel, u32 n, u64 size, i64 mtime, u8* tag_mem) {
    TrackMsg t{};
    t.root = root;
    const char* slash = strrchr(rel, '/');
    char dir[kPathMax];
    format(dir, sizeof dir, "%.*s", slash ? (int)(slash - rel) : 0, rel);
    t.dir = b.s.add(dir);
    t.path = b.s.add(rel, n);
    char name[512];
    shown(slash ? slash + 1 : rel, name, sizeof name);
    t.name = b.s.add(name);
    char text[kPathMax];
    shown(rel, text, sizeof text);
    t.shown = b.s.add(text);
    TagRead g = tags_from_path(rel, Span<u8>(tag_mem, 1u << 16));
    auto value = [&](TagField f) { const TagValue* v = tags_find(g.values, f, TagSource::Path); return v ? v->text : ""; };
    t.title = b.s.add(value(TagField::Title));
    t.artist = b.s.add(value(TagField::Artist));
    t.album = b.s.add(value(TagField::Album));
    t.track = parse_u32(value(TagField::TrackNumber));
    t.disc = parse_u32(value(TagField::DiscNumber));
    t.size = size;
    t.mtime = mtime;
    b.tracks.push(t);
}

bool audio_name(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8];
    u32 n = 0;
    for (const char* p = dot + 1; *p && n < sizeof ext - 1; ++p) ext[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    ext[n] = 0;
    return strcmp(ext, "flac") == 0 || strcmp(ext, "mp3") == 0;
}

// Lists one directory of a root's walk: its row, its subdirectories (to the walk), its
// audio files (to the batch). The root's own directory that cannot be listed makes the
// root offline.
void list_one(Shared& sh, RootWalk& w, const char* rel_in, Batch*& batch, f64* since, u8* tag_mem) {
    char rel[kPathMax];
    format(rel, sizeof rel, "%s", rel_in); // the queue may grow under the listing
    char dir[kPathMax];
    format(dir, sizeof dir, rel[0] ? "%s/%s" : "%s", w.path, rel);
    atomic_store(&sh.walking, w.root, MO_RELAXED);
    os_mutex_lock(&sh.lock);
    format(sh.walking_dir, sizeof sh.walking_dir, "%s", rel);
    os_mutex_unlock(&sh.lock);
    if (!batch) { batch = (Batch*)mem_alloc(sizeof(Batch)); new (batch) Batch{}; *since = time_now(); }
    // The directory's own row first, so its children find it; listed, unless it is not.
    char name[512];
    const char* slash = strrchr(rel, '/');
    shown(slash ? slash + 1 : rel, name, sizeof name);
    char parent[kPathMax];
    format(parent, sizeof parent, "%.*s", slash ? (int)(slash - rel) : 0, rel);
    u32 self = batch->dirs.len;
    batch->dirs.push(DirMsg{w.root, batch->s.add(rel), batch->s.add(parent), batch->s.add(name), batch->s.add(""), 1});
    Lister l{&w, batch, rel, tag_mem, 0};
    i32 err = 0;
    bool listed = io_list_dir(dir, IoListSizes, on_entry, &l, &err);
    if (!listed) { batch->dirs[self].state = 2; batch->dirs[self].reason = batch->s.add(errno_words(err)); }
    atomic_add_fetch(&sh.dirs_listed, (u64)1, MO_RELAXED);
    atomic_add_fetch(&sh.files_found, (u64)l.found, MO_RELAXED);
    w.files += l.found;
    if (!listed && rel[0] == 0) {
        // The root itself: offline, with the reason, which the walk's end carries to the
        // writer (in order after its start). Its rows, if it had any, stay.
        w.root_failed = true;
        format(w.reason, sizeof w.reason, "%s", errno_words(err));
    }
}

void scan_thread(void* arg) {
    Shared& sh = *(Shared*)arg;
    Array<RootWalk*> walks;
    u32 held_root = 0;          // the slot's directory, to be listed as soon as its root's walk begins
    char held[kPathMax] = "";
    Array<u32> started;
    Batch* batch = nullptr;
    f64 since = 0.0;
    u8* tag_mem = (u8*)mem_alloc(1u << 16);
    for (;;) {
        // What the main thread asked: roots to walk, roots to drop, a directory to list first.
        os_mutex_lock(&sh.lock);
        while (!sh.quit && sh.walks.len == 0 && sh.drops.len == 0 && walks.len == 0 && !sh.walk_next_root) os_cond_wait(&sh.scan_wake, &sh.lock);
        if (sh.quit) { os_mutex_unlock(&sh.lock); break; }
        started.clear();
        for (const Walk& w : sh.walks) {
            bool walking = false;
            for (RootWalk* x : walks) if (x->root == w.root) { walking = true; x->again = true; }
            if (walking) continue; // asked again while it is walked: walked again once this walk is done
            RootWalk* r = (RootWalk*)mem_alloc(sizeof(RootWalk));
            new (r) RootWalk{};
            r->root = w.root;
            format(r->path, sizeof r->path, "%s", w.path);
            r->queue.add("");
            walks.push(r);
            started.push(w.root);
        }
        sh.walks.clear();
        for (u32 id : sh.drops) {
            for (u32 i = 0; i < walks.len; ++i) {
                if (walks[i]->root != id) continue;
                walks[i]->queue.free();
                walks[i]->early.free();
                mem_free(walks[i]);
                for (u32 j = i + 1; j < walks.len; ++j) walks[j - 1] = walks[j];
                --walks.len;
                break;
            }
        }
        sh.drops.clear();
        os_mutex_unlock(&sh.lock);
        for (u32 id : started) send_walk(sh, WriteKind::WalkStart, id); // before any of its batches
        os_mutex_lock(&sh.lock);
        if (sh.walk_next_root) { // a newer one takes the place of one held
            held_root = sh.walk_next_root;
            format(held, sizeof held, "%s", sh.walk_next);
            sh.walk_next_root = 0;
        }
        os_mutex_unlock(&sh.lock);

        // The slot's directory, if its root is still being walked and the walk has not
        // listed it: now, ahead of the rest. The walk steps over it when it comes to it.
        // Held until its root's walk begins (a root is watched before it is walked).
        if (held_root) {
            for (RootWalk* r : walks) {
                if (r->root != held_root) continue;
                const char* next = held;
                held_root = 0;
                bool done = false;
                for (u32 at = 0; at < r->head && !done;) { const char* q = r->queue.at(at); done = strcmp(q, next) == 0; at += (u32)strlen(q) + 1; }
                for (u32 at = 0; at < r->early.bytes.len && !done;) { const char* q = r->early.at(at); done = strcmp(q, next) == 0; at += (u32)strlen(q) + 1; }
                if (done) break;
                if (r->started == 0.0) r->started = time_now();
                list_one(sh, *r, next, batch, &since, tag_mem);
                r->early.add(next);
                send(sh, batch, &since); // at once: someone is looking
                break;
            }
        }
        if (walks.len == 0) { send(sh, batch, &since); atomic_store(&sh.walking, 0u, MO_RELAXED); continue; }

        // One directory of the first root still walking.
        RootWalk& w = *walks[0];
        if (w.started == 0.0) w.started = time_now();
        const char* rel = w.queue.at(w.head);
        w.head += (u32)strlen(rel) + 1;
        bool early = false;
        for (u32 at = 0; at < w.early.bytes.len && !early;) { const char* q = w.early.at(at); early = strcmp(q, rel) == 0; at += (u32)strlen(q) + 1; }
        if (!early) list_one(sh, w, rel, batch, &since, tag_mem);

        if (batch && (batch->dirs.len + batch->tracks.len >= kBatchRecords || time_now() - since >= kBatchSeconds)) send(sh, batch, &since);
        if (!w.has_more()) {
            send(sh, batch, &since);
            IoStat st;
            u64 volume = io_stat(w.path, &st) ? st.volume : 0;
            send_walk(sh, WriteKind::Walked, w.root, time_now() - w.started, w.files, w.root_failed, volume, w.reason);
            bool again = w.again;
            u32 root = w.root;
            char path[kPathMax];
            format(path, sizeof path, "%s", w.path);
            w.queue.free();
            w.early.free();
            mem_free(&w);
            for (u32 j = 1; j < walks.len; ++j) walks[j - 1] = walks[j];
            --walks.len;
            if (again) {
                // What changed under the walk (before the watcher was watching it) is found now.
                RootWalk* r = (RootWalk*)mem_alloc(sizeof(RootWalk));
                new (r) RootWalk{};
                r->root = root;
                format(r->path, sizeof r->path, "%s", path);
                r->queue.add("");
                walks.push(r);
                send_walk(sh, WriteKind::WalkStart, root);
            }
            if (walks.len == 0) atomic_store(&sh.walking, 0u, MO_RELAXED);
        }
    }
    if (batch) { batch->free(); mem_free(batch); }
    for (RootWalk* r : walks) { r->queue.free(); r->early.free(); mem_free(r); }
    walks.free();
    started.free();
    mem_free(tag_mem);
}

} // namespace kv::lib
