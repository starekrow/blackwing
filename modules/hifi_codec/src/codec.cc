// The list of decoders and the search through it.
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_codec/flac.h"
#include "korvid/hifi_codec/mp3.h"
#include <string.h>

namespace kv {

namespace {

constinit const DecoderDesc* const k_decoders[] = {&k_flac_decoder, &k_mp3_decoder};

} // namespace

Span<const DecoderDesc* const> decoders() { return k_decoders; }

const DecoderDesc* decoder_for(Span<const u8> head) {
    for (const DecoderDesc* d : k_decoders)
        if (d->probe(head)) return d;
    return nullptr;
}

u64 decoder_head_bytes(Span<const u8> first, u64 size) {
    // Past every ID3v2 tag in a row (iTunes has been seen to write two), as far as `first`
    // shows them: a tag whose header is beyond it is asked for, header and all, and the
    // caller asks again with more.
    u64 at = 0;
    for (u32 tags = 0; tags < 4; ++tags) {
        if (at + 10 > first.len) { if (tags) { u64 w = at + 10; return w < size ? w : size; } break; }
        const u8* h = first.data + at;
        if (memcmp(h, "ID3", 3) != 0 || ((h[6] | h[7] | h[8] | h[9]) & 0x80)) break;
        at += 10 + (((u64)h[6] << 21) | ((u64)h[7] << 14) | ((u64)h[8] << 7) | h[9]) + ((h[5] & 0x10) ? 10u : 0u);
    }
    // And past zeros after them: padding a tag did not count in its size (iTunes again, some
    // hundreds of bytes), up to kPadding of it.
    constexpr u64 kPadding = 1u << 16;
    u64 zeros = 0;
    while (at + zeros < first.len && zeros < kPadding && first.data[at + zeros] == 0) ++zeros;
    if (!at) zeros = 0; // only after a tag
    if (zeros && at + zeros == first.len && zeros < kPadding && first.len < size) {
        u64 w = first.len + 4096; // the zeros go on past what was given: ask for more
        return w < size ? w : size;
    }
    u64 want = at + zeros + 64;
    return want < size ? want : size;
}

const char* decode_status_name(DecodeStatus s) {
    switch (s) {
    case DecodeStatus::Ok:      return "ok";
    case DecodeStatus::End:     return "end";
    case DecodeStatus::Damaged: return "damaged";
    case DecodeStatus::Wait:    return "wait";
    case DecodeStatus::Count:   break;
    }
    return "?";
}

} // namespace kv
