# ui_dev

The development face: a plain, dense, complete window over the player and the library.
It is built first, and it is how the rest is developed. It reads their tables and emits
their commands, and calls no function of either.

**Status:** slice 4, milestone 1. The frame is built (the screen, a panel that fails
alone, asleep while hidden, still when nothing changes) with one panel in it, Transport,
which has play, pause, the state and the position. The other panels are the
[slice's](../../docs/PRDs/Blackwing-Slice-4-PRD.md) next milestones.

## How it works

**One screen, of panels.** `assets/shell.kim` is the face's one Krome screen, in
`krome/std`'s dark theme. Each panel is a definition (`transport-panel`) in a file of its
own, `assets/<name>.kim`, with its model's first values and any handlers in
`assets/<name>.lua` beside it; the shell `use`s the one and names the other as a
`script`. `assets/widgets.kim` is the few styles the panels share. Krome's screens are
whole-window layers, so panels cannot be screens side by side.

**A reader and an emitter.** The C++ does two things, per panel, in two units:

- `ui_dev.sync` (PostSim) reads the tables through the context and writes the panel's
  part of the model. The writer (`src/model.cc`) keeps a hash of what each key last
  showed, and a write of the same value is nothing. One that goes through asks the loop
  for a frame, which is the only reason the face ever asks for one.
- `ui_dev.events` (a handler on `KromeEvent`) offers each of the screen's events to the
  panels by name, and the one that knows it emits the command. The face then asks for
  four ticks (`engine_want_ticks`), since the command's answer is a tick or two away.

It keeps nothing of the player's or the library's that it could not read again.

**The position** is written every tick, as the text the readout shows; the text changes
once a second, so that is how often it is written.

**Hidden, it is asleep.** On `WindowVisibility` saying the window is not seen, `.sync`
returns before it reads anything; seen again, the writer forgets what it showed and
everything is written in the next tick.

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

What it registers: `ui_dev.sync` (PostSim, reads `player.transport`), `ui_dev.events`
(on `KromeEvent`; emits `PlayerPlay`, `PlayerPause`), `ui_dev.visibility` (on
`WindowVisibility`).

## Tests

`tests/` runs the face with no window and no player: Krome's view at a fixed size
(`krome_viewport`), controls pressed by id (`krome_press`), and `standin.cc`, a module
named `player` that registers the player's events, tables and resources from
`blackwing_player/schema.h`, publishes a transport the test sets by hand and writes down
the commands it is sent. They check that a table reaches the screen and a press reaches
the stand-in as the right command, once; that ten still seconds cost no layout, paint,
model write or request; that ten seconds of a moving position are ten writes; that
hidden, nothing is read or written; and that a panel whose file is not markup says so
where it would be.

## Behaviour worth knowing

- **A stand-in for a broken panel stays until the next start.** What Krome is given by
  hand (`krome_provide`) wins over the file, so mending the file needs a restart.
- **The model is one table for every panel**, since they are one screen: keys carry
  their panel's letter (`t_state`, `t_pos`).
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
