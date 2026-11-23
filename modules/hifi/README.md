# hifi

The hard-realtime audio path and the interfaces around it: sources are decoded ahead of
time into the lead by a render thread, a device's realtime callback takes from the lead,
and the path says, in frames of the device's clock, what the listener has heard. It
knows nothing of tracks, queues, tags or players; `blackwing_player` decides what it is
handed.

## How it works

**Three threads.**

```
the caller's thread                render thread (hifi's)        device thread (the OS's)
  hifi_add_source, hifi_poll...      decodes, converts             hifi's callback
        |                            fills the lead                takes from the lead
        +--- commands -------------->|                             |
        |<-- notes ------------------+                             |
        |<-- read, silent_run, underruns --------------------------+
                                     +--- two rings (two renders) ->+  the join
```

The caller and the render thread pass commands and notes under one mutex that neither
holds while it works. The render thread and the callback share two rings and a few
atomic counters, and nothing else: the callback never waits.

**Renders.** A render is everything from decoder to ring: the sources in order, their
decoders, and a ring the render thread fills. There are two slots, each a ring and its
counters. One is heard: the callback takes from it. A flush or a jump starts a new
render, empty, in the other slot, and asks the callback to join it.

**The stream.** Frames are counted from zero when the `Hifi` is made and never reset:
they are the frames the device has taken. A slot counts its own frames from zero, and
its `base` is the stream frame of its frame 0, which the callback fixes when it joins
it. Everything that happens at a moment in the audio is a stream frame.

**The jump.** `hifi_jump` (a seek, a skip, a row chosen) replaces everything with new
sources. The callback fades what it is playing out at once (`fade_seconds`, 10 ms), then
switches to the new render as soon as it has `join_seconds` (50 ms) of lead, or all it
will have, and fades it in. Between them is silence, and the status names no source.
On a warm file the silence is shorter than a callback period. A jump that arrives
before the last one was joined takes its place: the render thread takes the request
back (a compare-exchange; the callback takes it up with one, so exactly one wins) and
starts again in the same slot. A mark the new render makes before it is joined (its
`Started` or `Seeked`) carries its slot's frame, and the caller places it once the
callback has said where that slot's frame 0 fell. A flush is the same with nothing after
it, ramped (`ramp_seconds`) rather than faded, and what is added afterwards starts clean
at full level.

**A source can start anywhere.** `HifiSource::start` is the frame it begins at (the
decoder seeks there, waiting for bytes like any other read), and `seek` says it was
being heard already: its first mark is `Seeked`, not `Started`. The status's `position`
is the frame being heard, counted from that start.

**The lead.** The render thread decodes 4096 frames at a time, widens them into the
device's sample format, and writes them until the ring is full: `lead_seconds` of audio
at the stream's rate. Then it sleeps a few milliseconds at a time, or until spoken to if
playback is paused.

**The hold.** With nothing to play (paused, stopped, or at the end of the queue), the
render thread holds the device (`hifi_device_hold`) once the device has played out what
it was given: everything up to the pause, or all of the lead, followed by the device's
delay's worth of silence. A held device stops calling back, so a paused player wakes no
thread of its own. `hifi_play` lets it go at once, on the caller's thread; something to
play lets it go on the render thread. Until it can hold, the render thread looks every
2 ms (Korvid has no timed wait on a condition), and every 50 ms after six seconds, for a
device that has stopped calling back on its own.

**The callback** copies frames from the ring to the device. That is all it does in normal
playback: the bytes are not touched. Otherwise:

- It **holds** (writes silence, takes nothing) until there is `start_seconds` of lead, or
  the whole of a stream that is shorter than that.
- It **ramps** over `ramp_seconds` to silence and back on pause, on a flush, and when the
  lead runs dry with more expected. The ramp is the only arithmetic on this thread.
- It counts an **underrun** when the lead runs dry and the stream was not at its end.

