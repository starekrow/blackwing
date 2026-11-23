# Blackwing Slice 2 — The recut

Nov 23, 2026 · draft, first review folded in

## Goal and exit criteria

Slice 1 built a path that plays what it is handed, from the start, to the end, with
nothing that changes the sound. Slice 2 adds the controls: seek, skip, previous, a queue
that can be edited, volume and the equalizer. The [master PRD](Blackwing-Master-PRD.md)
says how they are to feel: the lead stays seconds long, and a control is heard at once
anyway, because the path **recuts** (renders again from just ahead of the playhead) and
the output **joins** the new render with a short fade.

This is the slice where the lead has to earn its cost. If a control cannot be made to
feel immediate over a five-second lead, the lead shrinks and the first goal ("audio
never glitches") pays for it. It is also where the path stops being integers end to end,
so it is where "exact" needs a rule, not just an absence of arithmetic.

Two things slice 1 measured shape it. A track is read whole before it plays, so on a
slow disk it starts as late as the read takes: 24 seconds from a USB 2 flash drive under
load. And a decoder cannot start anywhere but the beginning. Both go first, because a
seek and a skip stand on them.

It is done when:

- **A recut that changes nothing changes nothing.** On the capture device, a track
  recut any number of times with the chain unchanged arrives with the MD5 in its FLAC
  header. This is the test of the join's alignment: one frame out and it fails.
- **A seek lands on the frame.** After `PlayerSeek`, the capture holds the file's
  samples from the target on, exactly, and `player.transport`'s position counts from the
  target, at the frame heard.
- **A change of volume is the two renders and the fade between them.** The capture is
  the old render up to the join, the new render after it, and between them the fade the
  PRD names, to the sample.
- **No join clicks.** Across every join in the tests, no step between neighbouring
  samples is larger than the larger of the two renders' own steps there.
- **A control is heard within the target.** Counted from the change reaching `hifi` to
  the join's first frame leaving the callback, through PipeWire on the Linux laptop:
  20 ms at most for a volume or EQ change once the path is **ready** (below); 100 ms at
  most for the first such change, and for a seek or a skip on a warm file. What the
  device adds after the callback (14 ms here) and what the tick adds before `hifi` are
  measured and reported beside it, not hidden in it.
- **A cold file is late, not silent, where there is something to play.** With a
  source's bytes made to arrive late: a volume or EQ change is heard late and the music
  does not stop; a seek or a skip fades the old audio at once, says *starting*, and
  starts when the first piece arrives, not when the file has.
- **A track starts from a piece of the file.** On the USB 2 flash drive under slice 1's
  load, a skip is heard within two seconds, and the skipping run at nine seconds plays
  every track it lands on, with no underrun.
- **A control that is dragged does not pile up.** Two hundred volume changes in a second
  end at the last value, with no underrun and at most one recut waiting at any time.
- **With every stage flat, the path is slice 1's.** The capture's MD5 is the file's, and
  `player.output` says *exact*; with any stage not flat, or during a join, it does not.
- **The equalizer does what its numbers say.** A band's measured response is within
  0.1 dB of its design, and no preset clips a full-scale signal.
- **A file larger than the memory cap plays**, seeks and ends, holding no more than the
  cap.
- **Queue edits behave**: remove, move, insert, clear, previous, jump to a row; an edit
  to a row whose audio is already in the lead is heard as the edit, not as the old row.
- Slice 1's tests still pass, and its ninety-second runs still show no underrun.
- Numbers, in the notes: control to ear for each control; seek time by distance; time to
  first sound from each of the three disks; memory held; the cost of a recut.

## What the code has

- **The path** (`hifi`): a render thread that decodes ahead into one ring, the lead; a
  callback that takes from it and ramps on pause, flush and underrun; marks that turn
  the device's frame count into "this source, this frame, heard now"; a device held when
  there is nothing to play. Sources are played from their first frame, in order.
  `hifi_flush` drops everything and is the only way to change what comes next.
- **The decoder contract** (`hifi_codec`): `probe`, `open`, `decode`, `close` over the
  whole file's bytes and a block of memory. No seek. FLAC only.
- **The player** (`blackwing_player`): rows appended and consumed in order; `PlayerNext`
  flushes and starts the next; each file read whole through the I/O queue, within 256 MB,
  two rows ahead. Rows are never removed, and the table is full at 4096.
- **The capture device** (`hifi_platform`): a driven clock and a declared delay, so every
  test so far is to the frame. It stays the device for every test here.
- **`hifi_eq`**: a page.
- **The engine** (Korvid, after its slice 10): ranged reads and stat through the I/O
  queue (KR5, KR6), reserve and commit of address space (`os_memory.h`), sanitizer runs
  from one command. No timed wait on a condition yet (KR10).

## The shape

### Where things are

| module | this slice |
|---|---|
| `hifi_codec` | seeking; the file as a sparse span; `Wait` |
| `hifi` | the second render and the join; the jump; the stage interface; floats and dither; the chain |
| `hifi_eq` | the gain stage (volume) and the equalizer |
| `blackwing_player` | reading in pieces; seek, previous, jump; queue edits; parameters; the chain and the indicator as data |
| `blackwing` | commands for the above |

`hifi_platform` changes only to open the device at its widest word (below). The library,
the tags, the faces and `media_platform` are untouched.

### The file in pieces

Today a decoder gets the whole file. From this slice it gets a **sparse span**: an
address range the size of the file, cut into pieces of a fixed size, and a map of which
pieces are there. Plain data, shared between threads now and between processes in P1.

- A decoder reads only pieces that are there. When it needs one that is not, `decode`
  (or `seek`, or `open`) returns **`Wait`** with the range it needs, having changed
  nothing, and is called again when the range has come. It still opens nothing,
  allocates nothing and calls nothing.
- `hifi` passes each `Wait` up as a want. The player turns wants into ranged reads
  through the engine's I/O queue, and on its own keeps reading ahead of each decoder's
  place in the file. `hifi` still does no I/O.
- The order of reading is the point: **the piece at the target first**, then on from
  there, then the rest of the file behind everything else. A track starts when its
  first pieces are in, not when it all is.
- The span is reserved at the file's size and committed a piece at a time. The cap is on
  bytes committed across all sources, not on a file's size: past it, pieces behind the
  frame heard are given back. Pieces from the frame heard forward are kept, because a
  recut starts there.
- A piece that fails to read (a pulled disk, a dead share) fails the source at that
  point: what was decoded plays, and the row ends *damaged* with the reason.

Whole-file reads go away. A short file is one or two pieces.

### Seeking in a decoder

The contract gains `seek(state, frame)`: the next `decode` returns that frame of the
file. It may `Wait`, more than once (a FLAC seek without a seek table searches). It may
fail with a reason, and then the decoder is where it was. FLAC seeks to the sample.

The test is the obvious one: decode from a seek, and compare with the same stretch of a
straight decode, for many targets in each file. The broken-file corpus gains seeks into
and past the damage.

### Two renders and the join

`hifi` keeps the lead as it is and adds a second one. A **render** is everything from
decoder to ring: a place in the programme, the decoders it has open, the chain's state,
a ring. One is heard. A recut starts the other.

```
           render A (heard)   ───────── ring A ──┐
sources ─►                                       ├─► the join ─► device
           render B (the recut) ─────── ring B ──┘
```

The join is in the callback: it takes from both rings and mixes them with a pair of
gains, for the length of a fade. This is the "at most the join" that B4 allows, and it
is slice 1's ramp grown a second input. Everything else about a recut (seeking, decoding,
the stages) happens on the render thread, which runs both renders and serves the new one
first.

