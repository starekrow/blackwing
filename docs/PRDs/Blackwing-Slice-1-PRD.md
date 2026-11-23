# Blackwing Slice 1 — Sound out

Nov 22, 2026 · draft, first review folded in

## Goal and exit criteria

Slice 1 builds the audio path at its smallest useful size: a FLAC file is read, decoded
ahead of time into the lead, and pulled out by a device's realtime callback, with a
player in front that is driven by events and read through tables. The
[master PRD](Blackwing-Master-PRD.md) says what each piece is; this says what is built
now, in what order, and how we know it worked.

The slice is the proof of the spine. If three threads cannot hand audio along without
the realtime one ever waiting, if "when the listener hears it" cannot be computed from
what a device reports, or if a player with no function calls is awkward to drive, this
is where it shows, before a recut, a library and four faces are built on top. There is
no control that changes the sound in this slice: no seek, no volume, no EQ, no recut.

It is done when:

- `blackwing play a.flac b.flac` plays both through the default PipeWire device, in
  order, and exits after the last. Ctrl-C stops cleanly.
- Played into the capture device (below), the samples that reach the device are the
  file's samples exactly: their MD5 is the MD5 that FLAC keeps in its own header, for
  16-bit and 24-bit files, mono and stereo, at 44.1, 48, 96 and 192 kHz.
- Two files of one format played back to back arrive as one stream: the capture is the
  two files' samples end to end, with no frame added or lost between them.
- Ninety seconds play through PipeWire with the underrun count at zero while another
  process hammers the disk the file is on, three times over: from the laptop's built-in
  disk and from two USB drives of different speeds. (The hour-long run is owed, not
  dropped: see Owed after the gate.) On the capture device, a reader made to stall for two seconds
  causes no underrun; one made to stall past the lead causes one, which is reported,
  and playback resumes by itself.
- `PlayerTrackStarted` is emitted when the track's first frame is heard, not when it is
  decoded: on the capture device with a declared latency, at exactly that frame.
  `player.transport`'s position follows the device's clock.
- Pause and resume take effect within one device period and do not click.
- The device thread is marked realtime and a dev build runs every test without it
  touching the allocator, a lock or the log.
- A file that is missing, is not FLAC, or is cut short fails its own queue row with a
  reason in words, and the next file plays. A cut-short file plays what it had.
- Everything above is driven by events: the command line's `play`, `pause` and the rest
  emit the same `Player*` events a face will, and `--trace-events` shows them.
- Paused, the process uses no CPU to speak of.
- Numbers, in the notes: decode speed as a multiple of realtime, the callback's worst
  time, the memory held, on the Linux laptop.

## What the code has

- **Blackwing**: skeletons. Every module is a `module.lua` and a page; the executable
  prints a line.
- **Events, tables and resources** (Korvid `core`): event types as streams the schedule
  orders, ktables, published resources, systems with declared read and write sets. The
  player's API is made of these and needs nothing new from them.
- **Asynchronous I/O** (Korvid `platform`, `io.h`): reads queued with a priority and
  reaped as completions, on workers. The player reads files with it.
- **The realtime flag** (Korvid `core`, `thread.h`): `thread_set_realtime` marks a thread
  that must not reach the general heap, and dev builds abort when one does.
- **A headless main** (Korvid `tool`): `tool_main` starts the engine with no window and
  runs a command-line request. It ticks as fast as the work allows, which suits a
  converter and not a player that sits for hours (see What the engine gets).
- **The command line** (Korvid `cli`): modules declare commands; a tool's arguments
  become one request.
- **`platform_audio`** (Korvid): the model for a device module, and not used. It opens
  the default device through SDL3 and fills f32 frames; it has no device choice, no
  integer formats and no reported latency.
- **PipeWire**: the machine runs 1.6.2. Its development headers are not installed, and
  Korvid's Linux toolchain has no `pipewire` external yet.

## The shape

