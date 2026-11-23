// Korvid hifi_codec: the MP3 decoder, on the decoder contract.
//
// MPEG 1, 2 and 2.5 Layer III, mono and stereo, CBR, VBR and free format, with any number
// of ID3v2 tags in front and ID3v1 and APEv2 tags behind, over dr_mp3 (vendored). Samples
// are dr_mp3's floats rounded to 24 bits: "the file's samples" for a lossy file means this
// decoder's. The length is the Xing, Info or VBRI header's, or an estimate from the size
// (DecoderInfo::estimated); delay and padding are the LAME tag's. Seeks land on the frame
// asked for, by reading forward from the nearest place the decoder has been. Reach it
// through decoder_for() (korvid/hifi_codec/decoder.h); this is for a caller that wants
// this decoder by name.
#pragma once
#include "korvid/hifi_codec/decoder.h"

namespace kv {

// The MP3 decoder's record.
extern const DecoderDesc k_mp3_decoder;

} // namespace kv
