// The equalizer on its own. Its measured response is within 0.1 dB of its design: a
// sine through it, after its run-in, against the cookbook's formulas worked out here
// again (and the attenuation they imply). No preset raises a full-scale sine past full
// scale, at any of sixty frequencies across the audio band. Flat is untouched.
#include "korvid/hifi_eq/stages.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include <math.h>

using namespace kv;

namespace {

constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kRate = 48000.0;

// The cookbook, again: one band's |H| at f.
f64 band_mag(const EqBand& b, f64 f) {
    if (b.gain_db == 0.0) return 1.0;
    f64 A = pow(10.0, b.gain_db / 40.0), w0 = 2 * kPi * b.freq / kRate, al = sin(w0) / (2 * b.q), cw = cos(w0);
    f64 n[3], d[3];
    if (b.kind == EqBand::Peak) { n[0] = 1 + al * A; n[1] = -2 * cw; n[2] = 1 - al * A; d[0] = 1 + al / A; d[1] = -2 * cw; d[2] = 1 - al / A; }
    else {
        f64 s = 2 * sqrt(A) * al, sg = b.kind == EqBand::LowShelf ? 1.0 : -1.0;
        n[0] = A * ((A + 1) - sg * (A - 1) * cw + s); n[1] = sg * 2 * A * ((A - 1) - sg * (A + 1) * cw); n[2] = A * ((A + 1) - sg * (A - 1) * cw - s);
        d[0] = (A + 1) + sg * (A - 1) * cw + s;       d[1] = -sg * 2 * ((A - 1) + sg * (A + 1) * cw);    d[2] = (A + 1) + sg * (A - 1) * cw - s;
    }
    f64 w = 2 * kPi * f / kRate;
    f64 nr = n[0] + n[1] * cos(w) + n[2] * cos(2 * w), ni = -(n[1] * sin(w) + n[2] * sin(2 * w));
    f64 dr = d[0] + d[1] * cos(w) + d[2] * cos(2 * w), di = -(d[1] * sin(w) + d[2] * sin(2 * w));
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}
f64 design(const EqBand* bands, f64 f) { f64 m = 1.0; for (u32 i = 0; i < kEqBands; ++i) m *= band_mag(bands[i], f); return m; }
f64 design_peak(const EqBand* bands) {
    f64 best = 1.0;
    for (u32 k = 0; k <= 20000; ++k) { f64 m = design(bands, 10.0 * pow(2400.0, k / 20000.0)); if (m > best) best = m; }
    return best;
}

struct Rig {
    u8* mem = nullptr;
    void* e = nullptr;
    EqBand bands[kEqBands];
    void make() {
        mem = (u8*)mem_alloc(k_eq_stage.memory_bytes);
        e = k_eq_stage.open(AudioFormat{48000, 2, 24}, Span<u8>(mem, k_eq_stage.memory_bytes));
        const EqPreset* flat = eq_preset("flat");
        for (u32 i = 0; i < kEqBands; ++i) set_band(i, eq_preset_band(*flat, i));
    }
    void set_band(u32 i, const EqBand& b) {
        bands[i] = b;
        k_eq_stage.set(e, i * 4 + 0, (f64)b.kind); k_eq_stage.set(e, i * 4 + 1, b.freq);
        k_eq_stage.set(e, i * 4 + 2, b.gain_db);   k_eq_stage.set(e, i * 4 + 3, b.q);
    }
    // The steady amplitude of a sine of amplitude `amp` at f, through the stage: after
    // the run-in (and a second), by correlation over the next half second. And the
    // largest sample of that half second.
    f64 measure(f64 f, f64 amp, f64* largest = nullptr) {
        k_eq_stage.reset(e);
        const u32 chunk = 4800;
        f64 buf[chunk * 2];
        u64 settle = k_eq_stage.run_in(e) + 48000, total = settle + 24000, t = 0;
        f64 si = 0, co = 0, top = 0;
        u64 counted = 0;
        while (t < total) {
            for (u32 k = 0; k < chunk; ++k) { f64 v = amp * sin(2 * kPi * f * (f64)(t + k) / kRate); buf[k * 2] = buf[k * 2 + 1] = v; }
            k_eq_stage.process(e, buf, chunk);
            for (u32 k = 0; k < chunk; ++k, ++t) {
                if (t < settle) continue;
                f64 ph = 2 * kPi * f * (f64)t / kRate;
                si += buf[k * 2] * sin(ph); co += buf[k * 2] * cos(ph);
                ++counted;
                f64 a = fabs(buf[k * 2]); if (a > top) top = a;
            }
        }
        if (largest) *largest = top;
        return 2.0 * sqrt(si * si + co * co) / (f64)counted / amp;
    }
    void free() { mem_free(mem); }
};

} // namespace