**The end of the stream is not an underrun.** Before the render thread lets the last
frames of a source be seen, it says whether the stream goes on past them (the slot's `end`).
To know, it opens the next source's decoder first, as soon as the current one has
decoded its last frame: if there is one and the open device can take its format, the
stream goes on and the join is seamless; if not, the stream ends there, and the
callback plays the last frame at full level.

**Bytes that are not there yet.** A source is a sparse span its caller fills (hifi does
no I/O). When a decoder returns `Wait`, the render thread says so once with a `Want`
event (the source and the range of its file), and tries again every 2 ms. What it has
decoded meanwhile plays: a source whose bytes come too slowly runs the lead dry, which
is an underrun like any other. A next source whose open is still waiting when the
current one's last frames are due holds those frames back while the lead lasts (down
to `start_seconds`), so the stream can go on into it without a gap; past that the
stream ends, and the next starts on its own. `hifi_starve` says a source's bytes will
not come: at its next wait it ends `Damaged` with the reason, or fails if it had not
opened.

**Giving pieces back.** The caller may take pieces out of a span while hifi plays it
(the memory cap). A decoder may have seen a piece's byte set an instant before it was
cleared. Every decoder call is made inside a render step, and a counter is odd while
one runs: `hifi_read_fence`, taken after clearing the byte, is passed (by
`hifi_read_passed`) once any step that was running has ended, and then nothing reads
the piece.

**Marks and the clock.** When a source's first frame is written at stream frame *n*, the
render thread notes a mark. The caller's `hifi_poll` works out the frame heard now:

```
heard = read - (the device's delay, less the silence written since the last real frame)
```

and gives out `Started` when that passes *n*, and `Ended` at the source's last frame the
same way. The second term is why the tail of the last track is heard: silence behind it
pushes it out to the ear.

