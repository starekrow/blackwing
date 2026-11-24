# blackwing_script

The script host: Lua over the player's and the library's API, by the names native code
uses. It is the first piece of [the script-module spec](../../docs/specs/script-modules.md),
built as the spec has it so that the rest moves it and does not rewrite it.

**Status:** slice 4. One script the user runs, in process and trusted. Built: the record
(handlers and systems, each with its `reads` and `emits`), `ctx`, the binding from the
field tables, `kv.surface`, `kv.enum`, `kv.has`, `kv.log`, `kv.version`. Not built:
modules found on disk, the manifest, versions checked, capabilities, quotas, the
sandbox and the process boundary, a module's own tables and events, stage and decoder
states, snapshots.

## Using it

```
blackwing run FILE.lua [ARG...]
```

The script runs over a player and a library with no face (give `--face dev` for one),
and ends the run itself: with the code it gives `kv.quit`, or 1 if it raises. Its
arguments are `kv.args`. `print` and `kv.log("info", ...)` go to stdout.

```lua
-- Plays a file and says when it started.
local function on_started(ctx, batch)
  for _, ev in ipairs(batch) do
    local t = ctx:resource("player.transport")
    print(("started %d at %d Hz, %d queued"):format(ev.item, ev.rate, t.queued))
    kv.quit(0)
  end
end

return {
  name = "hello",
  handlers = { { name = ".started", event = "PlayerTrackStarted", fn = on_started, reads = "player.transport" } },
  startup = function(ctx)
    ctx:emit("PlayerEnqueue", { path = kv.args[1] })
    ctx:emit("PlayerPlay")
  end,
}
```

The same, read top to bottom:

```lua
return kv.steps(function(s)
  s:emit("PlayerEnqueue", { path = kv.args[1] })
  s:emit("PlayerPlay")
  local ev = s:wait("PlayerTrackStarted")
  print(("started %d at %d Hz"):format(ev.item, ev.rate))
  return 0
end)
```

## How it works

**The binding is the engine's.** `modules/script` in the engine's checkout binds an event
type, a table's rows and a resource to Lua from their field tables, with one mapping of
types and one set of errors (a wrong type, an integer that does not fit, a string too
long, an unknown field: each a Lua error). The host adds nothing to it and describes no
event or row a second time.

**The surface is made from the modules' own tables.** `kv.surface("player")` and
`("library")` are built at start-up from `blackwing_player/schema.h` and
`blackwing_library/schema.h`: the event types with their fields and whether each is a
command, the tables and resources with theirs, the enums' values by name, the constants.
`kv.enum` and `kv.has` are made from those.

**One unit, after PostSim.** `script.slice` runs the script's handlers whose event came,
in the record's order, then its systems that are due. It is declared to read every event
type, table and resource of the two APIs, a list put together from the schemas when the
program is compiled (`k_reads` in `host.cc`), not typed. What the script emits goes
through the engine's writer for code outside the schedule, so a command is delivered at
the start of the next tick: the spec's "acted on a tick later than a native unit's".

**`ctx` and the record are Lua** (`src/prelude.h`), over that: a unit reaches what it
named and nothing else. `ctx:emit` of a type not in its `emits`, and `ctx:resource`,
`ctx:ktable` or `ctx:received` of a name not in its `reads`, are errors at the call. A
table or a resource that is not the script's own is read-only: `spawn`, `set`,
`destroy` and an assignment through a row are errors. A record is checked when it loads:
a name in `reads` or `emits` that no surface has is said then.

**A system's batch is what came since it last ran.** A handler is handed the events of
the tick. A system with a period may not have run for many ticks; the host keeps what it
reads for it until it does (4,096 of a type at most).

**A script that raises** is said on stderr with its file, its line and a traceback, and
is stopped; its other units do not run again. With `run`, that ends the run with 1. The
player is not touched by it.

**It keeps nothing awake** that it did not ask for: a system waiting on the clock for
less than a resting loop's heartbeat asks for its ticks, and a script of handlers alone
asks for none.

## Quick reference

In Lua:

