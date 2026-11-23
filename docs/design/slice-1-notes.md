# Slice 1 notes

What building [slice 1](../PRDs/Blackwing-Slice-1-PRD.md) found, milestone by milestone,
with the numbers. Written as it goes.

## Milestone 1, the decoder

Nov 22, 2026. Linux laptop, `linux-dev`.

**As planned**, with these particulars:

- The contract has `probe`, `open`, `decode`, `close` and a `memory_bytes` each decoder
  declares, so the caller knows how big a block to hand over. `open` takes the whole
  file's bytes.
- `decode` returns `i32` samples right-justified at the file's depth. dr_flac's own
  32-bit output is left-justified; the wrapper shifts it down.
- dr_flac is pinned at a commit of its `master` (0.13.5, still in development there),
  since the project publishes no tagged releases.

**Exactness.** Eight libFLAC-encoded files (16 and 24 bits, mono and stereo, 44.1, 48, 96
and 192 kHz), each decoded a frame at a time, 1009 frames at a time and all at once:
every decode hashes to the MD5 in the file's header. The generator checked each header's
MD5 against the PCM it encoded, so the oracle does not rest on a FLAC decoder.

A real album (ten tracks, 16-bit stereo 44.1 kHz, 39 minutes, 302 MB) decodes exact on
every track through the heavy test.

**Speed.** That album decodes at about 1150 times real time on one core (2344 s of audio
in 2.0 s), the slowest track at 1140 times. Decoding is not where the lead will be lost.

**Broken files.** Truncated, not FLAC, empty, magic only, header only, a length that
lies, a format that lies, a spoiled frame: all behave as the contract says. Six hundred
more with random bit flips and cuts: 571 open, 564 of those end `Damaged`, none
misbehaves. Seven open and end clean; why was not looked into (a flip in padding or a
seek point would do it).

**Sanitizers.** The tests were built once by hand with AddressSanitizer and
UndefinedBehaviorSanitizer. No memory error. One finding: dr_flac's predictor sum
overflows a signed int on a spoiled frame (`dr_flac.h`, the LPC decode). The frame fails
its checksum and is dropped, so nothing wrong is played, but the overflow is undefined
behaviour. `src/flac.cc` compiles dr_flac with wrapping arithmetic under GCC, and the
run is clean with it. Clang and MSVC have no such switch per file; that is to look at
when those toolchains build this.

**Not done here, and noted:**

- A sanitizer build is not something `kormake` offers a project, so the run above is not
  repeatable from one command. Worth asking the engine for.
- A dropped frame shifts later audio earlier without saying where. Fine for a damaged
  file today; gapless and the clock may want to know the hole's position later.
- The decoder's 4 MB block is reserved whole for each open decoder. Two are open at
  once at most in slice 1.

## Milestone 2, the path into the capture device

Nov 22, 2026. Linux laptop, `linux-dev` and `linux-debug`.

**Built**: `hifi_platform`'s device interface and the capture device; `hifi`'s render
thread, lead, marks, clock and callback. 218 checks, all on the capture device with its
clock driven by the test, so each is to the frame.

**Where the build departed from the plan:**

- **The caller and the render thread share a mutex, not lock-free queues.** The PRD said
  every arrow was lock-free. The one that must be is the one into the realtime thread,
  and it is: a ring and atomic counters. Commands and notes between two ordinary threads
  go under a mutex that neither holds while working, and the render thread sleeps on a
  condition variable when idle, which is what makes a paused player cost nothing.
- **The next source's decoder is opened before the current one's last frames go out.**
  Found by the tests. The callback must know whether the stream goes on past the frames
  it can see: if it does and they run out, that is an underrun and it ramps; if it does
  not, the last frame plays at full level. The first build said "it goes on" whenever
  anything was queued, and so faded the tail of a track that was followed by a change of
  format. Whether the stream goes on depends on the next source's format, so the next
  source has to be open to say.
