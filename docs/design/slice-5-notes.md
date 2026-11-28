# Slice 5 notes

What building [slice 5](../PRDs/Blackwing-Slice-5-PRD.md) found, milestone by milestone,
with the numbers. Linux and `linux-dev` unless said.

## Before milestone 0

Nov 27, 2026.

- **Blackwing did not build with Korvid's kormake.** Korvid's `bff599d` added
  `test_files`, checked against the project's root rather than the engine's
  (`tools/kormake/scripts/emit.lua`), so a project linking `krome` failed before
  compiling. Fixed in Korvid on the branch `kormake-test-files-root` (`3b7221e`), not
  yet merged there. Blackwing needs that branch, or Korvid's `main` once it has it.
- **Nothing else in Korvid's slice 13 or Krome's 7 and 8 broke Blackwing.** Every suite
  passed on the fixed kormake before any change of this slice's.

## Milestone 0, the catch-up

Nov 27, 2026.

**KR10, the render thread waits.** Its naps were sleeps of 2 to 50 ms for want of a
timed wait. Each is now `os_cond_timed_wait` on the thread's own condition, ended at once
by a command, `hifi_play`, or the new `hifi_wake`, which the player calls when a piece of
a file `hifi` holds comes in. A wake sent before the wait began is kept in a flag. Playing
out, the thread now looks every 20 ms rather than every 2. Clean under the thread
sanitizer. Two short waits remain as sleeps: for the callback to take up a join, which the
realtime thread cannot signal, half a millisecond at a time and a fade's length at most.
Not measured yet: slice 2's 2 to 13 ms for a control through PipeWire, which this should
move down by up to the old nap (5 ms with a full lead). Milestone 1 plays through PipeWire
and measures it again.

**KR15 and KR16.** The corpus's 1,100-byte path is made in one `io_make_dirs`; the
watcher's link filter is gone, and the corpus's loop (a link back up to the root) still
costs nothing. On Apple `io_make_dirs` stops at the OS's 1,024 bytes, so the corpus test's
long path will want a shorter form there (milestone 6).

**The index in the cache directory** (KR24): `~/.cache/blackwing/blackwing/library/`
on Linux; the roots and overrides stay in `~/.local/share/blackwing/blackwing/library/`.
An index found in the old place is moved once, with its WAL. Tried by hand with the XDG
directories pointed at a scratch tree: moved, and opened.

**An override file set aside is published**: `LibraryOverridesSetAside` and
`library.scan`'s `set_aside`, on `ui_dev`'s Scan panel. The conformance walk now leaves a
broken override file for the root it adds, so it hears the event with its fields.

**The window's place** (KR42). Kept and put back where the platform lets a program place
a window: under X11 (XWayland on GNOME), put back at 300, 200 as kept. Places off every
screen were clamped by Mutter before Blackwing's own check was needed (50000, 40000 landed
at 0, 138; -1250, 300 at 0, 300); Windows does not clamp, and is where the check will be
seen to work. Under Wayland nothing is placed and the place last kept is left alone.

| suite | checks |
|---|---|
| `hifi` | 342 |
| `blackwing_player` | 195 |
| `blackwing_library` | 219 (215 before: the long path checked, the set-aside event and count) |
| `blackwing_script` | 41 |
| `ui_dev` | 284 |
| the rest | as before |

## Milestone 1, devices on Linux

Nov 27, 2026.

**What was built.** `hifi_platform` lists the sinks from PipeWire's registry and the
default from its metadata, watches both, opens a sink by its `node.name` and says when an
open one has gone. `hifi` takes `hifi_set_device` and says `Lost`. The player publishes
`player.devices`, takes `PlayerSelectDevice`, says `PlayerDeviceChanged`, and holds the
rules; the session keeps the device chosen; `blackwing` reads its first setting,
`absent_device`. The capture device can be pulled and plugged on cue.

**No reconnecting.** WirePlumber moves a stream whose sink went to another sink. A
headphone DAC pulled would then become the laptop's speakers, which is the one thing a
pull must not do. So every stream names its sink (`target.object`) and asks not to be
moved (`node.dont-reconnect`), and a pull is a loss, which the player turns into a pause.
Following the default is the player's: on a new default it opens the new one and jumps
there from the frame heard.

**Found: a pull taken for an absence.** The first build moved the music to the default
when the sink chosen went, because "the chosen device is not there: the default stands
in" was applied to a device pulled from under the music. Through a real PipeWire it put
about three seconds of an 8 kHz test tone on this machine's USB DAC. Now a device that
goes while open is left to the loss, and the stand-in rule is applied only at a start, a
choice and a press of play. **Also made sound:** testing the stand-in at start (a sink
chosen that did not exist yet), WirePlumber had set the default back to the USB DAC, and
about 1.5 s of the tone played there as designed. Checks through real PipeWire since name
a null sink of their own, which cannot be rerouted.

**Found: a loss leaves `hifi` clean.** A device reopened for a source the lost one had
left half played was a state no test had seen, and a jump after it was sometimes never
joined. A loss now drops what `hifi` held, as a flush does; the player keeps the frame
heard and jumps there. Also found under the thread sanitizer: the stall check (two
seconds with no callback) read the callback's own `running`; it now goes by the callback
count and whether the device was held at the last look.

**Measured, through PipeWire into null sinks** (their graph period is 1024 frames, about
21 ms at 48 kHz):

| what | |
|---|---|
| a sink made, to listed | 10 ms |
| a sink destroyed under a stream, to lost | 2.1 ms |
| the default moved, to the music on the new one | the tick after the metadata changed; it went on from the frame heard (7,180 of 8 kHz) |
| the sink chosen made after start, to moved back from the stand-in | the tick after it was listed; from the frame heard (7,181) |
| a volume change, to heard (20, a real album) | 0.4 to 21.5 ms, median 15: within one period of the null sink |
| a seek, to heard (20) | 3.9 to 25 ms, median 16 |

The last two are not slice 2's 2 to 13 ms through the USB DAC, whose period is 128 frames:
a null sink's period is eight times longer, and bounds them. Through a real device they
are owed (milestone 8), as is KR10's effect on them.

**Not done, and owed:**

- **Headphones out on the same card**: a jack is a route of the device, not a sink of
  its own, and a route changing (headphones to speakers) is not seen yet. It wants the
  device's `Route` parameter watched, and a hand to pull a plug: by hand, at the close.
- **A sink's channels and whether it can be opened alone**: not in the registry's
  properties, so `0` and false for now; milestone 7 reads the node's own.
- **The USB DAC pulled by hand**, and a Bluetooth headset: the null sinks stand in for
  them in the tests; the real ones are for the close.
- **`play FILE` with its device pulled** waits, paused, for a press (`player play` on the
  command line); a command that cannot finish is a question for the faces' slice.

| suite | checks |
|---|---|
| `hifi_platform` | 44 (24 before: the capture device pulled; a null sink made, opened, destroyed) |
| `hifi` | 356 (342: the loss) |
| `blackwing_player` | 214 (195: pulled, waited, plugged back, chosen) |
| `blackwing_session` | 56 (47: the device kept and put back) |
| `blackwing_script` | 42 (41: the walk chooses a device and hears it move) |
| `ui_dev` | 284 (the Tables panel lists `player.devices`) |
