// Korvid hifi_codec: the FLAC decoder, on the decoder contract.
//
// Native FLAC (a file that starts "fLaC", with or without an ID3v2 tag in front of it), up
// to 8 channels and 32 bits, over dr_flac (vendored). Ogg FLAC is not read. Frames whose
// checksum fails are dropped, not played, and the decode ends Damaged with the count.
// Reach it through decoder_for() (korvid/hifi_codec/decoder.h); this is for a caller that
// wants this decoder by name.
#pragma once
#include "korvid/hifi_codec/decoder.h"

namespace kv {

// The FLAC decoder's record.
extern const DecoderDesc k_flac_decoder;

} // namespace kv
