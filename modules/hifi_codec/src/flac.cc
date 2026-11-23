// The FLAC decoder: dr_flac, once, behind the decoder contract. dr_flac is given a bump
// allocator over the block the contract hands the decoder, and nothing else to allocate
// from; what it frees is forgotten, since the whole block goes when the decoder does.
#include "korvid/hifi_codec/flac.h"
#include "korvid/core/format.h"
#include <new>
#include <string.h>

// dr_flac is vendored, not ours: its warnings are silenced for its own lines only.
#if KV_COMPILER_MSVC
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
// A damaged file can make its predictor's sums overflow an int. The frame is thrown away
// when its checksum fails, but the overflow itself must be defined: wrap, for these lines.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("wrapv")
#endif
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_WCHAR
// Its asserts state its own invariants; a file must never be able to stop the player by
// reaching one, so they are off in every config.
#define DRFLAC_ASSERT(expression) ((void)0)
// Its fallbacks when no allocator is given. One always is; these make sure of it.
#define DRFLAC_MALLOC(sz)     ((void)(sz), (void*)0)
#define DRFLAC_REALLOC(p, sz) ((void)(p), (void)(sz), (void*)0)
#define DRFLAC_FREE(p)        ((void)(p))
#define DR_FLAC_IMPLEMENTATION
#include "vendor/dr_flac.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
#if KV_COMPILER_MSVC
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace kv {

namespace {

constexpr usize kBlockAlign = 16; // what malloc would give; dr_flac aligns its sample buffer itself

// The head of the decoder's memory; the rest of the block is the bump allocator's.
struct FlacState {
    drflac*      flac     = nullptr;
    u8*          arena    = nullptr;
    usize        cap      = 0;
    usize        used     = 0;
    bool         starved  = false; // an allocation did not fit
    DecoderInfo  info;
    u64          produced = 0;
    u32          shift    = 0;     // dr_flac's samples are left-justified in 32 bits; ours are not
    DecodeStatus status   = DecodeStatus::Ok;
    char         reason[96] = "";
};

// Each block is preceded by its size, for realloc.
struct BlockHead { usize size; usize pad; };
static_assert(sizeof(BlockHead) == kBlockAlign);
static_assert(alignof(FlacState) <= kBlockAlign);

void* flac_malloc(size_t size, void* user) {
    FlacState& s = *(FlacState*)user;
    usize need = sizeof(BlockHead) + ((size + kBlockAlign - 1) & ~(kBlockAlign - 1));
    if (size > s.cap || need > s.cap - s.used) { s.starved = true; return nullptr; }
    BlockHead* h = (BlockHead*)(s.arena + s.used);
    h->size = size;
    h->pad  = 0;
    s.used += need;
    return h + 1;
}

void* flac_realloc(void* p, size_t size, void* user) {
    if (!p) return flac_malloc(size, user);
    void* q = flac_malloc(size, user);
    if (!q) return nullptr;
    usize old = ((BlockHead*)p - 1)->size;
    memcpy(q, p, old < size ? old : size);
    return q;
}

void flac_free(void*, void*) {}

// Where the FLAC stream starts in `bytes`: 0, or past an ID3v2 tag. False when `bytes` is
// not long enough to show a FLAC stream there.
bool flac_start(Span<const u8> bytes, u32* at) {
    u32 off = 0;
    if (bytes.len >= 10 && memcmp(bytes.data, "ID3", 3) == 0) {
        const u8* h = bytes.data;
        if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return false; // not a syncsafe size: not a tag
        u32 size = ((u32)h[6] << 21) | ((u32)h[7] << 14) | ((u32)h[8] << 7) | (u32)h[9];
        off = 10 + size + ((h[5] & 0x10) ? 10u : 0u);          // and its footer, if it has one
    }
    if (off > bytes.len || bytes.len - off < 4) return false;
    if (memcmp(bytes.data + off, "fLaC", 4) != 0) return false;
    *at = off;
    return true;
}

bool flac_probe(Span<const u8> bytes) {
    u32 at = 0;
    return flac_start(bytes, &at);
}

DecoderOpen flac_open(Span<const u8> bytes, Span<u8> memory) {
    DecoderOpen out;
    u32 at = 0;
    if (!flac_start(bytes, &at)) { out.reason = "not a FLAC file"; return out; }

    // The state at the first aligned address of the block, the allocator after it.
    usize skew = (usize)(-(intptr_t)memory.data) & (kBlockAlign - 1);
    usize head = (sizeof(FlacState) + kBlockAlign - 1) & ~(kBlockAlign - 1);
    if (!memory.data || memory.len < skew + head) { out.reason = "the decoder was given no memory"; return out; }
    FlacState* s = new (memory.data + skew) FlacState();
    s->arena = memory.data + skew + head;
    s->cap   = memory.len - skew - head;

    drflac_allocation_callbacks cb{s, flac_malloc, flac_realloc, flac_free};
    s->flac = drflac_open_memory(bytes.data, bytes.len, &cb);
    if (!s->flac) {
        out.reason = s->starved ? "the file needs more memory than a decoder is given" : "the FLAC header is damaged";
        return out;
    }
    const drflac& f = *s->flac;
    if (f.channels < 1 || f.channels > 8 || f.bitsPerSample < 1 || f.bitsPerSample > 32 || f.sampleRate == 0) {
        out.reason = "the FLAC header describes a format that cannot be played";
        return out;
    }
    s->info.format = AudioFormat{f.sampleRate, f.channels, f.bitsPerSample};
    s->info.frames = f.totalPCMFrameCount;
    s->shift       = 32u - f.bitsPerSample;
    out.state = s;
    out.info  = s->info;
    return out;
}

DecodeResult flac_decode(void* state, i32* out, u32 frames) {
    FlacState& s = *(FlacState*)state;
    DecodeResult r;
    r.status = s.status;
    r.reason = s.reason;
    if (s.status != DecodeStatus::Ok || frames == 0) return r;

    u32 got = (u32)drflac_read_pcm_frames_s32(s.flac, frames, out);
    if (got > frames) got = frames;
    if (s.shift) {
        usize n = (usize)got * s.info.format.channels;
        for (usize i = 0; i < n; ++i) out[i] >>= s.shift; // arithmetic: the sign comes down with it
    }
    s.produced += got;
    r.frames = got;
    if (got < frames) {
        if (s.info.frames && s.produced < s.info.frames) {
            s.status = DecodeStatus::Damaged;
            format(s.reason, sizeof s.reason, "the file is cut short or damaged: %llu of %llu frames could be read",
                   (unsigned long long)s.produced, (unsigned long long)s.info.frames);
        } else {
            s.status = DecodeStatus::End;
        }
        r.status = s.status;
        r.reason = s.reason;
    }
    return r;
}

void flac_close(void* state) {
    if (!state) return;
    FlacState& s = *(FlacState*)state;
    if (s.flac) drflac_close(s.flac); // frees into the bump allocator, which forgets
    s.flac = nullptr;
}

} // namespace

constinit const DecoderDesc k_flac_decoder = {
    .name         = "flac",
    .memory_bytes = 4u << 20, // dr_flac's state, and a block of 65535 frames of 8 channels (2.1 MB), and a seek table
    .probe        = flac_probe,
    .open         = flac_open,
    .decode       = flac_decode,
    .close        = flac_close,
};

} // namespace kv
