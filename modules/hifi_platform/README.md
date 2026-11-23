# hifi_platform

The audio output device: opened at a format, pulling frames from a callback on a
realtime thread, and saying how far it is from the listener's ear. `hifi` calls it;
players and faces do not. It also holds the capture device, an output with no hardware
that keeps what it is given, which every test plays into. On Linux the system's device
is PipeWire's default sink.

## What it provides

**The device interface** (`device.h`): `hifi_device_open` takes a name, a wanted format
and a callback, and answers with the format granted and whether the device is shared.
`hifi_device_delay` is the frames between the callback and the ear. `hifi_device_close`
returns only when no callback is running. Sample formats are `S16`, `S24` (in an `i32`),
`S32` and `F32`.

**The capture device** (`capture.h`): the device named `"capture"`, playing into a
`HifiCapture`. It grants any format, unshared, with the delay it was told to report.
Its clock is either driven (`hifi_capture_pull` runs the callback on the calling thread
for exactly the frames asked) or the wall's (a thread pulls a period at a time). What it
was given is kept in segments, one per device opened on it, so a stream that changes
format can be read back in order.

**The PipeWire backend** (`src/linux/pipewire.cc`): the device named `"default"` or
`"pipewire"`, a playback stream on the default sink. It asks for the source's rate,
channels and sample format and nothing else, and is granted that, shared: PipeWire's
graph mixes it with other streams and resamples it to the graph's rate when the two
differ. The stream's `process` runs on PipeWire's data thread, which is the callback's
thread. The delay is what PipeWire reports for the stream (queued, in its resampler, and
the graph's delay to the hardware) just after each buffer is queued, less the time since.
The connection to PipeWire is made at the first open and kept for the process; each
device is a stream on it (see Platform notes).

Header `korvid/hifi_platform/device.h`:

| name | kind | what |
|---|---|---|
| `HifiSampleFormat` | enum | `S16`, `S24`, `S32`, `F32` |
| `HifiDeviceFormat` | record | `rate`, `channels`, `sample` |
| `HifiDeviceDesc` | record | `name`, `want`, `callback`, `user`, `backend` |
| `HifiDeviceOpened` | record | `device`, `format`, `shared`, `reason` |
| `hifi_device_open(desc)`, `hifi_device_start(d)`, `hifi_device_delay(d)`, `hifi_device_close(d)` | functions | a device's life |
| `hifi_sample_bytes(f)`, `hifi_sample_bits(f)`, `hifi_frame_bytes(f)`, `hifi_sample_format_name(f)` | functions | sizes and names |

Header `korvid/hifi_platform/capture.h`:

| name | kind | what |
|---|---|---|
| `HifiCaptureDesc` | record | `keep_bytes`, `delay_frames`, `period_frames`, `wall_clock` |
| `HifiCaptureSegment` | record | `format`, `offset`, `bytes` |
| `hifi_capture_create(desc)`, `hifi_capture_destroy(c)` | functions | the place captured frames go |
| `hifi_capture_pull(c, frames)` | function | driven clock: take this many frames now |
| `hifi_capture_segments(c, out, cap)`, `hifi_capture_data(c)`, `hifi_capture_dropped(c)`, `hifi_capture_opens(c)` | functions | what was captured |

## Tests

`kormake test hifi_platform` checks the device names and, where PipeWire is running, the
backend: each sample format is granted as asked and shared; half a second of silence (so
no sound) is taken at the wall's pace, on a thread marked realtime, with a delay under a
second; no callback runs after close. Without PipeWire those checks are skipped with a
line in the log. The capture device is tested through `hifi`.

## Building

The PipeWire backend needs PipeWire's development package (`libpipewire-0.3-dev` on
Debian and Ubuntu). The module defines the external itself (`externals` in its
`module.lua`), with `pkg_config`, so `kormake check` names the package and the line that
installs it when it is missing.

## Platform notes

- **Linux: PipeWire only.** There is no ALSA `hw:` backend, so no real device is
  unshared and the bit-perfect state cannot read *identical* on Linux hardware yet.
- **One PipeWire connection for the process**: a thread loop, a context and a connection,
  made at the first open and never closed. Closing them at each device close leaked
  about 3.7 KB a time: the context's module-rt loads libdbus for rtkit, and each unload
  strands libdbus's static state. A connection the daemon drops is made again at the
  next open.
- **PipeWire puts its data thread in the realtime class itself**, through rtkit where
  `RLIMIT_RTPRIO` is 0 (as on a stock Ubuntu desktop: `SCHED_RR`, priority 20). The grant
  arrives a few callbacks into the first stream, whose first callbacks run at the normal
  priority; the thread is kept, so later streams start in the class.
  The backend logs, at close and at INFO, the thread's policy, the callback count, the
  largest period, the slowest callback and the largest delay.
- **The graph's rate wins.** At a rate the graph does not allow, PipeWire resamples:
  44.1 kHz into a 48 kHz graph arrives as periods of 498 frames rather than 512.
- **Opening waits up to five seconds** for PipeWire to take the format, and fails in words
  if it does not.
- **The callback's thread is marked realtime** for as long as it is in the callback, so a
  dev build aborts if anything it calls reaches the allocator.
- **A driven clock can pull faster than any decoder decodes.** A test that pulls without
  waiting for the lead will see underruns that no hardware would cause.

## See also

- [hifi](../hifi/README.md): the only caller.
- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md), "The output": what the
  real backends must do.
