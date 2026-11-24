# blackwing_session

The session: how the application was left, there at the next start. What was queued,
where it had got to, how the chain was set, and whatever a face asks to have kept.

It is a customer of the player like any other. It reads the player's tables to save and
emits the player's commands to restore, and the player keeps no file of its own.

## How it works

**One file, one moment.** `session.json`, in the user's settings directory
(`platform_user_dir`'s `Settings`: `~/.config/blackwing/blackwing/` on Linux), or in the
directory the executable names. It holds the queue's paths in order, the place (a
position in the queue) and the frame heard with the rate and length it is counted in,
every parameter of the chain by its stage's name and its own, and the values others
asked to have kept. A queue saved at one time and a place at another would not belong
to each other, so they are one file.

**Saved when it changes.** Four times a second `session.tick` hashes what is kept: the
rows' ids in the list's order, the place, the parameters' values, the kept values, and
the frame when nothing is playing. When that is not what was last written, a save is due
once nothing more has changed for `change_seconds` (3): a slider dragged, or a thousand
rows added one by one, is one write. While something plays the position is saved every
`position_seconds` (10) besides, which is what a kill can lose. Paused or stopped with
nothing changing, nothing is written and no tick is asked for.

**A write is whole or not at all.** The text goes to `session.json.new` through the I/O
queue at `Low` priority, below the player's reads, and is renamed over the old file when
the queue says it is written. A crash or a full disk leaves the old session. At
shutdown a session that has changed is written before the module goes, the same way but
at once.

**Restored through the API, paused.** The file is read at start-up. In the ticks after:
the kept values are said, each as a `SessionKept`; the parameters are set
(`PlayerSetParam`, for each saved one whose stage and name the chain has); the queue is
enqueued `rows_per_tick` (64) rows a tick, so a thousand rows take sixteen ticks and do
not hold the first frame; and when the player has them all, the place is taken up
without playing (`PlayerCue`, with the frame, the rate and the length that were saved).
A program that makes sound by being started is a surprise: one press plays from where
it was. `session.status` says how far the restore has got.

**A row whose file has gone is restored anyway.** The disk may be unplugged today and
back tomorrow. It fails with a reason when it is reached, as any row does.

**A file that will not parse is set aside**, as `session.json.unreadable-<time>`, with a
line on stderr, and the start is an empty one: it is not overwritten. A file with fields
nobody knows is read for what is known, and one that lacks a field keeps that field's
default, so an older version's file is read and a newer one's as far as it is
understood.

**Anyone may keep values with it.** `SessionKeep{owner, key, number, text}` keeps a
number and a text for an owner under a key; the newest replaces the one before. At the
next start each is said once (`SessionKept`), and an owner takes what is its own. This is
how a face's part of the session is kept (which tab was up, where its list was, the
window's size) with the session knowing no face.

## Using it

```cpp
SessionConfig c;
c.enabled = true;
session_configure(c);        // before engine_init; off unless said
```

`blackwing` turns it on for a sitting (a face, or `library` kept open) and leaves it off
for a command (`play`, `run`), and `--no-session` turns it off.

## Quick reference

Header `korvid/blackwing_session/session.h`:

| name | kind | what |
|---|---|---|
| `SessionKeep { owner, key, number, text }` | event in | keeps a value for an owner under a key |
| `SessionKept { owner, key, number, text }` | event out | a kept value, said once at start-up |
| `SessionStatus` | resource | `session.status`: `phase` (`Off`, `Restoring`, `Ready`), `set_aside` and `reason`, `dirty`, `writing`, `queue_total`, `queue_restored`, `saves`, `save_bytes`, `save_seconds` |
| `SessionConfig`, `session_configure(config)` | record, function | `enabled`, `dir`, `change_seconds` (3), `position_seconds` (10), `rows_per_tick` (64); before `engine_init` |
| `session_status(e)` | function | the resource, for code outside the schedule |
| `session_peek(dir, owner, key, fallback)` | function | a number someone kept, read from the file with no engine: what a window's size is read with before it opens |
| `session_file_in(dir, out, cap)` | function | the file's path in a directory |

Header `korvid/blackwing_session/schema.h`: the field tables.

What the module registers, as module `session`: the two event types; the resource
`session.status` (cosmetic, with a field table); `session.keep` (a handler on
`SessionKeep`); `session.tick` (PostSim; reads `player.transport`, `player.queue`,
`player.params`, `player.chain`; emits `PlayerEnqueue`, `PlayerCue`, `PlayerSetParam`,
`SessionKept`).

## Behaviour worth knowing

- **A row's id stands for its path** when the session looks for a change: a row never
  changes the file it is of, so the paths are read only when a save is written.
- **The place is a position, not an id.** Ids count from 1 in each run; the third row is
  the third row.
- **Parameters are kept by name**, the stage's and their own, so a chain put in another
  order still finds them, and a stage that is gone is passed over.
- **The position kept is the frame heard**, in the device's time, as everywhere.
- **What is not kept:** a play in progress (it comes back paused), the library (which
  keeps its own roots and index), and anything nobody asked to have kept.

## Tests

`tests/` starts twice: an engine with the player on a capture device is run and shut
down, and another started on the same directory.

- **It comes back**: a thousand rows queued (most of them files that are not there), a
  jump to the five-hundredth, the volume, the mute and a band set, two values kept; quit
  while playing. The second start has the same thousand paths in order, is paused at
  row 500 at the frame it was left at with the rate and length it had, the three
  parameters as they were, the two values said once each; it restores 64 rows a tick for
  18 ticks, the longest of them under a millisecond; it writes nothing for the next
  second; and one `PlayerPlay` starts that row from that frame.
- **Killed**: the file as it stood after the position's clock had saved it three times is
  kept aside, the run goes on a third of a second and quits, and the file is put back as
  a kill would have left it. The next start is behind where it was by less than the
  clock's period.
- **Unreadable**: half a file is set aside and the start is empty, and the next change is
  saved to a new file; a version-0 file with a field nobody knows, a stage nobody has
  and a parameter with no name is read for what it has.
- **Still**: forty parameter changes in forty ticks are one write; nothing after; off is
  off.

## See also

- [blackwing_player](../blackwing_player/README.md): `PlayerCue`, and the tables it reads.
- [ui_dev](../ui_dev/README.md): a face's part of a session.
- [The slice 4 notes](../../docs/design/slice-4-notes.md): the numbers, and what the
  engine cannot yet put back.
