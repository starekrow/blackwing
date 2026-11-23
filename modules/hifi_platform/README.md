# hifi_platform

The audio output device: opened at a format, pulling frames from a callback on a
realtime thread, and saying how far it is from the listener's ear. `hifi` calls it;
players and faces do not. It also holds the capture device, an output with no hardware
that keeps what it is given, which every test plays into.

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

## Platform notes

- **The capture device is the only device so far.** `"default"` has no backend until
  PipeWire arrives (slice 1, milestone 4); opening it fails in words.
- **The callback's thread is marked realtime** for as long as it is in the callback, so a
  dev build aborts if anything it calls reaches the allocator.
- **A driven clock can pull faster than any decoder decodes.** A test that pulls without
  waiting for the lead will see underruns that no hardware would cause.

## See also

- [hifi](../hifi/README.md): the only caller.
- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md), "The output": what the
  real backends must do.