There are two kinds of recut.

**The aligned recut**: the same music, rendered differently. A parameter changed, or a
row behind the playhead was edited. The new render starts at a stream frame a little
ahead of the callback and renders the same programme from there. When it has a short
lead ready, the callback drops whatever of it is already in the past and crossfades,
old into new, at whatever frame it has reached. The old render plays until then, so a
slow start is a later change and never silence. Both renders count in the same stream
frames, which is what the null test checks.

**The jump**: different music. A seek, a skip, previous, a row chosen. The old render
fades out at once, which is how the listener knows the control was taken. The new one
fades in when its short lead is ready. Between them is silence, and the transport says
*starting*. On a warm file the silence is shorter than a callback period or two.

### Ready

A recut that seeks a decoder and decodes its way back to the playhead cannot be heard in
20 ms, and a control that is being dragged asks for one after another. So the first
aligned recut takes as long as it needs, and leaves the path **ready**.

Ready, the path keeps the **source**: the decoder's own samples, before any stage, from
a run-in behind the frame heard to the front of the lead, counted in stream frames. The
decoder goes on feeding its front and never goes back. A recut is then the chain run
again over samples already in memory, from the next frame the callback will take. No
seek, no decode, no file. Rendering twenty milliseconds of it takes microseconds, so the
join waits for one callback and not for a short lead: the old render is still there if
anything is late.

