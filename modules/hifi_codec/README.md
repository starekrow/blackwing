# hifi_codec

The decoder contract and the decoders behind it. A decoder is handed one audio file as a
sparse span (its bytes, arriving in pieces) and a block of memory, and returns a format,
a length, gapless facts and PCM as integers, from the start or from any frame it is sent
to. It opens no files, takes no memory but that block, calls nothing back and waits by
returning, so the same decoder can run in a sandboxed process in P1.

## How it works

**The contract** (`decoder.h`) is a record of five functions, `DecoderDesc`: `probe` says
whether a file's first bytes look like its kind of file, `open` reads the headers and
sets up its state inside the memory it was given, `decode` returns the next frames,
`seek` makes the next `decode` start at a given frame, `close` ends it. Everything a
decoder knows lives in that one block, so two decoders of a kind run side by side
without knowing of each other, and a decoder is dropped by forgetting its block.

**The file is a sparse span** (`sparse.h`): the file's whole address range, cut into
pieces of one size (a power of two, a page or more), and a byte per piece that says
whether it is there. Whoever fills it writes a piece and then sets its byte with a
release store; a decoder loads the byte with acquire and reads only pieces it has seen
there. A piece that is not there may not be mapped at all. `sparse_whole` makes a span of
bytes already in memory, as one piece, for a caller with nothing to wait for.

**Waiting is returning.** When `open`, `decode` or `seek` needs bytes that are not
there, it returns `Wait` and the range it needs, having changed nothing but the frames
it returned, and is called again once the range has come. A `decode` that waits may
still return good frames first. A `seek` that waits, or fails, leaves the decoder where
it was; so does an `open` that waits, which starts again from nothing next time.

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

