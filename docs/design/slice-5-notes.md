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

## Milestone 2, D-Bus

Nov 27, 2026.

**The engine first (KR49).** The engine's sockets were TCP to loopback, polled once per
frame, and their waiting twins wait in quarter-second slices. A bus is a Unix-domain
socket, the connection's thread must sleep until the bus speaks or a caller posts, and
logind answers with a descriptor. Asked, the user said to build it into the engine rather
than around it: Korvid's `platform` has `net_connect_unix_now`, `net_socket_pair`,
`read_with` and `write_now_with` (`SCM_RIGHTS`), and `NetWaker` with `net_wait`, an exact
wait. On Korvid's branch `net-unix-sockets`, not merged; its 1,336 platform checks pass,
clean under the address and undefined-behaviour sanitizers. Windows says `EAFNOSUPPORT`;
the macOS lines are compiled when milestone 6 reaches the Mac.

**What was built.** `dbus`: the wire format, messages, addresses, `EXTERNAL` with
descriptors, a thread per connection, calls matched to replies, names, matches, and
objects of our own whose `Introspect`, `Peer` and `Properties` the thread answers from
their description. About 2,600 lines with no library under it, and 1,100 of tests.

**The spec held no surprises; two things were ours to get right.** A dict entry
(`{sv}`) is a complete type only as an array's element, so measuring the type about to be
opened needed its own rule, which the first build lacked and the tests found at once. And
a connection that owns a name is sent the bus's own `NameAcquired`, which arrives in
`dbus_next` before anything else: a customer skips the bus's signals it did not ask for.

**Hostile input.** A message with sound framing and bad content is refused and the
connection kept; one past our 4 MiB is read past; bytes that cannot be framed end the
connection, since nothing after them can be found. All three tested over a stand-in bus.

**Measured:**

