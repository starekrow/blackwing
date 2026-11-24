# Script modules — the spec

Nov 23, 2026 · draft 0.1 · nothing here is built

What a script module is, the environment its Lua runs in, and every name it can reach.
This page is the spine: the runtime, the rules and the map. What each of Blackwing's
modules shows a script is on a page beside that module, linked from
[The surfaces](#the-surfaces).

The requirements this answers are in
[the design record](../design/script-module-requirements-2026-11-23.md). Where this draft
had to choose, the choice is marked **Draft choice**, and the ones worth arguing are
collected under [Open](#open).

The rule the draft follows: a script sees what native code sees, by the same names. A
header's comment stays the home of what a field means; the pages here say how it looks
from Lua, what a script may do with it, and under which capability.

## A script module

A directory in the modules location, holding:

| file | what |
|---|---|
| `module.lua` | the manifest: data, read before any of the module's code runs |
| `main.lua` | the control script: returns the module's record |
| a file per stage or decoder | each returns one contract record, and runs in a state of its own |
| anything else | Lua files `require` reaches, and data the module reads through `kv.asset` |

The same directory loads on every platform. Nothing in it is compiled.

### The manifest

`module.lua` has the form a native module's has, so one tool reads both:

```lua
module "tilt" {
  kind    = "script",
  version = "1.0.0",                       -- the module's own, semantic
  about   = "A tilt control: one knob, bass against treble.",
  uses    = {                              -- a range per surface it touches
    script       = "^0.1",
    player       = "^0.1",
    ["hifi.stage"] = "^0.1",
  },
  capabilities = { "player.params", "hifi.stage" },
  main    = "main.lua",                    -- the default
}
```

It is evaluated in an empty state: no libraries, one global (`module`), 64 KB of memory
and 100,000 instructions. What it yields is checked as data: unknown keys are refused, a
string is at most 256 bytes.

**Ranges** are semantic-version ranges: `^0.1` is 0.1.x (below 1.0 a minor bump may
break), `^1.2` is 1.2 and up, below 2. The host loads the module when every range holds,
and otherwise refuses it and says which surface did not match. It never loads on a
guess.

**Capabilities** asked for are shown to the user by their one-line descriptions
([Capabilities](#capabilities)) and granted or not as a set. A grant belongs to the
module's name and version; a later version that asks for more asks again.

### The record

`main.lua` returns a table shaped like a native module's `ModuleDesc`
(`korvid/core/module.h`), with the same field names where the thing is the same:

```lua
local function on_started(ctx, batch)
  for _, ev in ipairs(batch) do
    local t = ctx:resource("player.transport")
    kv.log("info", ("started %d at %d Hz"):format(ev.item, ev.rate))
  end
end

return {
  name     = "tilt",
  handlers = {
    { name = ".started", event = "PlayerTrackStarted", fn = on_started,
      reads = "player.transport" },
  },
  systems  = {},
  events   = {},          -- event types of its own
  components = {},        -- row layouts of its own
  ktables  = {},          -- tables of its own
  resources = {},         -- records of its own
  stages   = { "stage.lua" },
  decoders = {},
  startup  = function(ctx) return true end,
  shutdown = function(ctx) end,
  snapshot = function() return {} end,
  restore  = function(saved) end,
}
```

| field | what |
|---|---|
| `name` | as in `module.lua`. `".x"` in the tables below means `"<name>.x"`, as in a `ModuleDesc` |
| `handlers` | a function on one event type: `name`, `event`, `fn(ctx, batch)`, `reads`, `writes`, `emits` |
| `systems` | a function run on a period or a change: `name`, `fn(ctx)`, `reads`, `writes`, `emits`, and `every` (seconds, 0.05 at least) or `on_change = true` |
| `events` | event types the module emits: `{ name = "TiltChanged", fields = { {"db", "f64"} } }` |
| `components` | row layouts: `{ name = "TiltBand", fields = { {"hz", "f64"}, {"label", "char[32]"} } }` |
| `ktables` | `{ name = ".bands", components = "TiltBand", capacity = 16 }`; 4096 rows a module at most |
| `resources` | `{ name = ".state", fields = { ... } }`: one record |
| `stages` | files, each returning a stage record ([hifi](../../modules/hifi/SCRIPT.md)) |
| `decoders` | files, each returning a decoder record ([hifi_codec](../../modules/hifi_codec/SCRIPT.md)) |
| `startup(ctx)` | once, after a load or a restore; `false` fails the load |
| `shutdown(ctx)` | once, before an unload the host chose (not after a failure) |
| `snapshot()` | returns the module's state as plain values: what a reload, a respawn or the next run starts from |
| `restore(saved)` | handed what `snapshot` returned, before `startup` |

`reads`, `writes` and `emits` are comma-separated names, as a `SystemDesc`'s are. They
are the contract: a unit reaches nothing it did not name. `reads` names tables, resources
and event types from any surface the manifest uses. `writes` names the module's own
tables and resources only. `emits` names its own event types and the commands its
capabilities grant.

A script unit has no `phase`: it runs in the module's slice ([Threads and
time](#threads-and-time)), not in the schedule. It has no `after` or `before`: within a
slice, handlers run in the record's order, then systems in theirs.

Everything a script module publishes is cosmetic, in Korvid's sense: it is never sim
state and never in a state hash.

## The environment

### Globals

| name | what |
|---|---|
| Lua's base library | without `dofile`, `loadfile` and `collectgarbage`; `load` takes text only (mode `"t"`); `print` is `kv.log("info", ...)` |
| `string`, `table`, `math`, `utf8`, `coroutine` | Lua 5.5's, whole |
| `require(name)` | a Lua file of the module's own directory, by name, once; nothing else |
| `kv` | the engine's and the host's libraries, below |

There is no `io`, `os`, `debug` or `package`, no C modules and no bytecode.

`math` is the engine's (`modules/lua/README.md` in the engine's checkout): `math.sin`,
`math.exp`, `x ^ y` and their kin are bit for bit the same on every platform, and
`math.random` draws from a fixed seed. Integers are 64-bit and floats are doubles.

### `kv`

| name | what |
|---|---|
| `kv.version` | the surfaces this host has: `kv.version.script == "0.1.0"`, `kv.version.player`, `kv.version["hifi.stage"]` ... |
| `kv.has(name)` | whether a table, resource, event type, field (`"PlayerItem.size"`), enum value (`"PlayerState.Paused"`) or capability exists and is granted here |
| `kv.surface(name)` | a surface's declaration as a table: its tables, resources, events, their fields and types, its enums |
| `kv.enum` | every enum of every surface used, both ways: `kv.enum.PlayerState.Paused == 3`, `kv.enum.PlayerState[3] == "Paused"` |
| `kv.log(level, text)` | to the host's log under the module's name; `"info"`, `"warn"`, `"error"`. 20 lines a second, the rest counted and dropped |
| `kv.json`, `kv.sha256`, `kv.random` | the engine's script libraries, as `modules/script/README.md` in the engine's checkout has them |
| `kv.tags` | the tags' names, ranking and readers ([blackwing_tags](../../modules/blackwing_tags/SCRIPT.md)) |
| `kv.asset(name)` | a file of the module's own directory as a string, 4 MB at most; `nil` when there is none |
| `kv.frames` | sample buffers and the operations on them ([Buffers](#buffers)); in stage and decoder states only |

`kv.fs` and `kv.zip` are not there: a script names no path.

### `ctx`

What a handler, a system, `startup` and `shutdown` are handed. It mirrors the context a
native unit gets.

| call | what |
|---|---|
| `ctx:emit(type, fields)` | emits an event: `ctx:emit("PlayerSeekBy", { seconds = 10 })`. Fields left out are the struct's defaults. An error if `type` is not in the unit's `emits`, or a field does not fit |
| `ctx:resource(name)` | a resource as a read-only record: `ctx:resource("player.transport").position`. The module's own, named in `writes`, can be assigned to |
| `ctx:ktable(name)` | a table object, below |
| `ctx:received(type)` | a system's batch of an event type it reads: a list, oldest first |
| `ctx.now` | seconds on the host's monotonic clock, as of this slice |
| `ctx.slice` | a count of the module's slices, from 1 |

A table object answers as `script_push_ktable`'s does in the engine (`modules/script`):

```lua
local q = ctx:ktable("player.queue")
for h in q:each() do                      -- live rows, slot order
  local r = q:row(h)                      -- r.path, r.state, r.handle
end
q:get(h, "state")   q:live()   q:capacity()   q:name()   q:fields()   q:valid(h)
```

`spawn`, `set`, `destroy` and assigning through `row` work on the module's own tables
named in `writes`, and are errors on any other.

### Types

How a field crosses, both ways. It is the engine's mapping, unchanged.

| in the struct | in Lua |
|---|---|
| `bool` | boolean |
| `u8` to `u64`, `i8` to `i64`, an enum | integer. A `u64` above 2^63 - 1 reads as negative |
| `f32`, `f64` | number |
| `char[N]` | string, to the first NUL; at most N - 1 bytes going in |
| `u8[N]` | string of exactly N bytes |
| any other array | list |
| a handle | its 64 bits as an integer, 0 for none |

A value of the wrong type, an integer that does not fit, a string too long, an unknown
field: each is a Lua error, never a quiet change. Padding fields (`pad`, `unused`) are
not shown.

A script must ignore fields, rows' columns and enum values it does not know. A minor
version adds them.

## The surfaces

Each has its own semantic version, starting at 0.1.0. The bus surfaces go to 1.0 when P0
is done; the two contracts stay 0.x until native guests have run on them.

| surface | version | what a script gets | page |
|---|---|---|---|
| `script` | 0.1.0 | this page: the manifest, the record, `kv`, `ctx`, `script.modules` | here |
| `player` | 0.1.0 | the transport, the queue, the chain's parameters, what the output is doing | [blackwing_player](../../modules/blackwing_player/SCRIPT.md) |
| `library` | 0.1.0 | roots, windows onto the index, overrides, the scan | [blackwing_library](../../modules/blackwing_library/SCRIPT.md) |
| `hifi.stage` | 0.1.0 | the stage contract: changing audio | [hifi](../../modules/hifi/SCRIPT.md) |
| `hifi.decoder` | 0.1.0 | the decoder contract and the sparse file: producing audio, from a file or from nothing | [hifi_codec](../../modules/hifi_codec/SCRIPT.md) |
| `hifi.eq` | 0.1.0 | the built-in stages' parameters and the presets | [hifi_eq](../../modules/hifi_eq/SCRIPT.md) |
| `tags` | 0.1.0 | field and source names, ranking, the path guess, reading a file's tags | [blackwing_tags](../../modules/blackwing_tags/SCRIPT.md) |

A surface's version covers its names, its fields and their types, and its behaviour:
what a command does, when a notification is emitted, which state runs a function.

## Not exposed

| what | why |
|---|---|
| `hifi`'s calls (`hifi_add_source`, `hifi_jump`, `hifi_set_param` ...) | it has one caller, the player. What they do is reached through the player's commands; what they report is in `player.output` |
| `hifi_platform` (devices, the callback) | the realtime thread runs no script (B4). A device list is not published yet |
| `PlayerConfig`, `LibraryConfig`, `player_configure`, `library_configure` | set before the engine starts, by the executable |
| `media_platform`, the faces | skeletons; they get pages when they have surfaces |
| the network, the file system, the clipboard, other processes | no exits ([Capabilities](#capabilities)) |

## Capabilities

Reading is not a capability. A module reads any table, resource or notification of a
surface its manifest uses, by naming it in `reads`. Nothing it reads can leave, because
there is nowhere for it to go.

Everything else is one of these.

| capability | lets a module | said to the user as |
|---|---|---|
| `player.transport` | emit `PlayerPlay`, `PlayerPause`, `PlayerStop`, `PlayerNext`, `PlayerPrevious`, `PlayerJump`, `PlayerSeek`, `PlayerSeekBy` | Play, pause, skip and seek |
| `player.queue` | emit `PlayerQueueEdit`, and `PlayerEnqueue` with a path the library holds | Change what is queued, from your library |
| `player.params` | emit `PlayerSetParam` | Change the volume and the equalizer |
| `library.query` | emit `LibraryQuery`, `LibraryClose`, and `LibraryPrioritize` without a path | Search and browse your library |
| `library.override` | emit `LibraryOverride` | Correct your tracks' details (kept by Blackwing, never written into your files) |
| `library.roots` | emit `LibraryAddRoot` with a directory the user picks, and `LibraryRemoveRoot` | Add and remove library folders |
| `hifi.stage` | provide stages | Change the sound |
| `hifi.decoder` | provide decoders | Play kinds of file, or make sound of its own |
| `publish` | declare tables, resources and event types of its own | Show information to other parts of Blackwing |

**Paths.** A script reads paths (`PlayerItem.path`, `LibraryRow.path`) and never makes
one the host will open. `PlayerEnqueue.path` must be, byte for byte, a `path` of a
`library.rows` row the module has been shown, or of a `player.queue` row, or name a
decoder of the module's own (`script:<module>/<decoder>`,
[hifi_codec](../../modules/hifi_codec/SCRIPT.md#sound-from-nothing)); anything else is
refused before it reaches the player. `LibraryAddRoot.path` from a script is ignored:
the host asks the user for the directory. `LibraryPrioritize.path` is refused.

**Exits, and how each is closed.**

| exit | closed by |
|---|---|
| the network | no library reaches it; none in P1 |
| the file system | no `io`, `os` or `kv.fs`; files arrive as handles the player opened |
| a path the host opens | the rule above |
| the clipboard, URLs, other programs | no call reaches them. Text a script publishes is data: a face draws it and never opens, runs or follows it |
| the log | local, rate-limited, and read by the user |
| another script module | it has no exits either |
| the audio itself | not closed: a stage or a decoder controls what is heard. `hifi.stage` and `hifi.decoder` are granted knowing that |

## Threads and time

A script module has one **control state**, and one more state for each stage or decoder
instance it provides. States share nothing: a control state reaches its stages only
through their parameters.

| state | runs | may call | budget |
|---|---|---|---|
| control | in slices, on a thread of the module's own; never the tick's, the render thread's or the device's | everything on this page but `kv.frames` | 2 ms and 16 MB; 64 events and 256 row writes a slice |
| stage | on hifi's render thread (desktop: in the guest process, fed from it) | base, `string`, `table`, `math`, `kv.frames`, `kv.log` | the block's audio duration, a quarter of it at most; `memory_bytes`, 4 MB at most |
| decoder | the same | the same, and the file object | the same; `memory_bytes`, 16 MB at most |

Every number in that table is a placeholder until it is measured.

**A slice.** After a tick's PostSim, the host takes a snapshot of what the module reads:
the rows and records as they stand, and the events emitted since its last slice. The
module's handlers run over it, then its systems that are due. What they emit and write is
applied at the start of the next tick. So a script sees one consistent moment per slice,
never state changing under it, and its commands are acted on a tick later than a native
unit's: two ticks after the event that prompted them. On the desktop the snapshot crosses
a process boundary and on the phone it does not; a script cannot tell which.

A module with no handler due and no system due runs no slice, and keeps nothing awake.

**Time is the DAC's**, as everywhere in Blackwing: `position`, `PlayerTrackStarted` and
the rest say what the listener has heard. `ctx.now` is the host's clock, for periods.
Stage and decoder states have no clock at all: their output is a function of their input.

**Running over.** An instruction-count hook stops a state that exceeds its budget. A
control state that runs over loses that slice: what it emitted in it is dropped. Three in
a row and the module has failed. A stage or decoder that runs over is treated as
[Lifecycle and failure](#lifecycle-and-failure) says.

## Buffers

Audio crosses as buffer objects, never as a table per sample. `kv.frames` exists in
stage and decoder states.

| call | what |
|---|---|
| `#buf`, `buf:channels()` | frames in it; samples per frame |
| `buf:get(frame, channel)`, `buf:set(frame, channel, value)` | one sample; frames and channels count from 1 |
| `buf:fill(value)`, `buf:copy(from)` | every sample |
| `buf:scale(gain)`, `buf:scale(gain, channel)` | multiply |
| `buf:mix(from, gain)` | add another buffer's samples, scaled |
| `buf:biquad(coeffs, state)` | run a biquad over every channel: `coeffs` is `{b0, b1, b2, a1, a2}`, `state` a `kv.frames.biquad_state(channels)` kept between blocks |
| `buf:peak()` | the largest magnitude in it |
| `kv.frames.scratch(frames, channels)` | a buffer of the state's own memory, for working in; allocated at `open`, not in `process` |

A stage's buffers hold `f64`, full scale ±1. A decoder's hold integers at the file's
depth. The vector calls do in the host what a loop in Lua would, the same on every
platform. The list is short on purpose and grows by minor versions as measurements say
what is worth having.

Script DSP is best effort: the contract and what happens on failure are promised,
throughput is not.

## Lifecycle and failure

The same as a native module's: load, snapshot, replace, discard.

- **Load.** The manifest is read and checked, the grant looked up or asked for, the
  control state made, `main.lua` run, `restore` called with the last snapshot if there is
  one, then `startup`.
- **Snapshot.** `snapshot()` is called after a slice, at most once a second and when the
  host is about to replace or stop the module. What it returns is plain values (numbers,
  strings, booleans, tables of them), 1 MB at most as JSON. It is kept across runs.
- **Replace.** A changed file on disk loads a new control state from the last snapshot;
  the old one is discarded when the new one's `startup` has returned true. Handles and
  window ids carry a generation: what the old state had in flight is dropped. A stage is
  swapped at a recut, with its crossfade; a decoder at a source's start.
- **Discard.** The state is closed. Its tables' rows go, its windows close, its stages
  leave the chain. On the desktop the process is killed.

What goes wrong, and what the host does:

| what | the host |
|---|---|
| a Lua error in a handler or a system | logs it with a traceback under the module's name; drops what that unit emitted in the slice; goes on |
| eight errors in sixteen slices, or three slices over budget in a row | stops the module: `failed`, with the reason |
| a row or an event that does not fit its declaration | an error at the call, as above |
| over a quota (events, rows, log lines, memory) | the call that crossed it is an error |
| a stage over its deadline, or raising an error | the stage is bypassed with a recut's crossfade, the module marked `bypassed`, the user told which |
| a stage's output | sanitised as a native guest's: NaN and Inf to 0, clamped, the full-scale guard |
| a decoder raising an error or over its deadline | the source ends `Damaged` with the reason, or `Failed` if it had not opened |
| a decoder's answers | checked: `frames` at most what was asked, a format hifi plays, a `need` inside the file |
| the guest process dying (desktop) | respawned from the last snapshot, twice in a minute at most; then `failed` |

### `script.modules`

The host publishes what it is running, to faces and to scripts.

The ktable `script.modules`, component `ScriptModule`, a row per module found:

| field | type | what |
|---|---|---|
| `name` | `char[64]` | the module's |
| `version` | `char[32]` | the module's |
| `state` | `u8`, a `ScriptModuleState` | `Found`, `Refused`, `Loading`, `Running`, `Bypassed`, `Failed`, `Stopped` |
| `reason` | `char[96]` | why it was refused, bypassed or failed |
| `granted` | `u8` | its capabilities are granted |
| `generation` | `u32` | counts its loads |
| `memory` | `u64` | bytes its states hold now |
| `slice_seconds` | `f64` | its last slice |
| `stage_load` | `f64` | its stages' time over the audio's, over the last second |
| `errors` | `u32` | ever |

Events: `ScriptModuleFailed { name, reason, seq }` and `ScriptModuleBypassed { name,
reason, seq }`, out. Loading, stopping and granting are the user's, through a face: they
are not commands a script can emit.

## What this needs

The draft is written to change the existing modules as little as it can. A new module,
`blackwing_script`, is the host: it reads the other modules' `ModuleDesc` field tables,
which already describe every event and row, and builds the binding from them.

In Blackwing's modules:

| where | change | why |
|---|---|---|
| `blackwing_player`, `blackwing_library` | field tables for `player.transport`, `player.output` and `library.scan`, as their rows and events already have | a resource with no field table cannot be read by name |
| `blackwing_player` | the chain is `gain`, `eq` and two guest slots, each a `StageDesc` the host supplies that is flat while empty | script stages, with no change to `hifi`: `kHifiMaxStages` is already 4, and a flat stage costs nothing |
| `blackwing_player` | where it calls `decoder_for`, ask the host's decoders after the linked ones | script decoders, with no change to `hifi_codec` |
| `blackwing_player` | a queue row that names a script decoder and has no file | a module that makes its own sound ([hifi_codec](../../modules/hifi_codec/SCRIPT.md#sound-from-nothing)) |
| each module with a surface | a `script.lua` beside its `module.lua`: the surface's version, its enums by name, and which commands need which capability | data the host reads; the conformance tests check it against the headers |

Nothing changes in `hifi`, `hifi_codec`, `hifi_eq`, `hifi_platform` or `blackwing_tags`.
The path rule, window ids and quotas are enforced in the host, not in the player or the
library.

In Korvid, written up as requirements in
[Korvid changes for Blackwing](../PRDs/Korvid-Changes-For-Blackwing.md#found-in-drafting-the-script-module-spec):

| ID | change | why |
|---|---|---|
| KR17 | a resource's field table without the state hash | `ResourceDesc::fields` makes a resource sim state; these are cosmetic |
| KR18 | events in Lua by field table: emit one from a table, read a batch as tables | `script_push_ktable` does this for rows; nothing does it for events |
| KR19 | event types and ktables registered after `engine_init` | a script module's own, declared when it loads |

A state on a fixed block that loads text only needs nothing from the engine:
`kv_lua_newstate` takes any allocator, and the rest is the host's.

## Conformance

Every row of every table on these pages is a test: the name exists, its fields have these
types, the command needs this capability and does what its header says. The suite runs
three ways: against the native surface, against the binding in process, and against the
binding across the process boundary. A module author's harness runs a module headless
against recorded snapshots and, for a stage, nulls its output against the same stage in
exact mode.

## Open

- **Where this spine lives.** `docs/specs/` is a new directory; Korvid's layout has no
  layer for a contract that outlives its plan. When it is built, most of this page is a
  concept page and `blackwing_script`'s README.
- **The record mirrors `ModuleDesc`.** The alternative is a smaller, script-shaped API
  (`on("PlayerTrackStarted", fn)`). The mirror was chosen because it is the inside, and
  because declared `reads` and `emits` are what make the snapshot small and the grant
  checkable.
- **Reading needs no capability.** It rests on there being no exit. A host-mediated
  fetch, when it comes, makes reads worth guarding for any module that has it.
- **Two guest stages.** The chain holds four and two are taken. More means raising
  `kHifiMaxStages`, which is a change to `hifi`.
- **A stage is a state of its own.** It follows from the stage contract (no state outside
  the block, two of one kind side by side), and it means a stage cannot read the queue or
  the library. A stage that should follow the track is driven by its control state,
  through a parameter.
- **A recut renders the whole lead through a script stage.** Five seconds of audio
  through the interpreter on every parameter change. `player.output.recut_render_seconds`
  will say what that costs.
- **`library.roots` for a hostile module.** It may be better not grantable at all.
- **Persisting the snapshot across runs** makes it the module's only storage. A keyed
  store may be wanted later.
- **The budgets** are guesses.
