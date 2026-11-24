# blackwing

The application: the executable around `blackwing_player`, `blackwing_library` and a
face. With a face it is a window; with none it plays the files named on its command
line and exits when the queue is done, or keeps a library and plays from it until told
to quit, and says what is happening on stdout.

## Using it

```
blackwing [options]                  the window, with the development face
blackwing [options] play FILE...     play the files in order, then exit
blackwing [options] library [DIR...] keep the library, adding the directories to it, until quit
blackwing [options] run FILE.lua [ARG...]  run a script over the player and the library
blackwing help                       the commands the engine knows
```

| option | what |
|---|---|
| `--face NAME` | which face is up: `dev`, or `none` (no window). Without a command it is `dev`; with one, `none` unless said |
| `--device NAME` | the output device: `default` (PipeWire on Linux), or `capture` (no hardware) |
| `--capture-to FILE` | play into the capture device and write what reached it, as raw PCM, at exit (with no face) |
| `--lead SECONDS` | how far ahead of the playhead audio is kept (5) |
| `--data DIR` | where the library keeps its index and its list of roots (the per-user data directory's `library/`), and with it the session (the user's settings directory) |
| `--no-session` | neither restores how it was left nor saves it |
| `--verbose` | the engine's INFO lines |
| `--trace-events` | every event as it is delivered, with who emitted it (dev builds) |
| `--run-seconds N` | ends the run after N seconds: tests and measurements |
| `--cli-port N` | the engine's command line on a loopback port |
| `--size WxH`, `--renderer auto\|gpu\|cpu`, `--screenshot FILE`, `--screenshot-frame N`, `--borderless`, `--no-vsync`, `--hitch-ms N`, `--goldberg PORT`, `--no-goldberg` | the frame loop's own, passed to it when there is a face |

With no display, `blackwing` alone says so and exits 1; a command given `--face dev`
says so and runs with no face. `play`, `library`, `run` and `help` need no display.

**The session.** With a face, or a library kept open, `blackwing` comes back as it was
left ([blackwing_session](../blackwing_session/README.md)): the queue in its order, the
place and the position, paused, the volume and the equalizer, and the face's own part.
It is saved a few seconds after anything changes, every ten seconds while playing, and
at exit, so a kill loses seconds. `play` and `run` are commands, not sittings: they
neither restore nor save.

`run` loads one Lua script ([blackwing_script](../blackwing_script/README.md)): every
command, notification, table and resource of the player and the library by the name
native code uses. The script ends the run: with the code it gives `kv.quit`, or 1 if it
raises, which is said with its file and line.

With a terminal on stdin, the engine's command line is live while it plays: `player
pause`, `player play`, `player next`, `player previous`, `player jump ROW`, `player seek
SECONDS`, `player seekby SECONDS`, `player volume DB`, `player mute on|off`, `player set
STAGE PARAM VALUE`, `player eq BAND DB`, `player eq preset NAME`, `player eq presets`,
`player remove ROW`, `player move ROW [BEFORE]`, `player insert BEFORE FILE...`, `player
clear`, `player stop`, `player status`, `library add DIR`, `library remove ROOT`, `library
status`, `library find WORDS`, `library play N` (the N-th the last find listed), `library
override N FIELD [VALUE...|--clear]` (no value hides the field), `quit`; and with a
face, `window status [reset]`, `window minimize`, `window restore`, `window stall
SECONDS`, `face press ID`, `face show KEY`, `face stats`. Ctrl-C stops cleanly, and so does closing the window. The exit status is 0 when the queue ran out, 130 when interrupted, 2 for
a bad command line, 3 if a dev build leaked; with `run`, the script's own.

## How it works

`play` is sugar: in the first tick `blackwing.tick` emits `PlayerEnqueue` for each file
and `PlayerPlay`, the events the player's own commands emit (`library DIR...` emits
`LibraryAddRoot`). A path is whatever the shell passed, any length the OS takes. The module (`blackwing_module`) has six
handlers on the player's notifications: five print a line ("at 71.5 s of 1" for a
seek), and `PlayerQueueEnded` asks the engine to quit (not in `library` mode). Two more
print the library's walks as they finish and `library find`'s answer; `library play`
reads the find's row from `library.rows` and emits `PlayerEnqueue` and `PlayerPlay`. The
player and the library do not know each other; what joins them (a file queued is scanned
next, what the library collects is queued) is [blackwing_glue](../blackwing_glue/README.md),
which is in both engines.

**Two loops, one rule.** The player asks for the next tick (`engine_want_ticks`) while it
plays, or has anything on its way to the ear or off the disk; the library asks while it
scans or owes a window an answer. A loop runs ticks at the clock's rate while anyone
asks, and four times a second when no one does.

- **With a face** the loop is Korvid's frame loop (`frameloop_main`, given a command line
  this file makes), paced `Decoupled`: those ticks, a frame only when a screen changed,
  and no frame while the window is hidden. The engine holds every module linked. The
  command line's files become the engine's commands in the first tick
  (`blackwing.tick`), which also ends the run on Ctrl-C; closing the window ends it too
  ([blackwing_window](../blackwing_window/README.md)). The frame loop shuts everything
  down, memory included, so nothing of this file's runs after it.
- **With none** the loop is this file's own, forty lines over `engine_frame` and
  `platform_pump`, and the engine is started from eight modules by name (`platform`,
  `cli`, `player`, `library`, `glue`, `session`, `script`, `blackwing`): the frame loop, the window, Krome and the
  face are linked and left out, so nothing asks for a display.

With `hifi` holding the device, a player left paused with no face wakes four times a
second and does nothing else: 0.03% of a core on the Linux laptop. With the face up it
draws nothing and ticks as rarely, but the frame loop polls the window every 4 ms:
about 230 wakeups a second, 2.5% of a core
([the notes](../../docs/design/slice-4-notes.md), KR26).

## Platform notes

- **Linux plays through PipeWire**, which mixes and may resample, so `player status` says
  `shared`. Where PipeWire is not running, each file fails in words.
- **stdin is a command line only when it is a terminal**, and only on POSIX so far.

## See also

- [ui_dev](../ui_dev/README.md): the development face.

- [blackwing_player](../blackwing_player/README.md): everything this executable says to
  the player, and hears from it.
- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): what it will grow into.
