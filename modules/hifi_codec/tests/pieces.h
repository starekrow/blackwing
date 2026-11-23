// The tests' harness for a decoder over a file in pieces: a sparse span whose pieces are
// committed only as they are delivered (so a decoder that reads one it was not given
// faults), the orders they are delivered in, and a driver that answers each Wait. A
// straight decode of a whole file is the oracle the pieces are checked against.
#pragma once
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_codec/flac.h"
#include "korvid/hifi_codec/sparse.h"
#include "korvid/core/atomic.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/random.h"
#include "data.h"
#include <initializer_list>
#include <string.h>

namespace kvtest {

using namespace kv;

constexpr u32 kGuard = 0xC0FFEE11u;
constexpr u32 kGuardWords = 64;
constexpr u32 kMaxWaits = 200000; // a decoder that waits for ever is a failure, not a hang

// A file in a sparse span whose pieces are committed as they are delivered.
struct Fed {
    SparseSpan span;
    const File* src = nullptr;
    u8*   base = nullptr;
    usize reserved = 0;
    u8*   have = nullptr;
    u64   pieces = 0, given = 0;

    void make(const File& f, u32 shift) {
        src = &f;
        span.size = f.len;
        span.piece_shift = shift;
        pieces = sparse_pieces(span);
        reserved = os_page_align((usize)(pieces ? pieces : 1) << shift);
        base = (u8*)os_reserve(reserved);
        have = (u8*)mem_alloc((usize)pieces + 1);
        memset(have, 0, (usize)pieces + 1);
        span.bytes = base;
        span.have = have;
        given = 0;
    }
    bool has(u64 i) const { return have[i] != 0; }
    void give(u64 i) {
        if (i >= pieces || has(i)) return;
        u64 from = i << span.piece_shift;
        u64 len = span.size - from < ((u64)1 << span.piece_shift) ? span.size - from : (u64)1 << span.piece_shift;
        os_commit(base + from, os_page_align((usize)len));
        memcpy(base + from, src->data + from, (usize)len);
        atomic_store(&have[i], (u8)1, MO_RELEASE);
        ++given;
    }
    void give_all() { for (u64 i = 0; i < pieces; ++i) give(i); }
    void free() { os_release(base, reserved); mem_free(have); *this = Fed{}; }
};

enum class Order : u8 {
    Asked,     // every piece of the range asked for
    Stingy,    // the first missing piece of it, and each Wait is asked twice: nothing may change
    Backward,  // the last missing piece of it
    Scattered, // any missing piece of the file, at random
    Count
};
inline const char* order_name(Order o) {
    switch (o) {
    case Order::Asked: return "asked"; case Order::Stingy: return "stingy";
    case Order::Backward: return "backward"; case Order::Scattered: return "scattered";
    case Order::Count: break;
    }
    return "?";
}

// What a range asked for must be: inside the file, and not already there at its front.
inline bool fair(const Fed& f, ByteRange need) {
    if (need.len == 0 || need.at >= f.span.size || f.span.size - need.at < need.len) return false;
    return !f.has(need.at >> f.span.piece_shift);
}

// Delivers something in answer to a Wait for `need`, by `order`.
inline void serve(Fed& f, ByteRange need, Order order, Rng& rng) {
    u64 first = need.at >> f.span.piece_shift, last = (need.at + need.len - 1) >> f.span.piece_shift;
    switch (order) {
    case Order::Asked:
        for (u64 i = first; i <= last; ++i) f.give(i);
        return;
    case Order::Stingy:
        for (u64 i = first; i <= last; ++i) if (!f.has(i)) { f.give(i); return; }
        return;
    case Order::Backward:
        for (u64 i = last + 1; i-- > first;) if (!f.has(i)) { f.give(i); return; }
        return;
    case Order::Scattered: {
        u64 missing = 0;
        for (u64 i = 0; i < f.pieces; ++i) missing += !f.has(i);
        if (!missing) return;
        u64 pick = rng.next() % missing;
        for (u64 i = 0; i < f.pieces; ++i)
            if (!f.has(i) && pick-- == 0) { f.give(i); return; }
        return;
    }
    case Order::Count: break;
    }
}

// A decoder over a Fed, with the waiting done: each call goes on until it has an answer
// that is not Wait, serving pieces by `order` meanwhile, and checks what each Wait asked.
struct Driver {
    const DecoderDesc* d = &k_flac_decoder;
    u32    mem_bytes = 0;
    Fed*   fed = nullptr;
    Order  order = Order::Asked;
    Rng    rng = Rng::seeded(20261123, 2);
    u8*    memory = nullptr;
    i32*   out = nullptr;
    u32    out_frames = 0;
    void*  state = nullptr;
    DecoderInfo info;
    const char* reason = "";
    u32    waits = 0;
    bool   behaved = true;