dr_flac reads by pulling, through callbacks, and cannot stop half way and come back, so
the wrapper sees that it is never asked for a piece that is not there. Before dr_flac
starts a new FLAC frame, the bytes that frame can need from its read cursor (the largest
frame STREAMINFO allows, or the largest the format allows when it does not say, and
dr_flac's 4 KB read-ahead twice over) must all be there, or `decode` returns `Wait`
without calling it. Where that cannot be said ahead (opening, seeking, a damaged frame's
search for the next sync), the read callback refuses a read that reaches a missing
piece, the call is let fail, and the decoder is put back at the last frame it returned:
from the stream's first frame, by dr_flac's own seek. dr_flac is shown the stream from
its `fLaC`, past any ID3v2 tag the wrapper has already found.

**Seeking** is dr_flac's: by the seek table when the file has one, by binary search when
the file says its length, and from the start otherwise. All three land on the sample.

## Using it

**Decode a file you have in memory.**

```cpp
const DecoderDesc* d = decoder_for(bytes);
if (!d) ... nobody's kind of file
u8* memory = (u8*)alloc_in(a, d->memory_bytes);
DecoderOpen o = d->open(sparse_whole(bytes), Span<u8>(memory, d->memory_bytes));
if (!o.state) ... o.reason
i32 out[4096 * 8];
for (;;) {
    DecodeResult r = d->decode(o.state, out, 4096);
    ... r.frames frames of o.info.format.channels samples
    if (r.status != DecodeStatus::Ok) break; // End, or Damaged with r.reason
}
d->close(o.state);
```

**Decode a file that arrives in pieces.** The same, with a span whose map fills in, and
a loop that answers `Wait`:

```cpp
SparseSpan file{base, size, 18, have};          // 256 KB pieces over reserved address space
DecoderOpen o;
while ((o = d->open(file, memory)).need.len) ... fetch o.need, then try again
for (;;) {
    DecodeResult r = d->decode(o.state, out, 4096);
    ... r.frames frames, good whatever the status
    if (r.status == DecodeStatus::Wait) { ... fetch r.need; continue; }
    if (r.status != DecodeStatus::Ok) break;
}
```

**Seek.** `d->seek(o.state, frame)`: `Ok`, and the next `decode` starts there; `Wait`,
and the same call is made again once `need` is there (or it is abandoned, and the
decoder is where it was); `Failed`, with a reason, and the decoder is where it was.

**Add a decoder.** Write its `DecoderDesc` and add it to the list in `src/codec.cc`.

## Quick reference

Header `korvid/hifi_codec/decoder.h`:

| name | kind | what |
|---|---|---|
| `AudioFormat` | record | `rate`, `channels`, `bits` |
| `DecoderInfo` | record | `format`, `frames` (0: not said), `delay`, `padding` |
| `DecoderOpen` | record | `state` (null on failure or a wait), `reason`, `need` (a wait), `info` |
| `DecodeStatus` | enum | `Ok`, `End`, `Damaged`, `Wait` |
| `DecodeResult` | record | `frames`, `status`, `reason`, `need` |
| `SeekStatus` | enum | `Ok`, `Wait`, `Failed` |
| `SeekResult` | record | `status`, `reason`, `need` |
| `DecoderDesc` | record | `name`, `memory_bytes`, `probe`, `open`, `decode`, `seek`, `close` |
| `decoders()`, `decoder_for(head)` | functions | the decoders linked in; the first that claims a file's first bytes |
| `decode_status_name(s)` | function | "ok", "end", "damaged", "wait" |

Header `korvid/hifi_codec/sparse.h`:

| name | kind | what |
|---|---|---|
| `ByteRange` | record | `at`, `len` |
| `SparseSpan` | record | `bytes`, `size`, `piece_shift`, `have` (a byte per piece) |
| `sparse_pieces(s)`, `sparse_has_piece(s, i)` | functions | how many pieces; whether one is there |
| `sparse_has(s, at, len)`, `sparse_missing(s, at, len)` | functions | whether a range is there; what of it is not yet |
| `sparse_whole(bytes)` | function | a span of bytes already in memory, all there |

Header `korvid/hifi_codec/flac.h`: `k_flac_decoder`, the FLAC decoder's record.

The module registers nothing with the engine: it is a library.

## Behaviour worth knowing

- **A decoder may ask for more than it strictly needs.** FLAC's window before each
  frame is the largest frame plus 8 KB, so near a piece's end it waits for the next
  piece a little early. It never asks for anything past the end of the file, and the
  front of what it asks for is always a piece that is not there.
- **A wait inside dr_flac costs a seek.** When the window was wrong (a damaged frame, a
  header that understates its frames), the decoder is put back by seeking from the
  first frame. Good files never take that path in the tests.
- **A seek into a damaged frame** either fails, or lands and plays the file's own
  samples from a later frame. A seek before the damage plays exactly up to it and ends
  `Damaged`, as a straight decode does.
- **Pieces may be given back only behind every decoder.** A decoder can be anywhere in
  the file after a seek. Whoever gives pieces back must know no decoder will read them;
  one that needs one again asks for it with `Wait`.
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

Then the file in pieces (`tests/seek_test.cc`): each good file, and one with a seek
table, decoded from a span whose pieces (4 KB to 256 KB) are committed only as they are
delivered, so reading one not delivered faults. Pieces come in four orders: what was
asked; one piece at a time, with each `Wait` asked twice to show nothing changed; the
asked range from its far end; anywhere in the file at random. Every decode is the MD5.
Seeks to forty places in each file (block edges, the last frame, the end, at random),
in order and shuffled, whole and in pieces, and behind an ID3v2 tag, each compared with
a straight decode; past the end is refused and the decoder carries on from where it
was; a seek that waits and is abandoned leaves it where it was. Seeks into a truncated
file, past a lying length, into a spoiled frame, in a file that does not say its length,
and two hundred spoiled files seeked at random in pieces.

`kormake test hifi_codec --heavy -- DIR...` does the MD5 check over real music, which is
not in the repository, and prints decode speed; then seeks each file to sixteen places
and compares each with the straight decode, and prints what a seek costs.

## Not in this module

Reading files (the player does, filling the span). Tags and pictures
(`blackwing_tags`). Resampling, dither and anything else done to samples (`hifi`).
MP3 (slice 3) and the other codecs (P1).

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md), "Loading through codecs":
  why the contract is this strict.
- [Slice 1](../../docs/PRDs/Blackwing-Slice-1-PRD.md): the contract as it began.
- [Slice 2](../../docs/PRDs/Blackwing-Slice-2-PRD.md), "The file in pieces" and "Seeking
  in a decoder": why it waits and seeks.
- [src/vendor](src/vendor/README.md): where dr_flac came from and how to upgrade it.
