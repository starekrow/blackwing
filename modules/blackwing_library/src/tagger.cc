// The tags pass, on the tagger's thread. Each track the writer says wants reading is a file
// in a sparse span (the decoders' kind) of 64 KB pieces, filled by reads the main thread
// puts through the engine's I/O queue: its head and its tail first, then whatever range a
// reader asks for. blackwing_tags reads every tag in it; the decoder that would play it
// reads its format and length from the same pieces. Both start again from nothing after a
// wait, so a track between reads is only its span. A track asks at most kMaxWaits times
// beyond its first two reads; then what was there is what there is.
//
// Up to kActive tracks are under way at once. The slot's tracks (what someone is looking
// at) go before the rest, and their reads go at the queue's normal priority instead of
// the scan's.
#include "lib.h"
#include "overrides.h"
#include "korvid/blackwing_tags/tags.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/time.h"
#include "korvid/hifi_codec/decoder.h"
#include <new>
#include <stdlib.h>
#include <string.h>

namespace kv::lib {

namespace {

constexpr u32 kActive    = 24;        // tracks under way at once
constexpr u32 kShift     = 14;        // a piece is 16 KB
constexpr u64 kHead      = 64u << 10; // the head read first,
constexpr u64 kTail      = 16u << 10; // and the tail, for a kind whose tags may be there
constexpr u32 kMaxWaits  = 8;         // ranges a track may ask for after those
constexpr f64 kSendEvery = 0.1;       // results go to the writer at least this often

struct Active {
    bool   used = false;
    u32    gen = 0;                 // a read's answer for an older track in this slot is not this one's
    TagJob job;
    bool   slot = false;
    char   rel[kPathMax] = "";
    char   path[kPathMax] = "";
    SparseSpan span;
    u8*    base = nullptr;
    usize  reserved = 0;
    u8*    have = nullptr;
    u32    out = 0;                 // reads asked and not answered
    u32    waits = 0;
    bool   failed = false;
    bool   tail = true;             // the back of the file is looked at for tags
    i32    err = 0;
};

const char* read_words(i32 err) {
    switch (err) {
    case 2:  return "the file is gone";              // ENOENT
    case 13: return "the file may not be read (no permission)"; // EACCES
    case 5:  return "the disk could not read the file (an I/O error)";
    default: return "the file could not be read";
    }
}

struct Tagger {
    Shared* sh = nullptr;
    Active  act[kActive];
    u8*     tag_mem = nullptr;
    u8*     path_mem = nullptr;
    u8*     dec_mem = nullptr;
    u32     dec_bytes = 0;
    Array<TagJobs*> queue;          // the scan's, in order
    TagJobs* slot = nullptr;         // the slot's
    Array<Shared::RootLine> roots;
    TagBatch* out = nullptr;
    f64     out_since = 0.0;
    Array<ReadAsk> asks;              // to hand to the main thread
    // The overrides of the directory last read in, kept while the writer writes none.
    OverrideFile ovr;
    char    ovr_path[kPathMax] = "";
    u32     ovr_epoch = ~0u;
};

// The override file of a track's directory, read once for its tracks. One that will not
// parse is no overrides here; the writer sets it aside when it next looks.
const OverrideFile& overrides_of(Tagger& t, const char* root, const char* rel) {
    char dir[kPathMax], path[kPathMax];
    format(dir, sizeof dir, "%s", rel);
    char* slash = strrchr(dir, '/');
    if (slash) *slash = 0; else dir[0] = 0;
    override_path(path, sizeof path, t.sh->data_dir, root, dir);
    u32 epoch = atomic_load(&t.sh->overrides_epoch, MO_ACQUIRE);
    if (epoch != t.ovr_epoch || strcmp(path, t.ovr_path) != 0) {
        char problem[160];
        if (!override_load(path, &t.ovr, problem, sizeof problem)) t.ovr.clear();
        format(t.ovr_path, sizeof t.ovr_path, "%s", path);
        t.ovr_epoch = epoch;
    }
    return t.ovr;
}

const char* root_path(Tagger& t, u32 id) {
    for (const Shared::RootLine& r : t.roots) if (r.id == id) return r.path;
    return nullptr;
}

void release(Active& a) {
    if (a.base) os_release(a.base, a.reserved);
    mem_free(a.have);
    u32 gen = a.gen;
    a = Active{};
    a.gen = gen + 1;
}

// Asks for [at, at + len) of the file, widened to whole pieces and cut to what is missing.
void ask(Tagger& t, Active& a, u32 index, u64 at, u64 len) {
    if (len == 0 || at >= a.span.size) return;
    u64 end = a.span.size - at < len ? a.span.size : at + len;
    u64 p0 = at >> kShift, p1 = (end - 1) >> kShift;
    while (p0 <= p1 && a.have[p0]) ++p0;
    while (p1 >= p0 && a.have[p1]) { if (p1 == 0) break; --p1; }
    if (p0 > p1 || a.have[p0]) return;
    ReadAsk r;
    r.job = index; r.gen = a.gen; r.slot = a.slot;
    r.at = p0 << kShift;
    u64 stop = (p1 + 1) << kShift;
    r.len = (stop < a.span.size ? stop : a.span.size) - r.at;
    format(r.path, sizeof r.path, "%s", a.path);
    t.asks.push(r);
    ++a.out;
}

bool start(Tagger& t, u32 index, const TagJobs& b, const TagJob& j, bool slot) {
    Active& a = t.act[index];
    const char* rp = root_path(t, j.root);
    if (!rp) return false; // a root taken out: nothing to read
    a.used = true;
    a.job = j;
    a.slot = slot;
    format(a.rel, sizeof a.rel, "%s", b.s.at(j.rel));
    format(a.path, sizeof a.path, "%s/%s", rp, a.rel);
    a.span.size = j.size;
    a.span.piece_shift = kShift;
    u64 pieces = sparse_pieces(a.span);
    a.reserved = os_page_align((usize)(pieces ? pieces : 1) << kShift);
    a.base = (u8*)os_reserve(a.reserved);
    a.have = (u8*)mem_alloc((usize)pieces + 1);
    memset(a.have, 0, (usize)pieces + 1);
    a.span.bytes = a.base;
    a.span.have = a.have;
    if (!a.base) { a.failed = true; return true; }
    // A FLAC's tags are at its front; anything else may have some at its back.
    const char* dot = strrchr(a.rel, '.');
    a.tail = !(dot && (dot[1] == 'f' || dot[1] == 'F') && (dot[2] == 'l' || dot[2] == 'L'));
    ask(t, a, index, 0, kHead);
    if (a.tail && j.size > kHead) ask(t, a, index, j.size - kTail, kTail);
    atomic_add_fetch(&t.sh->tags_active, 1u, MO_RELAXED);
    return true;
}

void send(Tagger& t) {
    if (!t.out || t.out->recs.len == 0) return;
    WriteMsg* m = (WriteMsg*)mem_alloc(sizeof(WriteMsg));
    new (m) WriteMsg{};
    m->kind = WriteKind::Tags;
    m->tags = t.out;
    t.out = nullptr;
    os_mutex_lock(&t.sh->lock);
    t.sh->writes.push(m);
    os_cond_signal(&t.sh->write_wake);
    os_mutex_unlock(&t.sh->lock);
}

// Reads what can be read of a track whose pieces are in, or asks for more. True when it is done.
bool step(Tagger& t, u32 index) {
    Active& a = t.act[index];
    if (a.out) return false;
    const bool last = a.failed || a.waits >= kMaxWaits;
    TagRead tags;
    if (!a.failed) {
        tags = tags_read(a.span, Span<u8>(t.tag_mem, kTagReadMemory), last, a.tail);
        if (tags.status == TagStatus::Wait) { ask(t, a, index, tags.need.at, tags.need.len); ++a.waits; return false; }
    }
    // The audio, as the decoder that would play it sees it. The probe is shown the bytes
    // after any ID3v2 tag in front, so a tag of megabytes of art is not read for it.
    DecoderInfo info;
    const char* why = "";
    if (a.failed) why = read_words(a.err);
    else if (a.span.size < 10) why = "the file is too short to be audio";
    else {
        // Every ID3v2 tag in a row in front, each stepped over by its size.
        u64 skip = 0;
        bool waiting = false;
        for (u32 k = 0; k < 4 && skip + 10 <= a.span.size; ++k) {
            if (!sparse_has(a.span, skip, 10)) {
                if (!last) { ask(t, a, index, skip, 10); ++a.waits; waiting = true; }
                break;
            }
            const u8* h = a.span.bytes + skip;
            if (memcmp(h, "ID3", 3) != 0 || ((h[6] | h[7] | h[8] | h[9]) & 0x80)) break;
            skip += 10 + (((u64)h[6] << 21) | ((u64)h[7] << 14) | ((u64)h[8] << 7) | h[9]) + ((h[5] & 0x10) ? 10u : 0u);
        }
        if (waiting) return false;
        if (skip > a.span.size) skip = a.span.size;
        // And zeros after them that the tags did not count (decoder_head_bytes' padding).
        for (u64 pad = 0; skip && skip < a.span.size && pad < (1u << 16); ++pad, ++skip) {
            if (!sparse_has(a.span, skip, 1)) {
                u64 want = a.span.size - skip < 4096 ? a.span.size - skip : 4096;
                if (!last) { ask(t, a, index, skip, want); ++a.waits; return false; }
                break;
            }
            if (a.span.bytes[skip] != 0) break;
        }
        u64 n = a.span.size > skip ? a.span.size - skip : 0;
        if (n > 64) n = 64;
        if (!n) why = "the file is nothing but a tag";
        else if (!sparse_has(a.span, skip, n)) {
            if (!last) { ask(t, a, index, skip, n); ++a.waits; return false; }
            why = "the file's first bytes could not be read";
        } else {
            const DecoderDesc* d = decoder_for(Span<const u8>(a.span.bytes + skip, (u32)n));
            if (!d) why = "not a kind of audio file this build plays";
            else {
                DecoderOpen o = d->open(a.span, Span<u8>(t.dec_mem, d->memory_bytes <= t.dec_bytes ? d->memory_bytes : t.dec_bytes));
                if (o.need.len) {
                    if (!last) { ask(t, a, index, o.need.at, o.need.len); ++a.waits; return false; }
                    why = "the file's headers could not all be read";
                } else if (!o.state) {
                    why = o.reason;
                } else {
                    info = o.info;
                    d->close(o.state);
                }
            }
        }
    }

    // What was found, with the guess from the path beside it, and the merge.
    if (!t.out) { t.out = (TagBatch*)mem_alloc(sizeof(TagBatch)); new (t.out) TagBatch{}; t.out_since = time_now(); }
    TagBatch& b = *t.out;
    TagRec r;
    r.track = a.job.track; r.root = a.job.root; r.size = a.job.size; r.mtime = a.job.mtime;
    r.rate = info.format.rate; r.channels = info.format.channels; r.bits = info.format.bits;
    r.frames = info.frames - (info.frames >= (u64)info.delay + info.padding ? (u64)info.delay + info.padding : 0);
    r.estimated = info.estimated;
    char reason[160];
    if (why[0]) format(reason, sizeof reason, "%s", why);
    else format(reason, sizeof reason, "%s", !a.failed ? tags.problem : "");
    r.reason = b.s.add(reason);
    r.vals = b.vals.len;
    TagValue all[1100];
    u32 nall = 0;
    for (const TagValue& v : tags.values) {
        b.vals.push(TagVal{(u8)v.field, (u8)v.source, b.s.add(v.text, v.len)});
        if (nall < 1100) all[nall++] = v;
    }
    TagRead guess = tags_from_path(a.rel, Span<u8>(t.path_mem, 1u << 16));
    for (const TagValue& v : guess.values) {
        b.vals.push(TagVal{(u8)v.field, (u8)v.source, b.s.add(v.text, v.len)});
        if (nall < 1100) all[nall++] = v;
    }
    // The user's, which outrank every source.
    {
        const char* rp = root_path(t, a.job.root);
        const char* name = strrchr(a.rel, '/');
        TagValue ov[64];
        u32 n = rp ? override_values(overrides_of(t, rp, a.rel), name ? name + 1 : a.rel, ov, 64) : 0;
        for (u32 i = 0; i < n; ++i) {
            b.vals.push(TagVal{(u8)ov[i].field, (u8)ov[i].source, b.s.add(ov[i].text, ov[i].len)});
            if (nall < 1100) all[nall++] = ov[i];
        }
    }
    r.nvals = b.vals.len - r.vals;
    char shown[kPathMax];
    shown_utf8(a.rel, shown, sizeof shown);
    r.shown = b.s.add(shown);
    Merged m;
    merge(Span<const TagValue>(all, nall), &m);
    r.title = b.s.add(m.title);
    r.artist = b.s.add(m.artist);
    r.album = b.s.add(m.album);
    r.albumartist = b.s.add(m.albumartist);
    r.date = b.s.add(m.date);
    r.genre = b.s.add(m.genre);
    r.trackno = m.trackno;
    r.discno = m.discno;
    r.titlesort = b.s.add(m.titlesort);
    r.artistsort = b.s.add(m.artistsort);
    r.albumsort = b.s.add(m.albumsort);
    r.albumartistsort = b.s.add(m.albumartistsort);
    b.recs.push(r);
    atomic_add_fetch(&t.sh->tags_read, (u64)1, MO_RELAXED);
    atomic_sub_fetch(&t.sh->tags_active, 1u, MO_RELAXED);
    release(a);
    return true;
}

// The next job: the slot's first, then the scan's in order.
bool next_job(Tagger& t, u32 index) {
    if (t.slot) {
        while (t.slot->next < t.slot->jobs.len) {
            const TagJob& j = t.slot->jobs[t.slot->next++];
            bool busy = false;
            for (const Active& a : t.act) busy |= a.used && a.job.track == j.track;
            if (!busy && start(t, index, *t.slot, j, true)) return true;
        }
    }
    while (t.queue.len) {
        TagJobs& b = *t.queue[0];
        while (b.next < b.jobs.len) {
            const TagJob& j = b.jobs[b.next++];
            bool busy = false;
            for (const Active& a : t.act) busy |= a.used && a.job.track == j.track;
            if (!busy && start(t, index, b, j, false)) return true;
        }
        b.free();
        mem_free(t.queue[0]);
        for (u32 k = 1; k < t.queue.len; ++k) t.queue[k - 1] = t.queue[k];
        --t.queue.len;
    }
    return false;
}

} // namespace

void tag_thread(void* arg) {
    Shared& sh = *(Shared*)arg;
    Tagger* tp = (Tagger*)mem_alloc(sizeof(Tagger));
    new (tp) Tagger{};
    Tagger& t = *tp;
    t.sh = &sh;
    t.tag_mem = (u8*)mem_alloc(kTagReadMemory);
    t.path_mem = (u8*)mem_alloc(1u << 16);
    for (const DecoderDesc* d : decoders()) if (d->memory_bytes > t.dec_bytes) t.dec_bytes = d->memory_bytes;
    t.dec_mem = (u8*)mem_alloc(t.dec_bytes);
    Array<ReadDone> dones;
    for (;;) {
        // Asleep until there is something to do: new tracks, the slot, reads that came, or a
        // place free and a track for it. Before sleeping, what is done goes to the writer.
        os_mutex_lock(&sh.lock);
        while (!sh.quit && !sh.jobs.len && !sh.slot_jobs && !sh.read_dones.len) {
            bool free_place = false;
            for (const Active& a : t.act) free_place |= !a.used;
            bool have = (t.slot && t.slot->next < t.slot->jobs.len) || t.queue.len;
            if (free_place && have) break;
            if (t.out) { os_mutex_unlock(&sh.lock); send(t); os_mutex_lock(&sh.lock); continue; }
            os_cond_wait(&sh.tag_wake, &sh.lock);
        }
        if (sh.quit) { os_mutex_unlock(&sh.lock); break; }
        for (TagJobs* b : sh.jobs) t.queue.push(b);
        sh.jobs.clear();
        if (sh.slot_jobs) { if (t.slot) { t.slot->free(); mem_free(t.slot); } t.slot = sh.slot_jobs; sh.slot_jobs = nullptr; }
        for (const ReadDone& d : sh.read_dones) dones.push(d);
        sh.read_dones.clear();
        t.roots.clear();
        for (const Shared::RootLine& r : sh.root_lines) t.roots.push(r);
        os_mutex_unlock(&sh.lock);

        // Reads that came: their bytes into their tracks' spans.
        for (const ReadDone& d : dones) {
            Active& a = t.act[d.job];
            if (a.used && a.gen == d.gen) {
                if (a.out) --a.out;
                if (!d.ok) { a.failed = true; a.err = d.err; }
                else if (d.size) {
                    u64 end = d.at + d.size < a.span.size ? d.at + d.size : a.span.size;
                    usize from = (usize)(d.at & ~(u64)(os_page_size() - 1));
                    os_commit(a.base + from, os_page_align((usize)(end - from)));
                    memcpy(a.base + d.at, d.data, (usize)(end - d.at));
                    for (u64 p = d.at >> kShift; p <= (end - 1) >> kShift; ++p) {
                        u64 pe = ((p + 1) << kShift) < a.span.size ? (p + 1) << kShift : a.span.size;
                        if (end >= pe || end == a.span.size) atomic_store(&a.have[p], (u8)1, MO_RELEASE);
                    }
                }
            }
            if (d.data) free_in(mem_general(), d.data, d.size ? d.size : 1);
        }
        dones.clear();

        // Each track whose reads are in goes as far as it can; free places take new tracks.
        for (u32 i = 0; i < kActive; ++i) {
            if (t.act[i].used) step(t, i);
            if (!t.act[i].used) { if (next_job(t, i)) step(t, i); }
        }
        if (t.out && (t.out->recs.len >= 256 || time_now() - t.out_since >= kSendEvery)) send(t);
        // The slot is empty when the last of what was handed over has been read and nothing
        // more of it is waiting. If it was asked for again meanwhile, that ask stands.
        {
            bool work = t.slot && t.slot->next < t.slot->jobs.len;
            for (const Active& a : t.act) work = work || (a.used && a.slot);
            if (!work) {
                os_mutex_lock(&sh.lock);
                bool more = sh.slot_jobs != nullptr;
                os_mutex_unlock(&sh.lock);
                u32 handed = 2;
                if (!more) atomic_compare_exchange(&sh.slot_state, &handed, 0u, false, MO_ACQ_REL, MO_RELAXED);
            }
        }
        if (t.asks.len) {
            os_mutex_lock(&sh.lock);
            for (const ReadAsk& a : t.asks) sh.read_asks.push(a);
            os_mutex_unlock(&sh.lock);
            t.asks.clear();
        }
    }
    send(t);
    for (Active& a : t.act) if (a.used) release(a);
    for (TagJobs* b : t.queue) { b->free(); mem_free(b); }
    if (t.slot) { t.slot->free(); mem_free(t.slot); }
    os_mutex_lock(&sh.lock);
    for (ReadDone& d : sh.read_dones) if (d.data) free_in(mem_general(), d.data, d.size ? d.size : 1);
    sh.read_dones.clear();
    os_mutex_unlock(&sh.lock);
    for (ReadDone& d : dones) if (d.data) free_in(mem_general(), d.data, d.size ? d.size : 1);
    dones.free();
    t.queue.free(); t.roots.free(); t.asks.free();
    if (t.out) { t.out->free(); mem_free(t.out); }
    t.ovr.free();
    mem_free(t.tag_mem); mem_free(t.path_mem); mem_free(t.dec_mem);
    mem_free(tp);
}

} // namespace kv::lib