    // `decoder` reads it, with `mem` bytes of memory (0: what it asks for).
    void make(Fed& f, Order o, u32 frames, const DecoderDesc* decoder = &k_flac_decoder, u32 mem = 0) {
        fed = &f; order = o; out_frames = frames; d = decoder;
        mem_bytes = mem ? mem : d->memory_bytes;
        memory = (u8*)mem_alloc(mem_bytes + kGuardWords * 4);
        out = (i32*)mem_alloc(((usize)frames * 8 + kGuardWords) * 4);
        for (u32 i = 0; i < kGuardWords; ++i) {
            memcpy(memory + mem_bytes + i * 4, &kGuard, 4);
            out[(usize)frames * 8 + i] = (i32)kGuard;
        }
    }
    bool intact() const {
        for (u32 i = 0; i < kGuardWords; ++i) {
            u32 w; memcpy(&w, memory + mem_bytes + i * 4, 4);
            if (w != kGuard || (u32)out[(usize)out_frames * 8 + i] != kGuard) return false;
        }
        return true;
    }
    void wait_for(ByteRange need) {
        if (!fair(*fed, need)) behaved = false;
        serve(*fed, need, order, rng);
        if (++waits > kMaxWaits) behaved = false;
    }
    bool open() {
        for (;;) {
            DecoderOpen o = d->open(fed->span, Span<u8>(memory, mem_bytes));
            if (!intact()) behaved = false;
            if (o.need.len && behaved) {
                if (o.state) behaved = false;
                if (order == Order::Stingy) {
                    DecoderOpen again = d->open(fed->span, Span<u8>(memory, mem_bytes));
                    if (again.state || again.need.at != o.need.at || again.need.len != o.need.len) behaved = false;
                }
                wait_for(o.need);
                continue;
            }
            state = o.state; info = o.info; reason = o.reason;
            return state != nullptr;
        }
    }
    // Up to `frames` frames into out; the status is never Wait.
    DecodeResult decode(u32 frames) {
        u32 got = 0;
        DecodeResult r;
        for (;;) {
            DecodeResult x = d->decode(state, out + (usize)got * info.format.channels, frames - got);
            if (x.frames > frames - got || !intact()) { behaved = false; r.status = DecodeStatus::Damaged; break; }
            got += x.frames;
            r = x;
            if (x.status == DecodeStatus::Wait) {
                if (order == Order::Stingy) {
                    DecodeResult again = d->decode(state, out + (usize)got * info.format.channels, frames - got);
                    if (again.frames != 0 || again.status != DecodeStatus::Wait || !fair(*fed, again.need)) behaved = false;
                }
                wait_for(x.need);
                if (!behaved) break;
                if (got == frames) { r.status = DecodeStatus::Ok; break; }
                continue;
            }
            if (x.status != DecodeStatus::Ok || got == frames) break;
            behaved = false; // short with Ok is against the contract
            break;
        }
        r.frames = got;
        return r;
    }
    SeekResult seek(u64 frame) {
        for (;;) {
            SeekResult s = d->seek(state, frame);
            if (s.status == SeekStatus::Wait && behaved) {
                if (order == Order::Stingy) {
                    SeekResult again = d->seek(state, frame);
                    if (again.status != SeekStatus::Wait || !fair(*fed, again.need)) behaved = false;
                }
                wait_for(s.need);
                continue;
            }
            if (s.status == SeekStatus::Failed && !s.reason[0]) behaved = false;
            return s;
        }
    }
    void free() {
        if (state) d->close(state);
        mem_free(memory); mem_free(out);
        state = nullptr; memory = nullptr; out = nullptr;
    }
};

// A straight decode of a whole file in memory: the oracle.
struct Pcm { i32* s = nullptr; u64 frames = 0; u32 ch = 0; DecoderInfo info; DecodeStatus status = DecodeStatus::Ok; };
// `most`: frames to stop at (0: what the file says, or a million where it does not).
inline Pcm straight(Span<const u8> bytes, const DecoderDesc& d = k_flac_decoder, u64 most = 0) {
    Pcm p;
    u8* mem = (u8*)mem_alloc(d.memory_bytes);
    DecoderOpen o = d.open(sparse_whole(bytes), Span<u8>(mem, d.memory_bytes));
    if (o.state) {
        p.info = o.info;
        p.ch = o.info.format.channels;
        u64 cap = most ? most : (o.info.frames && !o.info.estimated ? o.info.frames : o.info.frames * 2 + (1u << 20)) + 16;
        p.s = (i32*)mem_alloc((usize)cap * p.ch * 4);
        for (;;) {
            u32 ask = cap - p.frames < 4096 ? (u32)(cap - p.frames) : 4096u;
            DecodeResult r = d.decode(o.state, p.s + p.frames * p.ch, ask);
            p.frames += r.frames;
            p.status = r.status;
            if (r.status != DecodeStatus::Ok || p.frames == cap) break;
        }
        d.close(o.state);
    }
    mem_free(mem);
    return p;
}
inline void drop(Pcm& p) { mem_free(p.s); p = Pcm{}; }

// The piece sizes: the page, a few pages, more than the largest frame, more than the file.
inline u32 shifts(u32* out) {
    u32 page = 0;
    while (((usize)1 << page) < os_page_size()) ++page;
    u32 n = 0;
    for (u32 s : {page, page + 1, page + 3, 18u}) if (s >= page) out[n++] = s;
    return n;
}

// Decodes `frames` frames from where `dr` is, and whether they are `want`'s from `at` on.
inline bool same_from(Driver& dr, const Pcm& want, u64 at, u64 frames, DecodeStatus* end = nullptr) {
    u64 done = 0;
    bool same = true;
    while (done < frames) {
        u32 ask = frames - done < dr.out_frames ? (u32)(frames - done) : dr.out_frames;
        DecodeResult r = dr.decode(ask);
        if (at + done + r.frames > want.frames) return false;
        if (memcmp(dr.out, want.s + (at + done) * want.ch, (usize)r.frames * want.ch * 4) != 0) same = false;
        done += r.frames;
        if (r.status != DecodeStatus::Ok) { if (end) *end = r.status; break; }
    }
    return same && done == frames;
}

} // namespace kvtest
