// The resampler on its own, measured. For each pair of rates: sines across the passband,
// fitted in the output at the frequency and phase they should have there, give the gain,
// the phase (0: nothing is delayed in the timeline) and what is left over (the filter's and
// the arithmetic's error); a sine in the stopband, its alias; chunks of any size make the
// same output as one call; the drain makes what the input owed.
#include "korvid/hifi_resample/resample.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <math.h>
#include <string.h>

using namespace kv;

namespace {

struct Fit { f64 gain_db; f64 phase; f64 rest_db; };

// y over frames [a, b) of channel 0 against sin and cos at f (cycles per output frame).
Fit fit(const f64* y, u32 ch, u64 a, u64 b, f64 f) {
    f64 ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (u64 k = a; k < b; ++k) {
        f64 s = sin(2.0 * M_PI * f * (f64)k), c = cos(2.0 * M_PI * f * (f64)k), v = y[k * ch];
        ss += s * s; cc += c * c; sc += s * c; ys += v * s; yc += v * c;
    }
    f64 det = ss * cc - sc * sc;
    f64 A = (ys * cc - yc * sc) / det, B = (yc * ss - ys * sc) / det;
    f64 rest = 0, n = 0;
    for (u64 k = a; k < b; ++k) {
        f64 e = y[k * ch] - (A * sin(2.0 * M_PI * f * (f64)k) + B * cos(2.0 * M_PI * f * (f64)k));
        rest += e * e; ++n;
    }
    Fit out;
    f64 amp = sqrt(A * A + B * B);
    out.gain_db = 20.0 * log10(amp);
    out.phase = atan2(B, A);
    out.rest_db = 10.0 * log10(rest / n / (amp * amp / 2.0) + 1e-300);
    return out;
}

// The whole of `in` through a resampler of these rates, drained.
u64 run(u32 from, u32 to, u32 ch, const f64* in, u64 frames, f64* out, u64 out_cap, u32 chunk = 0) {
    usize bytes = resample_memory_bytes(from, to, ch);
    u8* mem = (u8*)mem_alloc(bytes);
    Resampler* r = resample_open(from, to, ch, Span<u8>(mem, bytes));
    u64 at = 0, made = 0;
    u32 step = 0;
    while (at < frames) {
        u32 n = chunk ? 1 + (u32)((step++ * 2654435761u) % chunk) : (u32)(frames - at);
        if (n > frames - at) n = (u32)(frames - at);
        u32 cap = (u32)(out_cap - made < 65536 ? out_cap - made : 65536);
        u32 used, m;
        resample_process(r, in + at * ch, n, &used, out + made * ch, cap, &m);
        at += used;
        made += m;
    }
    for (;;) {
        u32 m;
        bool done = resample_drain(r, out + made * ch, (u32)(out_cap - made), &m);
        made += m;
        if (done) break;
    }
    mem_free(mem);
    return made;
}

} // namespace

