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
