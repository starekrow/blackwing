# hifi_platform

The audio output device: opened at a format, pulling frames from a callback on a
realtime thread, and saying how far it is from the listener's ear. `hifi` calls it;
players and faces do not. It also holds the capture device, an output with no hardware
that keeps what it is given, which every test plays into. On Linux the system's device
is PipeWire's default sink.

## What it provides

**The device interface** (`device.h`): `hifi_device_open` takes a name, a wanted format
and a callback, and answers with the format granted and whether the device is shared.
`hifi_device_delay` is the frames between the callback and the ear. `hifi_device_hold`
lets a device with nothing to play stop calling back, and go on again. `hifi_device_close`
returns only when no callback is running. Sample formats are `S16`, `S24` (in an `i32`),
`S32` and `F32`.

**The devices there are** (`device.h`): `hifi_devices` lists every output the backends
have (PipeWire's sinks, the capture device), each with the name `hifi_device_open` takes,
what it is called, whether it is the default and whether it is there; a device that goes
stays listed, not present. `hifi_devices_generation` moves on at every change, and
`hifi_devices_watch` names one function called (on a backend's thread) after each.
`hifi_device_lost` says when an open device has gone; it then never calls back again,
and its delay is the one it had. `HifiDeviceOpened::name` is the device opened: `default`
resolved to the one it is.

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
| `hifi_device_open(desc)`, `hifi_device_start(d)`, `hifi_device_delay(d)`, `hifi_device_hold(d, hold)`, `hifi_device_close(d)` | functions | a device's life |
| `hifi_device_lost(d)` | function | null while it plays; why, once it has gone |
| `HifiDeviceInfo`, `kHifiMaxDevices`, `kHifiDeviceNameMax` | record, constants | a device as listed: `name`, `label`, `backend`, `is_default`, `present`, `shared`, `can_exclusive`, `channels` |
| `hifi_devices(out, cap)`, `hifi_devices_generation()`, `hifi_devices_watch(fn, user)` | functions | the list; its change count; who is told of a change |
| `hifi_sample_bytes(f)`, `hifi_sample_bits(f)`, `hifi_frame_bytes(f)`, `hifi_sample_format_name(f)` | functions | sizes and names |

Header `korvid/hifi_platform/capture.h`:

| name | kind | what |
|---|---|---|
| `HifiCaptureDesc` | record | `keep_bytes`, `delay_frames`, `period_frames`, `wall_clock`; `only_rate`, `only_sample` (a device that takes only these), `can_alone` (asked alone, it says it is) |
| `HifiCaptureSegment` | record | `format`, `offset`, `bytes` |
| `hifi_capture_create(desc)`, `hifi_capture_destroy(c)` | functions | the place captured frames go |
| `hifi_capture_pull(c, frames)` | function | driven clock: take this many frames now |
| `hifi_capture_segments(c, out, cap)`, `hifi_capture_data(c)`, `hifi_capture_dropped(c)`, `hifi_capture_opens(c)` | functions | what was captured |
| `hifi_capture_held(c)`, `hifi_capture_holds(c)` | functions | whether the device is held, and how often it has been; a held capture device keeps its clock |
| `hifi_capture_unplug(c)`, `hifi_capture_plug(c)` | functions | pulled out (a device on it Lost, none openable, not present) and plugged back |

**Alone on Linux** (`src/linux/alsa.cc`, `src/linux/reserve.cc`; slice 5, milestone 7).
`HifiDeviceDesc::alone` opens a PipeWire sink's card itself: the kernel's PCM device
(`/dev/snd/pcmC<card>D<device>p`), spoken to through its own ioctls with no library under
it (alsa-lib is LGPL). The card is asked of WirePlumber first through its reservation
(`org.freedesktop.ReserveDevice1.Audio<card>` on the session bus, through `dbus`), and
given back on close. The format is chosen for the file (BR22): its rate if the device
has it, else the nearest of its family, else any (hifi resamples); the narrowest word
that holds its depth, else the widest the device has (padded), else a narrower one (hifi
dithers); packed 24-bit (`S24_3LE`, common on USB DACs) is packed from `S24` on the way.
About 10 ms periods and up to 200 ms buffered: hifi keeps seconds of its own. A thread of
its own writes a period when the kernel has room; an underrun is prepared and played on.
The card unplugged is lost; another program asking for it at a higher priority than
Blackwing's (WirePlumber's, 0) is told yes, and the device is lost, which the player
takes as a pull. WirePlumber asking it back at its own priority is told no. Refused
(another program has the card, no reservation), the sink is opened shared and
`HifiDeviceOpened::alone_why` says why.

**Names that last.** WirePlumber makes a card's sink again when it has the card back, and
names the new node with a `.<n>` after the old name (`...analog-stereo.3`). A sink is
listed, chosen and kept by its name without that suffix; the node's name now is what a
stream targets. Two identical cards with no serial number between them share a name.

## Tests

`kormake test hifi_platform` checks the capture device pulled and plugged; and, where
PipeWire is running, lists the sinks, makes a null sink of its own (it plays nowhere),
opens it by name, destroys it under the stream and checks the stream is lost, not moved,
and its row gone. It also checks the device names and, where PipeWire is running, the
backend: each sample format is granted as asked and shared; half a second of silence (so
no sound) is taken at the wall's pace, on a thread marked realtime, with a delay under a
second; held, it makes no callback, and let go it calls back within 100 ms (6 ms on the
Linux laptop); no callback runs after close. Without PipeWire those checks are skipped with a
line in the log. The capture device is tested through `hifi`.

**Alone, on a real card**, only when asked, since it takes the card from PipeWire for a few
seconds: `BLACKWING_ALONE_SINK=<a sink's name> hifi_platform_tests`. Silence: the card's
reservation changes hands and comes back; 16-bit 44.1 kHz is granted the device's
nearest; frames go at the device's rate; a hold stops them; another program asking for
the card is a loss within milliseconds; 24-bit opened again at once, while WirePlumber is
still making the sink again, is the word that holds it (packed where the device packs).

## Building

The PipeWire backend needs PipeWire's development package (`libpipewire-0.3-dev` on
Debian and Ubuntu). The module defines the external itself (`externals` in its
`module.lua`), with `pkg_config`, so `kormake check` names the package and the line that
installs it when it is missing.

## Platform notes

- **The sinks come from PipeWire's registry**, and the default from its `default`
  metadata (`default.audio.sink`). The registry is made at the first list or open and
  waited for (a second at most) so the first list is whole. What the registry's own
  properties do not carry is not known yet: `channels` is 0 and `can_exclusive` false
  for every sink (the node's own properties have them; milestone 7 reads them).
- **A stream is linked to the sink it names and never moved** (`target.object`,
  `node.dont-reconnect`). Without that, WirePlumber moves a stream whose sink went to
  another sink, which is the speakers when a headphone DAC is pulled. With it, the sink
  going (its node removed from the registry) marks the device lost within milliseconds
  (2.1 ms in the test), and no callback comes after.
- **`default` is the default as it is when opened.** A default that changes later moves
  nothing here; the player opens the new one.

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
- **Held, the PipeWire stream is inactive** (`pw_stream_set_active`): the graph stops
  calling it and its data thread sleeps. Let go, the first callback comes within a
  period.
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
