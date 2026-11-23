// Korvid hifi_eq: the stages of Blackwing's chain, on hifi's stage contract
// (korvid/hifi/stage.h). The chain is fixed in this slice: gain, then the equalizer.
//
//   constinit const StageDesc* const chain[] = {&k_gain_stage, &k_eq_stage};
#pragma once
#include "korvid/hifi/stage.h"

namespace kv {

// Volume. Parameters: 0, "gain_db", 0 at most (-100 to 0, initially 0); 1, "mute", 0 or 1.
// Flat at 0 dB and not muted. Mute is its own parameter, so unmuting returns to the level.
extern const StageDesc k_gain_stage;

enum GainParam : u32 { kGainDb = 0, kGainMute = 1 };

// The equalizer: ten bands, each a peak or a shelf with a frequency, a gain and a Q
// (ordinary biquads from the RBJ cookbook, in f64), after an attenuation by the highest
// point of their combined response, so no setting raises a full-scale sine past full
// scale. Flat when every band's gain is 0. Parameters, four a band: 4b + 0, the kind
// (0 peak, 1 low shelf, 2 high shelf); 4b + 1, the frequency in Hz; 4b + 2, the gain in
// dB (-24 to 24); 4b + 3, the Q. The bands begin on octaves from 31.25 Hz to 16 kHz,
// the first a low shelf and the last a high shelf, at 0 dB.
extern const StageDesc k_eq_stage;

constexpr u32 kEqBands = 10;

struct EqBand {
    enum Kind : u32 { Peak = 0, LowShelf = 1, HighShelf = 2 };
    Kind kind    = Peak;
    f64  freq    = 0.0;  // 0 in a preset: the default band, flat
    f64  gain_db = 0.0;
    f64  q       = 1.41;
};

// Presets are data: a name and ten bands.
struct EqPreset {
    const char* name;
    EqBand      bands[kEqBands];
};
Span<const EqPreset> eq_presets();
const EqPreset* eq_preset(const char* name); // null when there is none of that name
EqBand eq_preset_band(const EqPreset& p, u32 band); // a band the preset leaves out is the default one, flat

} // namespace kv
