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
