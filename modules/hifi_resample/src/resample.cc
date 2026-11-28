// The resampler (resample.h). A table of L phases of T taps each: phase φ is the filter
// sampled at the offsets an output frame at input time n + φ/L sees. Output k is at input
// time k·M/L; its input frames are n - T/2 + 1 .. n + T/2, with n = floor(k·M/L). The input
// is kept in a buffer that starts with T zeros (the silence before the stream), compacted
// to its last T frames when it fills.
#include "korvid/hifi_resample/resample.h"
#include <math.h>
#include <string.h>

namespace kv {

namespace {

constexpr f64 kAtten    = 160.0;  // dB, the stopband the window is made for
constexpr f64 kPassEdge = 0.4535; // of the lower rate: 20 kHz at 44.1
constexpr f64 kStopEdge = 0.5;
constexpr u32 kHistory  = 8192;   // input frames the buffer holds past the filter's length
constexpr usize kMaxTable = (usize)16 << 20;

u64 gcd(u64 a, u64 b) { while (b) { u64 t = a % b; a = b; b = t; } return a; }

f64 bessel_i0(f64 x) {
    f64 sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (u32 k = 1; k < 200; ++k) {
        term *= q / ((f64)k * (f64)k);
        sum += term;
        if (term < sum * 1e-21) break;
    }
    return sum;
}

struct Design { u32 L, M, T; f64 fc; f64 beta; };

bool design(u32 in, u32 out, Design* d) {
    if (!in || !out || in == out) return false;
    u64 g = gcd(in, out);
    d->L = (u32)(out / g);
    d->M = (u32)(in / g);
    f64 low = (f64)(in < out ? in : out);
    f64 pass = kPassEdge * low, stop = kStopEdge * low;
    d->fc = (pass + stop) / 2.0 / (f64)in;            // the cutoff, in cycles per input frame
    f64 width = (stop - pass) / (f64)in;              // the transition, likewise
    u32 t = (u32)ceil((kAtten - 7.95) / (14.36 * width)) + 1;
    d->T = (t + 1) & ~1u;                             // even: as many taps each side
    d->beta = 0.1102 * (kAtten - 8.7);
    return true;
}

} // namespace

struct Resampler {
    u32 L, M, T, ch;
    f64* table;      // L * T
    f64* buf;        // (T + kHistory) * ch
    u32 cap;         // frames buf holds
    u32 len;         // frames in it
    u64 first;       // the input frame buf[0] is (T before the stream's first: the silence)
    u64 taken;       // input frames taken
    u64 made;        // output frames made
};

usize resample_memory_bytes(u32 in_rate, u32 out_rate, u32 channels) {
    Design d;
    if (!design(in_rate, out_rate, &d) || channels == 0 || channels > kResampleMaxChannels) return 0;
    usize table = (usize)d.L * d.T * sizeof(f64);
    if (table > kMaxTable) return 0;
    return sizeof(Resampler) + 16 + table + (usize)(d.T + kHistory) * channels * sizeof(f64);
}

Resampler* resample_open(u32 in_rate, u32 out_rate, u32 channels, Span<u8> memory) {
    usize need = resample_memory_bytes(in_rate, out_rate, channels);
    if (!need || memory.len < need) return nullptr;
    Design d;
    design(in_rate, out_rate, &d);
    u8* p = memory.data;
    Resampler* r = (Resampler*)p;
    p += sizeof(Resampler);
    p = (u8*)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    r->L = d.L; r->M = d.M; r->T = d.T; r->ch = channels;
    r->table = (f64*)p;
    p += (usize)d.L * d.T * sizeof(f64);
    r->buf = (f64*)p;
    r->cap = d.T + kHistory;
    // The table: for phase φ, tap j sees the input frame at offset t = (T/2 - 1 - j) + φ/L
    // before the output's time. Each phase is made to sum to exactly 1.
    const f64 half = (f64)d.T / 2.0, i0b = bessel_i0(d.beta);
    for (u32 ph = 0; ph < d.L; ++ph) {
        f64* row = r->table + (usize)ph * d.T;
        f64 sum = 0.0;
        for (u32 j = 0; j < d.T; ++j) {
            f64 t = (half - 1.0 - (f64)j) + (f64)ph / (f64)d.L;
            f64 x = 2.0 * d.fc * t;
            f64 sinc = x == 0.0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            f64 u = t / half;                              // the window over (-1, 1)
            f64 w = fabs(u) >= 1.0 ? 0.0 : bessel_i0(d.beta * sqrt(1.0 - u * u)) / i0b;
            row[j] = 2.0 * d.fc * sinc * w;
            sum += row[j];
        }
        for (u32 j = 0; j < d.T; ++j) row[j] /= sum;
    }
    resample_reset(r);
    return r;
}

void resample_reset(Resampler* r) {
    r->len = r->T;
    memset(r->buf, 0, (usize)r->T * r->ch * sizeof(f64));
    r->first = 0;    // buf[0] is input frame -T, kept as `first` with the T added: see at()
    r->taken = 0;
    r->made = 0;
}

namespace {

// Room for one more input frame: the oldest go, all but the filter's length.
void make_room(Resampler* r) {
    if (r->len < r->cap) return;
    u32 keep = r->T;
    memmove(r->buf, r->buf + (usize)(r->len - keep) * r->ch, (usize)keep * r->ch * sizeof(f64));
    r->first += r->len - keep;
    r->len = keep;
}

// Output frame k into out, reading input frames n - T/2 + 1 .. n + T/2, any past what was
// taken being silence (a drain).
void one(Resampler* r, f64* out) {
    u64 km = r->made * r->M;
    u64 n = km / r->L;
    u32 ph = (u32)(km % r->L);
    const f64* row = r->table + (usize)ph * r->T;
    // Input frame i is buf[i + T - first]: buf starts T frames before input 0.
    i64 start = (i64)n - (i64)(r->T / 2) + 1 + (i64)r->T - (i64)r->first;
    for (u32 c = 0; c < r->ch; ++c) out[c] = 0.0;
    for (u32 j = 0; j < r->T; ++j) {
        i64 at = start + (i64)j;
        if (at < 0 || at >= (i64)r->len) continue; // before what is kept (never, but for the drain's tail) or past what came
        const f64 h = row[j];
        const f64* x = r->buf + (usize)at * r->ch;
        for (u32 c = 0; c < r->ch; ++c) out[c] += h * x[c];
    }
    ++r->made;
}

} // namespace

void resample_process(Resampler* r, const f64* in, u32 in_frames, u32* used, f64* out, u32 out_cap, u32* made) {
    *used = 0;
    *made = 0;
    while (*made < out_cap) {
        u64 n = r->made * r->M / r->L;
        u64 need = n + r->T / 2;      // the last input frame this output reads
        if (need >= r->taken) {
            if (*used == in_frames) return;
            make_room(r);
            memcpy(r->buf + (usize)r->len * r->ch, in + (usize)*used * r->ch, r->ch * sizeof(f64));
            ++r->len;
            ++r->taken;
            ++*used;
            continue;
        }
        one(r, out + (usize)*made * r->ch);
        ++*made;
    }
}

u64 resample_output_at(const Resampler* r, u64 in_frame) {
    // The first k with k·M/L >= in_frame: ceil(in_frame·L / M).
    return (in_frame * r->L + r->M - 1) / r->M;
}

bool resample_drain(Resampler* r, f64* out, u32 out_cap, u32* made) {
    *made = 0;
    u64 end = resample_output_at(r, r->taken);
    while (r->made < end && *made < out_cap) {
        one(r, out + (usize)*made * r->ch);
        ++*made;
    }
    return r->made >= end;
}

u64 resample_taken(const Resampler* r) { return r->taken; }
u64 resample_made(const Resampler* r) { return r->made; }
u32 resample_taps(const Resampler* r) { return r->T; }

} // namespace kv
