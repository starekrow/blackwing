# hifi_codec

The decoder contract and the decoders behind it. A decoder is handed a readable handle
and returns a format, a length, gapless facts and blocks of PCM, with seeking. It opens
no files and calls nothing back, so the same decoder can run in a sandboxed process in P1.

**Status:** a skeleton. No code yet; this page says what the module will hold.

## What it will hold

- The contract, as plain data.
- P0 decoders: FLAC (dr_flac) and MP3 (dr_mp3).
- P1: WAV, AIFF, Ogg Vorbis, Opus, ALAC, AAC and their kin, and someone else's native
  codec as a sandboxed module on this contract.
- The broken-file corpus and its tests.

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): the plan this module is part of.
- [modules](../README.md): the other modules and how they depend on each other.
