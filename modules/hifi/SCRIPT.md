# hifi — the script surface

Surface `hifi.stage`, version 0.1.0 · draft, with [the spec](../../docs/specs/script-modules.md)

What a script module sees of the audio path: the stage contract, and nothing else. A
script stage is the `StageDesc` of `korvid/hifi/stage.h` written in Lua: a record of
functions over memory it is handed, that changes samples in place. It opens nothing and
calls nothing of the player's. How the chain, the render and the recut work is in
[hifi's page](README.md).

Needs the capability `hifi.stage`. The rest of `hifi` (sources, jumps, the lead, the
device) has one caller, the player, and reaches a script as
[the player's surface](../blackwing_player/SCRIPT.md).

## A stage

A file named in the module record's `stages`, returning one record:

```lua
-- stage.lua: a tilt, bass against treble, as two shelves.
local function coeffs(s) ... end          -- from s.db and s.rate, with math.*

return {
  name         = "tilt",
  memory_bytes = 1 << 20,
  params       = { { name = "tilt_db", initial = 0, lo = -6, hi = 6 } },

  open = function(format)                 -- format.rate, format.channels, format.bits
    return { rate = format.rate, db = 0, z = kv.frames.biquad_state(format.channels) }
  end,
  set     = function(s, param, value) s.db = value; s.c = coeffs(s) end,
  process = function(s, frames) frames:biquad(s.c, s.z) end,
  flat    = function(s) return s.db == 0 end,
  run_in  = function(s) return s.rate // 10 end,
  reset   = function(s) s.z:clear() end,
}
```

| field | native | what |
|---|---|---|
| `name` | `StageDesc::name` | 31 bytes at most; shown in `player.chain` |
| `memory_bytes` | `memory_bytes` | the block the stage's whole Lua state lives in: its code, its values, its scratch buffers. 4 MB at most |
| `params` | `params` | up to 48 of `{ name, initial, lo, hi }`; a name is 15 bytes at most |
| `open(format)` | `open` | sets up for a stream of this rate and channel count, at the parameters' initial values. Returns the state: any Lua value. It must not fail |
| `set(state, param, value)` | `set` | a parameter, by index from 0, as `PlayerSetParam.param` counts. `value` is already inside `[lo, hi]`: the host brings it there |
| `process(state, frames)` | `process` | changes `#frames` interleaved frames in place. `frames` is a `kv.frames` buffer of `f64`, full scale ±1 |
| `flat(state)` | `flat` | whether, with its parameters as they are, it passes audio unchanged |
| `run_in(state)` | `run_in` | how many frames of input it needs before its output has settled |
| `reset(state)` | `reset` | forgets its history, as at the start of a source |

## Where it runs

Each instance of a stage is a Lua state of its own, made inside its `memory_bytes`. It is
not the module's control state and shares nothing with it. Two instances run side by
side during a recut: the render being heard and the one that will replace it.

A stage's state has the base library, `string`, `table`, `math`, `kv.frames` and
`kv.log`, and no `ctx`: it cannot read the queue, the library or the clock. Its output is
a function of its input and its parameters. A stage that should follow something (the
track, the time of day) is driven by its module's control state through a parameter.

`process` runs on hifi's render thread, ahead of the playhead by the lead; on the
desktop, in the guest process, fed from that thread. Never on the device's thread.

## Behaviour worth knowing

**A render's parameters never change while it runs.** A change is a new render, a recut,
joined with a fade. A stage needs no smoothing of its own, and `set` is never called
between two `process` calls of one render.

**A recut renders the whole lead again.** With the default five seconds, a parameter
change runs five seconds of audio through `process` as fast as it will go.
`player.output.recut_render_seconds` says how long the last one took.

**`flat` is believed by not calling you.** While `flat` returns true the host does not
call `process`, so a stage cannot say it is flat and change the audio. A chain whose
stages are all flat passes the decoder's integers through untouched, and
`player.output.exact` can be 1.

**A stage that is not flat ends exactness**, whatever it does to the samples.

**Allocate in `open`, not in `process`.** The collector runs between blocks. A `process`
that needs more memory than the block has left raises an error, and an error bypasses
the stage.

**Deadlines.** A block's `process` may take a quarter of the block's own duration, at
most. Over that, or on an error, the stage is bypassed with a recut's crossfade, its
module is marked `bypassed` in `script.modules`, and the user is told which. The number
is a placeholder until it is measured.

**The output is sanitised** after `process`: NaN and Inf become 0, samples are clamped,
and a sudden full-scale block ramps to silence.

**Two slots.** The chain holds four stages and Blackwing's own take two (`gain`, `eq`).
Guest stages take the other two, in the order their modules were enabled. A third is
refused, and `script.modules` says why.

## Enums

`HifiSampleFormat` (`S16` 0, `S24` 1, `S32` 2, `F32` 3) is in `kv.enum` for
`player.output.sample`. A stage never sees a device format: it is handed `f64`.

`kv.surface("hifi.stage").constants` has `kHifiMaxStages` (4) and `kHifiMaxParams` (48).

## Not here

- `hifi_create`, `hifi_add_source`, `hifi_jump`, `hifi_replace_after`, `hifi_set_param`,
  `hifi_poll` and the rest of `korvid/hifi/hifi.h`: the player's.
- `HifiStatus`: published as `player.output`.
- The device (`hifi_platform`).

## What hifi changes for this

Nothing. The player's chain gains two guest slots, each a `StageDesc` the script host
supplies that is flat while empty and otherwise runs a script's stage.
