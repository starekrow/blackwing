# hifi_codec

The decoder contract and the decoders behind it. A decoder is handed the bytes of one
audio file and a block of memory, and returns a format, a length, gapless facts and PCM
as integers. It opens no files, takes no memory but that block and calls nothing back, so
the same decoder can run in a sandboxed process in P1.

## How it works

**The contract** (`decoder.h`) is a record of four functions, `DecoderDesc`: `probe` says
whether some bytes look like its kind of file, `open` reads the headers and sets up its
state inside the memory it was given, `decode` returns the next frames, `close` ends it.
Everything a decoder knows lives in that one block, so two decoders of a kind run side
by side without knowing of each other, and a decoder is dropped by forgetting its block.

**Samples are integers at the file's depth**, each an `i32` in the range its bit depth
allows, interleaved. Not floats: a 16-bit file's samples come out as the 16-bit numbers
that went in, and "exact" can be checked with a hash.

**The end is one of two things.** `End` when the stream stopped where the file said it
would, `Damaged` when it stopped early or could not be read, with a reason in words.
What was returned before a `Damaged` is good audio: a file cut short plays what it had.

**FLAC** (`flac.h`, `src/flac.cc`) is dr_flac, vendored. It is given a bump allocator
over the decoder's block and nothing else to allocate from: its fallback `malloc` is
compiled to return nothing. Its samples arrive left-justified in 32 bits and are shifted
down to the file's depth.

## Using it

**Decode a file you have in memory.**

```cpp
const DecoderDesc* d = decoder_for(bytes);
if (!d) ... nobody's kind of file
u8* memory = (u8*)alloc_in(a, d->memory_bytes);
DecoderOpen o = d->open(bytes, Span<u8>(memory, d->memory_bytes));
if (!o.state) ... o.reason
i32 out[4096 * 8];
for (;;) {
    DecodeResult r = d->decode(o.state, out, 4096);
    ... r.frames frames of o.info.format.channels samples
    if (r.status != DecodeStatus::Ok) break; // End, or Damaged with r.reason
}
d->close(o.state);
```

**Add a decoder.** Write its `DecoderDesc` and add it to the list in `src/codec.cc`.

## Quick reference

Header `korvid/hifi_codec/decoder.h`:

| name | kind | what |
|---|---|---|
| `AudioFormat` | record | `rate`, `channels`, `bits` |
| `DecoderInfo` | record | `format`, `frames` (0: not said), `delay`, `padding` |
| `DecoderOpen` | record | `state` (null on failure), `reason`, `info` |
| `DecodeStatus` | enum | `Ok`, `End`, `Damaged` |
| `DecodeResult` | record | `frames`, `status`, `reason` |
| `DecoderDesc` | record | `name`, `memory_bytes`, `probe`, `open`, `decode`, `close` |
| `decoders()`, `decoder_for(bytes)` | functions | the decoders linked in; the first that claims some bytes |
| `decode_status_name(s)` | function | "ok", "end", "damaged" |

Header `korvid/hifi_codec/flac.h`: `k_flac_decoder`, the FLAC decoder's record.

The module registers nothing with the engine: it is a library.

## Behaviour worth knowing

- **The whole file is in memory.** `open` takes all of a file's bytes. Reading a window
  of a file too large to hold, and seeking, join the contract in slice 2.
- **A decoder's block is 4 MB for FLAC**, sized for the largest block the format allows.
  A file that needs more is refused with that as its reason.
- **A frame whose checksum fails is dropped, not played.** The audio after it arrives
  earlier than it should, and the decode ends `Damaged` with the count of frames read.
  Nothing says where the hole was.
- **A lying length is caught at the end.** A header that promises more frames than the
  file has gives every real frame and then `Damaged`.
- **`delay` and `padding` are 0 for FLAC**, which is sample-exact by itself.
- **Ogg FLAC is not read.** Native FLAC only, with or without an ID3v2 tag in front.
- **dr_flac's asserts are off** in every config, so no file can stop the program by
  reaching one, and its integer overflow on damaged input is made to wrap under GCC.
  Clang and MSVC builds have not been through the sanitizers yet.

## Tests

`kormake test hifi_codec` decodes eight files made by libFLAC (16 and 24 bits, mono and
stereo, 44.1 to 192 kHz) in three sizes of piece and checks each against the MD5 FLAC
keeps in its own header; then eight broken files; then six hundred spoiled ones from a
seeded generator. `tests/data/make_data.py` makes the files and says how.

`kormake test hifi_codec --heavy -- DIR...` does the MD5 check over real music, which is
not in the repository, and prints decode speed.

## Not in this module

Reading files (the player does, and hands over bytes). Tags and pictures
(`blackwing_tags`). Resampling, dither and anything else done to samples (`hifi`).
MP3 (slice 3) and the other codecs (P1).

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md), "Loading through codecs":
  why the contract is this strict.
- [Slice 1](../../docs/PRDs/Blackwing-Slice-1-PRD.md): what is built on this next.
- [src/vendor](src/vendor/README.md): where dr_flac came from and how to upgrade it.