- The first change pays for the seek and the decode, within the 100 ms. Every change
  after it, while ready, is within the 20 ms.
- Ready ends after five seconds with no change. The source is dropped and the path is
  slice 1's again.
- At its largest (192 kHz, stereo, a five-second lead) the source is 8 MB.
- A seek that lands inside the source (a few seconds forward) is served from it too.
  Any other jump is different music and gains nothing from it.
- An edit to a row in the lead is an aligned recut whose programme differs after the
  boundary: the source is good up to there, and the new row is decoded as usual.

A change that arrives while a recut is in flight does not queue behind it. The path
keeps one desired state (the programme and the parameters) and one recut in flight; a
newer state replaces what the next recut will render. Dragging a control produces a
recut per join, not per event.

Paused or held, nothing is sounding, so a recut joins with no fade and the device stays
held.

### The clock across a join

Stream frames keep counting through a join; they never go back. Each render posts marks
as slice 1's does. The callback records the frame at which it moved from one render to
the other, and from that frame the new render's marks are the ones that count. So after
a seek, the position is the target plus the frames heard since the join, by the same
subtraction as before.

A seek within a track is not a new `PlayerTrackStarted`. It is `PlayerSeeked`, emitted
when the target frame is heard.

### Stages

A **stage** is the decoder contract's sibling: a record of functions over memory it is
handed, that opens nothing, allocates nothing and calls nothing.

| | what |
|---|---|
| `open(format, memory)` | set up for a rate and channel count |
| `set(param, value)` | a parameter, by index; takes effect in the next render that starts |
| `process(frames, n)` | in place, in f64 |
| `flat()` | whether, with its parameters as they are, it passes audio unchanged |
| `run_in()` | how many frames of input it needs before its output has settled |
| `reset()` | forget its history |

The chain is fixed at start-up in this slice: gain, then the equalizer. Parameters
change only by a recut, so a stage never sees a parameter move under it and needs no
smoothing of its own; the join is the smoothing.

A render that starts mid-track runs `run_in` frames before its start through the chain
and throws their output away, so a filter is settled by the time it is heard. Ready,
those frames are in the source; otherwise they are decoded.

### Integers, floats and exact

- **Every stage flat: the path is slice 1's.** Integers from the decoder to the device,
  widened and nothing else. No float is made. The render checks at its start and takes
  this branch.
- **Any stage not flat**: samples become f64, pass through the chain, and come back to
  the device's integer format with TPDF dither. Into a float device, no dither.
