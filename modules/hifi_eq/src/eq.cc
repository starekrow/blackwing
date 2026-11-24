// The equalizer: ten biquad bands in f64 (the RBJ Audio EQ Cookbook's peaking and shelf
// designs), after an attenuation by the highest point of their combined response, so no
// setting can raise a full-scale sine past full scale. Coefficients and the attenuation
// are worked out when a parameter has changed, on the first process() after it: a chain
// is opened and all forty parameters set in one go, and that is one design, not forty.
#include "korvid/hifi_eq/stages.h"
#include <math.h>
#include <new>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kChannels = 8;
constexpr f64 kPi = 3.14159265358979323846;

struct Biquad { f64 b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

struct Eq {
    EqBand  band[kEqBands];
    Biquad  coef[kEqBands];
    f64     z[kEqBands][kChannels][2] = {}; // transposed direct form II: two of state per band and channel
    f64     pre = 1.0;                      // the attenuation, as a factor
    f64     rate = 48000.0;
    u16     channels = 2;
    bool    dirty = true;
};

// The cookbook's designs. A band at 0 dB is a pass-through, exactly.
Biquad design(const EqBand& b, f64 rate) {
    Biquad q;
    if (b.gain_db == 0.0) return q;
    f64 f = b.freq < 1.0 ? 1.0 : b.freq > rate * 0.49 ? rate * 0.49 : b.freq;
    f64 A = pow(10.0, b.gain_db / 40.0);
    f64 w = 2.0 * kPi * f / rate, cw = cos(w), sw = sin(w);
    f64 Q = b.q < 0.1 ? 0.1 : b.q;
    f64 alpha = sw / (2.0 * Q);
    f64 b0, b1, b2, a0, a1, a2;
    if (b.kind == EqBand::Peak) {
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else {
        f64 sa = 2.0 * sqrt(A) * alpha;
        if (b.kind == EqBand::LowShelf) {
            b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa;       a1 = -2 * ((A - 1) + (A + 1) * cw);    a2 = (A + 1) + (A - 1) * cw - sa;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa;       a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - sa;
        }
    }
    q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0;
    return q;
}

// |H| of one biquad at frequency f.
f64 magnitude(const Biquad& q, f64 f, f64 rate) {
    f64 w = 2.0 * kPi * f / rate;
    f64 c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    f64 nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = -(q.b1 * s1 + q.b2 * s2);
    f64 dr = 1.0 + q.a1 * c1 + q.a2 * c2, di = -(q.a1 * s1 + q.a2 * s2);
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

f64 response(const Eq& e, f64 f) {
    f64 m = 1.0;
    for (u32 i = 0; i < kEqBands; ++i) m *= magnitude(e.coef[i], f, e.rate);
    return m;
}

// The coefficients, and the attenuation: the combined response's highest point, found on
// a fine logarithmic grid from 10 Hz to Nyquist and then refined around the best of it.
void update(Eq& e) {
    e.dirty = false;
    bool any = false;
    for (u32 i = 0; i < kEqBands; ++i) { e.coef[i] = design(e.band[i], e.rate); any = any || e.band[i].gain_db != 0.0; }
    e.pre = 1.0;
    if (!any) return;
    const u32 kGrid = 1024;
    f64 lo = log(10.0), hi = log(e.rate * 0.5), best = 0.0, best_x = lo;
    for (u32 k = 0; k <= kGrid; ++k) {
        f64 x = lo + (hi - lo) * k / kGrid;
        f64 m = response(e, exp(x));
        if (m > best) { best = m; best_x = x; }
    }
    f64 step = (hi - lo) / kGrid; // golden-section search either side of the best point
    f64 a = best_x - step, b = best_x + step;
    if (a < lo) a = lo;
    if (b > hi) b = hi;
    const f64 g = 0.6180339887498949;
    for (u32 k = 0; k < 40; ++k) {
        f64 c = b - g * (b - a), d = a + g * (b - a);
        if (response(e, exp(c)) > response(e, exp(d))) b = d; else a = c;
    }
    f64 m = response(e, exp((a + b) * 0.5));
    if (m > best) best = m;
    if (best > 1.0) e.pre = 1.0 / best;
}

constinit const EqBand k_default[kEqBands] = {
    {EqBand::LowShelf, 31.25, 0.0, 0.707}, {EqBand::Peak, 62.5, 0.0, 1.41}, {EqBand::Peak, 125.0, 0.0, 1.41},
    {EqBand::Peak, 250.0, 0.0, 1.41},     {EqBand::Peak, 500.0, 0.0, 1.41}, {EqBand::Peak, 1000.0, 0.0, 1.41},
    {EqBand::Peak, 2000.0, 0.0, 1.41},    {EqBand::Peak, 4000.0, 0.0, 1.41}, {EqBand::Peak, 8000.0, 0.0, 1.41},
    {EqBand::HighShelf, 16000.0, 0.0, 0.707},
};

// The parameters, four a band: its kind, frequency, gain and Q.
struct ParamTable {
    StageParam p[kEqBands * 4];
    char names[kEqBands * 4][16];
    constexpr ParamTable() : p(), names() {
        for (u32 i = 0; i < kEqBands; ++i) {
            const char* what[4] = {"kind", "freq", "gain", "q"};
            for (u32 k = 0; k < 4; ++k) {
                char* n = names[i * 4 + k];
                u32 at = 0;
                n[at++] = 'b'; n[at++] = (char)('0' + i);
                n[at++] = '.';
                for (const char* w = what[k]; *w; ++w) n[at++] = *w;
                n[at] = 0;
            }
            p[i * 4 + 0] = StageParam{names[i * 4 + 0], (f64)k_default[i].kind, 0.0, 2.0, 1.0, ""};
            p[i * 4 + 1] = StageParam{names[i * 4 + 1], k_default[i].freq, 10.0, 24000.0, 0.0, "Hz"};
            p[i * 4 + 2] = StageParam{names[i * 4 + 2], 0.0, -24.0, 24.0, 0.5, "dB"};
            p[i * 4 + 3] = StageParam{names[i * 4 + 3], k_default[i].q, 0.1, 10.0, 0.01, ""};
        }
    }
};
constinit const ParamTable k_params;

void* eq_open(const AudioFormat& f, Span<u8> memory) {
    Eq* e = new (memory.data) Eq();
    memcpy(e->band, k_default, sizeof k_default);
    e->rate = f.rate ? (f64)f.rate : 48000.0;
    e->channels = f.channels > kChannels ? (u16)kChannels : f.channels;
    return e;
}

void eq_set(void* state, u32 param, f64 value) {
    Eq& e = *(Eq*)state;
    if (param >= kEqBands * 4) return;
    const StageParam& sp = k_params.p[param];
    f64 v = value < sp.lo ? sp.lo : value > sp.hi ? sp.hi : value;
    EqBand& b = e.band[param / 4];
    switch (param % 4) {
    case 0: b.kind = (EqBand::Kind)(u32)(v + 0.5); break;
    case 1: b.freq = v; break;
    case 2: b.gain_db = v; break;
    case 3: b.q = v; break;
    }
    e.dirty = true;
}

void eq_process(void* state, f64* x, u32 n) {
    Eq& e = *(Eq*)state;
    if (e.dirty) update(e);
    const u32 ch = e.channels;
    if (e.pre != 1.0) for (usize i = 0; i < (usize)n * ch; ++i) x[i] *= e.pre;
    for (u32 i = 0; i < kEqBands; ++i) {
        if (e.band[i].gain_db == 0.0) continue;
        const Biquad q = e.coef[i];
        for (u32 c = 0; c < ch; ++c) {
            f64 z1 = e.z[i][c][0], z2 = e.z[i][c][1];
            for (u32 k = 0; k < n; ++k) {
                f64 in = x[(usize)k * ch + c];
                f64 out = q.b0 * in + z1;
                z1 = q.b1 * in - q.a1 * out + z2;
                z2 = q.b2 * in - q.a2 * out;
                x[(usize)k * ch + c] = out;
            }
            e.z[i][c][0] = z1; e.z[i][c][1] = z2;
        }
    }
}

bool eq_flat(const void* state) {
    const Eq& e = *(const Eq*)state;
    for (u32 i = 0; i < kEqBands; ++i) if (e.band[i].gain_db != 0.0) return false;
    return true;
}

// Long enough for the slowest band to have settled: twelve of its time constants
// (Q / (pi f) seconds), a band's ringing down by a hundred thousand times. At most half a
// second, which is what the path keeps behind the frame heard.
u32 eq_run_in(const void* state) {
    const Eq& e = *(const Eq*)state;
    f64 longest = 0.0;
    for (u32 i = 0; i < kEqBands; ++i) {
        if (e.band[i].gain_db == 0.0) continue;
        f64 tau = (e.band[i].q < 0.5 ? 0.5 : e.band[i].q) / (kPi * (e.band[i].freq < 10.0 ? 10.0 : e.band[i].freq));
        if (tau > longest) longest = tau;
    }
    f64 frames = 12.0 * longest * e.rate;
    f64 cap = 0.5 * e.rate;
    return (u32)(frames < cap ? frames : cap);
}

void eq_reset(void* state) { Eq& e = *(Eq*)state; memset(e.z, 0, sizeof e.z); }

constinit const EqPreset k_presets[] = {
    {"flat", {}},
    {"bass", {{EqBand::LowShelf, 31.25, 6.0, 0.707}, {EqBand::Peak, 62.5, 4.0, 1.41}, {EqBand::Peak, 125.0, 2.0, 1.41}}},
    {"treble", {{}, {}, {}, {}, {}, {}, {EqBand::Peak, 2000.0, 1.0, 1.41}, {EqBand::Peak, 4000.0, 3.0, 1.41}, {EqBand::Peak, 8000.0, 4.0, 1.41}, {EqBand::HighShelf, 16000.0, 6.0, 0.707}}},
    {"loudness", {{EqBand::LowShelf, 31.25, 8.0, 0.707}, {EqBand::Peak, 62.5, 5.0, 1.41}, {}, {}, {}, {EqBand::Peak, 1000.0, -2.0, 1.41}, {}, {EqBand::Peak, 4000.0, 2.0, 1.41}, {EqBand::Peak, 8000.0, 4.0, 1.41}, {EqBand::HighShelf, 16000.0, 6.0, 0.707}}},
    {"vocal", {{EqBand::LowShelf, 31.25, -4.0, 0.707}, {EqBand::Peak, 62.5, -3.0, 1.41}, {}, {}, {EqBand::Peak, 500.0, 1.0, 1.41}, {EqBand::Peak, 1000.0, 3.0, 1.41}, {EqBand::Peak, 2000.0, 4.0, 1.41}, {EqBand::Peak, 4000.0, 2.0, 1.41}}},
    {"rock", {{EqBand::LowShelf, 31.25, 5.0, 0.707}, {EqBand::Peak, 62.5, 4.0, 1.41}, {EqBand::Peak, 125.0, 2.0, 1.41}, {}, {EqBand::Peak, 500.0, -2.0, 1.41}, {EqBand::Peak, 1000.0, -1.0, 1.41}, {}, {EqBand::Peak, 4000.0, 3.0, 1.41}, {EqBand::Peak, 8000.0, 4.0, 1.41}, {EqBand::HighShelf, 16000.0, 5.0, 0.707}}},
    {"extreme", {{EqBand::LowShelf, 31.25, 24.0, 0.707}, {EqBand::Peak, 62.5, 24.0, 4.0}, {EqBand::Peak, 125.0, 24.0, 10.0}, {EqBand::Peak, 250.0, 24.0, 0.3}, {EqBand::Peak, 500.0, 24.0, 10.0}, {EqBand::Peak, 1000.0, 24.0, 1.41}, {EqBand::Peak, 2000.0, 24.0, 10.0}, {EqBand::Peak, 4000.0, 24.0, 0.5}, {EqBand::Peak, 8000.0, 24.0, 10.0}, {EqBand::HighShelf, 16000.0, 24.0, 2.0}}},
};

// The presets as the stage contract has them: a value for each of the forty parameters.
constexpr u32 kEqPresets = KV_COUNTOF(k_presets);
struct PresetTable {
    f64         values[kEqPresets][kEqBands * 4];
    StagePreset list[kEqPresets];
    constexpr PresetTable() : values(), list() {
        for (u32 i = 0; i < kEqPresets; ++i) {
            for (u32 b = 0; b < kEqBands; ++b) {
                EqBand band = k_presets[i].bands[b];
                if (band.freq == 0.0) { band = k_default[b]; band.gain_db = 0.0; } // a band left out: the default one, flat
                values[i][b * 4 + 0] = (f64)band.kind;
                values[i][b * 4 + 1] = band.freq;
                values[i][b * 4 + 2] = band.gain_db;
                values[i][b * 4 + 3] = band.q;
            }
            list[i] = StagePreset{k_presets[i].name, Span<const f64>(values[i], kEqBands * 4)};
        }
    }
};
constinit const PresetTable k_preset_table;

} // namespace

constinit const StageDesc k_eq_stage = {
    .name         = "eq",
    .memory_bytes = sizeof(Eq),
    .params       = k_params.p,
    .presets      = k_preset_table.list,
    .open         = eq_open,
    .set          = eq_set,
    .process      = eq_process,
    .flat         = eq_flat,
    .run_in       = eq_run_in,
    .reset        = eq_reset,
};

Span<const EqPreset> eq_presets() { return k_presets; }

const EqPreset* eq_preset(const char* name) {
    for (const EqPreset& p : k_presets) if (strcmp(p.name, name) == 0) return &p;
    return nullptr;
}

// A preset's band, filled in: a band it leaves out is the default band, flat.
EqBand eq_preset_band(const EqPreset& p, u32 band) {
    EqBand b = p.bands[band];
    if (b.freq == 0.0) { b = k_default[band]; b.gain_db = 0.0; }
    return b;
}

} // namespace kv
