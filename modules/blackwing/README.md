# blackwing

The application: the executable around `blackwing_player` and `blackwing_library`. For
now it is headless. It plays the files named on its command line and exits when the queue
is done, or keeps a library and plays from it until told to quit; it says what is
happening on stdout.

## Using it

```
blackwing [options] play FILE...     play the files in order, then exit
blackwing [options] library [DIR...] keep the library, adding the directories to it, until quit
blackwing help                       the commands the engine knows
```

| option | what |
|---|---|
| `--device NAME` | the output device: `default` (PipeWire on Linux), or `capture` (no hardware) |
| `--capture-to FILE` | play into the capture device and write what reached it, as raw PCM, at exit |
| `--lead SECONDS` | how far ahead of the playhead audio is kept (5) |
| `--data DIR` | where the library keeps its index and its list of roots (the per-user data directory's `library/`) |
| `--verbose` | the engine's INFO lines |
| `--trace-events` | every event as it is delivered, with who emitted it (dev builds) |

With a terminal on stdin, the engine's command line is live while it plays: `player
pause`, `player play`, `player next`, `player previous`, `player jump ROW`, `player seek
SECONDS`, `player seekby SECONDS`, `player volume DB`, `player mute on|off`, `player set
STAGE PARAM VALUE`, `player eq BAND DB`, `player eq preset NAME`, `player eq presets`,
`player remove ROW`, `player move ROW [BEFORE]`, `player insert BEFORE FILE...`, `player
clear`, `player stop`, `player status`, `library add DIR`, `library remove ROOT`, `library
status`, `library find WORDS`, `library play N` (the N-th the last find listed), `quit`. Ctrl-C
stops cleanly. The exit status is 0 when the queue ran out, 130 when interrupted, 2 for
a bad command line, 3 if a dev build leaked.

## How it works

`play` is sugar: it runs `player add` for each file and `player play`, the player's own
commands, which emit the player's events. The module (`blackwing_module`) has six
handlers on the player's notifications: five print a line ("at 71.5 s of 1" for a
seek), and `PlayerQueueEnded` asks the engine to quit (not in `library` mode). Two more
print the library's walks as they finish and `library find`'s answer; `library play`
reads the find's row from `library.rows` and emits `PlayerEnqueue` and `PlayerPlay`. The
player and the library do not know each other: this is the glue. So is one more handler:
`PlayerEnqueue`, heard as the player hears it, becomes `LibraryPrioritize` for its path, so
playing a file has its tags read next.

The loop is this executable's own, not `tool_main`'s. It runs the engine's ticks at the
clock's rate while something is playing or about to, or the library is walking, and four
times a second otherwise.
With `hifi` holding the device, a player left paused wakes four times a second and does
nothing else: 0.03% of a core on the Linux laptop.

## Platform notes

- **Linux plays through PipeWire**, which mixes and may resample, so `player status` says
  `shared`. Where PipeWire is not running, each file fails in words.
- **stdin is a command line only when it is a terminal**, and only on POSIX so far.

## See also

- [blackwing_player](../blackwing_player/README.md): everything this executable says to
  the player, and hears from it.
- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): what it will grow into.
