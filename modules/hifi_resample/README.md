# hifi_resample

One sample rate made another, as exactly as f64 allows. Written, not taken: libsoxr is
LGPL, and what the resampler does to the audio, and how precisely, is Blackwing's to say
and to measure (slice 5, decided Nov 27). `hifi` runs it when a device will not take a
file's rate.

## How it works

**Polyphase windowed sinc at the exact ratio.** The two rates in lowest terms, L output
frames for every M input frames (44100 to 48000 is 147 to 160): output frame k is the
input at time k·M/L, exactly, and is made from the T input frames around it with one of
L precomputed phases of the filter. No interpolation between phases, so no error from
it. Each phase is scaled to a gain of exactly 1 at DC.

**The filter**: a sinc cut off between the passband's edge (0.4535 of the lower rate:
20 kHz at 44.1, 21.8 at 48) and the stopband's (half the lower rate), under a Kaiser
window made for 160 dB. Its length T follows from the transition's width against the
input's rate: about 230 taps for 44.1 to 48, about 1,000 for 192 to 44.1. The table is
L × T values: 290 KB for 44.1 to 48, 1.1 MB for 44.1 to 192. A pair whose table would
pass 16 MB is not taken.

**In time.** Frame 0 of the input and of the output are the same instant, and nothing is
delayed in the timeline: `resample_output_at(r, i)` is the output frame input frame i
falls at. The output lags the input by half the filter, so the last of it is made by
`resample_drain`, with silence after the input.

**Plain code**, as a decoder or a stage is: it opens, allocates and calls nothing, and
works in the memory it is handed (`resample_memory_bytes`).

## Using it

| name | what |
|---|---|
| `resample_memory_bytes(in, out, ch)` | the memory it wants; 0 for rates it does not take |
| `resample_open(in, out, ch, memory)` | a resampler, from the memory |
| `resample_process(r, in, n, &used, out, cap, &made)` | input taken as needed, output made as it can be; interleaved f64, full scale ±1 |
| `resample_drain(r, out, cap, &made)` | the rest at the stream's end; true when done |
| `resample_reset(r)` | a new stream |
| `resample_output_at(r, i)`, `resample_taken`, `resample_made`, `resample_taps` | where input frame i falls; the counts; the filter's length |

## Measured

`kormake test hifi_resample`, over eight pairs of rates (44.1 to and from 48 and 96, 48
to 96, 192 to 44.1 and back, 88.2 to 48), sines from 20 Hz to 0.45 of the lower rate,
fitted in the output at the frequency and phase they should have there:

| | |
|---|---|
| passband | within 8.2e-8 dB |
| phase | within 3.1e-12 rad: in time with the input |
| everything that is not the sine | -170.4 dB |
| a sine in the stopband (0.55 of the output's rate, going down), its alias | -166.2 dB |
| any chunking | the same output, bit for bit |
| cost, stereo, on this machine | 44.1 to 48: 35x real time (29 ms a second of audio); 96 to 48: 17x; 192 to 44.1: 9x |

## See also

- [hifi](../hifi/README.md): where it runs, and how a resampled source's frames are counted.
- [The slice 5 PRD](../../docs/PRDs/Blackwing-Slice-5-PRD.md), "The resampler".
