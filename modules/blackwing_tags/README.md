# blackwing_tags

Tags, read from every source and never written, as functions of bytes. A reader is handed
a file as a sparse span (the decoders' kind: its address range, and the pieces of it that
are there) and memory to work in, and returns the fields it found, or the range it needs
next. It opens nothing, allocates nothing and never asks for a file whole: the art in the
middle of a tag is stepped over by its size. Each source has a rank; the best-ranked one
with a value for a field wins it, and every source's values are kept so the ranking can
change without reading a file again.

## How it works

**The sources and their ranks.**

| source | rank | read from |
|---|---|---|
| the user's override | 1 | the library's override files (`blackwing_library`) |
| Vorbis comments | 2 | a FLAC's metadata blocks |
| ID3v2.2, 2.3, 2.4 | 2 | the front of the file, or the back with a 2.4 footer |
| APEv2 | 3 | the back, before any ID3v1 |
| ID3v1 | 4 | the last 128 bytes |
| the path | 5 | the file's name and the directories above it |

Two sources share rank 2; between them the earlier in `TagSource` wins, so a FLAC's own
comments beat an ID3v2 tag someone put in front of it.

**One read is one call, started again after each wait.** `tags_read` looks in every
place a tag can be: ID3v2 tags at the front, one after another; FLAC's metadata blocks
after them; at the back an ID3v1 tag, an APEv2 tag before it, and an ID3v2 tag with a
footer before those. Each place is reached through one accessor that answers with the
bytes or with the first range that is not there, and the read stops at the first such
range and returns `Wait` with it. Called again, it starts from nothing, as a decoder's
`open` does: parsing is cheap next to the read it waited for. The first and last few
kilobytes of a file are usually all it needs; a picture of megabytes in an ID3v2 tag costs
one more wait, for the frames after it, and none of the picture.

**Frames are walked by their sizes.** An ID3v2 frame, a Vorbis comment, an APE item or a
FLAC block is read only when it is a field kept here, and then whole (256 KB at most); an
APIC frame or a FLAC picture block has only its head read, for its kind, its MIME type and
where its bytes are (`TagPicture`). The exception is an ID3v2.2 or 2.3 tag unsynchronised
as a whole: its sizes count the bytes after the scheme is undone, so the tag is read and
undone first (its first megabyte).

**Text leaves as UTF-8.** Latin-1, UTF-16 with a mark (either way) and without (as
big-endian when the encoding says so, else little-endian, as most writers mean it),
UTF-8, and ID3v2's unsynchronisation, per frame in 2.4 and per tag before it. Anything
that cannot be decoded (a stray byte, an odd byte at the end of UTF-16, a lone surrogate)
becomes U+FFFD; nothing is dropped whole. Values are trimmed of spaces and control
characters at their ends, and an empty one is not kept.

**Fields** (`TagField`): title, artist, album, album artist, track and disc numbers and
totals, date, genre, composer, the compilation flag, the sort names; and, kept for later,
ReplayGain values and MusicBrainz ids. A field may have several values: ID3v2.4's NUL-
separated values, Vorbis's repeated keys, APE's NUL-separated values. Numbers are split
("03/12" is 3 and 12) and lose their leading zeros; something that is not a number
("A1") is kept as it is. Genres given as ID3v1's numbers ("(52)", "52", "(52)(17)") become
names; "(52)Electronic", a reference refined by words, is the words.

**The ceilings.** No file can make a read go on for ever: 2048 frames, comments, items or
blocks in one tag, 256 KB in one, 4 MB asked for in all by one read, 1024 values, 64 of
one field from one source, 16 pictures. Past each, what came before is kept and the
`problem` says what was left. How many waits to answer is the caller's ceiling: after its
last, it calls with `last` and gets what could be read.

**The guess from a path** (`tags_from_path`) takes the file's name and up to three
directories above it: `Artist/Album/03 - Title`, `Artist - Album/03 Title`, `Artist/Album
(2003)/CD2/1-07 Title`, `[1999] Album`, `2003 - Album`, `Various/Hits/04 - Someone -
Song`, `Artist - Title`, a bare name (underscores as spaces when it has none). A dash in a
title stays in it when a directory already says who the artist is and nothing numbers
the track. A disc directory ("CD2", "Disc 1") gives the disc and is stepped over. Bytes
that are not UTF-8 become U+FFFD.

**The ranking** (`tags_merge`) is a function of the values: for each field the best-ranked
source with a value. An override whose value is empty hides the field (its source is
`TagSource::Count`, as for a field nobody has).

## Using it