### Where things are

| module | this slice |
|---|---|
| `hifi_codec` | the decoder contract; the FLAC decoder (dr_flac, vendored) |
| `hifi_platform` | the device interface; the PipeWire backend; the capture device |
| `hifi` | sources, the render thread, the lead, the marks, the clock, the output callback |
| `blackwing_player` | the queue, the transport, reading files, the events and tables |
| `blackwing` | the headless executable and its `play` command |

`hifi_eq`, `media_platform`, the library, the tags and the faces are untouched.

### Three threads

```
main thread (the engine's tick)        render thread (hifi's)        device thread (the OS's)
  blackwing_player's systems             decodes, converts             the callback
  commands in, tables out                fills the lead                takes from the lead
        |                                      |                             |
        +--- sources, pause, flush ----------->|                             |
        |<-- marks, frames taken, underruns ---+-----------------------------+
                                               +------- the lead (a ring) -->+
```

Each arrow is a lock-free queue or a counter; nothing on the right waits for anything on
its left. The render thread is `hifi`'s own, not a job in the engine's pool, so a busy
frame cannot starve it. The device thread belongs to the backend and is marked realtime
on its first callback.

### The decoder contract

A decoder is a table of functions over a state it keeps in memory it is given:

| function | what |
|---|---|
| `probe(bytes)` | whether these are its kind of file, from the first bytes |
| `open(bytes, memory)` | the format (rate, channels, bits), the length in frames if known, the gapless facts |
| `decode(out, frames)` | up to that many frames, as integers at the file's depth; fewer at the end; an error with a reason |
| `close()` | nothing to free that it did not get from `memory` |

`bytes` is the whole file in memory in this slice, read by the player; a file larger than
the read-ahead cap fails with that as its reason. A decoder opens nothing, allocates
only from what it is handed and calls nothing of the player's. Seeking, and reading a
window of a file too large to hold, join the contract in slice 2.

Decoders return integers, not floats, so that "the file's samples" means something.

### The lead

The render thread turns sources into frames in the device's format and writes them into
the lead, a single-producer, single-consumer ring sized for the lead's length at the
stream's rate. It fills the ring to the target and sleeps until the level drops below a
refill mark or it is told something changed.

In this slice the only conversion is a widening of sample format (16 bits into a 24- or
32-bit device word, or into f32 when that is all a device takes). Each is exact for
sources up to 24 bits. There is no resampler and no dither: the device is opened at the
source's rate and at its depth or wider.

When a source ends and the next has the same rate and channels, the render thread goes
straight on into the same ring. When it differs, the lead drains, the device is reopened
at the new format, and playback continues after a gap.

### Marks and the clock

The stream is counted in frames from when the device was opened. When the render thread
writes the first frame of a source at stream frame *n*, it posts a **mark**: (*n*, the
source, its position). The device thread counts the frames it has taken. The backend
reports its delay: how many frames lie between the callback and the listener's ear.

```
frame heard now  =  frames taken  -  the device's delay
```

Each tick the player compares the frame heard now against the marks. A mark that has
been passed is a `PlayerTrackStarted`; the position in `player.transport` is the frame
heard now, less the current mark's frame. Nothing reads the decoder's position. This is
the mechanism gapless, the recut and the visualizer all stand on later, so it is built
whole here.

### The device

`hifi_platform`'s interface, per backend:

| | what |
|---|---|
| devices | the list, each with the formats it takes, and events when it changes |
| open | a device, a rate, a channel count, a sample format, a callback; what was actually granted |
| the callback | "fill these frames", on the backend's realtime thread |
| delay | frames between the callback and the ear, as of now |
| close | |

Two backends in this slice:

- **PipeWire**: a playback stream on the default device, asking for the source's rate.
  PipeWire's graph is shared: it may resample to the graph's rate and it mixes, so this
  backend plays everywhere and proves nothing about bit-perfect output.
