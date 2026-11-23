// Korvid hifi_codec: the decoder contract, and finding the decoder for a file.
//
// A decoder turns the bytes of one audio file into PCM. The contract is a record of
// functions, DecoderDesc, over a state the decoder keeps in memory the caller gives it:
//
//   const DecoderDesc* d = decoder_for(bytes);            // null: nobody's kind of file
//   DecoderOpen o = d->open(bytes, memory);                // memory: d->memory_bytes of it
//   if (!o.state) ... o.reason says why, in words
//   for (;;) {
//       DecodeResult r = d->decode(o.state, out, frames);  // out: frames * channels i32
//       ... r.frames frames are in out
//       if (r.status != DecodeStatus::Ok) break;           // End, or Damaged with r.reason
//   }
//   d->close(o.state);
//
// What a decoder may not do is the point. It opens no file and names no path: it sees
// the bytes it is handed and nothing else. It takes no memory but the block it is handed.
// It calls nothing of its caller's. It keeps no state outside that block, so two of one
// kind run side by side, and one can be dropped by forgetting its memory. A decoder that
// keeps to this can be moved into another process without its caller changing
// (docs/PRDs/Blackwing-Master-PRD.md, "Loading through codecs").
//
// Samples are integers at the file's own depth, not floats, so "the file's samples" means
// something exact. modules/hifi_codec/README.md has the rest.
#pragma once
#include "korvid/core/types.h"

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
};

// open()'s answer.
struct DecoderOpen {
    void*       state  = nullptr; // inside the memory handed to open(); null when it failed
    const char* reason = "";      // when it failed: why, in words; valid while the memory is
    DecoderInfo info;
};

enum class DecodeStatus : u8 {
    Ok,      // more to come
    End,     // the stream ended where the file said it would, or the file did not say
    Damaged, // the stream ended early or could not be read; what was returned is good
    Count
};

// decode()'s answer.
struct DecodeResult {
    u32          frames = 0;               // frames written, at most what was asked
    DecodeStatus status = DecodeStatus::End;
    const char*  reason = "";              // with Damaged: why, in words; valid until close()
};

// One kind of decoder. Every function is safe on any thread, on its own state; none
// allocates, blocks or logs.
struct DecoderDesc {
    const char* name = "";        // "flac"
    u32         memory_bytes = 0; // the block open() wants: enough for any file of this kind it will accept

    // Whether `bytes` starts a file of this kind. The first 64 bytes are enough to say no;
    // a yes is a promise only that open() is worth trying.
    bool (*probe)(Span<const u8> bytes) = nullptr;

    // Reads the headers of the whole file in `bytes` and sets up to decode from its first
    // frame. `bytes` and `memory` must stay where they are, unchanged, until close(). On
    // failure state is null, reason says why, and `memory` holds nothing worth keeping.
    DecoderOpen (*open)(Span<const u8> bytes, Span<u8> memory) = nullptr;

    // Decodes up to `frames` frames into `out` (frames * channels samples, interleaved).
    // Fewer than asked only with End or Damaged; after either, every call returns 0 frames
    // and the same status.
    DecodeResult (*decode)(void* state, i32* out, u32 frames) = nullptr;

    // Ends the decoder's use of its memory. It has nothing else to give back.
    void (*close)(void* state) = nullptr;
};

// The decoders linked in, in the order they are asked.
Span<const DecoderDesc* const> decoders();

// The first decoder whose probe accepts `bytes`; null when none does.
const DecoderDesc* decoder_for(Span<const u8> bytes);

// "ok", "end", "damaged".
const char* decode_status_name(DecodeStatus s);

} // namespace kv
