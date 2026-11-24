// Korvid hifi: the stage contract. A stage is the decoder contract's sibling: a record of
// functions over memory it is handed, that changes samples in place. It opens nothing,
// allocates nothing and calls nothing.
//
//   void* s = d->open(format, memory);          // memory: d->memory_bytes of it
//   d->set(s, 0, -6.0);                         // a parameter, by index
//   d->reset(s);                                // forget history: a source starts
//   d->process(s, frames, n);                   // n interleaved frames of f64, in place
//
// hifi runs a fixed chain of them in each render. A render's parameters never change
// while it runs: a change is a new render (a recut), joined with a fade, so a stage
// needs no smoothing of its own. With every stage flat, the render passes the decoder's
// integers through untouched and makes no float. modules/hifi/README.md has the rest.
#pragma once
#include "korvid/hifi_codec/decoder.h"

namespace kv {

constexpr u32 kHifiMaxStages = 4;  // stages in a chain
constexpr u32 kHifiMaxParams = 48; // parameters a stage has

// One of a stage's parameters: enough of a description that a control can be drawn for
// it by something that has never heard of the stage.
struct StageParam {
    const char* name = "";
    f64         initial = 0.0; // what a new chain starts with
    f64         lo = 0.0, hi = 0.0; // what set() keeps it within
    f64         step = 0.0;    // the smallest change worth offering; 0: any. 1 with lo 0 and hi 1 is a switch
    const char* unit = "";     // "dB", "Hz"; "" when it has none
};

// A preset: a name and a value for every parameter, in order. Data the stage carries.
struct StagePreset {
    const char*     name = "";
    Span<const f64> values;
};

struct StageDesc {
    const char*       name = "";        // "gain"
    u32               memory_bytes = 0; // the block open() wants
    Span<const StageParam> params;
    Span<const StagePreset> presets = {}; // none for most

    // Sets up for a stream of this rate and channel count, inside `memory`, at its
    // parameters' initial values. Never fails: a stage takes any format hifi plays.
    void* (*open)(const AudioFormat& format, Span<u8> memory) = nullptr;
    // A parameter, by index; values outside [lo, hi] are brought inside.
    void  (*set)(void* state, u32 param, f64 value) = nullptr;
    // Changes `n` interleaved frames in place. Full scale is +-1.
    void  (*process)(void* state, f64* frames, u32 n) = nullptr;
    // Whether, with its parameters as they are, it passes audio unchanged.
    bool  (*flat)(const void* state) = nullptr;
    // How many frames of input it needs before its output has settled.
    u32   (*run_in)(const void* state) = nullptr;
    // Forgets its history, as at the start of a source.
    void  (*reset)(void* state) = nullptr;
};

} // namespace kv