void test_eq(TestCtx& c) {
    c.begin("eq");
    Rig r; r.make();
    KV_CHECK(c, k_eq_stage.params.len == kEqBands * 4 && k_eq_stage.flat(r.e));
    {
        f64 x[8] = {0.1, -0.2, 0.3, -0.4, 0.99, -1.0, 0.0, 0.5}, y[8];
        for (u32 i = 0; i < 8; ++i) y[i] = x[i];
        k_eq_stage.process(r.e, y, 4);
        bool same = true;
        for (u32 i = 0; i < 8; ++i) same = same && x[i] == y[i];
        KV_CHECK(c, same);                                           // flat: bit for bit
    }
    r.free();
    // One band at a time, and two together: the response measured where it bends.
    struct Case { u32 band; EqBand b; };
    const Case cases[] = {
        {5, {EqBand::Peak, 1000.0, 6.0, 1.41}},   {2, {EqBand::Peak, 125.0, -9.0, 4.0}},
        {0, {EqBand::LowShelf, 31.25, 6.0, 0.707}}, {9, {EqBand::HighShelf, 16000.0, -6.0, 0.707}},
        {7, {EqBand::Peak, 3000.0, 12.0, 2.0}},   {4, {EqBand::Peak, 500.0, -24.0, 10.0}},
    };
    const f64 freqs[] = {40.0, 100.0, 125.0, 300.0, 500.0, 1000.0, 3000.0, 6000.0, 12000.0, 18000.0};
    f64 worst = 0.0;
    for (u32 pass = 0; pass < 2; ++pass)
        for (const Case& k : cases) {
            r.make();
            r.set_band(k.band, k.b);
            if (pass) r.set_band((k.band + 3) % kEqBands, EqBand{EqBand::Peak, 2000.0, 4.0, 1.0}); // and another with it
            KV_CHECK(c, !k_eq_stage.flat(r.e));
            f64 pre = 1.0 / design_peak(r.bands);
            for (f64 f : freqs) {
                f64 want = 20.0 * log10(design(r.bands, f) * pre);
                f64 got = 20.0 * log10(r.measure(f, 0.25));
                f64 err = fabs(got - want);
                if (err > worst) worst = err;
                if (err > 0.1) KV_ERROR("test", "eq: band %u at %.0f Hz: %.3f dB, its design %.3f dB", k.band, f, got, want);
                KV_CHECK(c, err <= 0.1);
            }
            r.free();
        }
    KV_INFO("test", "eq: the measured response within %.4f dB of the design", worst);
    // No preset raises a full-scale sine past full scale.
    u32 presets = 0;
    f64 highest = 0.0;
    for (const EqPreset& p : eq_presets()) {
        r.make();
        for (u32 i = 0; i < kEqBands; ++i) r.set_band(i, eq_preset_band(p, i));
        bool under = true;
        for (u32 k = 0; k < 60; ++k) {
            f64 f = 20.0 * pow(1000.0, k / 59.0), top = 0.0;
            r.measure(f, 1.0, &top);
            if (top > highest) highest = top;
            under = under && top <= 1.0;
        }
        if (!under) KV_ERROR("test", "eq: preset %s clips a full-scale sine", p.name);
        KV_CHECK(c, under);
        ++presets;
        r.free();
    }
    KV_CHECK(c, presets >= 6 && eq_preset("rock") && !eq_preset("nothing"));
    KV_INFO("test", "eq: %u presets, the loudest a full-scale sine came out %.5f of full scale", presets, highest);
    // The run-in: long for a low band, never past half a second; reset forgets.
    r.make();
    KV_CHECK(c, k_eq_stage.run_in(r.e) == 0);
    r.set_band(0, EqBand{EqBand::LowShelf, 31.25, 6.0, 0.707});
    u32 ri = k_eq_stage.run_in(r.e);
    KV_CHECK(c, ri > 1000 && ri <= 24000);
    f64 imp[200] = {};
    imp[0] = imp[1] = 1.0;
    k_eq_stage.process(r.e, imp, 100);
    k_eq_stage.reset(r.e);
    f64 zero[2] = {0.0, 0.0};
    k_eq_stage.process(r.e, zero, 1);
    KV_CHECK(c, zero[0] == 0.0 && zero[1] == 0.0);
    r.free();
}
