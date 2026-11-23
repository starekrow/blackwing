# blackwing_player

The player, without a face or a library: a queue, a transport, and an API that is nothing
but events and tables on Korvid's busses. It turns "play this, then that, pause, next"
into sources on `hifi`, and turns what `hifi` says back into rows and events. A face, a
script, the command line and a test all drive it the same way, and another project can
link it and have a player. Its engine name is `player`.

## How it works

**One system**, `player.pump`, in PreSim on the main thread, every tick. In order: it
takes the command events that arrived, collects file reads that landed, takes `hifi`'s
events, moves items along, and publishes.

**An item's life.** `PlayerEnqueue` makes a row in `player.queue`, *waiting*. Nothing is
read until there is a mind to play. Then the item playing and the one after it are read
whole through the engine's I/O queue (*reading*), checked for a decoder (*ready*), and
lent to `hifi` in queue order. `hifi` decodes at once, seconds ahead of the ear. The row
turns *playing* when `hifi` says the first frame was heard, and *done* when the last was.
Anything that goes wrong on the way makes it *failed*, with the reason in the row.

**Lent and in play.** Two things are true of an item in `hifi`'s hands and they end at
different times. It is lent (hifi may read its bytes) until `hifi` has decoded it,
which is at once. It is in play until it has been heard to its end, seconds later. The
player keeps an item's bytes until it is done, so a stop or a next can put what was
flushed straight back.

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
| `PlayerTrackStarted` | event type | out: `item`, `rate`, `channels`, `bits`, `frames`, `seq`; the first frame was heard |
| `PlayerTrackEnded` | event type | out: `item`, `frames`, `seq`, `damaged`, `reason`; the last frame was heard |
| `PlayerTrackFailed` | event type | out: `item`, `seq`, `reason`; it could not be played |
| `PlayerQueueEnded` | event type | out: `seq`; everything queued is done and nothing is sounding |
| `HifiUnderrun` | event type | out: `count`, `seq`; the lead ran dry |
| `PlayerItem` | component | `id`, `state`, `damaged`, `channels`, `bits`, `rate`, `frames`, `path`, `reason` |
| `PlayerItemState`, `PlayerState` | enum | an item's state; the transport's |
| `PlayerTransport` | record | `state`, `item`, `position`, `frames`, `rate`, `queued`, `left` |
| `PlayerOutput` | record | `open`, `shared`, `exact`, `sample`, `channels`, `rate`, `lead_frames`, `underruns` |
| `PlayerConfig`, `player_configure(config)` | record, function | the device and the caps, before `engine_init` |
| `player_transport(e)`, `player_output(e)` | functions | the resources, for code outside the schedule |
| `player_item_state_name(s)`, `player_state_name(s)` | functions | "waiting", "playing"...; "stopped", "paused"... |

Header `korvid/blackwing_player/module.h`: `blackwing_player_module`, the record.

What the module registers with the engine, as module `player`:

- the ktable `player.queue` (component `PlayerItem`, cosmetic, 4096 rows);
- the resources `player.transport` and `player.output`;
- the ten event types above;
- the system `player.pump`, PreSim, main thread, every tick;
- the commands `player add`, `play`, `pause`, `stop`, `next`, `status`.

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
- **Stop keeps the place in the queue, not in the track.** The item starts over.
- **Rows are never removed.** The queue is also the history, up to 4096 items.
- **The whole file is read into memory**, at most two at once, each under
  `read_ahead_bytes` (256 MB). A larger file fails its item.
- **Paths over 255 bytes fail**: that is the engine's I/O queue's limit today.
- **A skip cancels the skipped item's read**, if it is still being read. A slow drive
  still costs a late start: the item after it is read whole before it plays.
- **The file is checked with a blocking `stat`** before it is read. On a sleeping
  network share that will stall a tick.
- **`player.output` is the player's, not `hifi`'s**: `hifi` is not an engine module, so
  the player publishes what it reports.

## Tests

`kormake test blackwing_player` runs a real engine (platform, cli, the player, and a
module that writes down what the player emits) against the capture device with the test
driving its clock: exact at the device and joined, by events alone; `PlayerTrackStarted`
in the tick after the frame the device's delay says; pause, next and stop; failures
between good files; the command line; shutdown in the middle of everything.

## Not in this module

The audio path (`hifi`). Decoders (`hifi_codec`). Tags and the library. Seek, volume and
the EQ (slice 2). Playlists and a saved session (later in P0).

## See also

- [hifi](../hifi/README.md): what the player drives, and where its sense of time comes from.
- [blackwing](../blackwing/README.md): the executable around it.
- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md), "The player as an API".
