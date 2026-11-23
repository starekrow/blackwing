# blackwing_player

The player, without a face or a library: a queue, a transport, and an API that is nothing
but events and tables on Korvid's busses. It turns "play this, then that, seek, skip,
louder" into sources, stages and recuts on `hifi`. A face, a script, the command line and
the OS's media keys all drive it the same way, and another project can link it and have a
player.

**Status:** a skeleton. No code yet; this page says what the module will hold.

## What it will hold

- The queue and the transport, and what plays next.
- Commands in (`PlayerPlay`, `PlayerSeek`, `PlayerEnqueue`, `PlayerSetParam` and the
  rest), notifications out (`PlayerTrackStarted`, `PlayerTrackFailed`), state in
  `player.transport` and `player.queue`. The PRD has the sketch.
- Opening what it is handed: a handle to a decoder to a source on `hifi`.
- Deciding when a change needs a recut.
- Failure: a track that will not play fails its row and the next one plays.
- The session: the queue and position survive a restart.

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): the plan this module is part of.
- [modules](../README.md): the other modules and how they depend on each other.
