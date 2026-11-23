# Slice 3 notes

What building [slice 3](../PRDs/Blackwing-Slice-3-PRD.md) found, milestone by milestone,
with the numbers. Written as it goes.

## Milestone 1, MP3

Nov 23, 2026. Linux laptop, `linux-dev`.

**As planned**, with these particulars:

- dr_mp3 is vendored from the dr_libs commit dr_flac came from, and only its low-level
  push decoder is used: it takes a buffer and decodes the frame at its front, so it never
  pulls and the decoder has nothing to undo after a wait. It is handed a fixed window, 32
  KB from where the last frame ended, which makes every decision it takes a function of
  the bytes and that place alone: every piece size and order gives the same samples.
- The reference is libmpg123 1.33.3 and the encoder libmp3lame 3.101, both already on
  the machine as shared libraries for its players, driven from `make_mp3.py` through
  ctypes. Nothing was installed.
- `DecoderInfo` gains `estimated`, for a length guessed from the size (no Xing, Info or
  VBRI header). The PRD asked for the length "marked as an estimate"; this is the mark.
- BR23 is done in `hifi`, not the player: the render thread drops `delay` frames at a
  source's start and everything from `frames - padding` on. Everything above counts the
  music's frames.

**Within a bit.** The ten good files, trimmed by their own delay and padding and rounded
to 16 bits, are within one of libmpg123 on every sample; 1128 of 361605 samples differ,
all by one. Each trimmed decode is exactly the 22050 (or 12000, 16000, 16537, 24000)
samples LAME was given. "Within a bit" is one, at 16 bits.

**Seeking does not use the Xing table to land.** The table says roughly where a time is
in the file (to a 256th of its size), never which frame begins there, and a frame's
number cannot be read from its bytes. Landing on the frame asked for, which the contract
promises and a gapless queue needs, means counting frames from a place whose count is
known. So the decoder keeps a mark every 8 frames as it walks (where the search starts,
and the two things dr_mp3 remembers there), and a seek walks forward from the nearest:
headers and side information only, with dr_mp3 keeping its bit reservoir, to two frames
before the target, which it decodes to fill the filter bank's history. A seek from a
mark (rather than from the decoder's own place) starts the reservoir's depth further back
(nine frames at MPEG 1's smallest, sixty-five at MPEG 2's). Every seek in the tests is
the straight decode, sample for sample. The table is used for what it is good for: how
much to ask for at once when a seek must read ahead. The PRD's "a wait per stretch" for a
cold file is what it costs, with or without the table.

**dr_mp3's sync wants four frames, not ten.** Its search takes a sync as a frame when
the next ten in the window agree. Junk in the middle of the broken-file corpus's CBR
file sat ten frames from the start, and the first half of the file was thrown away to
find ten in a row after the junk. Four agree as surely on any file not made to fool it,
and the file plays up to the junk.

**What a broken MP3 does**, from the corpus: a length that lies long ends `Damaged` with
the count, after every real frame; one that lies short decodes every frame and ends
`End` (`hifi` cuts at the claim). Junk between frames costs the frame before it and the
first after (silence: its reservoir was where the junk is) and ends `Damaged` with the
junk's size. A file cut short or inside a frame plays what it has. A front tag that says
it is longer than the file is refused. Six hundred spoiled files in pieces with seeks
kept every promise of the contract, under ASan and UBSan as well.

**Not done here**: an MP3's true length by reading it through (the background pass,
BL23), and the player's head read, which still reads a front tag whole before
`decoder_for` is asked (the decoder itself steps over it).

## Milestone 2, tags

Nov 23, 2026.

**As planned**, with these particulars:

- The reader is handed the decoders' `SparseSpan`, so the library can give the same
  pieces to a tag reader and to a decoder's `open`. `blackwing_tags` gains `hifi_codec` as
  a dependency for the header; nothing else of it.
- A read is one call over every place a tag can be, restarted from nothing after each
  wait, like a decoder's `open`. The PRD's "fields, or the range it needs next" is that:
  one range per call, the first it lacks. Parsing again is nothing next to the read.
- The ceiling on ranges is the caller's: after its last wait it calls with `last`, and
  gets what was there and a problem in words. The ceiling on bytes is the reader's, 4 MB
  asked for in one read, and on items, values and text.
- Two sources share rank 2, Vorbis comments and ID3v2. The tie goes to Vorbis, the
  FLAC's own: an ID3v2 tag in front of a FLAC is somebody else's software.

**The numbers.** A hundred megabytes of art between ID3v2 frames, with more frames after
it and two tags at the back, in pieces of 4 KB: three waits and three pieces read of
25602; in 64 KB or 256 KB pieces, two. Six hundred spoiled files: every value valid UTF-8,
227 with a problem said.

