# hifi_eq

The stages of Blackwing's chain, on `hifi`'s stage contract (`korvid/hifi/stage.h`):
volume, and the equalizer (ten parametric bands, presets as data, an attenuation by the
largest boost so no setting can clip a full-scale sine).

## How it works

**The gain stage** (`k_gain_stage`) is a multiply with no history. Parameter 0,
`gain_db`, is 0 at most (down to -100); parameter 1, `mute`, is 0 or 1, and is its own
parameter so that unmuting returns to the level. It is flat at 0 dB and not muted, and
then `hifi` takes the decoder's integers straight through. Its run-in is 0, so a recut
with it is exact.

**The equalizer** (`k_eq_stage`): ten bands, each a peak or a shelf with a frequency, a
gain (-24 to 24 dB) and a Q, as ordinary biquads from the RBJ Audio EQ Cookbook in f64
(transposed direct form II, state per band and channel). The bands begin on octaves
from 31.25 Hz to 16 kHz, the first a low shelf and the last a high shelf, all at 0 dB:
flat. Its forty parameters are four a band: kind (0 peak, 1 low shelf, 2 high shelf),
frequency, gain, Q. Ahead of the bands, an attenuation by the highest point of their
combined response, found on a 1024-point logarithmic grid from 10 Hz to Nyquist and
refined by golden-section search around the best point. The coefficients and the
attenuation are worked out on the first `process` after a change, so a chain opened and
given all forty values designs once. Its run-in is twelve time constants (Q / (pi f)) of
its lowest band that is not flat, at most half a second: enough that a recut with it
unchanged comes within a bit of a straight render.

**Presets are data** (`eq_presets()`): a name and ten bands, a band left out being the
default band, flat. `flat`, `bass`, `treble`, `loudness`, `vocal`, `rock`, and
`extreme` (every band at +24 dB, some narrow), which is there to prove the attenuation.
The stage carries them in the contract's form too (`StageDesc::presets`: a name and a
value for each of the forty parameters, made from the same table when the program is
built), which is where the player takes them from: it publishes them as `player.presets`
and applies one on `PlayerApplyPreset`, without knowing this module's names.

**Parameters describe themselves** (`StageParam`): beside a name, a first value and a
range, a step and a unit. The gain's level moves in steps of 0.5 dB and its mute is 0 to
1 in steps of 1, a switch; a band's kind steps by 1, its gain by 0.5 dB, its Q by 0.01,
and its frequency, in Hz, by anything.

A stage changes nothing while it runs: a new value is a new render, joined by `hifi`'s
crossfade, so the stage needs no smoothing of its own.

## Using it

The chain is the player's, fixed at startup:

```cpp
constinit const StageDesc* const k_chain[] = {&k_gain_stage, &k_eq_stage};
HifiConfig hc; hc.stages = k_chain;
hifi_set_param(h, 0, kGainDb, -6.0);            // volume
hifi_set_param(h, 1, 5 * 4 + 2, 3.0);           // the 1 kHz band, +3 dB
const EqPreset* p = eq_preset("rock");          // a preset: its forty values, set in one go
```

**A preset.** For each band b, `EqBand x = eq_preset_band(*p, b)`, then the four
parameters `b * 4 + 0` to `+ 3` from `x.kind`, `x.freq`, `x.gain_db`, `x.q`. Sent in one
tick, they are one recut.

```cpp
```

## Quick reference

Header `korvid/hifi_eq/stages.h`:

| name | kind | what |
|---|---|---|
| `k_gain_stage`, `GainParam` | stage, enum | volume: `kGainDb`, `kGainMute` |
| `k_eq_stage`, `kEqBands` | stage, constant | the equalizer, ten bands |
| `EqBand` | record | `kind` (`Peak`, `LowShelf`, `HighShelf`), `freq`, `gain_db`, `q` |
| `EqPreset`, `eq_presets()`, `eq_preset(name)` | record, functions | the presets |
| `eq_preset_band(preset, b)` | function | a preset's band b, filled in |

## Tests

`kormake test hifi_eq`: the gain stage alone: flat at 0 dB and untouched, the level its
decibels say, mute that keeps the level, values kept within its range. The equalizer
alone: flat is bit for bit; six band settings, alone and with another band, measured
with a sine at ten frequencies after the run-in, within 0.1 dB of the cookbook's
formulas worked out again in the test (and the attenuation they imply): 0.03 dB at
worst; every preset through sixty full-scale sines from 20 Hz to 20 kHz, none above full
scale; the run-in and reset. And the equalizer in `hifi` over the capture device: six
recuts with the rock preset unchanged against a straight render, a bit apart at most.

## Behaviour worth knowing

- **The attenuation is for sines.** It is the peak of the magnitude response, so a
  full-scale sine is never pushed past full scale. A signal whose components add up in
  phase after the filter (a full-scale square through a boost) can still go over, and
  is clipped at the device's word.
- **Ordinary biquads.** They cramp near Nyquist (a high shelf at 16 kHz at 44.1 kHz is
  not quite its analog shape), and their coefficients come from libm. Matched designs
  near Nyquist and the engine's transcendentals are later work.

## Not in this module

The chain's running, the f64 path and the dither (`hifi`). Which stages there are and
their values (`blackwing_player`, `player.chain`).

## See also

- [hifi](../hifi/README.md), "Stages" and "The aligned recut".
- [Slice 2](../../docs/PRDs/Blackwing-Slice-2-PRD.md), "Volume and the equalizer".
- [The script surface](SCRIPT.md): the built-in stages' parameters and the presets, from Lua. A draft.