| what | |
|---|---|
| a round trip through a private `dbus-daemon` (`Peer.Ping`, one connection to another's thread and back), 200 | median 40 us, 99th 96 us, worst 129 us |
| an idle connection's thread, for a second | 0 wakeups |
| fuzzing under the address sanitizer, a million rounds | 1,000,000 made messages parsed and copied back byte for byte; 40,000,000 mutated, 4,570,308 parsed and read to their end, 35,449,692 refused in words, none read out of bounds; 90 s |
| `busctl --user get-property` on a name with MPRIS's identity | `s "Blackwing (dbus test)"`: the milestone's proof |

**Not done, and owed:** the system bus is opened by its address and not yet tested (logind
is milestone 3's); nothing sends a descriptor but the tests; GNOME's view of the name is
milestone 3's, when there is a player behind it.

| suite | checks |
|---|---|
| `dbus` | 162 (new) |
| Korvid `platform` | 1,336 (KR49's sockets, on its branch) |

## Milestone 3, the surface on Linux

Nov 27, 2026.

**What was built.** `media_platform` has its Linux backend: MPRIS, keep-awake and sleep,
answered on a thread of the surface's own. `blackwing_media` joins it to the busses:
commands in as the player's, the track heard (titled by the library, with its art) and
the state out, keep-awake while playing, sleep as an interruption. `blackwing_window`
hides a window closed while the music plays, and shows it again on MPRIS's `Raise`
(BP29). `blackwing` asks for the surface in a sitting and in `play`; `--no-media` says no.

**On this desktop** (GNOME on Wayland), playing the test tone into a null sink of its own:
`busctl --user` read the name, `Identity`, `Playing` and a `Position` that moved between
two reads; the portal's suspend inhibitor was in `gnome-session-inhibit --list` and the
delay in `systemd-inhibit --list`; the name went with the run. Driven through `--cli-port`:
`window close` while playing hid the window (0 frames drawn while hidden, the music on),
`busctl --user call ... Raise` showed it, and closed again and stopped, the run ended with
0. The default output was the USB DAC before and after.

**Found:**

- **A relative path is not a URL.** `blackwing play some/file.flac` showed
  `file://some/file.flac`. The surface's path is made absolute from the current directory.
- **The portal names the app by its process tree.** Run from inside the Claude app, the
  portal's inhibitor said `com.anthropic.Claude: Playing music`. From a terminal of your
  own it is Blackwing's; with a `.desktop` file installed it is Blackwing's name.
- **Not logind's `sleep` for keep-awake.** The PRD said `idle:sleep`; `sleep` also holds
  off a suspend the user asks for, which keep-awake must not do. `idle` only.
- **Closed and paused waits.** The PRD has "closed while paused ends the run"; a window
  closed while playing and then paused from the flyout stays, hidden, so it can be played
  again from there. Stopped (the queue out, or a stop) ends it.
- **Two engine calls a test must make** when its engine interns enough names:
  `names_shutdown()` before `mem_shutdown()`, or the name table is counted as a leak.
- **My mistake, harmless:** removing the manual null sink, the first `pw-cli destroy` was
  given an id read off a `grep` with context lines, which was a QEMU stream's port and not
  the sink. It was refused (this client may not destroy another's port) and the port is
  there. Sinks made by hand are now removed by the id read from their own record.

**Not done, and owed:**

- **GNOME's controls and the keys by hand**: the flyout, the lock screen, the media keys,
  a headset's buttons, by hand at the close.
- **A real suspend**: tested against a stand-in logind, not by closing the lid.
- **A `.desktop` file**, so the shell has Blackwing's icon and name, and the portal
  attributes the inhibitor right; with the packaging.
- **MPRIS's `LoopStatus` and `Shuffle`**, which the player does not have.
- **The library's overrides in the title**: the title is the library's row, so a title
  overridden by hand shows; the art is not an override yet.

| suite | checks |
|---|---|
| `media_platform` | 46 (new) |
| `blackwing_media` | 32 (new) |
| every other suite | as before; `dbus` 162 |

## Milestone 4, the face

Nov 27, 2026.

**What was built.** The Output panel lists `player.devices` (backend; playing, default,
chosen, gone) with a button to use each and one to follow the default, and says what is
chosen and why nothing is open. A Media tab shows `media.status`. The Tables panel lists
`media.status` too, last, so the others keep their places. Seen in a screenshot of the
face on this machine: the capture device and the three PipeWire sinks, the USB DAC the
default.

**Found:** a face's dev run reads its markup from the executable's asset pack, which is
made by building `blackwing`, not by `kormake test ui_dev`: a screenshot taken after only
the tests showed the old Output panel.

| suite | checks |
|---|---|
| `ui_dev` | 292 (284: the devices shown and chosen, the Media panel with no surface) |

## Milestones 5 and 6: waiting for their machines

The engine's Windows toolchain is MSVC, found on Windows itself, so nothing is
cross-compiled from Linux; macOS is the M3's. Both wait for a session on those machines.
Milestone 7's resampler went first instead: it needs neither.

## Milestone 7, part 1: the resampler

Nov 27, 2026.

**Its own module**, `hifi_resample`: polyphase windowed sinc at the exact ratio of the two
rates, a Kaiser window for 160 dB, passband to 0.4535 of the lower rate, in f64 over
memory it is handed. Measured over eight pairs of rates: the passband within 8.2e-8 dB,
the phase within 3.1e-12 rad, everything that is not the sine at -170.4 dB, a stopband
sine's alias at -166.2 dB. Cost, stereo: 44.1 to 48 kHz 35 times real time, 96 to 48 17
times, 192 to 44.1 9 times (about an eighth of a core while the lead fills).

**Where it runs, kept simple** (the user's word): between the decoder and the source
ring. The PRD called it a stage, but a stage changes frames in place, one in and one out,
and a resampler does not. Before the source ring, everything after it (the ring, the
chain, recuts, the stream frames, the callback) counts the device's frames as it would
at the file's rate, and none of it changed. What did: where a source's marks fall (where
its first frame lands in the resampler's output), its position (stream frames at the
ratio of the rates), the resampler running on across a gapless join of two sources at
one rate and drained where the stream ends, and a device asked for one rate that grants
another going on with the next source at the rate it was asked for. Resampled frames
take the float path, dithered to the device's word.

**Proved** through a capture device that takes only 48 kHz (`HifiCaptureDesc::only_rate`),
under three 44.1 kHz MP3s played gaplessly: what reached it was the three run through a
resampler of the test's own as one stream, to within 2 LSB of 24 bits (the dither), with no
seam at the joins; a seek while resampled was heard at its frame. `player.output` has
`resampled_from` (the player's surface is 0.3.0) and is not `exact` while it is.

**Not yet**: nothing on Linux asks for it, since PipeWire takes any rate (shared) and
resamples itself. It is for alone (ALSA `hw:`, WASAPI exclusive, hog mode), which is the
rest of milestone 7; BR22 with it. Also owed: a source that fails to open just after a
gapless join leaves the last half filter of the one before undrained (a few
milliseconds), since the stream ends where it failed.

| suite | checks |
|---|---|
| `hifi_resample` | 40 (new) |
| `hifi` | 369 (356: resampled, gapless and sought) |
| `blackwing_script` | 42 (the surface's version) |

## Milestone 7, part 2: alone on Linux

Nov 27, 2026.

**What was built.** A device can be opened alone (`--alone`, `PlayerSelectDevice::alone`,
`player device NAME alone`, a button on the face's Output panel), kept in the session.
On Linux that is the card's kernel PCM device, spoken to through its own ioctls
(`sound/asound.h`) with no library under it: alsa-lib is LGPL, and the rule that made
`dbus` ours makes this ours. PipeWire lets go through WirePlumber's device reservation,
asked over `dbus`, and has the card back on close. The format follows the file (BR22),
`hifi` resampling where the device has not the rate and dithering down where it has not
the word. Paused alone past `exclusive_hold` (the second setting read, 60 s), the device
is let go and the next play opens it alone again from what was heard. Refused, it plays
shared and says why.

**On this machine's USB DAC**, which takes only 48 kHz, 16-bit or packed 24-bit
(`S24_3LE`), with silence (a FLAC of zeros made for it):

| what | |
|---|---|
| 16-bit 44.1 kHz asked | 48 kHz S16 granted, resampled; 240-frame periods, 9,600 buffered (200 ms) |
| 24-bit asked | packed `S24_3LE`, from `S24` |
| another program asking for the card (priority 100) | lost 5.1 to 5.8 ms after it asked |
| `blackwing --alone play` | `player status`: alone, resampled; the reservation Blackwing's |
| paused past `exclusive_hold` (2 s, for the run) | the device let go, the reservation WirePlumber's again, the transport paused at what was heard |
| played again | opened alone again, going on from there |
| the default output, before and after | the USB DAC |

**Found:**

- **WirePlumber asks for the card back at once.** The moment it has let go, it calls our
  `RequestRelease` at its own priority. The first build answered yes to anyone and lost
  the device in under a millisecond. The protocol says a holder lets go to a higher
  priority: now it does, and only then.
- **WirePlumber renames the sink it makes again**: `...analog-stereo`, then `.3`, `.4`,
  `.6`, each time it has the card back. A device chosen by name (and kept in the session)
  would have looked gone. `hifi_platform` names sinks without the suffix, and targets the
  node's name now; alone uses the card it knew while the sink is being made again, so a
  reopen at once (a rate change between tracks) works.
- **The registry's announcement of a node has no card.** `alsa.card` and `alsa.device`
  are only in the node's info: each sink's node is bound to hear it.
- **The settings file's values are under `"values"`.** A hand-made `settings.json` of
  `{"exclusive_hold": 2}` is read as empty, silently; the page says the shape now. (Slice
  5's milestone 1 read `absent_device` the same way, and its test never wrote a file.)
- **Not realtime.** The device's thread asks for the realtime class and this session's
  limit refuses it (`RLIMIT_RTPRIO` 0): a desktop grants it through rtkit, over D-Bus,
  which the engine's core does not speak and Blackwing now does. Owed; no underrun was
  seen in these runs.

**Not done, and owed:** realtime through rtkit; a refusal while another program has the
card, seen by hand (JACK, or a second Blackwing); BR22's padding on a device whose
widest word is wider than the file's (this DAC's is not); exclusive output on Windows and
macOS (milestones 5 and 6 first); the reopen's cost on a DAC (a relock, a click) by ear.

| suite | checks |
|---|---|
| `hifi` | 374 (369: a narrower word dithered down) |
| `blackwing_player` | 227 (214: alone let go when paused and taken again; refused, shared and why) |
| `ui_dev` | 294 (292: the alone button) |
| `hifi_platform` | 44, and 13 more on a real card when asked |

## Milestone 7, part 3: realtime through rtkit

Nov 27, 2026.

**What was built.** The ALSA device's thread is made realtime through rtkit, over the
system bus, through `dbus`: the thread that opens the device asks
`MakeThreadRealtimeWithPID` for the device thread's kernel id (that thread allocates
nothing, and a D-Bus call does), after setting the process's `RLIMIT_RTTIME` to rtkit's
ceiling (200 ms here), as rtkit requires. `hifi_device_realtime` says what the thread got.

| what | |
|---|---|
| a test thread made realtime | `SCHED_RR` 20, with `SCHED_RESET_ON_FORK`, in 3.9 to 5.9 ms |
| the device's thread, alone on the USB DAC | `SCHED_RR 20, from rtkit` |
| PipeWire's own thread, shared, for comparison | `SCHED_RR` 20 (PipeWire asks rtkit itself) |

**Found:**

- **rtkit's class is round-robin.** `SCHED_RR`, not FIFO; the first check was wrong, the
  grant was not. What a thread got is read back and said as it is.
- **Alone on "default" took other cards.** Played alone on the default with a rate change
  between two files, the second opened card 1 (the laptop's own output) alone, and then
  card 0 (HDMI). A device opened alone as "default" was reported as "default"; its own
  sink is gone while Blackwing has the card, so the system's default had moved to another
  card, the player followed the "new" default, and each card it took moved the default
  again. Nothing was heard (the files were silence), but each card was taken from
  PipeWire for a moment; a run with music would have played it on the laptop's speakers.
  Fixed both ways: a device opened alone is named as the device it is, and `hifi` keeps
  that name for its reopens; and the player follows no default while it plays alone.
  The same two files now play on card 2 both times, and the hardware test opens
  "default" alone and then again by the name it was given, on the same card. Card 1 was
  opened alone for 0.2 s once more, with silence, by a check I had written wrong (it
  opened "default" in the moment the default was the laptop's own output).
- **Other programs, while Blackwing has the card**, play on whatever the default is then:
  the card's sink is gone, so a notification comes out of the laptop. That is alone on
  Linux, not a bug, and worth knowing.

| suite | checks |
|---|---|
| `hifi_platform` | 48 (44: rtkit), and 16 more on a real card when asked |
