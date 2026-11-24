# ui_dev

The development face: a plain, dense, complete window over the player and the library.
It is built first, and it is how the rest is developed. It reads their tables and emits
their commands, and calls no function of either.

**Status:** slice 4, milestone 2. The frame is built (the screen, a panel that fails
alone, asleep while hidden, still when nothing changes) and the player's four panels are
in it: Transport, Queue, Output and Chain. The library's panels and the inspectors are
the [slice's](../../docs/PRDs/Blackwing-Slice-4-PRD.md) next milestones.

## How it works

**One screen, of panels.** `assets/shell.kim` is the face's one Krome screen, in
`krome/std`'s dark theme. Each panel is a definition (`queue-panel`) in a file of its
own, `assets/<name>.kim`, with its model's first values in `assets/<name>.lua` beside it;
the shell `use`s the one and names the other as a `script`. `assets/widgets.kim` and
`widgets.lua` are the few styles and the one behaviour the panels share. Krome's screens
are whole-window layers, so panels cannot be screens side by side.

**A reader and an emitter.** The C++ does two things, per panel, in two units:

- `ui_dev.sync` (PostSim) reads the tables through the context and writes the panel's
  part of the model. The writer (`src/model.cc`) keeps a hash of what each key last
  showed, and a write of the same value is nothing. One that goes through asks the loop
  for a frame, which is the only reason the face ever asks for one.
- `ui_dev.events` (a handler on `KromeEvent`) offers each of the screen's events to the
  panels by name, and the one that knows it emits the command. Where it needs a table
  to say which command (the row above this one), it reads it as the phase found it
  (`^player.queue`): what the screen was showing. The face then asks for four ticks
  (`engine_want_ticks`), since the command's answer is a tick or two away.

It keeps nothing of the player's or the library's that it could not read again. Its own
is where the user is: what a list has in view, which slider is held.

**The panels.**

| panel | file | shows | does |
|---|---|---|---|
| Transport | `transport.cc` | the state, the row and its file's name, position and length in the device's time, a seek on its way | play, pause, stop, next, previous, ten seconds back and on, and the bar: a press or a drag is a `PlayerSeek` each time it moves |
| Queue | `queue.cc` | every row in the list's order: place, name, state, format, bytes held, reason | play a row now, remove it, move it up or down, clear, add a path typed in |
| Output | `output.cc` | the device, its format, the lead, underruns, the last recut and jump; exact, and why not | |
| Chain | `chain.cc` | each stage and whether it is flat; every parameter with the value heard | a slider a parameter, a switch for one that is 0 or 1, a button a preset |

**The position** is written every tick, as what would be shown: the readout's text
changes once a second and the bar's value once a pixel of its width, and the writer
does nothing in between.

**A control answers in the frame it is touched.** `dev-slider` is std's slider over 0 to
1 that emits `slide` as it moves and `slide_end` when let go (std's writes the model and
says nothing, and the C++ hears only events). While one is held its knob is the
pointer's: the face leaves that key alone. The readout beside it goes on showing the
value heard.

**The queue is a list over a window** (Krome's sparse `each`): it lays out as if every
row were there, says which it has built (`KromeView`), and `queue.cc` hands over those,
found in the table by `pos`. A queue of four thousand rows is sixty in the model.

**The chain's controls are written from the tables.** `chain.cc` writes the definition
`chain-stages` as markup from `player.params` and `player.presets`, compiles it
(`kim_compile`) and gives it to Krome in the place of `assets/chain_stages.kim`, which
says only that there is none yet. It is written again when what it was made from
changes (a hash of the stages, the parameters' names and ranges, the presets). A stage
the face has never heard of gets its controls from its own description. It is not a
list because a list is built again when a row's value changes, which while a slider is
dragged is every tick, under the pointer.

**Exact, and why not** is worked out in `output.cc` from what is published: the device
is shared, a stage is not flat (which), a join is under way, the device's word is
narrower than the file's.

**Hidden, it is asleep.** On `WindowVisibility` saying the window is not seen, `.sync`
returns before it reads anything; seen again, the writer forgets what it showed and
everything is written in the next tick.

**A frame costs a tick.** `ui_dev.frame` (in the Frame cycle, so once for each frame
drawn) asks for one: what Krome said in a frame is an event the next tick delivers, and
a resting loop's next tick may be a quarter second away.

**A panel that does not load fails alone.** A mistake anywhere in a Krome screen's files
fails the screen. When the shell fails, `ui_dev` reads `krome_errors`: for each mistake
that names a panel's file it compiles a stand-in (a definition of the same name that
shows the file, the line and the message, in `widgets.kim`'s `panel-broken`), gives it to
Krome in the file's place with a script that does nothing, and opens the shell again. A
mistake that names no panel (the shell's own, the prelude's) leaves a screen that lists
the mistakes. Either way the player is not involved.

## Using it

Put the module in an engine with a window (or a Krome viewport) and something that
publishes the player's tables; the face is up. `blackwing` does; so do this module's
tests, with a stand-in.

| name | what |
|---|---|
| `ui_dev_configure(UiDevConfig)` | before `engine_init`: `enabled` (false: in the engine and doing nothing), `open` (false: opened by `ui_dev_open`) |
| `ui_dev_open(e)` | opens the screen, or opens it again from the files as they are |
| `ui_dev_screen(e)`, `ui_dev_ready(e)` | the screen's name in Krome; whether it is built and written |
| `ui_dev_stats(e)` | model writes, commands, ticks looked at and passed over, panels standing in, whether the screen failed |

What it registers: `ui_dev.sync` (PostSim; reads the player's two resources and four
tables), `ui_dev.events` (on `KromeEvent`; emits the player's twelve commands),
`ui_dev.view` (on `KromeView`), `ui_dev.visibility` (on `WindowVisibility`),
`ui_dev.frame` (Frame).

## Tests

`tests/` runs the face with no window: Krome's view at a fixed size (`krome_viewport`),
controls pressed by id (`krome_press`) or by a pointer and a keyboard the test moves
(`input_pointer_override`, `input_text_override`).

- **Against a stand-in** (`standin.cc`): a module named `player` that registers the
  player's events, tables and resources from `blackwing_player/schema.h`, publishes
  values the test sets by hand and writes down the commands it is sent. Neither the
  player nor the library is in the engine. Every transport command from its own control;
  the bar pressed, dragged and let go; a thousand queued rows scrolled, edited and added
  to; exact and each reason why not; the chain's controls made from descriptions, a
  slider held while the value heard changes under it, a parameter added while it runs;
  ten still seconds costing no frame, layout, paint, model write or request; hidden,
  nothing read or written; a panel whose file is not markup saying so in its place.
- **On the player**: the same face over `blackwing_player` and a capture device on the
  wall clock. A path is typed into the edit and entered, played, the volume dragged, a
  preset pressed, the bar pressed, paused, played to its end, the row removed. The test
  emits no command and reads only what the screen shows.

## Behaviour worth knowing

- **A stand-in for a broken panel stays until the next start.** What Krome is given by
  hand (`krome_provide`) wins over the file, so mending the file needs a restart.
- **The model is one table for every panel**, since they are one screen: keys carry
  their panel's letter (`t_state`, `q_rows`, `o_why`, `c_v_1_14`).
- **A row's buttons carry its id** (`qx_17`, `cs_1_14`), and the C++ reads the number
  back out of the node's name: an event carries a node and one number, no more.
- **A sparse list's `placeholder` block needs `widgets.kim`'s definition of that name**:
  Krome's check does not know the word (KR27).
- **A string of 56 bytes or more** is written as a document (`krome_set_doc`); a
  `KromeSet` holds no more.

## See also

- [The slice 4 PRD](../../docs/PRDs/Blackwing-Slice-4-PRD.md) and its
  [notes](../../docs/design/slice-4-notes.md): what the face is for, and what building
  it found.
- [blackwing](../blackwing/README.md): which face is up, and the loop under it.
- [blackwing_player](../blackwing_player/README.md), [blackwing_library](../blackwing_library/README.md):
  the tables it reads and the commands it emits.
- `modules/krome/README.md` in the engine's checkout: screens, the model, markup.
