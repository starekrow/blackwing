# hifi_eq

The stages of Blackwing's chain, on `hifi`'s stage contract (`korvid/hifi/stage.h`):
volume now, and the equalizer (parametric bands, presets, pre-attenuation by the
largest boost so a preset cannot clip) in slice 2's milestone 5.

## How it works

**The gain stage** (`k_gain_stage`) is a multiply with no history. Parameter 0,
`gain_db`, is 0 at most (down to -100); parameter 1, `mute`, is 0 or 1, and is its own
parameter so that unmuting returns to the level. It is flat at 0 dB and not muted, and
then `hifi` takes the decoder's integers straight through. Its run-in is 0, so a recut
with it is exact.

A stage changes nothing while it runs: a new value is a new render, joined by `hifi`'s
crossfade, so the stage needs no smoothing of its own.

## Using it

The chain is the player's, fixed at startup:

```cpp
constinit const StageDesc* const k_chain[] = {&k_gain_stage};
HifiConfig hc; hc.stages = k_chain;
hifi_set_param(h, 0, kGainDb, -6.0);
```

## Quick reference

Header `korvid/hifi_eq/stages.h`: `k_gain_stage`; `GainParam` (`kGainDb`, `kGainMute`).

## Tests

`kormake test hifi_eq`: the gain stage alone: flat at 0 dB and untouched, the level its
decibels say, mute that keeps the level, values kept within its range.

## Not in this module

The chain's running, the f64 path and the dither (`hifi`). Which stages there are and
their values (`blackwing_player`, `player.chain`).

## See also

- [hifi](../hifi/README.md), "Stages" and "The aligned recut".
- [Slice 2](../../docs/PRDs/Blackwing-Slice-2-PRD.md), "Volume and the equalizer".
