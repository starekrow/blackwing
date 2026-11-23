# blackwing_player

The player, without a face or a library: a queue, a transport, and an API that is nothing
but events and tables on Korvid's busses. It turns "play this, then that, pause, next"
into sources on `hifi`, and turns what `hifi` says back into rows and events. A face, a
script, the command line and a test all drive it the same way, and another project can
link it and have a player. Its engine name is `player`.

## How it works

**One system**, `player.pump`, in PreSim on the main thread, every tick. In order: it
takes the command events that arrived, collects stats and reads that landed, takes
`hifi`'s events, lets go of pieces given back, moves items along, and publishes.

**An item's life.** `PlayerEnqueue` makes a row in `player.queue`, *waiting*. Nothing is
read until there is a mind to play. Then the item playing and the one after it are
looked at through the engine's I/O queue (a stat), and read in pieces (*reading*). Once
a file's first pieces are in and a decoder claims them (*ready*), it is lent to `hifi`
in queue order, and the rest of it arrives while it plays. The row turns *playing* when
`hifi` says the first frame was heard, and *done* when the last was. Anything that goes
wrong on the way makes it *failed*, with the reason in the row.

**The file in pieces.** Each item's file is a sparse span (`hifi_codec/sparse.h`):
address space reserved at its size, committed a piece (256 KB) at a time as each
arrives through ranged reads. Up to eight reads are out at once, in this order: the
pieces a decoder is waiting for (`hifi`'s wants), at high priority; the first pieces of
the item playing and the one after it, the same; then each of those two, on from where
it is, at normal priority. So a track starts when its first piece is in, not when the
file is.

**The memory cap** (`memory_bytes`, 256 MB) is on bytes committed across every item, not
on a file's size. Reading ahead stops at it. A piece a decoder waits for makes room: by
giving back pieces of items `hifi` no longer reads, then pieces of the item playing more
than four behind the frame heard (placed by its length in frames, loosely), and last, the
pieces read farthest ahead. A piece `hifi` may be reading goes in two steps: its byte in
the map is cleared, and its memory goes once `hifi_read_fence` has passed. A decoder
that needs a piece given back waits for it, and it is read again.

**A piece that cannot be read** (a pulled disk, a file cut short under it) is broken. When
a decoder waits for it, the player tells `hifi` the bytes will not come: what was decoded
plays, and the row ends *done*, damaged, with the reason. Before the file was lent, the
item fails.

**Lent and in play.** Two things are true of an item in `hifi`'s hands and they end at
different times. It is lent (hifi may read its span) until `hifi` has decoded it,
which is at once. It is in play until it has been heard to its end, seconds later. The
player keeps what an item has read until it is done, so a stop or a next can put what
was flushed straight back.

**Time is the device's.** `PlayerTrackStarted` is emitted in the tick after the first
frame reaches the listener, and `position` in `player.transport` counts frames heard.
The player reads no decoder position; it has none to read.

## Using it

**Play files.**

```cpp
PlayerEnqueue q{}; format(q.path, sizeof q.path, "%s", path);
ctx.emit(q);                       // a unit that declares .emits = "PlayerEnqueue, PlayerPlay"
ctx.emit(PlayerPlay{});            // outside the schedule: e.events.emit(...)
```

**Follow it.**

```cpp
const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
// t->state, t->item, t->position / t->rate seconds of t->frames / t->rate

void on_started(TickContext&, Span<const PlayerTrackStarted> events) { ... }
{.name = ".started", KV_ON(PlayerTrackStarted, on_started), .phase = Phase::PostSim},
```

**From a terminal**, in any engine that links it: `player add a.flac b.flac`, `player
play`, `player pause`, `player next`, `player stop`, `player status`.

## Quick reference

Header `korvid/blackwing_player/player.h`:

| name | kind | what |
|---|---|---|
| `PlayerEnqueue` | event type | in: `path`; a file joins the end of the queue |
| `PlayerPlay`, `PlayerPause`, `PlayerStop`, `PlayerNext` | event type | in: the transport |
| `PlayerPrevious`, `PlayerJump` | event type | in: the row before (or this one again); `item`, a row now |
| `PlayerSeek`, `PlayerSeekBy` | event type | in: `item` (0: the one playing) and `frame`; `seconds` on from the last seek |
| `PlayerSeeked` | event type | out: `item`, `frame`, `seq`; a seek's target was heard |
| `PlayerSetParam` | event type | in: `stage`, `param`, `value`; volume is stage 0, param 0 (dB), mute param 1 |
| `PlayerParamChanged` | event type | out: `stage`, `param`, `value`, `seq`; the value is what is heard |
| `PlayerStage` | component | `index`, `flat`, `params_len`, `name`, `param_names`, `params` (the values heard) |
| `PlayerTrackStarted` | event type | out: `item`, `rate`, `channels`, `bits`, `frames`, `seq`; the first frame was heard |
| `PlayerTrackEnded` | event type | out: `item`, `frames`, `seq`, `damaged`, `reason`; the last frame was heard |
| `PlayerTrackFailed` | event type | out: `item`, `seq`, `reason`; it could not be played |
| `PlayerQueueEnded` | event type | out: `seq`; everything queued is done and nothing is sounding |
| `HifiUnderrun` | event type | out: `count`, `seq`; the lead ran dry |
| `PlayerItem` | component | `id`, `state`, `damaged`, `channels`, `bits`, `rate`, `frames`, `size`, `bytes_read`, `path`, `reason` |
| `PlayerItemState`, `PlayerState` | enum | an item's state; the transport's |
| `PlayerTransport` | record | `state`, `item`, `position`, `frames`, `rate`, `queued`, `left`, `place`, `seek_item`, `seek_frame` |
| `PlayerOutput` | record | `open`, `shared`, `exact`, `sample`, `channels`, `rate`, `lead_frames`, `underruns`, `joining`, `bytes_held`, `recut_seconds`, `recuts` |
| `PlayerConfig`, `player_configure(config)` | record, function | the device, the lead, `memory_bytes` and `piece_bytes`, before `engine_init` |
| `player_transport(e)`, `player_output(e)` | functions | the resources, for code outside the schedule |
| `player_item_state_name(s)`, `player_state_name(s)` | functions | "waiting", "playing"...; "stopped", "paused"... |

Header `korvid/blackwing_player/module.h`: `blackwing_player_module`, the record.

What the module registers with the engine, as module `player`:

- the ktable `player.queue` (component `PlayerItem`, cosmetic, 4096 rows);
- the ktable `player.chain` (component `PlayerStage`, cosmetic, a row per stage: `gain`);
- the resources `player.transport` and `player.output`;
- the seventeen event types above;
- the system `player.pump`, PreSim, main thread, every tick;
- the commands `player add`, `play`, `pause`, `stop`, `next`, `previous`, `jump`, `seek`,
  `seekby`, `volume`, `mute`, `set`, `status`.

## Behaviour worth knowing

- **A command acts in the tick after it was emitted**, when the pump reads it.
- **Events of different types have no order within a tick.** An Ended and a Started
  emitted together reach their handlers in the handlers' order. Each notification
  carries `seq`, which counts up across all of them. For "what is playing now", read
  `player.transport`.
- **A failure is announced when it is found, which is ahead of the ear.** The item after
  the one playing is read and opened early, so its `PlayerTrackFailed` can come while
  the one before it is still being heard.
- **A file cut short is Done, not Failed**: it plays what it has, and its row and its
  `PlayerTrackEnded` say `damaged`, with the reason.
- **Next is not Ended.** A skipped item goes to Done with no `PlayerTrackEnded`.
- **A seek, a next, a previous and a jump are each a jump in `hifi`**: the old audio fades
  out at once, the transport says *starting* (no item) until the new audio is heard, and
  that fades in. A seek within the row heard says `PlayerSeeked`, not a new start, and
  `position` counts from its target. `seek_item` and `seek_frame` say what a seek is on
  its way to; a `PlayerSeekBy` counts from there, so repeats add up.
- **Volume is a parameter like any other**: `PlayerSetParam` sends it to `hifi`, which
  renders the lead again with it and crossfades (a recut), and `PlayerParamChanged`
  comes when it is heard. A control dragged faster than recuts are heard is said once,
  at the value heard. `player.output`'s `exact` is off while any stage is not flat, or a
  join is under way.
- **Previous** goes to the row before within the first three seconds of a row, and to
  this row's start after them (or when there is no row before).
- **Going back reopens what was done**: a row before the place that comes back (a
  previous, a jump) is read again if its file was let go of.
- **Stop keeps the place in the queue, not in the track.** The item starts over.
- **Rows are never removed.** The queue is also the history, up to 4096 items.
- **A file of any size plays** within the cap. A cap smaller than a decoder's window
  (FLAC's largest frame and 8 KB, a few pieces) cannot be played under: keep it well
  above a few megabytes.
- **A skip cancels the skipped item's reads and stat**, if they are still out, so a slow
  drive is not left reading what nobody will hear.
- **The stat goes through the I/O queue** (on io_uring, `statx`), so a sleeping network
  share does not stall a tick.
- **A file whose ID3v2 tag in front is long** is read up to the end of the tag before a
  decoder is asked about it.
- **`player.output` is the player's, not `hifi`'s**: `hifi` is not an engine module, so
  the player publishes what it reports.

## Tests

`kormake test blackwing_player` runs a real engine (platform, cli, the player, and a
module that writes down what the player emits) against the capture device with the test
driving its clock: exact at the device and joined, by events alone; `PlayerTrackStarted`
in the tick after the frame the device's delay says; pause, next and stop; failures
between good files; the command line; shutdown in the middle of everything. Two files
larger than a 40 KB cap, in 4 KB pieces: each starts before it is all read, both are
exact and joined, and no more than the cap is ever held. A file truncated on the disk
while it is read: what was read plays, and the item ends damaged, "cut short". Seek,
seek-by, previous and jump over a four-second file: the seek exact from its target
after the fade, the old audio unchanged until the jump reaches the device and then
only its fade and silence, `PlayerSeeked` with the frame, the position from the target;
two seek-bys in one tick added up; previous past three seconds a restart, within them a
row back; a jump to the third row. Volume and mute through events: the change said when
heard, the chain's row showing it and whether it is flat, `exact` off and on again, a
value above 0 dB kept at 0, mute as silence.

## Not in this module

The audio path (`hifi`). Decoders (`hifi_codec`). The stages (`hifi_eq`). Tags and the
library. The EQ and queue edits (slice 2, milestones 5 and 6). Playlists and a saved session (later in P0).

## See also

- [hifi](../hifi/README.md): what the player drives, and where its sense of time comes from.
- [blackwing](../blackwing/README.md): the executable around it.
- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md), "The player as an API".
