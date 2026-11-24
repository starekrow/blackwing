# blackwing_glue

Where the player and the library hear each other. `blackwing_player` is a player with no
library, which another project could link, and `blackwing_library` a library with no
player. An application with both puts this module in its engine.

## How it works

Two handlers, and no state:

- **A file queued is the library's priority.** `PlayerEnqueue`, heard as the player hears
  it, becomes `LibraryPrioritize` for its path: its tags are read next, ahead of the scan,
  so a file played from a library still being read has its title soon.
- **What the library collects is queued.** Each `LibraryCollected` (the answer to
  `LibraryCollect`: the paths of every track of a folder, an artist, an album, a genre, a
  search or one track, in order) becomes a `PlayerEnqueue` at the end of the queue.
  Queueing a row of any kind is then one command to the library, from a face or a script;
  neither it nor the library says the word "queue", and the player never hears of a
  library. The handler runs after the sim, so its enqueues are the next tick's.

Whoever asks for a collection decides what it is for. A face that wants the tracks played
empties the queue first (`PlayerQueueEdit`) and plays when the last path has come
(`PlayerPlay`); the glue only forwards.

Its engine name is `glue`. It was `blackwing`'s own until slice 4, which moved it out so
that the face's tests could run the player, the library and this together.

## See also

- [blackwing_player](../blackwing_player/README.md), [blackwing_library](../blackwing_library/README.md):
  the two sides.
- [blackwing](../blackwing/README.md): the application.
