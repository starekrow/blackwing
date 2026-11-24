# hifi_eq — the script surface

Surface `hifi.eq`, version 0.1.0 · draft, with [the spec](../../docs/specs/script-modules.md)

What a script module sees of Blackwing's own stages: their parameters, by index, and the
presets. A script changes them with `PlayerSetParam`
([the player's surface](../blackwing_player/SCRIPT.md)) and reads the values heard in
`player.chain`. What each stage does is in `korvid/hifi_eq/stages.h` and
[hifi_eq's page](README.md).

Reading needs no capability. Setting a parameter needs `player.params`.

```lua
-- Apply a preset by name.
local function apply(ctx, name)
  local eq
  local chain = ctx:ktable("player.chain")
  for h in chain:each() do
    if chain:get(h, "name") == "eq" then eq = chain:get(h, "index") end
  end
  for _, p in ipairs(kv.surface("hifi.eq").presets) do
    if p.name == name then
      for b, band in ipairs(p.bands) do
        local at = 4 * (b - 1)
        ctx:emit("PlayerSetParam", { stage = eq, param = at + 0, value = band.kind })
        ctx:emit("PlayerSetParam", { stage = eq, param = at + 1, value = band.freq })
        ctx:emit("PlayerSetParam", { stage = eq, param = at + 2, value = band.gain_db })
        ctx:emit("PlayerSetParam", { stage = eq, param = at + 3, value = band.q })
      end
    end
  end
end
```

Forty parameters in one slice are rendered as one recut: changes that come faster than
recuts are taken together.

## The stages

Find a stage by its `name` in `player.chain`, not by a fixed index: guest stages share
the chain.

### `gain`

Volume. Flat at 0 dB and not muted.

| param | name | range | what |
|---|---|---|---|
| 0 | `gain_db` | -100 to 0, initially 0 | the level in dB; 0 at most |
| 1 | `mute` | 0 or 1 | its own parameter, so unmuting returns to the level |

`kv.enum.GainParam`: `kGainDb` 0, `kGainMute` 1.

### `eq`

Ten bands, each a peak or a shelf, after an attenuation by the highest point of their
combined response, so no setting raises a full-scale sine past full scale. Flat when
every band's gain is 0. Four parameters a band; band `b` counts from 0.

| param | what | range |
|---|---|---|
| `4b + 0` | the kind, an `EqBand.Kind` | `Peak` 0, `LowShelf` 1, `HighShelf` 2 |
| `4b + 1` | the frequency | Hz |
| `4b + 2` | the gain | -24 to 24 dB |
| `4b + 3` | the Q | |

The bands begin on octaves from 31.25 Hz to 16 kHz, the first a low shelf and the last a
high shelf, at 0 dB. A value outside a parameter's limits is brought inside them, and `PlayerParamChanged`
says what is heard.

## Presets

`kv.surface("hifi.eq").presets` is `eq_presets()` as a list, in its order:

| field | Lua | what |
|---|---|---|
| `name` | string | the preset's |
| `bands` | list of 10 | each `{ kind, freq, gain_db, q }`, with a band the preset leaves out filled in as the default one, flat (`eq_preset_band`) |

Presets are data. Applying one is setting forty parameters, as above; there is no
"current preset" in the chain, only its values.

## Constants

`kv.surface("hifi.eq").constants` has `kEqBands` (10).

## Not here

- `k_gain_stage`, `k_eq_stage`: the stages' records. A script cannot run them on audio of
  its own; it writes a stage ([hifi](../hifi/SCRIPT.md)).
- A stage's parameter ranges as data. `StageParam`'s `lo` and `hi` are not in
  `player.chain` today.

## What hifi_eq changes for this

Nothing.
