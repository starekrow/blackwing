// The gain stage: a multiply, with no history.
#include "korvid/hifi_eq/stages.h"
#include <math.h>
#include <new>

namespace kv {

namespace {

struct Gain {
    f64  db    = 0.0;
    bool mute  = false;
    f64  scale = 1.0;
    u16  channels = 0;
};

constinit const StageParam k_gain_params[] = {
    {.name = "gain_db", .initial = 0.0, .lo = -100.0, .hi = 0.0},
    {.name = "mute",    .initial = 0.0, .lo = 0.0,    .hi = 1.0},
};

void* gain_open(const AudioFormat& f, Span<u8> memory) {
    Gain* g = new (memory.data) Gain();
    g->channels = f.channels;
    return g;
}

void gain_set(void* state, u32 param, f64 value) {
    Gain& g = *(Gain*)state;
    if (param == kGainDb) g.db = value > 0.0 ? 0.0 : value < -100.0 ? -100.0 : value;
    if (param == kGainMute) g.mute = value >= 0.5;
    g.scale = g.mute ? 0.0 : g.db == 0.0 ? 1.0 : pow(10.0, g.db / 20.0);
}

void gain_process(void* state, f64* frames, u32 n) {
    const Gain& g = *(const Gain*)state;
    if (g.scale == 1.0) return;
    usize k = (usize)n * g.channels;
    for (usize i = 0; i < k; ++i) frames[i] *= g.scale;
}

bool gain_flat(const void* state) { const Gain& g = *(const Gain*)state; return g.db == 0.0 && !g.mute; }
u32  gain_run_in(const void*) { return 0; }
void gain_reset(void*) {}

} // namespace

constinit const StageDesc k_gain_stage = {
    .name         = "gain",
    .memory_bytes = sizeof(Gain),
    .params       = k_gain_params,
    .open         = gain_open,
    .set          = gain_set,
    .process      = gain_process,
    .flat         = gain_flat,
    .run_in       = gain_run_in,
    .reset        = gain_reset,
};

} // namespace kv
