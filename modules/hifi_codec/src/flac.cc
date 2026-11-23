// The FLAC decoder: dr_flac, once, behind the decoder contract. dr_flac is given a bump
// allocator over the block the contract hands the decoder, and nothing else to allocate
// from; what it frees is forgotten, since the whole block goes when the decoder does.
//
// dr_flac reads by pulling, through callbacks, and has no way to stop and come back. So
// the wrapper makes sure it is never asked for a piece that is not there: before dr_flac
// starts a new FLAC frame, the bytes that frame can need (the stream's largest frame, and
// dr_flac's read-ahead) must be in the span, or decode() returns Wait without calling it.
// Where that cannot be known ahead (opening, seeking, a damaged frame's search for the
// next one), the read callback refuses the read, the call is let fail, and the decoder
// goes back to the last frame it returned before it goes on.
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
// when its checksum fails, but the overflow itself must be defined: this file is built
// with the wrapv profile (module.lua's codegen_sources).
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
#if KV_COMPILER_MSVC
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace kv {

namespace {

constexpr usize kBlockAlign = 16; // what malloc would give; dr_flac aligns its sample buffer itself
constexpr u32   kReadAhead  = 2 * DR_FLAC_BUFFER_SIZE + 64; // what dr_flac's cache may pull past the bytes a frame needs

// The head of the decoder's memory; the rest of the block is the bump allocator's.
struct FlacState {
    drflac*      flac     = nullptr;
    u8*          arena    = nullptr;
    usize        cap      = 0;
    usize        used     = 0;
    bool         starved  = false; // an allocation did not fit

    SparseSpan   file;
    u64          start    = 0;     // where the FLAC stream begins in the file: 0, or past an ID3v2 tag
    u64          length   = 0;     // the stream's bytes, from `start` to the end of the file
    u64          pos      = 0;     // dr_flac's read cursor, from `start`
    u64          window   = 0;     // the bytes from the cursor that a new FLAC frame can need
    bool         hungry   = false; // a read reached a piece that is not there; dr_flac's state is not to be trusted
    ByteRange    need;             // what that read needed

    DecoderInfo  info;
    u64          produced = 0;     // the frame decode() returns next, counted as it returns them
    u64          place    = 0;     // the same, as a frame of the file: past a dropped frame the two differ
    bool         lost     = false; // dr_flac is not at `place`, and is put back before it decodes again
    u32          shift    = 0;     // dr_flac's samples are left-justified in 32 bits; ours are not
    DecodeStatus status   = DecodeStatus::Ok;
    char         reason[96] = "";
    char         seek_reason[96] = "";
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

// dr_flac's stream: the file from `start`, read only where its pieces are there.
size_t flac_read(void* user, void* out, size_t bytes) {
    FlacState& s = *(FlacState*)user;
    if (s.hungry || s.pos >= s.length) return 0;
    u64 n = s.length - s.pos < bytes ? s.length - s.pos : bytes;
    u64 at = s.start + s.pos;
    ByteRange miss = sparse_missing(s.file, at, n);
    if (miss.len) { s.hungry = true; s.need = miss; return 0; } // all or nothing: dr_flac caches only what it was given
    memcpy(out, s.file.bytes + at, (usize)n);
    s.pos += n;
    return (size_t)n;
}

drflac_bool32 flac_seek_cb(void* user, int offset, drflac_seek_origin origin) {
    FlacState& s = *(FlacState*)user;
    i64 to = origin == DRFLAC_SEEK_SET ? 0 : origin == DRFLAC_SEEK_CUR ? (i64)s.pos : (i64)s.length;
    to += offset;
    if (to < 0 || (u64)to > s.length) return DRFLAC_FALSE;
    s.pos = (u64)to;
    return DRFLAC_TRUE;
}

drflac_bool32 flac_tell(void* user, drflac_int64* at) {
    *at = (drflac_int64)((FlacState*)user)->pos;
    return DRFLAC_TRUE;
}

// The length of the ID3v2 tag at the front of `head`, footer and all: 0 when there is
// none, ~0 when it is not a tag after all. `head` must hold its first 10 bytes.
u64 id3_length(Span<const u8> head) {
    if (head.len < 10 || memcmp(head.data, "ID3", 3) != 0) return 0;
    const u8* h = head.data;
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return ~0ull; // not a syncsafe size: not a tag
    u32 size = ((u32)h[6] << 21) | ((u32)h[7] << 14) | ((u32)h[8] << 7) | (u32)h[9];
    return 10 + size + ((h[5] & 0x10) ? 10u : 0u);
}

bool flac_probe(Span<const u8> head) {
    u64 off = id3_length(head);
    if (off == ~0ull || off > head.len || head.len - off < 4) return false;
    return memcmp(head.data + off, "fLaC", 4) == 0;
}

// Where the next frame to return is in the file, from the frame dr_flac is in.
void note_place(FlacState& s) {
    drflac_uint64 first = 0, last = 0;
    drflac__get_pcm_frame_range_of_current_flac_frame(s.flac, &first, &last);
    s.place = (u64)first + s.flac->currentFLACFrame.header.blockSizeInPCMFrames - s.flac->currentFLACFrame.pcmFramesRemaining;
}

// Puts dr_flac back at `place`, from its first frame, so nothing it held before counts.
// False with *r filled when it cannot yet (Wait) or at all (Damaged).
bool find_place(FlacState& s, DecodeResult* r) {
    s.hungry = false;
    drflac__seek_to_first_frame(s.flac);
    bool ok = drflac_seek_to_pcm_frame(s.flac, s.place);
    if (s.hungry) {
        r->status = DecodeStatus::Wait;
        r->need   = s.need;
        return false;
    }
    s.lost = false;
    if (!ok) {
        s.status = DecodeStatus::Damaged;
        format(s.reason, sizeof s.reason, "the decoder could not find frame %llu again in a damaged file", (unsigned long long)s.place);
        r->status = s.status;
        r->reason = s.reason;
        return false;
    }
    return true;
}

DecoderOpen flac_open(const SparseSpan& file, Span<u8> memory) {
    DecoderOpen out;

    // The head first: its first bytes, then the stream's magic, past a tag that can be long.
    ByteRange miss = sparse_missing(file, 0, 10);
    if (miss.len) { out.need = miss; return out; }
    u64 at = id3_length(Span<const u8>(file.bytes, file.size < 10 ? (u32)file.size : 10u));
    if (at == ~0ull || at > file.size || file.size - at < 4) { out.reason = "not a FLAC file"; return out; }
    miss = sparse_missing(file, at, 4);
    if (miss.len) { out.need = miss; return out; }
    if (memcmp(file.bytes + at, "fLaC", 4) != 0) { out.reason = "not a FLAC file"; return out; }

    // The state at the first aligned address of the block, the allocator after it.
    usize skew = (usize)(-(intptr_t)memory.data) & (kBlockAlign - 1);
    usize head = (sizeof(FlacState) + kBlockAlign - 1) & ~(kBlockAlign - 1);
    if (!memory.data || memory.len < skew + head) { out.reason = "the decoder was given no memory"; return out; }
    FlacState* s = new (memory.data + skew) FlacState();
    s->arena  = memory.data + skew + head;
    s->cap    = memory.len - skew - head;
    s->file   = file;
    s->start  = at;
    s->length = file.size - at;

    drflac_allocation_callbacks cb{s, flac_malloc, flac_realloc, flac_free};
    s->flac = drflac_open(flac_read, flac_seek_cb, flac_tell, s, &cb);
    if (s->hungry) { out.need = s->need; return out; } // whatever it made of a short read, it starts again
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

    // The largest frame: STREAMINFO says, when it says; otherwise the largest the format
    // allows at this block size (every subframe verbatim, a side channel's extra bit).
    const u8* si = file.bytes + at + 8; // dr_flac has read it, so it is there
    u64 largest = ((u64)si[7] << 16) | ((u64)si[8] << 8) | si[9];
    u64 bound = 18 + (u64)f.channels * (((u64)f.maxBlockSizeInPCMFrames * (f.bitsPerSample + 1u) + 7) / 8 + 8);
    if (largest == 0 || largest > bound) largest = bound;
    s->window = largest + kReadAhead;

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
    r.status = DecodeStatus::Ok;
    if (s.lost && !find_place(s, &r)) return r;

    const u32 ch = s.info.format.channels;
    u32 got = 0;
    bool ended = false;
    while (got < frames) {
        u32 ask  = frames - got;
        u32 left = s.flac->currentFLACFrame.pcmFramesRemaining;
        if (left == 0) {
            // dr_flac starts a new frame on this call: everything it can read must be there.
            ByteRange miss = sparse_missing(s.file, s.start + s.pos, s.window);
            if (miss.len) { r.status = DecodeStatus::Wait; r.need = miss; break; }
            ask = 1; // one frame, and then the window is looked at again
        } else if (ask > left) {
            ask = left;
        }
        s.hungry = false;
        i32* to = out + (usize)got * ch;
        u32 n = (u32)drflac_read_pcm_frames_s32(s.flac, ask, to);
        if (n > ask) n = ask;
        if (s.shift) {
            usize k = (usize)n * ch;
            for (usize i = 0; i < k; ++i) to[i] >>= s.shift; // arithmetic: the sign comes down with it
        }
        got += n;
        s.produced += n;
        if (n) note_place(s);
        if (s.hungry) {
            // A frame that was not where the window said (a damaged one, and dr_flac's
            // search past it): what came before is good, and the decoder goes back for the rest.
            s.lost = true;
            r.status = DecodeStatus::Wait;
            r.need   = s.need;
            break;
        }
        if (n < ask) { ended = true; break; }
    }
    r.frames = got;
    if (ended) {
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

SeekResult flac_seek(void* state, u64 frame) {
    FlacState& s = *(FlacState*)state;
    SeekResult r;
    r.reason = s.seek_reason;
    if (s.info.frames && frame > s.info.frames) {
        format(s.seek_reason, sizeof s.seek_reason, "frame %llu is past the end: the file has %llu",
               (unsigned long long)frame, (unsigned long long)s.info.frames);
        return r;
    }
    if (s.info.frames && frame == s.info.frames) {
        // The end itself: nothing to find, and the next decode says so.
        s.produced = s.place = frame;
        s.status = DecodeStatus::End;
        s.reason[0] = 0;
        s.lost = true;
        r.status = SeekStatus::Ok;
        return r;
    }
    // dr_flac's short cuts within a frame trust its count of frames, which a wait, a
    // dropped frame or the end has made wrong: from those it starts at the first frame.
    s.hungry = false;
    if (s.lost || s.status != DecodeStatus::Ok || s.flac->currentPCMFrame != s.place) drflac__seek_to_first_frame(s.flac);
    bool ok = drflac_seek_to_pcm_frame(s.flac, frame);
    if (s.hungry) {
        s.lost = true; // where it was is `place`, and it is put back there if decode comes first
        r.status = SeekStatus::Wait;
        r.need   = s.need;
        return r;
    }
    if (!ok) {
        s.lost = true;
        format(s.seek_reason, sizeof s.seek_reason, "the file could not be searched to frame %llu", (unsigned long long)frame);
        return r;
    }
    s.produced = s.place = frame;
    s.status = DecodeStatus::Ok;
    s.reason[0] = 0;
    s.lost = false;
    r.status = SeekStatus::Ok;
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
    .seek         = flac_seek,
    .close        = flac_close,
};

} // namespace kv
