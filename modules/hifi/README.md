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
                                     +------- the lead (a ring) -->+
```

The caller and the render thread pass commands and notes under one mutex that neither
holds while it works. The render thread and the callback share a ring and a few atomic
counters, and nothing else: the callback never waits.

**The stream.** Frames are counted from zero when the `Hifi` is made and never reset.
`write` is how many the render thread has put in the lead, `read` how many the callback
has taken; the ring holds the frames between. Everything that happens at a moment in
the audio is a stream frame.

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
frames of a source be seen, it says whether the stream goes on past them (`end_frame`).
To know, it opens the next source's decoder first: if there is one and the open device
can take its format, the stream goes on and the join is seamless; if not, the stream
ends there, and the callback plays the last frame at full level.

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

**Play a file's bytes and follow it.**

```cpp
HifiConfig cfg;
cfg.device = "capture"; cfg.device_backend = capture;   // or the default device
Hifi* h = hifi_create(cfg);
hifi_add_source(h, {id, decoder_for(bytes), bytes});
for (;;) {
    HifiEvent ev[16];
    u32 n = hifi_poll(h, ev, 16);
    for (u32 i = 0; i < n; ++i) {
        if (ev[i].kind == HifiEventKind::Released) ... the bytes may go
        if (ev[i].kind == HifiEventKind::Ended)    ... ev[i].status, ev[i].reason
    }
    HifiStatus st; hifi_status(h, &st);   // st.source, st.position, st.lead_frames
}
hifi_destroy(h);
```

**Skip to something else.** `hifi_flush(h)`, then `hifi_add_source`. Sources that were
flushed get `Released` and nothing more.

## Quick reference

Header `korvid/hifi/hifi.h`:

| name | kind | what |
|---|---|---|
| `HifiConfig` | record | `device`, `device_backend`, `lead_seconds` (5), `start_seconds` (0.25), `ramp_seconds` (0.005) |
| `HifiSource` | record | `id`, `decoder`, `bytes` |
| `HifiEventKind` | enum | `Started`, `Ended`, `Failed`, `Released`, `Underrun` |
| `HifiEvent` | record | `kind`, `source`, `info`, `status`, `frames`, `reason` |
| `HifiStatus` | record | `device_open`, `format`, `shared`, `exact`, `paused`, `held`, `flushing`, `written`, `taken`, `heard`, `lead_frames`, `underruns`, `source`, `position` |
| `hifi_create(config)`, `hifi_destroy(h)` | functions | the path and its render thread |
| `hifi_add_source(h, source)` | function | queue a source |
| `hifi_pause(h)`, `hifi_play(h)`, `hifi_flush(h)` | functions | hold, let go, drop everything |
| `hifi_poll(h, out, cap)`, `hifi_status(h, out)` | functions | events since last time; where things stand |

The module registers nothing with the engine yet: `blackwing_player` will hold a `Hifi`
and publish what it says.

## Behaviour worth knowing

- **A source's bytes are borrowed until `Released`**, which comes once for every source
  whatever happened to it, and usually long before it is heard: decoding runs ahead.
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
fail alone. One test runs
the capture device on the wall clock.

## Not in this module

Reading files, the queue and what plays next (`blackwing_player`). Devices
(`hifi_platform`). Decoders (`hifi_codec`). Seek, volume, the EQ and the recut (slice 2).

## See also

- [hifi_platform](../hifi_platform/README.md): the device this plays into, and the
  capture device the tests use.
- [hifi_codec](../hifi_codec/README.md): the decoder contract sources are read through.
- [Slice 1](../../docs/PRDs/Blackwing-Slice-1-PRD.md): what this is being built for.