- **A flush restarts in the same callback.** The first build ramped down and left the
  rest of that callback silent, a gap of most of a period before the next track. The
  callback now goes round again and starts what is behind the flush at once.
- **The ramp multiplies in double and copies at full gain.** A 32-bit sample does not
  fit a float; the first build's ramp would have rounded every S32 sample it touched.
- **`Released` is its own event.** Decoding runs ahead, so a source's bytes are free long
  before it is heard, and the player should not have to hold them until `Ended`.
- **`flushing` in the status**, so a caller (a test, mostly) can tell when a flush has
  taken effect.

**Checked beyond the tests.** Forty runs in a row with no failure. ThreadSanitizer and
AddressSanitizer with UndefinedBehaviorSanitizer, built by hand as in milestone 1: clean.
The callback runs with its thread marked realtime in every test, and never trips the
allocator check.

**Found about testing, worth keeping:** a driven clock takes frames as fast as the test
asks, which is far faster than a decoder makes them. Two tests failed until they paced
their pulls to the lead. That is the test's fault and not the path's, but it means an
underrun count from an unpaced driven run means nothing.

**Not done here, and noted:**

- No numbers yet for the callback's worst time; that wants the real device (milestone 4).
- The stream ends, and the device stays open, playing silence. Closing an idle device
  after a while is the player's or a later slice's call.
- Opening a device at a wider word up front (so a 16-bit track followed by a 24-bit one
  at the same rate needs no reopen) is not done: each change of depth upward is a gap.

## Milestone 3, the player

Nov 22, 2026. Linux laptop, `linux-dev` and `linux-debug`.

**Built**: `blackwing_player` as an engine module named `player` (the queue as a ktable,
two resources, ten event types, one system, six commands) and the `blackwing`
executable with its own loop. 86 checks in a real engine on the driven capture device;
`hifi`'s tests grew to 226.

**Where the build departed from the plan:**

- **The headless loop is `blackwing`'s own.** The PRD left it between teaching `tool` to
  pace and carrying a small loop. The loop is forty lines: tick at the clock's rate
  while playing, four times a second otherwise. Nothing was asked of the engine.
- **`player.output`, not `hifi.output`.** `hifi` is a library, not an engine module, so
  it cannot publish a resource. The player publishes what `hifi` reports.
- **Notifications carry `seq`.** Found by the tests. Korvid's events are a stream per
  type, and handlers of different types run in the schedule's order. So an Ended and a
  Started from one tick arrive Started first if that handler is first. `seq` is the
  order things happened in. The firmer advice is in the module page: read
  `player.transport` for what is playing.
- **`PlayerQueueEnded`** is new: the executable needed to know when to exit, and a face
  will want it too.
- **The reader's stall is tested at `hifi`, not at the player.** The exit criterion
  imagined a slow reader under a playing track. Files are read whole before they are
  lent, so a slow read is a late start or a gap between tracks, never an underrun in a
  track. The stalling-decoder test in `hifi` is the test of the lead.

**Bugs the tests found:**

- **An item played twice.** `hifi` releases a source's bytes as soon as it is decoded,
  seconds before it is heard. The player took Released to mean "no longer in hifi" and
  lent the item again. An item now has two flags, lent and in play, which end at
  different times.
- **`cli_run` needs a sink.** Passing none crashes in the engine's CLI. The executable
  passes a quiet one; worth a null check on the engine's side.

**Checked beyond the tests.** A hundred runs of the player's tests in a row with no
failure (one race in a test was found on the way and fixed). ThreadSanitizer and
AddressSanitizer with UndefinedBehaviorSanitizer over the player's tests: clean. The
executable plays a 24-bit 96 kHz file into the wall-clock capture device and the file
written by `--capture-to` hashes to FLAC's MD5 after 512 frames of leading silence (one
period, before the lead was ready). A real track interrupted after six seconds exits
cleanly with no leak reported.