- **The dither is a function of the frame**, not a running generator: the value for a
  sample comes from a seed, the source's frame number and the channel. Two renders of
  the same frames with the same parameters are then the same bits wherever they
  started, and the tests can compare a recut to a straight render exactly. (This is most
  of B34's "seeded dither", arriving early because the tests want it.)
- **The join** multiplies and adds in double and rounds, without dither, as slice 1's
  ramp does. Two renders that are the same samples join to those samples.

`exact` is true when the device is unshared, its format carries the source's bits, every
stage is flat and no join is in progress. `player.chain` has a row per stage with its
parameters and whether it is flat now, and `player.output` gains `joining`. The
indicator is still two states.

f64 and not f32: a 32-bit source fits, a biquad at 30 Hz and 192 kHz needs no care, and
at a thousand times real time the cost does not matter on a desktop.

### Volume and the equalizer

Both are stages in `hifi_eq`.

- **Gain**: decibels, 0 at most. At 0 dB it is flat. Mute is its own parameter, so
  unmuting returns to the level.
- **The equalizer**: ten bands, each a peak or a shelf with a frequency, a gain and a Q.
  All gains at zero is flat. Presets are data: a name and ten bands. Before the bands,
  an attenuation by the highest point of the combined response, computed when a
  parameter changes, so no setting can raise a full-scale signal past full scale.

`PlayerSetParam { stage, param, value }` is the one command for all of it; volume is a
parameter like any other.

### The queue becomes a list

Slice 1's queue is consumed: a row plays once and is done. With previous and seeking
back, that is the wrong model. From this slice `player.queue` is an ordered list and the
transport has a place in it.

- A row's state says what is true of it now: *idle*, *reading* (its first pieces),
  *ready*, *playing*, *failed*. "Done" is being before the place.
- `PlayerQueueEdit`: insert at, remove, move, clear. Rows have ids that survive a move.
  Removed rows leave the table, so it no longer fills.
- `PlayerPrevious`: within the first three seconds of a track, the row before; later,
  the start of this one. `PlayerJump { item }`: play this row now.
- An edit to a row that has not been rendered costs nothing. An edit to one whose audio
  is in the lead (the next track, in the last seconds of this one) is an aligned recut
  from the current track on. An edit to the row being heard is a jump.

### The player's API, as it grows

| name | kind | new in this slice |
|---|---|---|
| `PlayerSeek { item, frame }` | event in | to a frame of a row; item 0 is the one playing |
| `PlayerSeekBy { seconds }` | event in | from wherever the last seek was going, so repeats add up |
| `PlayerPrevious`, `PlayerJump { item }` | events in | |
| `PlayerQueueEdit { op, item, before }` | event in | insert is `PlayerEnqueue` with a `before` |
| `PlayerSetParam { stage, param, value }` | event in | |
| `PlayerSeeked { item, frame, seq }` | event out | when the target is heard |
| `PlayerParamChanged { stage, param, value, seq }` | event out | when the change is heard |
| `player.chain` | ktable | a row per stage: name, parameters, flat |
| `player.transport` | resource | gains the place in the list and the seek in flight |
| `player.output` | resource | gains `joining` |
| `player.queue` | ktable | rows are a list; gains bytes read and the file's size |

Between the player and `hifi`, still calls: a jump to a source and frame, a replacement
of the sources after a given one, a parameter, and wants coming back through the poll.

### The device's word

A device is opened at the widest integer word it grants, not at the track's depth, so a
16-bit track followed by a 24-bit one at the same rate joins in one stream. Widening is
exact, so nothing else changes.

## What the engine gets

Nothing blocks this slice. It uses what Korvid's slice 10 delivered: ranged reads (KR6),
stat through the queue (KR5, replacing the player's blocking `io_stat`) and
`os_memory.h`'s reserve and commit.

- **KR10, the timed wait**, is still wanted. Two renders and wants in flight give the
  render thread more reasons to look again shortly, and each is a nap today.

Anything else found goes in
[Korvid-Changes-For-Blackwing.md](Korvid-Changes-For-Blackwing.md) as it is found.

## Out of scope

- **Resampling.** The device is still opened at the source's rate. B11's resampler has
  no slice yet; this draft proposes slice 5, with the backends that need it.
- MP3, tags, the library (slice 3). Any face (slice 4). Device choice, hot-plug, ALSA
  `hw:`, Windows and macOS (slice 5).
- Shuffle and repeat. The queue and session surviving a restart (B28).
- A crossfade between tracks as a feature, and loudness normalisation.
- Stages added or reordered while running. The chain is two stages, in one order.
- The equalizer's matched designs near Nyquist, and its coefficients from the engine's
  near-exact transcendentals. Ordinary biquads first; both are on the module's page.
- Exact mode (B34) beyond the dither: pinned floating-point behaviour is P1.
- The hour-long run, still owed from slice 1. It is not this slice's, and this slice
  changes what it tests, so it is better run after.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| BR1 | The sparse span: a file as a reserved range, pieces, and a map of which are there | P0 |
| BR2 | `Wait` in the decoder contract, from `open`, `decode` and `seek`, changing nothing | P0 |
| BR3 | `seek` in the decoder contract, and in the FLAC decoder, to the sample | P0 |
| BR4 | The player reads in pieces through the I/O queue: the target first, then onward, then the rest | P0 |
| BR5 | The memory cap is on bytes committed; pieces from the frame heard forward are kept | P0 |
| BR6 | A piece that fails to read ends its source there, damaged, with a reason | P0 |
| BR7 | A second render and ring in `hifi`; the join in the callback, two gains over a fade | P0 |
| BR8 | The jump: the old render fades at once, the new one fades in when its short lead is ready | P0 |
| BR9 | The aligned recut: same stream frames, joined when ready, the old render heard until then | P0 |
| BR10 | One desired state and one recut in flight; a newer state replaces the next recut's | P0 |
| BR10a | Ready: the source kept from a run-in behind the frame heard to the front of the lead, recuts rendered from it, dropped after five idle seconds | P0 |
| BR11 | Marks and positions hold across a join, to the frame | P0 |
| BR12 | The stage contract, and a fixed chain of stages in a render, with run-in | P0 |
| BR13 | Every stage flat takes the integer path; otherwise f64 and TPDF dither keyed on the frame | P0 |
| BR14 | The gain stage, and volume and mute through `PlayerSetParam` | P0 |
| BR15 | The equalizer: ten bands, presets as data, attenuated by the response's peak | P0 |
| BR16 | `player.chain`, `joining`, and `exact` computed from them | P0 |
| BR17 | `PlayerSeek`, `PlayerSeekBy`, `PlayerPrevious`, `PlayerJump`, `PlayerSeeked` | P0 |
| BR18 | The queue as a list: insert, remove, move, clear; an edit inside the lead recuts | P0 |
| BR19 | Tests: the null recut; the seek to the frame; the gain join to the sample; steps at joins; late pieces; the drag | P0 |
| BR20 | Seeks in the broken-file corpus: into damage, past the end, in a file with no seek table | P0 |
| BR21 | Control to ear measured through PipeWire, and time to first sound on the three disks | P0 |
| BR22 | The device opened at its widest word | P1 |
| BR23 | Delay and padding trimmed in the render when a decoder reports them | P1 |
| BR24 | Stat through the I/O queue in place of the blocking one | P1 |
| BR25 | The player's events describe their fields, so `--trace-events` prints them | P1 |
| BR26 | Commands in `blackwing` for seek, previous, jump, the queue's edits and parameters | P1 |
| BR27 | Module pages brought up to date; numbers in `docs/design/slice-2-notes.md` | P1 |

## Milestones

Each leaves the tree building and its tests passing.

1. **Seeking, and the file in pieces** (BR1 to BR3, BR20). In `hifi_codec` alone: the
   sparse span, `Wait`, `seek`, FLAC on all three. Tested with pieces delivered in every
   awkward order. No change to what is heard.
2. **Reading in pieces** (BR4 to BR6, BR24). The player's reads become ranged, `hifi`
   reports wants, the cap moves to bytes committed. Still no control: slice 1's
   behaviour, starting sooner. The flash drive is measured here.
3. **The jump** (BR7, BR8, BR11, BR17). The second render, the join, seek, skip on the
   new mechanism, previous, jump to a row. `hifi_flush` stays for stop.
4. **Stages and the aligned recut** (BR9 to BR10a, BR12 to BR14, BR16). The stage
   contract, the float path and the dither, the gain stage, volume, and ready. The null
   test and the drag test land here, the null test both ready and not. This is the
   milestone the slice is named for.
5. **The equalizer** (BR15). Bands, presets, the attenuation, run-in.
6. **The queue** (BR18). The list, its edits, the recut at a boundary.
7. **Close** (BR19 to BR27, what is left of them). The numbers through PipeWire and from
   the disks, the pages, the master PRD corrected where this slice proved it wrong.

Volume comes before the equalizer on purpose: a gain has no history, so the first
aligned recut is checked to the sample, and the filter's run-in is added to a join
already known to be right.

## Decided

Nov 23, 2026, in review of the first draft.

- **The target for a join is 20 ms, not 100.** A volume or EQ change is heard that soon
  once the path is ready; the first one may take longer to be sure it works.
- **Ready**: after a recut the path keeps the source in memory back to the frame heard,
  and later changes are rendered from it. It ends after about five seconds without a
  change.
- **A jump fades the old audio at once.** A transition feels responsive even when the
  new audio is not there yet; silence and *starting* are the honest state until it is.
- **The draft's other choices stand**: the join runs in the callback, as two gains over
  two rings; volume goes through the recut like every other parameter; stages work in
  f64 and a flat chain makes no float; a decoder waits by returning, over a sparse span;
  the queue is a list with a place in it.
- **The draft's other open questions stand as written**: no dither in the join; run-in
  from nothing at a gapless boundary; `player.chain`; the resampler proposed for
  slice 5; `BR` for this slice's requirements.

## Open

- **The tick is most of 20 ms.** A command emitted in one tick is acted on in the next,
  and the loop ticks at the clock's rate while playing. Sixteen milliseconds can pass
  before `hifi` hears of a change, which the criterion above does not count and the
  listener does. Either the player takes parameter commands in the tick they are
  emitted, or the loop ticks faster while a control is in use. Milestone 4 measures it
  and picks.
- **The callback's period is the rest.** A join starts at the next callback: up to 11 ms
  away on PipeWire here, and up to 22 ms at a 96 kHz track's 1080-frame periods. The
  20 ms holds where the period allows it; asking PipeWire for a shorter one is an
  option with a cost in wakeups.
- **The numbers.** The short lead a first recut or a jump waits for (100 ms? less for a
  jump on a warm file), the aligned fade (10 ms, linear, since the two sides are the
  same music?), the jump's fades (10 ms out, 10 ms in?), the piece size (256 KB?), the
  cap on committed bytes (256 MB?).
- **Whether ready should end at all.** It costs 8 MB at the worst and a copy per frame
  decoded. Five idle seconds is the decision; if the copy does not show in the
  numbers, staying ready is simpler and makes the first change as fast as the rest.
- **What `Wait` costs a decoder that is not ours.** dr_flac reads by pulling; the
  wrapper turns a missing piece into a `Wait` and a return to the last frame. A codec
  that cannot be rewound that way needs its pieces there before it is called. P1's
  third-party codecs will meet this.
- **Whether the join's fade should be equal-power for a jump.** The two sides do not
  overlap, so it does not arise; it would if the fades were made to.

## As built

Numbers and findings are in [docs/design/slice-2-notes.md](../design/slice-2-notes.md).

**Milestone 1, seeking and the file in pieces** (Nov 23, 2026; BR1 to BR3, BR20). As
planned. `SparseSpan` keeps a byte per piece, not a bit. `decode` may return good frames
with its `Wait`; `seek` is a call that may itself wait. dr_flac is kept from reading a
missing piece by checking, before each FLAC frame, the bytes it can need; where that
cannot be known, a refused read and a seek back to the last frame returned. The nine
test files decode to their MD5 in four piece sizes and four orders of delivery, over
address space committed only as pieces arrive; 3600 seeks and 160 on a real album land
exact.

**Milestone 2, reading in pieces** (Nov 23, 2026; BR4 to BR6, BR24). As planned, in 256 KB
pieces under a 256 MB cap on bytes committed. `hifi` says what a decoder waits for with a
`Want` event, and ends a source whose bytes will not come with `hifi_starve`. A piece
`hifi` may be reading is given back behind a fence the render thread passes between
steps. Beyond the plan: `hifi` opens the next source as soon as the current one has
decoded, and holds the current one's last frames back while the next waits for its first
bytes, so a late head does not break a gapless join. A cap smaller than a decoder's
window (a few pieces) cannot be played under. On the USB 2 flash drive under slice 1's
load: the first sound at 0.6 s (slice 1: 24 s), and at a skip every 9 seconds all ten
tracks play, each heard 0.04 to 0.07 s after the skip, with no underrun.

**Milestone 3, the jump** (Nov 23, 2026; BR7, BR8, BR11, BR17). As planned: two slots,
each a ring; a jump fades the heard one out over 10 ms and joins the new one when it has
50 ms, fading it in; the new render's marks are placed in the stream when the callback
says where it joined. Seek, seek-by, previous, jump and next are jumps; stop is
`hifi_flush`, now a jump to nothing. Found on the way: a race that could leave the render
thread asleep with a source waiting for bytes, and two bugs in the player's place
(seek-bys in one tick; a done row coming back). The aligned recut, the other kind of
join, is milestone 4.

