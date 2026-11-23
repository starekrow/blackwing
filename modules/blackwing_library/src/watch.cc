// Changes, on the watch thread: the engine's FsWatcher over every root that is not
// offline, and over the overrides in Blackwing's own data. Its events are hints. A path it
// names is looked at again once it has been quiet for a moment (a file being written
// changes many times), and what is there is believed: a file there is a row, with its size
// and time (and read again if they changed); a file not there is gone. A directory that
// goes takes what was under it. A lost event is a walk of the root.
//
// A root on a share (KR12) is polled by the watcher, at an interval; so is one the OS will
// not watch (its watches run out). A root that is offline is looked at again at an
// interval, and walked when it is back.
#include "lib.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/platform/fs_watch.h"
#include "korvid/platform/io.h"
#include <new>
#include <string.h>

namespace kv::lib {

namespace {

struct Watched { u32 root; u32 fs; char path[kPathMax]; };
// A path a watcher named, waiting out its quiet moment.
struct Pending { u32 root; u32 rel; f64 last; bool dir_gone; bool takein; };

void send_batch(Shared& sh, Batch* b) {
    if (!b->tracks.len && !b->gone.len && !b->dirs.len) { b->free(); mem_free(b); return; }
    WriteMsg* m = (WriteMsg*)mem_alloc(sizeof(WriteMsg));
    new (m) WriteMsg{};
    m->kind = WriteKind::Batch;
    m->batch = b;
    os_mutex_lock(&sh.lock);
    sh.writes.push(m);
    os_cond_signal(&sh.write_wake);
    os_mutex_unlock(&sh.lock);
}

void send_takein(Shared& sh, const char* path) {
    WriteMsg* m = (WriteMsg*)mem_alloc(sizeof(WriteMsg));
    new (m) WriteMsg{};
    m->kind = WriteKind::TakeIn;
    format(m->path, sizeof m->path, "%s", path);
    os_mutex_lock(&sh.lock);
    sh.writes.push(m);
    os_cond_signal(&sh.write_wake);
    os_mutex_unlock(&sh.lock);
}

bool has_entries(const char* dir) {
    bool any = false;
    bool ok = io_list_dir(dir, 0, [](void* u, const IoDirEntry&) { *(bool*)u = true; return false; }, &any);
    return ok && any;
}

} // namespace

void watch_thread(void* arg) {
    Shared& sh = *(Shared*)arg;
    FsWatcher w;
    bool up = w.init(FsWatchBackend::Auto, sh.poll_seconds, sh.poll_seconds);
    if (!up) KV_WARN("library", "the watcher will not start: changes are found by the walk at start-up only");
    Array<Watched> watched;
    Strings ps;
    Array<Pending> pending;
    u8* tag_mem = (u8*)mem_alloc(1u << 16);
    char overrides[kPathMax];
    format(overrides, sizeof overrides, "%s/overrides", sh.data_dir);
    io_make_dirs(overrides);
    u32 ovr_fs = up ? w.add_root(overrides) : ~0u;
    f64 last_offline_look = time_now();
    for (;;) {
        // Which roots to watch: every one not offline.
        Array<Shared::RootLine> lines;
        os_mutex_lock(&sh.lock);
        if (sh.quit) { os_mutex_unlock(&sh.lock); lines.free(); break; }
        for (const Shared::RootLine& r : sh.root_lines) lines.push(r);
        os_mutex_unlock(&sh.lock);
        for (u32 i = 0; i < watched.len;) {
            bool keep = false;
            for (const Shared::RootLine& r : lines) keep |= r.id == watched[i].root && !r.offline;
            if (keep) { ++i; continue; }
            w.remove_root(watched[i].fs);
            watched[i] = watched[watched.len - 1];
            --watched.len;
        }
        for (const Shared::RootLine& r : lines) {
            if (!up || r.offline) continue;
            bool have = false;
            for (const Watched& x : watched) have |= x.root == r.id;
            if (have) continue;
            u32 fs = w.add_root(r.path, sh.poll_roots ? FsWatchBackend::Poll : FsWatchBackend::Auto);
            if (fs == ~0u && !sh.poll_roots) {
                fs = w.add_root(r.path, FsWatchBackend::Poll); // the OS will not watch it all: polled instead
                if (fs != ~0u) KV_INFO("library", "%s is polled every %.0f s: the OS will not watch all of it", r.path, sh.poll_seconds);
            }
            if (fs == ~0u) continue; // cannot be read now: the offline look will find it
            Watched x{r.id, fs, ""};
            format(x.path, sizeof x.path, "%s", r.path);
            watched.push(x);
        }

        // What the watcher heard: each path waits out its quiet moment.
        f64 now = time_now();
        if (up) {
            for (const FsEvent& e : w.poll()) {
                if (e.root == ovr_fs) {
                    const char* name = strrchr(e.path, '/');
                    if (strcmp(name ? name + 1 : e.path, "overrides.txt") != 0) continue;
                    pending.push(Pending{0, ps.add(e.path), now, false, true});
                    continue;
                }
                const Watched* x = nullptr;
                for (const Watched& c : watched) if (c.fs == e.root) x = &c;
                if (!x) continue;
                if (e.kind == FsEventKind::Rescan) {
                    // Events were lost: the root walked again (reading nothing that did not change).
                    os_mutex_lock(&sh.lock);
                    Walk wk{x->root, ""};
                    format(wk.path, sizeof wk.path, "%s", x->path);
                    sh.walks.push(wk);
                    os_cond_signal(&sh.scan_wake);
                    os_mutex_unlock(&sh.lock);
                    continue;
                }
                if (e.kind == FsEventKind::DirAdded) continue; // its files come as Added
                bool dir_gone = e.kind == FsEventKind::DirRemoved;
                if (!dir_gone && !audio_name(e.path)) continue;
                bool found = false;
                for (Pending& p : pending) {
                    if (p.root == x->root && p.dir_gone == dir_gone && !p.takein && strcmp(ps.at(p.rel), e.path) == 0) { p.last = now; found = true; break; }
                }
                if (!found) pending.push(Pending{x->root, ps.add(e.path), now, dir_gone, false});
            }
        }

        // What has been quiet long enough: looked at, and what is there believed.
        Batch* b = nullptr;
        u32 k = 0;
        for (u32 i = 0; i < pending.len; ++i) {
            const Pending& p = pending[i];
            if (now - p.last < sh.quiet_seconds) { pending[k++] = p; continue; }
            const char* rel = ps.at(p.rel);
            if (p.takein) {
                char path[kPathMax];
                format(path, sizeof path, "%s/%s", overrides, rel);
                send_takein(sh, path);
                continue;
            }
            const Watched* x = nullptr;
            for (const Watched& c : watched) if (c.root == p.root) x = &c;
            if (!x) continue;
            if (!b) { b = (Batch*)mem_alloc(sizeof(Batch)); new (b) Batch{}; }
            char abs[kPathMax];
            format(abs, sizeof abs, "%s/%s", x->path, rel);
            IoStat st;
            bool there = io_stat(abs, &st);
            if (p.dir_gone) {
                if (!there || !st.is_dir) b->gone.push(GoneMsg{p.root, b->s.add(rel), true});
            } else if (there && st.is_file) {
                add_track(*b, p.root, rel, (u32)strlen(rel), st.size, st.mtime_ns, tag_mem);
            } else if (!there) {
                b->gone.push(GoneMsg{p.root, b->s.add(rel), false});
            }
        }
        pending.len = k;
        if (!pending.len) ps.clear();
        // What went, from a root now empty or gone, is not believed file by file: an unplugged
        // disk leaves an empty mount point. The root is walked, and the walk says offline.
        if (b && b->gone.len) {
            Array<u32> emptied;
            for (const GoneMsg& g : b->gone) {
                bool seen = false;
                for (u32 r : emptied) seen |= r == g.root;
                const Watched* x = nullptr;
                for (const Watched& c : watched) if (c.root == g.root) x = &c;
                if (!seen && x && !has_entries(x->path)) emptied.push(g.root);
            }
            if (emptied.len) {
                u32 n = 0;
                for (const GoneMsg& g : b->gone) {
                    bool drop = false;
                    for (u32 r : emptied) drop |= r == g.root;
                    if (!drop) b->gone[n++] = g;
                }
                b->gone.len = n;
                os_mutex_lock(&sh.lock);
                for (u32 r : emptied)
                    for (const Watched& c : watched)
                        if (c.root == r) { Walk wk{r, ""}; format(wk.path, sizeof wk.path, "%s", c.path); sh.walks.push(wk); }
                os_cond_signal(&sh.scan_wake);
                os_mutex_unlock(&sh.lock);
            }
            emptied.free();
        }
        if (b) send_batch(sh, b);

        // Every root looked at again at the interval. An offline one that lists and has
        // something in it is back; one that is not offline but is empty or gone (a disk
        // unmounted leaves its mount point, and the watcher hears nothing of it) is walked,
        // and the walk says offline.
        if (now - last_offline_look >= sh.offline_seconds) {
            last_offline_look = now;
            for (const Shared::RootLine& r : lines) {
                if (!r.offline && r.tracks == 0) continue; // empty and never held anything: nothing to lose
                if (r.offline != has_entries(r.path)) continue;
                os_mutex_lock(&sh.lock);
                Walk wk{r.id, ""};
                format(wk.path, sizeof wk.path, "%s", r.path);
                sh.walks.push(wk);
                os_cond_signal(&sh.scan_wake);
                os_mutex_unlock(&sh.lock);
            }
        }
        lines.free();

        os_mutex_lock(&sh.lock);
        if (!sh.quit) os_cond_timed_wait(&sh.watch_wake, &sh.lock, 0.1);
        os_mutex_unlock(&sh.lock);
    }
    if (up) w.free();
    watched.free();
    ps.free();
    pending.free();
    mem_free(tag_mem);
}

} // namespace kv::lib
