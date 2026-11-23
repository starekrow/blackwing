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