**Found by the sanitizers**: the problem's text was kept in the reader's own stack frame
and returned. It lives in the caller's memory now, with the values.

**Not done here** (they are the library's): the override files, art beside the files,
and the sort keys.

## Milestone 3, the walk and the index

Nov 23, 2026. Linux laptop, `linux-dev`, the built-in NVMe disk, its page cache warm.

**Blocked, then unblocked in Korvid.** sqlite's amalgamation is C that does not compile
as C++, and kormake built only `.cc` and `.mm`. With the go-ahead, kormake now builds a
module's `.c` files with each toolchain's C compiler (KR14, in Korvid's slice 10 on its
`slice-10` branch); `src/sqlite.c` sets the options and includes the amalgamation. The
heavy test then found that a module's heavy tests did not take its `test_deps`, and that a
test's `test_deps` brought no externals; both are fixed there too. sqlite takes 1 m 45 s
to compile at `-O2 -g`, once.

**As planned**, with these particulars:

- **Four threads**: the main thread's system, a scan thread that walks, a writer with the
  index's one writing connection, a reader with a read-only one. They speak through
  inboxes under one mutex. Own threads rather than the I/O queue's tasks: a task's
  completion is reaped at the pump, and `blackwing` ticks four times a second when idle,
  which would pace the walk.
- **The list of roots is a file of its own** (`roots.txt`), not only a table: the index is
  a cache, and "rebuilt from the roots" needs the roots to be somewhere the index is not.
- **A directory is a row as soon as its parent is listed**, before it is listed itself, so
  a folder view shows the whole of what is known.
- **Links to directories are not followed** (the PRD left it open, and Korvid's slice 10
  asked): a walk cannot loop or leave its root. Links to files are listed.
- **Windows follow the index already**: open windows are asked again when the generation
  moves, at most four times a second. The rest of BL13 (more views, sorting, cancelling in
  sqlite) is milestone 5's.
- **`library add`, `remove` and `status` are the library module's own commands**, as the
  player's are the player's; `blackwing` has `find` and `play`, which glue the library to
  the player (the PRD put all of BL22 in `blackwing`).

**The numbers.** Fifty thousand generated files (12.2 MB, 4167 albums in four layouts,
names in several scripts), from `LibraryAddRoot` on an empty index:

| | |
|---|---|
| a chosen track found by name | 0.47 s, the walk not over |
| that track heard through the player | 0.50 s |
| another, found after the walk | 1.21 s (its batch committed), heard at 1.23 s |
| the names pass | 50,000 files in 0.56 s: 90,000 files a second |
| the index | 38.4 MB |
| the second start: a window of 100 at row 25,000 answered | 0.08 s after start-up |
| the second start: the walk again | 0.94 s, every id kept |

The three-second criterion holds with room to spare on the built-in disk with a warm
cache. A cold cache and the USB disks are for the close. The writer is the slower half:
the walk lists 50,000 files in 0.56 s and the commits that make them findable take about
twice that.

**Not done here**: the tags pass and the priority slot (milestone 4), the other views and
sorting (5), the watcher, missing files and an emptied root (7).

## Milestone 4, the tags pass and the slot

Nov 23, 2026. The built-in NVMe disk.

**As planned**, with these particulars:

- **A tagger thread**, a fifth, between the writer and the main thread. It cannot read
  files itself: the engine's I/O queue is submitted to and reaped on the main thread. So
  it asks the main thread for ranges, which go into the queue at `Low` (the slot's at
  `Normal`), at most 48 at once, and gets the bytes back into the track's sparse span.
  Reads are tick-bound: a round trip is two or three ticks.
- **The decoder's probe is shown the bytes after a front ID3v2 tag**, not the head with
  the tag in it, so the art in a tag is not read to find out what decodes the file. (The
  player's own head read still reads such a tag whole; that is the player's.)
- **`LibraryScanFinished` means the whole scan**, walk and tags; the walk's end is the root
  going from walking to reading. The PRD's event table left it open.
- **The slot is resolved by the writer**, which has the index: a path becomes a track, or
  a directory and its tracks, and a directory the walk has not listed is listed by the scan
  thread next, ahead of the walk.

**Found on the way.** The first tags pass ran at 425 files a second with the disk idle.
The tagger waited on reads 98% of the time, and the main thread's tick took 33 ms: the
writer held the shared mutex while it counted tracks after each commit, and it committed
after every message. Counting before taking the lock, and committing at most four times a
second, made it 5,400 files a second. The walk's rows now go in a commit of their own
before the tags', so a file is findable by name sooner.

**The numbers**, fifty thousand files of twenty to an album:

| | |
|---|---|
| a chosen track found by name / heard | 1.70 s / 1.73 s after `LibraryAddRoot` (the walk's rows still going in) |
| the walk | 50,000 files in 0.51 s; all of them rows at 1.83 s |
| a folder of twenty prioritized as the tags pass starts | its tags in the index 0.59 s later |
| the tags pass, warm cache | 5,500 files a second after the walk; all read 10.9 s after `LibraryAddRoot` |
| the index | 75.7 MB with every source's values |
| the second start | a window answered in 0.11 s; the walk again in 0.65 s; no file read again |
| the disk run: page cache emptied of the 50,000 files and the track | a scan of 16.0 s while a 12 s MP3 played eight times from the same disk: 0 underruns |

Found by name takes longer than at milestone 3 (0.47 s) because the writer now shares its
time with the tags. It is within the three seconds; the writer's inserts (about 30,000
rows a second, with the search table) are what to make faster if a slower machine needs
it.

**Not measured**: the USB disks. They are not attached to this machine; their numbers
are for the close.

## Milestone 5, queries

Nov 23, 2026.

**As planned**, with these particulars:

- **Albums and artists are groupings, named by a hash.** An album is its album artist and
  title, or, with no album artist, its title in its directory (so a compilation without
  one is one album, and two discs in two directories without one are two; the PRD's Open).
  An artist is the album artist, or the artist. Their ids are hashes of their sort keys,
  kept in each track's row with the keys, so an `Albums` or `AlbumTracks` query can name
  one.
- **Sort keys fold case and strip marks** for Latin, Greek and Cyrillic, from a table made
  from Unicode's data (BL26); other scripts sort by code point. Leading articles stay.
- **A window is a walk of an index.** The first version sorted: a window of 100 at the end
  of fifty thousand by title took 110 ms, because the index did not match the ORDER BY to
  its last column, and every skipped row was read. Each order now has an index that is it,
  a window's ids are read from it first and its rows fetched by id (2 ms where it was
  110), and the groupings have indexes that cover them.
- **Groupings and searches are made whole and kept** by the reader for one generation; a
  window is a slice. This is what makes a search's later windows 0.5 ms.
- **Cancelling** is a progress handler: when a newer ask for the window being answered
  arrives, the reader's query is interrupted in sqlite. Forty wide searches replaced at
  once by narrow ones: forty stopped, and only the narrow ones shown.
- **`LibraryResult` says how long the index took** (`seconds`), and `LibraryQuery` gained
  `sort`. Both are the PRD's table with a field more.

**The numbers**, windows of 100 over fifty thousand tracks, the index's time at the start,
the middle and the end, in ms:

| view | start | middle | end |
|---|---|---|---|
| every track (by artist, album, disc, number) | 3.8 | 5.2 | 7.8 |
| by title / album / date / path | 3.3–3.8 | 4.3–5.5 | 5.1–9.4 |
| a genre's tracks | 3.8 | 5.7 | 6.9 |
| artists (279) / albums (2,311) / genres | 7.8 / 10.4 / 4.9 | 0.0 | 0.0 |
| an artist's albums / an album's tracks / a folder of 904 | 4.0 / 0.3 / 0.1 | 0.0–0.2 | 0.0–0.2 |
| search "zq4" (11,111 matches) | 7.0 | 0.7 | 0.5 |
| search "track" (every track), best first / by artist | 30.1 / 34.5 | 0.5 | 0.5 |

The round trip from the query to its rows in the table is 4 ms more: the reader's thread
and the next tick. Every window is within the 20 ms of the exit criteria but the first of
a search that matches every track, which ranks fifty thousand matches; a search as it is
typed matches that many only at its first letter or two. The index is 117 MB with its
indexes and every source's values.

## Milestone 6, overrides

Nov 23, 2026.

**As planned**, with these particulars:

- **The format**, which the PRD left open: a small INI-like text of our own, a `[file
  name]` and `field = value` lines under it, with a comment at the top saying how. It is
  read and fixed in any editor, and needs nothing of Korvid's settings.
- **The tree mirrors each root by its path**, not its id: a root removed and added again
  gets a new id and keeps its overrides. A root that moves loses them, as a moved file does
  until B43's audio hash.
- **Override files beside the music are not read.** The PRD's Open; nothing here needs it
  yet.
- **The merge is shared**: the tagger merges a directory's overrides into every read, so a
  rebuild has them; the writer merges a track again, when one is set, from the other
  sources' values `tagvals` keeps, without reading the file.
- **"Read at start-up and when changed"**: at start-up and when a root is added, each of
  its override files whose time differs from the last time it was taken in. One changed
  by hand while the program runs is taken in at the next start (or, from milestone 7,
  when the watcher says).

**Found on the way.** A root removed just before the program stopped could come back at
the next start: its removal and the save of the list of roots are two messages to the
writer, and a writer told to stop between them dropped the second. It now does what it
was given before it stops.