void test_resample(TestCtx& c) {
    c.begin("hifi_resample");
    KV_CHECK(c, resample_memory_bytes(44100, 44100, 2) == 0 && resample_memory_bytes(0, 48000, 2) == 0 && resample_memory_bytes(44100, 48000, 9) == 0);

    struct Pair { u32 from, to; };
    const Pair pairs[] = {{44100, 48000}, {48000, 44100}, {44100, 96000}, {96000, 44100}, {48000, 96000}, {192000, 44100}, {44100, 192000}, {88200, 48000}};
    f64 worst_ripple = 0, worst_rest = -400, worst_phase = 0, worst_alias = -400;
    const u64 seconds_frames = 1;
    (void)seconds_frames;
    for (const Pair& p : pairs) {
        const u64 n = p.from / 2;                     // half a second
        const u64 cap = n * p.to / p.from + 64;
        f64* in = (f64*)mem_alloc(n * 2 * sizeof(f64));
        f64* out = (f64*)mem_alloc(cap * 2 * sizeof(f64));
        f64 low = p.from < p.to ? p.from : p.to;
        const f64 freqs[] = {20.0, 997.0, 10000.0, 0.45 * low}; // 0.45 of the lower rate: 19.8 kHz at 44.1
        for (f64 f : freqs) {
            for (u64 k = 0; k < n; ++k) { in[k * 2] = 0.5 * sin(2.0 * M_PI * f * (f64)k / p.from); in[k * 2 + 1] = 0.0; }
            u64 made = run(p.from, p.to, 2, in, n, out, cap);
            KV_CHECK(c, made == (n * p.to + p.from - 1) / p.from);
            u64 edge = made / 10;                     // clear of both ends' run-in
            Fit ft = fit(out, 2, edge, made - edge, f / p.to);
            f64 ripple = fabs(ft.gain_db - 20.0 * log10(0.5));
            if (ripple > worst_ripple) worst_ripple = ripple;
            if (ft.rest_db > worst_rest) worst_rest = ft.rest_db;
            if (fabs(ft.phase) > worst_phase) worst_phase = fabs(ft.phase);
        }
        // The stopband: going down, a sine above the output's half rate is not let through.
        if (p.to < p.from) {
            f64 f = 0.55 * p.to;
            for (u64 k = 0; k < n; ++k) { in[k * 2] = 0.5 * sin(2.0 * M_PI * f * (f64)k / p.from); in[k * 2 + 1] = 0.0; }
            u64 made = run(p.from, p.to, 2, in, n, out, cap);
            f64 e = 0;
            u64 edge = made / 10;
            for (u64 k = edge; k < made - edge; ++k) e += out[k * 2] * out[k * 2];
            f64 db = 10.0 * log10(e / (f64)(made - 2 * edge) / (0.125) + 1e-300); // against the sine's own power
            if (db > worst_alias) worst_alias = db;
        }
        mem_free(in);
        mem_free(out);
    }
    KV_INFO("test", "resampler, 8 pairs of rates: passband within %.2g dB, phase within %.2g rad, the rest at %.1f dB, a stopband sine's alias at %.1f dB",
            worst_ripple, worst_phase, worst_rest, worst_alias);
    KV_CHECK(c, worst_ripple < 1e-4);   // a ten-thousandth of a decibel, to 0.45 of the lower rate
    KV_CHECK(c, worst_phase < 1e-6);    // in time with the input: nothing delayed
    KV_CHECK(c, worst_rest < -140.0);   // what is not the sine, against it
    KV_CHECK(c, worst_alias < -140.0);

    // What it costs: ten seconds of stereo, timed, as a multiple of real time.
    {
        const Pair timed[] = {{44100, 48000}, {96000, 48000}, {192000, 44100}};
        for (const Pair& p : timed) {
            const u64 n = (u64)p.from * 10;
            f64* in = (f64*)mem_alloc(n * 2 * sizeof(f64));
            for (u64 k = 0; k < n * 2; ++k) in[k] = sin((f64)k * 0.001) * 0.5;
            const u64 cap = n * p.to / p.from + 64;
            f64* out = (f64*)mem_alloc(cap * 2 * sizeof(f64));
            f64 t0 = time_now();
            run(p.from, p.to, 2, in, n, out, cap);
            f64 took = time_now() - t0;
            KV_INFO("test", "resampler %u to %u, stereo: %.0fx real time (%.1f ms a second of audio)", p.from, p.to, 10.0 / took, took * 100.0);
            mem_free(in); mem_free(out);
        }
    }

    // Any chunking makes the same output, bit for bit; and the drain owes what it should.
    {
        const u64 n = 20000;
        f64* in = (f64*)mem_alloc(n * 2 * sizeof(f64));
        for (u64 k = 0; k < n; ++k) { in[k * 2] = sin((f64)k * 0.031) * 0.7; in[k * 2 + 1] = cos((f64)k * 0.017) * 0.3; }
        const u64 cap = n * 2 + 64;
        f64* a = (f64*)mem_alloc(cap * 2 * sizeof(f64));
        f64* b = (f64*)mem_alloc(cap * 2 * sizeof(f64));
        u64 ma = run(44100, 48000, 2, in, n, a, cap);
        u64 mb = run(44100, 48000, 2, in, n, b, cap, 37);
        KV_CHECK(c, ma == mb && memcmp(a, b, ma * 2 * sizeof(f64)) == 0);
        // Where an input frame falls: 44100 to 48000, frame 147 is frame 160.
        usize bytes = resample_memory_bytes(44100, 48000, 2);
        u8* mem = (u8*)mem_alloc(bytes);
        Resampler* r = resample_open(44100, 48000, 2, Span<u8>(mem, bytes));
        KV_CHECK(c, resample_output_at(r, 147) == 160 && resample_output_at(r, 1) == 2 && resample_output_at(r, 0) == 0);
        KV_CHECK(c, resample_taps(r) > 200 && resample_taps(r) < 260);
        mem_free(mem);
        mem_free(in); mem_free(a); mem_free(b);
    }
}