**A change of format** between sources (a different rate or channel count, or more bits
than the device's word holds) ends the stream at the last frame, waits for the device to
play out its delay, closes it and opens it again. The stream's count carries on.

**A flush** drops every source and tells the callback to skip to the frame the render
thread had reached. The callback ramps down what is sounding, steps over the rest, and in
the same callback starts whatever was put in the lead behind it, at full level from its
first frame.

## Using it

**Play a file and follow it.**

```cpp
HifiConfig cfg;
cfg.device = "capture"; cfg.device_backend = capture;   // or the default device
Hifi* h = hifi_create(cfg);
hifi_add_source(h, {id, decoder_for(head), file});     // file: a SparseSpan; sparse_whole(bytes) for one in memory
for (;;) {
    HifiEvent ev[16];
    u32 n = hifi_poll(h, ev, 16);
    for (u32 i = 0; i < n; ++i) {
        if (ev[i].kind == HifiEventKind::Want)     ... read ev[i].need into the span
        if (ev[i].kind == HifiEventKind::Released) ... the span may go
        if (ev[i].kind == HifiEventKind::Ended)    ... ev[i].status, ev[i].reason
    }
    HifiStatus st; hifi_status(h, &st);   // st.source, st.position, st.lead_frames
}
hifi_destroy(h);
```

**Skip to something else, or seek.** `hifi_jump(h, sources, n)`, with the first source's
`start` set for a seek (and `seek` if it is the one being heard). Sources dropped get
`Released` and nothing more. `hifi_flush(h)` stops: the same with nothing after it.

## Quick reference

Header `korvid/hifi/hifi.h`:

| name | kind | what |
|---|---|---|
| `HifiConfig` | record | `device`, `device_backend`, `lead_seconds` (5), `start_seconds` (0.25), `ramp_seconds` (0.005), `fade_seconds` (0.010), `join_seconds` (0.050) |
| `HifiSource` | record | `id`, `decoder`, `file` (a `SparseSpan`), `start`, `seek` |
| `HifiEventKind` | enum | `Started`, `Seeked`, `Ended`, `Failed`, `Released`, `Underrun`, `Want` |
| `HifiEvent` | record | `kind`, `source`, `info`, `status`, `frames`, `need`, `reason` |
| `HifiStatus` | record | `device_open`, `format`, `shared`, `exact`, `paused`, `held`, `flushing`, `joining`, `written`, `taken`, `heard`, `lead_frames`, `underruns`, `source`, `position` |
| `hifi_create(config)`, `hifi_destroy(h)` | functions | the path and its render thread |
| `hifi_add_source(h, source)` | function | queue a source |
| `hifi_pause(h)`, `hifi_play(h)`, `hifi_flush(h)` | functions | hold, let go, drop everything |
| `hifi_jump(h, sources, n)` | function | everything replaced by these: the old fades out, the new fades in |
| `hifi_starve(h, id, reason)` | function | a source's bytes will not come |
| `hifi_read_fence(h)`, `hifi_read_passed(h, fence)` | functions | when a piece given back is no longer read |
| `hifi_poll(h, out, cap)`, `hifi_status(h, out)` | functions | events since last time; where things stand |

The module registers nothing with the engine yet: `blackwing_player` will hold a `Hifi`
and publish what it says.

## Behaviour worth knowing

- **A source's span is borrowed until `Released`**, which comes once for every time it
  was added, whatever happened to it, and usually long before it is heard: decoding runs
  ahead. A jump may add a source that an earlier render still has: each lend gets its
  own `Released`.
- **`lead_frames` is the lead that will be heard**: while a jump waits to be joined, the
  new render's, not what is left of the old. `written` is the heard slot's, in stream
  frames.
- **A want is said once per range.** The caller keeps it until the range is there; the
  same range is not asked for again.
- **A waiting decoder is looked at every 2 ms**, for want of a timed wait on a condition
  (KR10): a source whose bytes are slow costs wakeups while it waits.
- **`Started` and `Ended` are only as timely as `hifi_poll` is called.** `heard`,
  `source` and `position` in the status are as of the last poll.
- **Exact means untouched.** With no pause, flush or underrun, every frame that reaches
  the device is the decoder's sample, shifted up into the device's word. `exact` in the
  status says the device's format holds the source's bits and the device is not shared.
- **The first and last frames of a ramp are not exact.** A pause costs `ramp_seconds` of
  changed samples each side.
- **An underrun is a ramp, not a cut**, and playback does not restart until the lead is
  back to `start_seconds`.
- **A format change is a gap**: the device's delay, and the time it takes to open.
- **The next source's decoder is open while the current one finishes**, so two decoders'
  memory are held at a join.
- **No resampler and no dither.** A device that will not grant the source's rate, or
  whose word is narrower than the source, fails the source in words.
- **Sources of more than 24 bits are not exact into a float device.**
- **256 commands and 256 events in flight.** Past that a command is dropped with an
  error in the log; a player that polls each tick is nowhere near it.

## Tests

`kormake test hifi` plays hifi_codec's files into the capture device with its clock
driven by the test, so every check is to the frame: the eight good files hash to FLAC's
own MD5 at the device; two files join with nothing between them; events arrive at the
frame the device's delay says; a change of format leaves each side exact; pause, flush
and underrun ramp without a step and lose no frame; a pause and the end of the stream
hold the device once it has played out, and play or a new source lets it go; failures
fail alone. One test runs the capture device on the wall clock. Two sources whose 4 KB
pieces are handed over only when they are wanted play exact and gapless; a source
starved half way ends `Damaged` with the reason, exact up to there, and one starved
before it opened fails. A seek within a source fades out at once, lands on its frame
(exact after the fade in), says `Seeked` and counts its position from the target; a
jump to a source whose bytes are slow is silence until they come; a jump that takes the
place of one never joined; a jump while paused waits for play.

## Not in this module

Reading files, the queue and what plays next (`blackwing_player`). Devices
(`hifi_platform`). Decoders (`hifi_codec`). Seek, volume, the EQ and the recut (slice 2,
milestones 3 to 6).

## See also

- [hifi_platform](../hifi_platform/README.md): the device this plays into, and the
  capture device the tests use.
- [hifi_codec](../hifi_codec/README.md): the decoder contract sources are read through.
- [Slice 1](../../docs/PRDs/Blackwing-Slice-1-PRD.md): what this is being built for.
