// Korvid hifi_resample: a stream at one rate made a stream at another.
//
//   usize bytes = resample_memory_bytes(44100, 48000, 2);       // 0: a pair it does not take
//   Resampler* r = resample_open(44100, 48000, 2, Span<u8>(mem, bytes));
//   u32 used, made;
//   resample_process(r, in, in_frames, &used, out, out_cap, &made); // interleaved f64, full scale +-1
//   ... feed what was not used again; the output lags the input by half the filter
//   resample_drain(r, out, out_cap, &made);                     // at the stream's end: the rest, the input followed by silence
//   resample_reset(r);                                          // a new stream: no history
//
// Output frame k is the input at time k * in_rate / out_rate, exactly: frame 0 of each is
// the same instant, so where an input frame lands in the output is known
// (resample_output_at), and nothing is delayed in the timeline. It opens nothing, allocates
// nothing and calls nothing; one thread uses a resampler at a time.
//
// The filter: polyphase windowed sinc at the exact ratio of the two rates in lowest terms
// (44100 to 48000 is 147 to 160), a Kaiser window for 160 dB, passband to 0.4535 of the
// lower rate (20 kHz at 44.1 kHz), stopband from half of it, each phase's gain at DC exactly
// 1. Its taps grow as the transition narrows against the input's rate (about 230 for 44.1
// to 48, about 1000 for 192 to 44.1). modules/hifi_resample/README.md has its measured
// response.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Resampler;

constexpr u32 kResampleMaxChannels = 8;

// The memory a resampler of these rates and channels wants; 0 for rates it does not take
// (0, equal, a ratio whose table would pass 16 MB).
usize      resample_memory_bytes(u32 in_rate, u32 out_rate, u32 channels);
Resampler* resample_open(u32 in_rate, u32 out_rate, u32 channels, Span<u8> memory);
// Takes input frames (as many as it needs, up to `in_frames`: *used) and makes output
// frames (up to `out_cap`: *made), each made once the input it needs has come.
void       resample_process(Resampler* r, const f64* in, u32 in_frames, u32* used, f64* out, u32 out_cap, u32* made);
// The output the input taken so far still owes, with silence after it: until every output
// frame up to the last input frame's time is made. False while there is more to make.
bool       resample_drain(Resampler* r, f64* out, u32 out_cap, u32* made);
void       resample_reset(Resampler* r);
// The output frame at which input frame `in_frame` (counted from the last reset) falls: the
// first output frame at or after its time.
u64        resample_output_at(const Resampler* r, u64 in_frame);
u64        resample_taken(const Resampler* r); // input frames taken since the reset
u64        resample_made(const Resampler* r);  // output frames made since the reset
u32        resample_taps(const Resampler* r);  // the filter's length, in input frames

} // namespace kv