| name | what |
|---|---|
| the record | `{ name, handlers = { { name, event, fn(ctx, batch), reads, emits } }, systems = { { name, fn(ctx), reads, emits, every = seconds or on_change = true } }, startup(ctx), shutdown(ctx) }` |
| `ctx:emit(type, fields)` | emits an event; fields left out are the struct's defaults |
| `ctx:resource(name)` | a resource as a read-only record |
| `ctx:ktable(name)` | a table: `each`, `row`, `get`, `live`, `capacity`, `name`, `fields`, `valid` |
| `ctx:received(type)` | a system's batch of an event type: a list, oldest first |
| `ctx.now`, `ctx.slice` | seconds on the host's clock; a count of slices |
| `kv.version` | `script`, `player`, `library`: `"0.1.0"` |
| `kv.surface(name)` | `{ version, events = { Name = { command, fields } }, tables = { ["player.queue"] = { component, resource, fields } }, enums, constants }` |
| `kv.enum` | every enum both ways (`kv.enum.PlayerState.Paused == 3`, `[3] == "Paused"`), and `.name(v)`, the lower-case name |
| `kv.has(name)` | an event type, a table, a resource, a field (`"PlayerItem.size"`), an enum's value (`"PlayerState.Paused"`), a capability |
| `kv.log(level, text)`, `print` | `"info"` to stdout; `"warn"` and `"error"` to stderr |
| `kv.json`, `kv.sha256`, `kv.random` | the engine's |
| `kv.args` | what followed the script on the command line (`run`'s) |
| `kv.quit(code)` | ends the run (`run`'s) |
| `kv.steps(function(s) ... end)` | a record that runs the function as a coroutine (`run`'s): `s:emit`, `s:resource`, `s:ktable`, `s:wait(seconds)`, `s:wait(type [, pred] [, timeout])` (the next event of the type not yet waited for; nil after the timeout, 10 s if not said), `s:check(ok, text)`; the run ends with the integer it returns |

The libraries are Lua's base (less `dofile`, `loadfile`, `collectgarbage`), `string`,
`table`, `math`, `utf8` and `coroutine`. There is no `io`, `os`, `debug`, `package` or
`require`.

Header `korvid/blackwing_script/host.h`:

| name | what |
|---|---|
| `ScriptHostConfig`, `script_host_configure(config)` | the file, its arguments, whether the script ends the run; before `engine_init` |
| `script_host_status(e)` | none, running, done or failed; the code; slices run, events emitted; why it failed |
| `script_host_exit_code()` | the last run's code, for after the engine has gone |

What the module registers, as module `script`: the system `script.slice` (PostSim, main
thread, every tick).

## Behaviour worth knowing

- **It is the command line's equal.** The script is the user's, run by the user's
  command: it may name a path, and nothing is granted or refused. The spec's rule that a
  script names no path is the host's to enforce for modules, in P1. `kv.has` says yes to
  every capability.
- **`every = 0` is every tick**, which `kv.steps` uses. The spec's floor of 0.05 s is a
  module's quota, not this script's.
- **`on_change` runs a system when an event it reads came.** A table it reads changing is
  not noticed: tables do not say when they change.
- **`kv.quit`, `kv.args` and `kv.steps` are `run`'s**, not the spec's: a module does not
  end a run. They are what make a script a command, and a test a script.
- **A notification is read by the name native code uses, and so is a command**: the unit
  reads every event type, so a script can watch what others say to the player.

## Tests

`tests/` runs each script in the directory as `blackwing run` would, over the player on a
capture device, the library on a directory it makes, and the glue.

- **`record.lua`**: the record itself: a handler with what it reads, a system on a
  period, `startup`, every rule above, and a run ended with a code of the script's own.
- **`raises.lua`**: a script that indexes nil on a line the test knows. The test finds the
  file and the line in what was said, the script stopped, and the player still playing
  what it is then told to.
- **`walk.lua`**: the conformance walk. Every event type, table, resource, field and enum
  value is asked for by name; every table and resource is read and has every field its
  surface lists; every command is emitted by its name; every notification the run can
  bring about is heard with its fields. The C++ side listens to every event type of both
  schemas (`engine_listen`, no handler written for any) and checks that each command
  arrived natively, and that the surfaces list as many names as the schemas have: 32
  event types, 21 of them commands, 9 tables and resources, 83 fields, 39 enum values.
  A command added to a schema and not to the walk fails it by name, as `PlayerCue` did.
- **`pass.lua`**: a root added, a track found by a word, played from its row, the next
  track sought in, a band set, the transport read, exact and why not, a pause that
  holds, the queue's end, no underrun: what slices 1 to 3 promised that a script can see.

## Not in this module

Everything in the spec past the bus surface: [the spec](../../docs/specs/script-modules.md)
says what, and B33 in the master PRD when.

## See also

- [blackwing_player/SCRIPT.md](../blackwing_player/SCRIPT.md),
  [blackwing_library/SCRIPT.md](../blackwing_library/SCRIPT.md): the two surfaces, name by name.
- [blackwing](../blackwing/README.md): `run`.
- `modules/script/README.md` in the engine's checkout: the binding.
