# blackwing

The application: the executable around `blackwing_player`. For now it is headless. It
plays the files named on its command line, says what is happening on stdout, and exits
when the queue is done.

## Using it

```
blackwing [options] play FILE...     play the files in order, then exit
blackwing help                       the commands the engine knows
```

| option | what |
|---|---|
| `--device NAME` | the output device: `default`, or `capture` (no hardware) |
| `--capture-to FILE` | play into the capture device and write what reached it, as raw PCM, at exit |
| `--lead SECONDS` | how far ahead of the playhead audio is kept (5) |
| `--verbose` | the engine's INFO lines |

With a terminal on stdin, the engine's command line is live while it plays: `player
pause`, `player play`, `player next`, `player stop`, `player status`, `quit`. Ctrl-C
stops cleanly. The exit status is 0 when the queue ran out, 130 when interrupted, 2 for
a bad command line, 3 if a dev build leaked.

## How it works

`play` is sugar: it runs `player add` for each file and `player play`, the player's own
commands, which emit the player's events. The module (`blackwing_module`) has five
handlers on the player's notifications: four print a line, and `PlayerQueueEnded` asks
the engine to quit.

The loop is this executable's own, not `tool_main`'s. It runs the engine's ticks at the
clock's rate while something is playing or about to, and four times a second otherwise,
so a player left paused costs nothing.

## Platform notes

- **There is no real output device yet.** `--device default` fails each file in words
  until PipeWire arrives (slice 1, milestone 4). Until then, `--device capture`.
- **stdin is a command line only when it is a terminal**, and only on POSIX so far.

## See also

- [blackwing_player](../blackwing_player/README.md): everything this executable says to
  the player, and hears from it.
- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): what it will grow into.
