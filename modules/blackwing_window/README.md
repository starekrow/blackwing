# blackwing_window

What every face shares of the window: closing it while the music plays hides it and the
music goes on; closing it otherwise ends the run; and what it costs can be counted.

## How it works

`blackwing` with a face runs Korvid's frame loop, paced `Decoupled`: ticks at the
clock's rate while the player or the library asks for them, a frame only when a screen
changed, none while the window is hidden. The frame loop says when someone wants out
(`ExitRequested`: the window's close button, or `--run-seconds`). This module's handler
decides (slice 5, BP29):

- **The close button while the transport plays**: the window is hidden (`show(false)`),
  and the run goes on headless with the OS's media surface live. No frame is drawn while
  it is hidden. `MediaRaise` (MPRIS's `Raise`, from `blackwing_media`) shows it again, as
  it was.
- **Closed while paused or stopped**, or anything else that wants out: `engine_request_quit`.
- **Hidden that way, and the transport stops** (the queue ran out, or a stop): the run
  ends. Paused while hidden, it waits: the flyout can play it again.

The engine then shuts down in order, so the session is saved and the device closed
cleanly.

It exists apart from `blackwing`'s own module because it names the frame loop's and the
window's events. `blackwing play`, `library` and `help` start an engine without the
frame loop, the window or a face, on a machine that may have no display, and a module
that named `ExitRequested` there would not start.

It also counts: sim ticks, frames drawn (a system in the Frame cycle, which runs once
for each), and both again for the time the window was not seen (`WindowVisibility`).
Slice 4's numbers for a still, a hidden and a stalled window were read from it.

## Using it

Its engine name is `window`. Commands, on the engine's command line (stdin, or
`--cli-port`):

| command | what |
|---|---|
| `window status [reset]` | shown or hidden; ticks and frames since the counts were last zeroed, and of those, while hidden; `reset` zeroes them |
| `window minimize`, `window restore` | hides and shows the window (a Wayland desktop restores nothing a program asks it to) |
| `window close` | what the close button does (hidden while playing, else the run ends), so it can be driven from a command line |
| `window stall SECONDS` | holds the main thread: no pump, tick or frame for that long, as a compositor stalling a resize does |

## See also

- [blackwing](../blackwing/README.md): which face is up, or none, and the two loops.
- [ui_dev](../ui_dev/README.md): the development face.
- [The slice 4 notes](../../docs/design/slice-4-notes.md): the numbers.