```cpp
u8* memory = (u8*)mem_alloc(kTagReadMemory);   // 1.5 MB
TagRead r;
u32 waits = 0;
while ((r = tags_read(file, Span<u8>(memory, kTagReadMemory), waits == 16)).status == TagStatus::Wait) {
    ... read r.need into the span
    ++waits;
}
// r.values: every source's values, in the memory given; r.problem: "" or what could not be read
TagRead p = tags_from_path("Artist/Album/03 - Title.flac", Span<u8>(other_memory, 1 << 16));
TagMerge m;
tags_merge(all_values, &m);                     // m.source[(u32)TagField::Title]: who says the title
const TagValue* title = tags_find(all_values, TagField::Title, m.source[(u32)TagField::Title]);
```

## Quick reference

Header `korvid/blackwing_tags/tags.h`:

| name | kind | what |
|---|---|---|
| `TagField` | enum | `Title` ... `MbReleaseGroupId`: the fields |
| `TagSource` | enum | `Override`, `Vorbis`, `Id3v2`, `Ape`, `Id3v1`, `Path`: best first |
| `TagValue` | record | `field`, `source`, `len`, `text` (UTF-8, NUL-terminated) |
| `TagPicture` | record | `source`, `kind` (3: front cover), `at`, `len`, `mime` |
| `TagStatus` | enum | `Done`, `Wait` |
| `TagRead` | record | `status`, `need`, `values`, `pictures`, `sources` (a bit each), `problem` |
| `kTagReadMemory` | constant | the memory a read wants: 1.5 MB |
| `tags_read(file, memory, last, tail)` | function | every tag in a file, or the next range; with `tail` false, the front only (a FLAC's tags are all there) |
| `tags_from_path(path, memory)` | function | the guess from a path within a root |
| `TagMerge`, `tags_merge(values, out)` | record, function | which source wins each field |
| `tags_find(values, field, source)` | function | the first value of a field from a source |
| `tag_field_name`, `tag_field_from_name` | functions | "title", "albumartist" ... and back |
| `tag_source_name`, `tag_source_rank` | functions | "id3v2" ...; 1 to 5 |

The module registers nothing with the engine: it is a library, and `blackwing_library`
calls it.

## Behaviour worth knowing

- **What a read returns lives in the memory it was given**, until that memory is used
  again: copy what is kept.
- **A read asks for one range at a time**, the first it lacks. A file whose tags are at
  both ends and in the middle takes a wait per place.
- **ID3v2.4 sizes written as 2.3's** (iTunes did) are taken as 2.3's where the syncsafe
  reading lands on no frame and the plain one does.
- **A picture stored unsynchronised is not recorded**: its bytes in the file are not the
  picture's. Nor is one in a tag unsynchronised as a whole.
- **Compressed and encrypted ID3v2 frames are not read**, and say so. There is no zlib
  here.
- **ID3v1 is taken as Latin-1.** A national code page is P1 (guessing a wrong encoding).
- **An ID3v2 tag that says it is longer than its file** is read to the file's end.
- **The path guess is a guess.** "99 Luftballons" is track 99; "1984" is a title (four
  digits are a year, not a track).

## Tests

`kormake test blackwing_tags`, on bytes made by the test itself: no files and no index.
ID3v2.2, 2.3 and 2.4 in each of their encodings (2.4's four, the others' two), with and
without unsynchronisation, an extended header and a footer, UTF-16 marked either way,
with a picture: every field as written, multi-byte and astral characters included, and
the picture's place to the byte; 2.4 with iTunes' sizes. A FLAC's comments and picture,
and the same FLAC behind an ID3v2 tag (both read; Vorbis wins). APEv2 with and without a
header, two values in one item and a cover; ID3v1.1 and 1.0 in Latin-1. Each rank over the
one below it, and an override over all, an empty one hiding. Thirteen paths. A hundred
megabytes of art between frames, in pieces of 4 KB to 256 KB: two or three pieces read and
none inside the art; tags at the back after it; and the same with nothing more to come.
Seventeen hostile tags (sizes that lie, frames past their tag, text not its encoding, a
field a thousand times, three thousand frames, six megabytes of titles, both ID3 versions,
APE and FLAC sizes that lie, an unsynchronised 2.3 tag, unknown versions). Six hundred
spoiled copies of a file with every kind of tag, whole and in pieces: every value valid
UTF-8, every wait fair, every read ending.

## Not in this module

The override files and the index (`blackwing_library`). MP4 atoms, RIFF and AIFF chunks,
cue sheets (P1, with their codecs). Decoding art, and art beside the files (the library
finds `cover.jpg`). Writing tags (P1).

## See also

- [Slice 3](../../docs/PRDs/Blackwing-Slice-3-PRD.md), "Tags are functions of bytes".
- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md), "Tags".
- [hifi_codec](../hifi_codec/README.md): the sparse span, and the decoder contract this is
  the cousin of.
- [The script surface](SCRIPT.md): the names, the ranking and the readers, from Lua. A draft.
