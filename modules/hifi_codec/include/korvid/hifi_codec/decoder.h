// Korvid hifi_codec: the decoder contract, and finding the decoder for a file.
//
// A decoder turns the bytes of one audio file into PCM. The contract is a record of
// functions, DecoderDesc, over a state the decoder keeps in memory the caller gives it.
// The file is a sparse span (sparse.h): its address range, and a map of which pieces of
// it are there. A decoder that needs a piece that is not there returns Wait, with the
// range it needs, and is called again once that range has come:
//
//   const DecoderDesc* d = decoder_for(head);                // head: the file's first bytes
//   DecoderOpen o = d->open(file, memory);                   // memory: d->memory_bytes of it
//   if (o.need.len) ... read o.need, then open again
//   else if (!o.state) ... o.reason says why, in words
//   for (;;) {
//       DecodeResult r = d->decode(o.state, out, frames);    // out: frames * channels i32
//       ... r.frames frames are in out
//       if (r.status == DecodeStatus::Wait) ... read r.need, then decode again
//       else if (r.status != DecodeStatus::Ok) break;        // End, or Damaged with r.reason
//   }
//   d->seek(o.state, frame);                                 // the next decode returns that frame
//   d->close(o.state);
//
// What a decoder may not do is the point. It opens no file and names no path: it sees
// the bytes it is handed and nothing else. It takes no memory but the block it is handed.
// It calls nothing of its caller's, and waits by returning, not by blocking. It keeps no
// state outside that block, so two of one kind run side by side, and one can be dropped
// by forgetting its memory. A decoder that keeps to this can be moved into another
// process without its caller changing (docs/PRDs/Blackwing-Master-PRD.md, "Loading
// through codecs").
//
// Samples are integers at the file's own depth, not floats, so "the file's samples" means
// something exact. modules/hifi_codec/README.md has the rest.
#pragma once
#include "korvid/core/types.h"
#include "korvid/hifi_codec/sparse.h"

namespace kv {

// What a stream of PCM is.
struct AudioFormat {
    u32 rate     = 0; // frames per second
    u16 channels = 0; // samples per frame, interleaved
    u16 bits     = 0; // significant bits per sample, 1 to 32; each sample is an i32 in [-2^(bits-1), 2^(bits-1))
};

// What open() found.
struct DecoderInfo {
    AudioFormat format;
    u64 frames  = 0; // the length the file claims, in frames; 0 when it does not say
    u32 delay   = 0; // gapless: frames at the start of what decode() returns that are not music (an encoder's delay)
    u32 padding = 0; // gapless: frames at the end that are not music. Both 0 for a lossless file
    bool estimated = false; // `frames` is a guess from the file's size, not the file's word: the decode may end either side of it
};

// open()'s answer: a state; or a range to wait for (state null, need not empty); or a
// failure (state null, need empty, reason).
struct DecoderOpen {
    void*       state  = nullptr; // inside the memory handed to open(); null when it failed or must wait
    const char* reason = "";      // when it failed: why, in words; valid while the memory is
    ByteRange   need;             // when it must wait: the bytes it needs, then open() again from the start
    DecoderInfo info;
};

enum class DecodeStatus : u8 {
    Ok,      // more to come
    End,     // the stream ended where the file said it would, or the file did not say
    Damaged, // the stream ended early or could not be read; what was returned is good
    Wait,    // what was returned is good; for more, `need` must be there
    Count
};

// decode()'s answer.
struct DecodeResult {
    u32          frames = 0;               // frames written, at most what was asked
    DecodeStatus status = DecodeStatus::End;
    const char*  reason = "";              // with Damaged: why, in words; valid until close()
    ByteRange    need;                     // with Wait: the bytes it needs before it can go on
};

enum class SeekStatus : u8 {
    Ok,     // the next decode() returns the frame asked for
    Wait,   // `need` must be there; then seek() again, to the same frame or another
    Failed, // `reason` says why; the decoder is where it was
    Count
};

// seek()'s answer.
struct SeekResult {
    SeekStatus  status = SeekStatus::Failed;
    const char* reason = "";               // with Failed: why, in words; valid until close()
    ByteRange   need;                      // with Wait
};

// One kind of decoder. Every function is safe on any thread, on its own state; none
// allocates, blocks or logs.
struct DecoderDesc {
    const char* name = "";        // "flac"
    u32         memory_bytes = 0; // the block open() wants: enough for any file of this kind it will accept

    // Whether `head`, the file's first bytes, starts a file of this kind. The first 64
    // bytes are enough to say no; a yes is a promise only that open() is worth trying.
    bool (*probe)(Span<const u8> head) = nullptr;

    // Reads the file's headers and sets up to decode from its first frame. The span's
    // bytes, its map and `memory` must stay where they are until close(); pieces may
    // arrive in it meanwhile, and pieces the decoder has gone past may be given back.
    // When it must wait, nothing in `memory` is worth keeping and open() is called again
    // with the same memory. On failure state is null, reason says why.
    DecoderOpen (*open)(const SparseSpan& file, Span<u8> memory) = nullptr;

    // Decodes up to `frames` frames into `out` (frames * channels samples, interleaved).
    // Fewer than asked only with End, Damaged or Wait. After End or Damaged, every call
    // returns 0 frames and the same status until a seek. A Wait changes nothing but the
    // frames it returned: the next call goes on from after them.
    DecodeResult (*decode)(void* state, i32* out, u32 frames) = nullptr;

    // Makes the next decode() return frame `frame` of the file (0 is its first; the
    // file's length is allowed, and decodes to End). After End or Damaged too. A seek
    // that waits, or fails, leaves the decoder where it was.
    SeekResult (*seek)(void* state, u64 frame) = nullptr;

    // Ends the decoder's use of its memory. It has nothing else to give back.
    void (*close)(void* state) = nullptr;
};

// The decoders linked in, in the order they are asked.
Span<const DecoderDesc* const> decoders();

// The first decoder whose probe accepts `head`; null when none does.
const DecoderDesc* decoder_for(Span<const u8> head);

// How many of a file's first bytes decoder_for needs to decide, given at least its first
// 10 (fewer only when that is the whole file): 64, or past an ID3v2 tag in front, the
// tag's length and 64 more. Never more than `size`, the file's length.
u64 decoder_head_bytes(Span<const u8> first, u64 size);

// "ok", "end", "damaged", "wait".
const char* decode_status_name(DecodeStatus s);

} // namespace kv