- **Capture**: no hardware. It pulls frames at a clock the test drives (or at wall-clock
  speed), keeps what it was given, and has a declared delay. It is how the exit criteria
  are checked, and it stays as the device for every later test.

`hifi.output` says which device is open, what was asked and what was granted, whether it
is shared, and so the bit-perfect state: *identical* only into a device that grants the
format unshared, which in this slice is the capture device alone.

### The callback

The callback takes frames from the lead. When the lead is empty it writes silence and
counts an underrun. Its one piece of arithmetic is a ramp of a few milliseconds to
silence and back, on pause, resume, stop and underrun, so none of them clicks. It is the
seed of slice 2's join.

### The player

`blackwing_player` owns the queue and the transport. Rows in `player.queue` go *waiting*,
*reading*, *ready*, *playing*, *done* or *failed*. The row playing and the one after it
are read into memory through the engine's I/O queue, within the read-ahead cap, and
handed to `hifi` as sources once a decoder accepts them.

| name | kind | in this slice |
|---|---|---|
| `PlayerEnqueue` | event in | a path; the row is appended |
| `PlayerPlay`, `PlayerPause`, `PlayerStop` | events in | the transport |
| `PlayerNext` | event in | drops what is in the lead, with the ramp, and starts the next row |
| `PlayerTrackStarted`, `PlayerTrackEnded` | events out | at the frame heard |
| `PlayerTrackFailed` | event out | the row and the reason |
| `HifiUnderrun` | event out | how long |
| `player.transport` | resource | state, the row, the position heard, the length |
| `player.queue` | ktable | the rows |
| `hifi.output` | resource | as above, with the lead's level and the underrun count |

Between the player and `hifi` are ordinary calls that queue a message and return: add a
source, pause, resume, flush, and a poll that returns marks, counts and failures. The
master PRD left open whether this should be events too; this slice's answer is no, on
the grounds that `hifi` has one caller and is below the level a face or a script reaches.

### The executable

`blackwing` is a headless engine with the modules above. `blackwing play FILE...`
enqueues and plays, and exits when the queue is done; `--device capture` and
`--capture-to FILE` choose the capture device and write what it got. The other commands
(`pause`, `next`, `stop`, `status`) exist so that the events have a second caller before
there is a face.

## What the engine gets

Three things this slice needs from Korvid, each small, each to be agreed before it is
built:

- **A `pipewire` external** in the Linux toolchain (its library and its two include
  directories). A project cannot define an external of its own today. The development
  package (`libpipewire-0.3-dev`) must also be installed on the build machine.
- **A headless loop that sleeps.** `tool_main` ticks as fast as the work allows. A
  player needs a loop that ticks at a modest rate while playing and waits for an event
  while paused. Either `tool` learns a pacing option or `blackwing` carries its own
  small loop.
- **A long-lived thread that is not a job**, for the render thread. `os_thread_start`
  may already be all that is needed; whether the engine wants to know about such
  threads (for its profiler, its shutdown, its leak report) is its call.

## Out of scope