**Numbers.** Playing a real 16-bit 44.1 kHz track into the capture device for six
seconds: 0.08 s of CPU, 33 MB resident (the 23 MB file, the lead, a decoder's block).

**Things the engine might want to know:**

- The I/O queue's paths are 255 bytes. Music libraries have longer ones.
- A project cannot get a sanitizer build from `kormake` (as in milestone 1).
- `cli_run` with a null sink crashes.

**Not done here, and noted:**

- The player's `stat` before a read blocks the tick; it wants to be a task.
- The queue is never trimmed; 4096 items and it is full.
- stdin as a command line is POSIX-only and was not exercised by a test.

## Milestone 4, sound

Nov 22, 2026. Linux laptop, `linux-dev`, PipeWire 1.6.2 with WirePlumber 0.5.13, the
graph at 48 kHz.

**Built**: the PipeWire backend in `hifi_platform` (`src/linux/pipewire.cc`), the device
named `default` on Linux. A playback stream on the default sink, `RT_PROCESS`, on a
`pw_thread_loop` of its own, asking for exactly the source's rate, channels and samples.
22 checks in a new `hifi_platform` test, which plays silence through it.

**From the engine**: KR1 was all it took. The module defines the `pipewire` external
itself, with `pkg_config`, and nothing in Korvid's tree changed. Without the development
package, `kormake check` says which module needs it and prints the `apt` line. KR7 was
not needed: PipeWire's data thread is the callback's, and PipeWire puts it in the
realtime class on its own.

**Where the build departed from the plan:**

- **One PipeWire connection for the process, not one per device.** Found by
  LeakSanitizer: the first build made a context per device and lost about 3.7 KB at each
  close, growing with every open (18 KB over 5 opens, 77 KB over 21). Keeping PipeWire's
  modules loaded made it vanish, and so did a client config without module-rt: the
  context's module-rt loads libdbus to ask rtkit for the realtime class, and each unload
  strands libdbus's static state. The player opens a device at every change of format,
  so this would have grown for as long as it ran. Now the thread loop, the context and
  the connection are made at the first open and kept; a device is a stream on them.
- **The callback's thread is PipeWire's, already realtime, but not at once.** On this
  machine `RLIMIT_RTPRIO` is 0, so PipeWire asks rtkit, and the thread becomes `SCHED_RR`
  at priority 20 a few callbacks into the first stream; those first callbacks run at the
  normal priority. Since the context is kept, so is its data thread, and every later
  stream starts in the class. The backend looks at the policy at the 1st, 64th and
  1024th callback.
- **The delay is a snapshot that ages.** PipeWire's `pw_time` gives the frames queued in
  the stream, those in its resampler and the graph's delay to the hardware. The backend
  takes their sum just after each buffer is queued, with the time, and
  `hifi_device_delay` subtracts the frames played since. Without the ageing, the position
  would step once a period.
- **The graph's rate wins.** WirePlumber's default graph here allows 48 kHz only, so a
  44.1 kHz stream is resampled (periods of 498 frames, not 512) and a 96 kHz one gets
  periods of up to 1080. The source's rate is asked for with `node.rate`, which is a
  request and not a grant.

**Checked beyond the tests.** ThreadSanitizer, and AddressSanitizer with
UndefinedBehaviorSanitizer, over `hifi_platform`, `hifi` and `blackwing_player`, now one
`kormake --sanitize` command each (Korvid's KR2). Clean, after the fix above.

**Heard.** Two test tones of different formats play in order with a gap between them,
as a reopen should. The first two tracks of a real album (16-bit, 44.1 kHz) play; Ctrl-C
after 27 seconds exits 130 with no leak reported. `player status` at 5 and 10 seconds
says 4.7 and 9.7: the position follows the device's clock, 0.3 s behind the wall because
of the start. A pause holds the position at 2.7 s for five seconds, and play goes on
from there. With PipeWire unreachable (`PIPEWIRE_REMOTE` pointed nowhere), each file
fails with "PipeWire is not running, or would not connect the playback stream".

**Numbers**, from the real album, 27 seconds through PipeWire:

| | |
|---|---|
| callbacks | 2439, of up to 498 frames (11.3 ms) |
| the slowest callback | 23.7 µs, 0.2% of its period |
| the delay to the ear | up to 16.6 ms; 14 ms typical |
| CPU | 0.37 s (0.29 user, 0.08 sys): 1.4% of one core |
| resident | 76 MB at most: both 31 MB tracks read, the lead, two decoders |
| the thread | `SCHED_RR`, priority 20, through rtkit |

**Not done here, and noted:**

- **Nothing proves the samples reach the speaker unchanged**, and on PipeWire nothing can:
  the graph resamples and mixes. `player status` says `shared`. ALSA `hw:` is the
  exclusive route (the PRD's open question, still open).
- **A stalled data thread is not detected.** If PipeWire stops calling back (a suspended
  sink, a lost connection), the position stops and the player waits. The backend logs
  a stream that stops, at WARN, and does nothing else.
- **The device stays open while paused**, playing silence a period at a time. The cost
  of that is milestone 5's idle check.

## Milestone 5, close

Nov 23, 2026. Linux laptop, `linux-dev`, PipeWire 1.6.2 at 48 kHz. The music is a real
album (ten tracks, 16-bit stereo 44.1 kHz, 31 MB a track on average).

**Built**: the device hold (`hifi_device_hold`, held by `hifi` when there is nothing to
play), `--trace-events` on the executable, and `scripts/stress-play.sh`, the disk check.

### The disks

`scripts/stress-play.sh` plays the album through PipeWire for 90 seconds while three
readers and a writer loop over a load file on the same disk, after evicting the album
from the page cache. Each disk ran twice: straight through (one track read under the
load, at the start), and skipping to the next track every 9 seconds (all ten read under
the load).

| disk | link | filesystem | the load | disk busy | straight | skipping |
|---|---|---|---|---|---|---|
| WD_BLACK SN850X, the built-in NVMe | PCIe | ext4 | O_DIRECT, 2.6 GB/s read, 1.8 GB/s written | 77% | 0 underruns | 0 underruns, 10 read |
| Seagate Backup Plus Ultra Touch, 2 TB disk | USB 3, 5 Gb/s | exFAT | buffered, dropping the cache: 90 MB/s read, 13 MB/s written | 96% | 0 underruns | 0 underruns, 10 read |
| SimpleTech SimpleDrive (Hitachi HTS545032B9, 320 GB disk) | USB 2, 480 Mb/s, behind two hubs | NTFS | | | not run | not run |

The USB 2 drive dropped off the bus two minutes into writing the 2 GB load file, before
the player started: the kernel logged "Cannot enable. Maybe the USB cable is bad?", and
the drive did not enumerate again. It is a bus-powered laptop disk behind a hub chain,
and a sustained write is the likeliest thing to brown it out. Its run is owed.

On the USB 3 disk under the load, the first track was heard 1.5 seconds late (88.5 of 90
seconds played): reading 31 MB from a disk 96% busy. A slow read is a late start, never
an underrun, because a track is read whole before it is lent (milestone 3).

The built-in disk also ran before the hold was built (Nov 22, 23:59), with the same
result: 0 underruns both ways at 78% busy.

### Idle

Measured from `/proc` over 30 seconds, after five seconds of play:

| | CPU | wakeups | who wakes |
|---|---|---|---|
| playing | 1.8% of a core | 350 a second | the main loop and the render thread 255, PipeWire's data thread 95 |
| paused, before the hold | 0.83% of a core | 99 a second | PipeWire's data thread 95, writing silence; the main loop 4 |
| paused, with the hold | 0.03% of a core | 4 a second | the main loop, four times a second |

**Where the build departed from the plan: the device is held when there is nothing to
play.** Found by the measurement above. Paused, the player woke nobody, but the device
went on calling back a period at a time, and the callback wrote silence. Now `hifi`'s
render thread holds the device once it has played out what it was given (up to the
pause, or all of the lead) and its delay's worth of silence after. PipeWire's stream goes
inactive, and its data thread sleeps. `hifi_play` lets it go on the caller's thread, and
the first callback comes 6.4 ms later on PipeWire: within one 11 ms period, so pause and
resume still take effect within a period. Something new to play lets it go too.

Until the device can be held, the render thread looks every 2 ms, and wakes for a
command. Korvid has no timed wait on a condition, so that is naps (now KR10 on the
engine's list). It lasts only as long as the device plays out: a few milliseconds after
a pause, up to the lead's length at the end of a queue. Past six seconds, which only a
device that stopped calling back on its own would reach, the naps grow to 50 ms.

The first build of the hold had a bug the tests found: after a play let the device go,
the render thread went back to waiting for a command (it had nothing to decode) and
never looked to hold again. The wait now also ends when the device is no longer held.

### The numbers the exit criteria asked for

| | |
|---|---|
| decode speed | 1165 times real time over the album, the slowest track 1144 times (one core) |
| the callback's worst time | 23.7 µs of an 11.3 ms period, through PipeWire (milestone 4) |
| memory held | 76 MB playing (the track, the next one read ahead, the lead, two decoders); 104 MB at most while skipping |
| CPU | 1.8% of a core playing, 0.03% paused |
| delay to the ear | up to 16.6 ms through PipeWire |

### The exit criteria

| criterion | where it stands |
|---|---|
| `blackwing play a.flac b.flac` through PipeWire, in order, exits; Ctrl-C clean | held (milestone 4) |
| capture MD5 is FLAC's own, 16 and 24 bits, mono and stereo, 44.1 to 192 kHz | held (`hifi`'s and the player's tests) |
| two files of one format arrive as one stream | held (the same) |
| 90 s, no underrun, disk hammered: built-in, two USB drives | built-in and USB 3 held; USB 2 owed (the drive dropped out) |
| a reader stalled inside the lead: no underrun; past it: one, reported, resumes | held, at a tenth of a second's scale (`hifi`'s stalling-decoder test) |
| `PlayerTrackStarted` at the frame heard; the position follows the device's clock | held on the capture device to the frame; through PipeWire, 5.0 s of position in 5 s |
| pause and resume within a period, without a click | held: the ramp, tested for steps; resume from a hold in 6.4 ms |
| the device thread realtime, no allocator, lock or log | held: every test's callback runs marked; PipeWire's thread is SCHED_RR |
| a missing, non-FLAC or cut-short file fails its own row, and the next plays | held (the player's tests) |
| all driven by events; `--trace-events` shows them | held: the trace is new in this milestone |
| paused, no CPU to speak of | held: 0.03% of a core, with the hold |
| the numbers in the notes | above |

**Checked beyond the tests.** ThreadSanitizer, and AddressSanitizer with
UndefinedBehaviorSanitizer, over `hifi_platform`, `hifi` and `blackwing_player`: clean.
Twenty runs in a row of `hifi`'s and the player's tests with no failure.

**Found about testing, worth keeping:**

- **exFAT takes an O_DIRECT write and refuses the read** (EINVAL). The first exFAT run's
  load was all errors and was thrown away. The script now tries both, and on such a
  filesystem reads with `dd`'s `nocache`, which drops the pages behind it.
- **The trace prints `{..}` for the player's events**: they do not describe their fields
  to the engine. The names, ticks and emitters are there; the fields are a small job for
  the player.

**Not done here, and noted:**

- The USB 2 run, and the hour on each of the three disks.
- The player's blocking `stat` before a read (Korvid's KR5 now offers one through the
  queue).
