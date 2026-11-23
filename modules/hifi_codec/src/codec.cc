// The list of decoders and the search through it.
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_codec/flac.h"
#include <string.h>

namespace kv {

namespace {

constinit const DecoderDesc* const k_decoders[] = {&k_flac_decoder};

} // namespace

Span<const DecoderDesc* const> decoders() { return k_decoders; }

const DecoderDesc* decoder_for(Span<const u8> head) {
    for (const DecoderDesc* d : k_decoders)
        if (d->probe(head)) return d;
    return nullptr;
}

u64 decoder_head_bytes(Span<const u8> first, u64 size) {
    u64 want = 64;
    if (first.len >= 10 && memcmp(first.data, "ID3", 3) == 0 && !((first.data[6] | first.data[7] | first.data[8] | first.data[9]) & 0x80)) {
        const u8* h = first.data;
        u64 tag = 10 + (((u64)h[6] << 21) | ((u64)h[7] << 14) | ((u64)h[8] << 7) | h[9]) + ((h[5] & 0x10) ? 10u : 0u);
        want = tag + 64;
    }
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