Seek, skip-with-crossfade, volume, the EQ and the recut (slice 2). MP3, tags, the
library (slice 3). Any face (slice 4). Windows, macOS, media keys, hot-plug, device
choice beyond default and capture (slice 5). Resampling and dither. Exclusive output on
real hardware. Gapless beyond lossless files of one format. Files too large to hold in
memory. Sources of more than 24 bits staying exact.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| BS1 | The decoder contract in `hifi_codec`, as plain data and functions | P0 |
| BS2 | The FLAC decoder on it (dr_flac), allocating only from memory it is handed | P0 |
| BS3 | The device interface in `hifi_platform`: devices, open, callback, delay, close | P0 |
| BS4 | The capture device, with a driven clock and a declared delay | P0 |
| BS5 | The PipeWire backend, on its realtime thread, reporting its delay | P0 |
| BS6 | The lead: a lock-free ring filled by `hifi`'s render thread to a target | P0 |
| BS7 | Exact widening of sample formats; no other conversion | P0 |
| BS8 | Consecutive sources of one format continue in one stream; a change of format reopens | P0 |
| BS9 | Marks, the frame count and the delay give the frame heard now | P0 |
| BS10 | The callback: take, silence and count on underrun, ramp on pause, stop and underrun | P0 |
| BS11 | The callback thread is marked realtime and never allocates, locks or logs | P0 |
| BS12 | `blackwing_player`: the queue, the transport, read-ahead of the next row within a cap | P0 |
| BS13 | The events and tables above, and nothing else, as the player's API | P0 |
| BS14 | A failed row has a reason and the next row plays | P0 |
| BS15 | `blackwing play` and the other commands, emitting the same events | P0 |
| BS16 | Tests: FLAC's own MD5 against the capture; the join; the stall; the clock | P0 |
| BS17 | The first files of the broken-file corpus: truncated, not FLAC, lying header | P0 |
| BS18 | An idle player sleeps | P1 |
| BS19 | `hifi.output` with the computed bit-perfect state | P1 |
| BS20 | Module pages for the five modules; numbers in `docs/design/slice-1-notes.md` | P1 |

## Milestones

Each leaves the tree building and its tests passing.

1. **The decoder** (BS1, BS2, BS17). The contract, dr_flac vendored, a test that decodes
   each file and checks FLAC's MD5. No audio path yet.
2. **The path, into the capture device** (BS3, BS4, BS6 to BS11). The device interface,
   the capture device, the render thread, the lead, the marks and the clock, the
   callback. Driven from a test with no player: bytes in, captured frames out.
3. **The player** (BS12 to BS16). The queue, the transport, the I/O, the events and
   tables, the commands; the exit criteria's tests, all on the capture device.
4. **Sound** (BS5, and the engine's part). The `pipewire` external, the backend, the
   headless loop. The first time it is heard.
5. **Close** (BS18 to BS20). The ninety-second runs on the three disks, the idle check, the pages, the numbers, the master
   PRD corrected where this slice proved it wrong.

Sound comes fourth on purpose: milestones 1 to 3 need nothing from the engine or the
machine, and everything they prove is checked by a number, not by ear.

## Owed after the gate

- **The hour.** An hour through PipeWire with the underrun count at zero while another
  process hammers the same disk, on each of the three disks. It does not hold the slice
  open, but it is still run, its result goes in the notes, and any underrun it finds is
  a defect to fix, not a number to record.

## Decided

Nov 22, 2026, in review of the first draft.

- **The draft stands**, its open questions included, until one of them bites.
- **The gate closes on ninety seconds**, on the built-in disk and two USB drives of
  different speeds. The hour is owed after it.

## Open

- **PipeWire or ALSA for the path that can be proved.** PipeWire is the notes' choice and
  plays on every desktop, but its graph is shared. Opening an ALSA `hw:` device directly
  is exclusive and is the usual bit-perfect route on Linux, and PipeWire has to let go
  of the device first. This draft builds PipeWire now and leaves ALSA `hw:` for slice 5
  or P1, which means the indicator cannot read *identical* on real Linux hardware until
  then.
- **Integers or floats in the lead.** The notes say float32 internally. This draft keeps
  the lead in the device's own format, so with no DSP the path is integers end to end
  and "exact" needs no argument. Slice 2's stages will want floats; where the change of
  representation sits is that slice's question, and 32-bit sources are not exact through
  float32.
- **The numbers**: the lead (five seconds?), the refill mark, the ramp (five
  milliseconds?), the read-ahead cap (256 MB?), the tick rate while playing.
- **Whether the player exits when the queue ends** once there is a face. In this slice
  it does.
- **MD5 in tests**: Korvid's `core` has SHA-256 and no MD5. A small one in the test
  directory is enough.
- **The three engine items** above, each a conversation with Korvid.
