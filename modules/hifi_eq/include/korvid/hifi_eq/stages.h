// Korvid hifi_eq: the stages of Blackwing's chain, on hifi's stage contract
// (korvid/hifi/stage.h). The chain is fixed in this slice: gain, then the equalizer.
#pragma once
#include "korvid/hifi/stage.h"

namespace kv {

// Volume. Parameters: 0, "gain_db", 0 at most (-100 to 0, initially 0); 1, "mute", 0 or 1.
// Flat at 0 dB and not muted. Mute is its own parameter, so unmuting returns to the level.
extern const StageDesc k_gain_stage;

enum GainParam : u32 { kGainDb = 0, kGainMute = 1 };

} // namespace kv
