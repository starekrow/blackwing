// The list of decoders and the search through it.
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_codec/flac.h"

namespace kv {

namespace {

constinit const DecoderDesc* const k_decoders[] = {&k_flac_decoder};

} // namespace

Span<const DecoderDesc* const> decoders() { return k_decoders; }

const DecoderDesc* decoder_for(Span<const u8> bytes) {
    for (const DecoderDesc* d : k_decoders)
        if (d->probe(bytes)) return d;
    return nullptr;
}

const char* decode_status_name(DecodeStatus s) {
    switch (s) {
    case DecodeStatus::Ok:      return "ok";
    case DecodeStatus::End:     return "end";
    case DecodeStatus::Damaged: return "damaged";
    case DecodeStatus::Count:   break;
    }
    return "?";
}

} // namespace kv
