// The gain stage on its own: flat at 0 dB, the level it says, mute that keeps the level,
// values kept within what it allows.
#include "korvid/hifi_eq/stages.h"
#include "korvid/core/test.h"
#include <math.h>

using namespace kv;

void test_gain(TestCtx& c) {
    c.begin("gain");
    const StageDesc& d = k_gain_stage;
    alignas(16) u8 mem[256];
    KV_CHECK(c, d.memory_bytes <= sizeof mem && d.params.len == 2 && d.run_in(nullptr) == 0);
    void* g = d.open(AudioFormat{48000, 2, 24}, Span<u8>(mem, sizeof mem));
    KV_CHECK(c, d.flat(g));
    f64 x[4] = {0.5, -0.25, 1.0, -1.0};
    d.process(g, x, 2);
    KV_CHECK(c, x[0] == 0.5 && x[3] == -1.0);                  // flat: untouched
    d.set(g, kGainDb, -6.0);
    KV_CHECK(c, !d.flat(g));
    d.process(g, x, 2);
    KV_CHECK(c, fabs(x[0] - 0.5 * pow(10.0, -6.0 / 20.0)) < 1e-15);
    d.set(g, kGainMute, 1.0);
    f64 y[2] = {0.7, -0.7};
    d.process(g, y, 1);
    KV_CHECK(c, y[0] == 0.0 && y[1] == 0.0 && !d.flat(g));
    d.set(g, kGainMute, 0.0);                                  // unmuted: the level it had
    f64 z[2] = {1.0, 1.0};
    d.process(g, z, 1);
    KV_CHECK(c, fabs(z[0] - pow(10.0, -6.0 / 20.0)) < 1e-15);
    d.set(g, kGainDb, 12.0);                                   // above 0 dB: kept at 0
    KV_CHECK(c, d.flat(g));
    d.set(g, kGainDb, -1000.0);
    f64 w[2] = {1.0, 1.0};
    d.process(g, w, 1);
    KV_CHECK(c, fabs(w[0] - 1e-5) < 1e-18);                    // -100 dB at the least
}
