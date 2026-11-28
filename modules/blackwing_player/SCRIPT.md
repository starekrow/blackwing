# blackwing_player — the script surface

Surface `player`, version 0.3.0 (0.2: the devices; 0.3: `player.output`'s `resampled_from`) · with [the spec](../../docs/specs/script-modules.md)

**Proven by slice 4** ([blackwing_script](../blackwing_script/README.md)'s walk): every name,
field and enum value on this page is reached from Lua as written. **Still a draft:** the
capability column and the rules about paths and window ids, which are the host's to
enforce for modules and are not built; a script the user runs is held to none of them.

What a script module sees of the player: every command, notification, table and resource
the player has, by the names native code uses. The player's API is already data, so this
page adds nothing to it: it says how each piece looks from Lua, which way it goes, and
which capability a command needs. What a field means is in
`korvid/blackwing_player/player.h`; how the player works is in [its page](README.md).

```lua
-- Skip anything shorter than ten seconds.
local function on_started(ctx, batch)
  for _, ev in ipairs(batch) do
    if ev.frames > 0 and ev.frames < 10 * ev.rate then ctx:emit("PlayerNext", {}) end
  end
end

return {
  name = "noshorts",
  handlers = { { name = ".started", event = "PlayerTrackStarted", fn = on_started,
                 emits = "PlayerNext" } },
}
```

## Commands

Events a script emits with `ctx:emit`, each named in the unit's `emits`. A command is
acted on in the tick after the host applies the slice it was emitted in.

| event | fields | capability | what |
|---|---|---|---|
| `PlayerPlay` | none | `player.transport` | plays: from the first item not done, or on from a pause |
| `PlayerPause` | none | `player.transport` | holds the playhead |
| `PlayerStop` | none | `player.transport` | stops; the item starts from its beginning on the next play |
| `PlayerNext` | none | `player.transport` | ends the item playing and goes on |
| `PlayerPrevious` | none | `player.transport` | in an item's first three seconds, the one before; later, this one from its start |
| `PlayerJump` | `item` integer | `player.transport` | plays this row now |
| `PlayerSeek` | `item` integer (0: the one playing), `frame` integer | `player.transport` | goes to a frame of a row |
| `PlayerSeekBy` | `seconds` number | `player.transport` | on (negative: back) from the last seek's target, so repeats add up |
| `PlayerCue` | `item`, `frame` integers; `frames`, `rate` integers (0: not known) | `player.transport` | takes up a place without playing: paused at a row and a frame, with nothing heard |
| `PlayerEnqueue` | `path` string, `before` integer (0: the end) | `player.queue` | adds a file to the queue |
| `PlayerQueueEdit` | `op` integer (a `PlayerEditOp`), `item`, `before` integers | `player.queue` | `Remove`, `Move` or `Clear` |
| `PlayerSetParam` | `stage`, `param` integers, `value` number | `player.params` | sets a parameter of a stage of the chain, by index |
| `PlayerApplyPreset` | `stage`, `preset` integers | `player.params` | sets every parameter of a stage to a preset's values: a `player.presets` row's `stage` and `index` |
| `PlayerSelectDevice` | `name` string (191 bytes at most) | `player.output` | plays through that device from now (a `player.devices` row's `name`), or `"default"` to follow the system's; the music goes on from the frame heard |

The fields `unused` of the six commands that carry nothing are not shown: emit them with
`{}`.

**`PlayerEnqueue.path` from a script** must be a path the module was shown: the `path`
of a `library.rows` row in one of its own windows, or of a `player.queue` row; or it
names a decoder of the module's own
([hifi_codec](../hifi_codec/SCRIPT.md#sound-from-nothing)). Any other is refused by the host before the player sees it, as a Lua error at the `emit`. A path
is any length the OS allows: 4095 bytes (`kPlayerPathMax` less its NUL).

**`PlayerSetParam`** reaches every stage, the built-in ones
([hifi_eq](../hifi_eq/SCRIPT.md) has their parameters) and guest ones. A control that
moves may emit one a slice: only the newest value is rendered.

## Notifications

Events a script reads: as a handler's batch, or with `ctx:received` in a system that
names the type in `reads`. No capability.

| event | fields | what |
|---|---|---|
| `PlayerTrackStarted` | `item`, `rate`, `channels`, `bits`, `frames`, `seq` integers | the item's first frame has been heard |
| `PlayerSeeked` | `item`, `frame`, `seq` integers | a seek's target frame has been heard |
| `PlayerParamChanged` | `stage`, `param` integers, `value` number, `seq` integer | a parameter's new value is what is heard |
| `PlayerTrackEnded` | `item`, `frames`, `seq` integers, `damaged` boolean, `reason` string | its last frame has been heard |
| `PlayerTrackFailed` | `item`, `seq` integers, `reason` string | it could not be played at all |
| `PlayerQueueEnded` | `seq` integer | every item is done or failed and nothing is sounding |
| `HifiUnderrun` | `count`, `seq` integers | the lead ran dry, `count` times since the last tick |
| `PlayerDeviceChanged` | `kind` integer (a `PlayerDeviceChange`), `name`, `reason` strings, `seq` integer | an output device came, went, came back or became the default; the one playing was lost; or the music moved to one |

`seq` counts up by one across all eight. A batch is in order within its type; across
types, `seq` is the order things happened in. For what is playing now, read
`player.transport`.

A script can also read the commands (name one in `reads`): it then sees what faces and
other scripts asked for, before the player has acted.

## Tables

### `player.queue`

Component `PlayerItem`, a row per item in the list, 4096 at most. The table's order is
not the list's: `pos` is.

| field | Lua | what |
|---|---|---|
| `id` | integer | from 1, in the order added; it stays through moves |
| `pos` | integer | its place in the list, from 1 |
| `state` | integer, a `PlayerItemState` | `Idle` 0, `Reading` 1, `Ready` 2, `Playing` 3, `Failed` 4 |
| `damaged` | integer, 0 or 1 | done, but the file ended early or was spoiled |
| `channels`, `bits`, `rate` | integers | the format, once the item has started |
| `frames` | integer | its length; 0 until started, or when the header does not say |
| `size` | integer | the file's bytes; 0 until it has been looked at |
| `bytes_read` | integer | of those, how many are in memory now |
| `path` | string | the OS's path |
| `reason` | string | why it failed, or how it was damaged |

### `player.chain`

Component `PlayerStage`, a row per stage of the chain, in order.

| field | Lua | what |
|---|---|---|
| `index` | integer | the stage's place, from 0: what `PlayerSetParam.stage` takes |
| `flat` | integer, 0 or 1 | with these parameters it passes audio unchanged |
| `params_len` | integer | how many parameters it has, 48 at most |
| `name` | string | `"gain"`, `"eq"`, a guest stage's |
| `param_names` | list of 48 strings | the first `params_len` are names |
| `params` | list of 48 numbers | the values heard |

Lua lists count from 1 and parameters from 0: parameter `p` is `params[p + 1]`.

### `player.params`

Component `PlayerParam`, a row per parameter of every stage: what it is called, what it
may be, and the value heard. Enough to draw a control for a stage nobody has heard of.
Fixed at start-up but for `value`.

| field | Lua | what |
|---|---|---|
| `stage` | integer | its stage's `index` |
| `index` | integer | its own within the stage, from 0: what `PlayerSetParam.param` takes |
| `value` | number | heard: the same number as `player.chain`'s `params[index + 1]` |
| `lo`, `hi` | numbers | what a value is kept within |
| `initial` | number | what a new chain starts with |
| `step` | number | the smallest change worth offering; 0: any. 0 to 1 in steps of 1 is a switch |
| `name` | string | `"gain_db"`, `"b3.gain"`; 15 bytes at most |
| `unit` | string | `"dB"`, `"Hz"`; `""` when it has none |

### `player.presets`

Component `PlayerPreset`, a row per preset a stage carries, fixed at start-up.

| field | Lua | what |
|---|---|---|
| `stage` | integer | the stage it is a preset of |
| `index` | integer | within the stage, from 0: what `PlayerApplyPreset.preset` takes |
| `name` | string | `"flat"`, `"rock"` |

### `player.devices`

Component `PlayerDevice`, a row per output device the system offers (and the capture
device, where there is one). A device that goes stays, not present, for the run.

| field | Lua | what |
|---|---|---|
| `name` | string | what `PlayerSelectDevice` takes; stable across runs where the system has a name for it |
| `label` | string | what it is called, for a person |
| `backend` | string | `"pipewire"`, `"capture"` |
| `is_default`, `present`, `shared`, `can_exclusive`, `open`, `chosen` | integers, 0 or 1 | the system's default now; there now; other programs' sound mixed in; it can be opened alone; the music is on it; it is the one chosen (choosing `"default"`, the default) |
| `channels` | integer | as the system says; 0 when it does not |

## Resources

### `player.transport`

| field | Lua | what |
|---|---|---|
| `state` | integer, a `PlayerState` | `Stopped` 0, `Starting` 1, `Playing` 2, `Paused` 3 |
| `item` | integer | the item being heard; 0 when none is |
| `position` | integer | the frame of it being heard |
| `frames` | integer | its length; 0 when its header does not say |
| `rate` | integer | frames per second of the item being heard |
| `queued` | integer | rows in the list |
| `left` | integer | items not yet done or failed |
| `place` | integer | the row the transport is at; 0 past the end |
| `place_pos` | integer | that row's `pos` |
| `seek_item`, `seek_frame` | integers | a seek on its way to the ear; `seek_item` 0 when none |

A script's snapshot of it is as of its slice: `position` is where the playhead was then.

### `player.output`

| field | Lua | what |
|---|---|---|
| `open` | integer, 0 or 1 | a device is open |
| `shared` | integer, 0 or 1 | it mixes other streams in, or converts on the way |
| `exact` | integer, 0 or 1 | every sample reaching the device is the file's own |
| `sample` | integer, a `HifiSampleFormat` | `S16` 0, `S24` 1, `S32` 2, `F32` 3 |
| `channels`, `rate` | integers | the device's format |
| `bits` | integer | the significant bits the device's word carries exactly: 16, 24 or 32 (24 for a float word); 0 with no device |
| `float_word` | integer, 0 or 1 | its word is a float |
| `lead_frames` | integer | decoded and ready, not yet taken by the device |
| `underruns` | integer | ever |
| `joining` | integer, 0 or 1 | a recut or a jump is being joined |
| `bytes_held` | integer | of the files, in memory now across every item |
| `recut_seconds`, `recut_render_seconds` | numbers | the last recut: to its first frame; its rendering of the whole lead |
| `recuts` | integer | ever |
| `jump_seconds` | number | the last seek or skip, to its first frame |
| `device` | string | the device open, by `player.devices`' name; `""` when none is |
| `chosen` | string | what was chosen: `"default"`, or a name |
| `following`, `standing_in`, `lost` | integers, 0 or 1 | the default is followed; the default stands in for a device chosen and not there; the device playing went, and the transport is paused until play is pressed |
| `none_why` | string | why no device is open while one is wanted |
| `resampled_from` | number | the device would not take the file's rate: what it is resampled from, to `rate`; 0 when it is not |

A guest stage that is not flat makes `exact` 0, as a built-in one does.

## Enums

In `kv.enum`, both ways:

| enum | values |
|---|---|
| `PlayerEditOp` | `Remove` 0, `Move` 1, `Clear` 2 |
| `PlayerItemState` | `Idle` 0, `Reading` 1, `Ready` 2, `Playing` 3, `Failed` 4 |
| `PlayerState` | `Stopped` 0, `Starting` 1, `Playing` 2, `Paused` 3 |
| `PlayerDeviceChange` | `Added` 0, `Gone` 1, `Back` 2, `Default` 3, `Lost` 4, `Moved` 5 |
| `HifiSampleFormat` | `S16` 0, `S24` 1, `S32` 2, `F32` 3 |

`kv.enum.PlayerState.name(v)` is the lower-case name the player's own
`player_state_name` gives (`"paused"`); the same for `PlayerItemState`. Every enum has
one: it is the value's name in lower case.

## Not here

- `PlayerConfig` and `player_configure`: the executable's, before the engine starts.
- `player_transport(e)` and `player_output(e)`: for code outside the schedule. A script
  reads the resources.
- The player's CLI commands (`player add`, `player seek` ...): a script emits the events
  they emit.

## What the player changes for this

Listed in [the spec](../../docs/specs/script-modules.md#what-this-needs): field tables
for its two resources, two guest slots in the chain, the host's decoders asked after the
linked ones, and a queue row with no file.

Changed by slice 4, which is the first thing to read this surface from outside the
player: the resources have their field tables; `player.params` and `player.presets` are
new, with `PlayerApplyPreset`; `player.output` says the device word's `bits`.
