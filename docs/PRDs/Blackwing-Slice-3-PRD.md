# Blackwing Slice 3 — The library

Nov 23, 2026 · draft, first review folded in

## Goal and exit criteria

Slices 1 and 2 built a player that plays what it is told to, by path. Slice 3 is how it
finds out what there is: you aim it at a directory and you are done. The
[master PRD](Blackwing-Master-PRD.md) says what that means: no import step, music
playable before the scan has finished, what you look at scanned next, tags read and
never written, and nothing that stops the scan.

Three things are built: `blackwing_tags`, which turns a file's bytes into fields;
`blackwing_library`, which walks roots, keeps the index, watches for changes and answers
queries; and an MP3 decoder, because a library of FLAC alone is nobody's library.

The risk in this slice is different from the last two. Nothing here is realtime. What
can go wrong is that it is slow where someone is waiting, that it steals the disk from
the audio path, or that one of fifty thousand files written by twenty years of other
people's software stops it. So the tests are a large synthetic library, a corpus of
broken files, and the slice 1 disk runs with the scan as the load.

There is no face until slice 4. Everything is driven by events and commands, and read
through tables, as the player is.

It is done when:

- **Aimed at fifty thousand tracks, it is playing a chosen one within seconds.** From
  `LibraryAddRoot` on an empty index: a track found by its file name is playing within
  three seconds on the built-in disk, with the scan nowhere near done.
- **Every file is listed after the names pass**, by folder, with a title, artist and
  album guessed from its path, and plays from that alone.
- **What is looked at is scanned next.** With the tags pass just started, a
  `LibraryPrioritize` for a folder of twenty tracks has their tags in the index within
  a second, on the built-in disk; the number for the USB disks is in the notes.
- **The scan never costs a glitch.** Slice 1's ninety-second runs, straight and
  skipping, show no underrun on any of the three disks while the library scans the
  disk being played from.
- **Nothing in the corpus stops the scan.** Every broken, hostile and merely odd file
  ends as a row: with its tags, or with a name from its path and a reason. None hangs
  it, crashes it or makes it read a file whole.
- **Tags come out right.** For the corpus's good files, every field matches what the
  generator wrote: Vorbis comments, ID3v2.2, 2.3 and 2.4 in each of their encodings,
  ID3v1, APEv2, and each rank beating the one below it.
- **An override wins and survives.** A field set with `LibraryOverride` shows in every
  query, is still there after the index is deleted and rebuilt, and no audio file's
  bytes or modification time have changed.
- **The second start is instant.** With the index built, the views answer within a
  second of starting, and files whose size and time are unchanged are not read again.
- **A list of fifty thousand rows answers anywhere.** A window of rows at any place in
  any view, sorted, comes back within 20 ms, and a query replaced by a newer one stops.
- **Changes are noticed.** A file added, changed, renamed or removed under a root shows
  in the index within two seconds on a local disk, with no rescan; a root on a share is
  polled.
- **A root that goes away is offline, not forgotten.** Its rows stay, marked; when it
  returns, they are checked and nothing is re-read that did not change.
- **A deleted or damaged index is rebuilt** from the roots and the overrides, with a
  message and no question.
- **MP3 plays**: decodes to within a bit of a reference decoder at 16 bits, seeks,
  arrives in pieces, and joins without its encoder's delay and padding where the file
  says what they are.
- Numbers, in the notes: the names pass and the tags pass in files a second on each of
  the three disks; time to first playable; a prioritized folder; a query window; the
  second start; the index's size; memory held.

## What the code has

- **The player** (`blackwing_player`): a list of paths with a place in it; seek, skip,
  the queue's edits; files read in pieces, the target first. It knows nothing of a
  library and is not changed to.
- **The decoder contract** (`hifi_codec`): `probe`, `open`, `decode`, `seek`, over a
  sparse span, with `Wait`. FLAC. `DecoderInfo` carries delay and padding; nothing
  trims them yet (slice 2's BR23, owed).
- **`blackwing_tags`, `blackwing_library`**: pages.
- **The engine** (Korvid): ranged reads and stat through the I/O queue, with
  priorities and a budget; tasks on worker lanes, cancellable; `FsWatcher`, native
  (inotify here) or polling; a recursive directory walk that is synchronous and
  whole-tree; UTF-8 helpers. No sqlite, no single-directory listing, no JPEG.
- **Three disks** and a stress script from slice 1.

## The shape

### Where things are

| module | this slice |
|---|---|
| `hifi_codec` | the MP3 decoder (dr_mp3, vendored) |
| `hifi` | delay and padding trimmed at a source's ends |
| `blackwing_tags` | the readers, the ranking, the guess from a path, the override files |
| `blackwing_library` | roots, the passes, the priority slot, the index, watching, queries |
| `blackwing` | `library` commands; the glue between the player and the library |

`blackwing_library` gains `hifi_codec` as a dependency, for a file's rate, depth and
length: the decoder that will play it is the one asked.

### MP3

dr_mp3, on the contract as FLAC is: a sparse span, `Wait`, `seek`.

- **Depth.** MP3 has none. The decoder reports 24 bits and returns dr_mp3's floats
  rounded to them, clamped. "The file's samples" for a lossy file means this decoder's.
- **Delay and padding** come from the LAME tag where there is one, and go out in
  `DecoderInfo`. `hifi` drops them at the source's start and end (BR23), so an album
  of such files joins as it was mastered. Without the tag, nothing is trimmed, as the
  master PRD allows in P0.
- **Length** from the Xing or VBRI header; without one, an estimate from the size and
  the first frames' bitrate, marked as an estimate.
- **Seeking** uses the header's table where there is one. Without one it reads forward
  from the nearest place it knows, which on a cold file is a wait per stretch.
- **The front of the file** may be megabytes of ID3v2 art. The decoder steps over it
  by its declared size and asks for nothing inside it.

There is no MD5 to check against. The oracle is a reference decoder at generation time
(what the reference made is checked in beside each file), and the decoder against
itself: every piece size and order of delivery gives the same samples, and a decode
from a seek equals the same stretch of a straight one.

### Tags are functions of bytes

A tag reader is the decoder contract's cousin. It is handed a window of a file's bytes
and memory to work in, and returns fields, or the range it needs next. It opens
nothing, allocates nothing else and never asks for a file whole: the art in the middle
of an ID3v2 tag is stepped over, not read.

| source | rank | read from |
|---|---|---|
| the user's override | 1 | Blackwing's own data |
| Vorbis comments (FLAC) | 2 | the file's metadata blocks |
| ID3v2.4, 2.3, 2.2 | 2 | the head of the file |
| APEv2 | 3 | the tail |
| ID3v1 | 4 | the last 128 bytes |
| the path | 5 | the name and its two parent directories |

For each field the best-ranked source that has it wins. Every source's value is kept
in the index, so the ranking can change without reading a file again.

- **Fields**: title, artist, album, album artist, track and disc numbers and totals,
  date, genre, composer, the compilation flag, sort names. Kept and not yet used:
  ReplayGain values and MusicBrainz ids. A field may have several values; all are kept.
- **Encodings**: ID3v2's Latin-1, UTF-16 with and without a mark, and UTF-8, and
  unsynchronisation. Everything leaves as UTF-8, with what cannot be decoded replaced
  and never dropped whole.
- **The guess from a path** covers the common layouts (`Artist/Album/03 - Title`,
  `Artist - Album/03 Title`, and a bare name). It is the lowest rank, so it is only
  ever seen where nothing better exists.
- **Art** is found, not decoded: a picture inside the tags is a file, an offset and a
  length; a `cover`, `folder` or `front` image in the directory is a path. Thumbnails
  wait for a face to show them.
- **A file's audio facts** (rate, channels, depth, length) are the decoder's `open`,
  on the same head bytes.

### Overrides

The user's corrections are the one thing here that is not a cache.

- They live in Blackwing's own data, one small text file per album directory, in a
  tree that mirrors each root. Nothing is written under a root, so read-only media and
  other people's shares work, and "never write to the user's music" covers the
  directory as well as the files.
- An override names a file by its path within the root, a field and a value. An empty
  value hides a field. Removing the line restores the source's value.
- They are read at start-up and when changed, and outrank every source.
- A file that will not parse is set aside under a new name, with a message, and never
  written over.

A file that is moved loses its overrides until B43's audio hash (P1). The library
softens this: see Changes.

### Roots and the passes

A root is a directory. A file is named by its root and its path within it, never by an
absolute path, so a root can move (and, in P1, be a disc image).

1. **Names.** A walk, a directory at a time, off the main thread, breadth first. Each
   audio file found (by its extension) becomes a row at once, with its size and time
   and its guessed tags. Rows go to the index in batches, a few times a second.
2. **Tags.** For each row whose size or time is new: the head of the file is read
   through the I/O queue, then the tail if the kind wants it, then whatever range a
   reader asks for. Fields and audio facts go to the index.
3. **Background.** What is slow and nobody is waiting for. In this slice it has one
   job: the true length of an MP3 that has no header, by reading it through.

The passes do not run in order across the whole library: a directory can be in its
tags pass while another has not been walked.

**The scan is the lowest priority there is.** Its reads go to the I/O queue below the
player's, and within the queue's budget, so a playing track's pieces go first. A read
under way is not interrupted, which is what the lead is for.

**A file that cannot be read** (no permission, an error, a reader that gives up) keeps
its row from the names pass and gains a reason. A directory that cannot be listed is
marked and the walk goes on. Readers have a ceiling on how many bytes and how many
ranges they may ask for, so a hostile file cannot make the scan read forever.

### The priority slot

`LibraryPrioritize` names what someone is looking at: a directory, a set of tracks, or
an open query. There is one slot, and a newer call replaces it.

- A directory not yet walked is walked next.
- Its files' tags are read next, at the I/O queue's normal priority instead of the
  scan's.
- When the slot's work is done, the scan goes back to where it was.

Playing a file prioritizes it. The player and the library do not know each other, so
this is glue in `blackwing`: it hears `PlayerEnqueue` and tells the library.

### The index

sqlite, vendored, in Blackwing's own data, in WAL mode.

- **It is a cache.** Everything in it can be made again from the roots and the
  overrides. A version that does not match, or a file sqlite calls corrupt, is deleted
  and rebuilt, with a message.
- **One writer, one reader, neither on the main thread.** The writer takes batches
  from the passes. The reader answers queries, so a long write never holds a query.
- **Tracks, their sources' values, the merged fields, art, roots and directories** are
  its tables. Albums and artists are not stored: they are groupings of tracks, by
  album artist and album title, made when asked.
- **Search** is sqlite's FTS5 over the merged title, artist, album and path, by prefix,
  so it answers as it is typed.
- **Sorting** uses a key made from each name: case folded, marks stripped. It is not
  the Unicode collation the design notes want; that is with Krome's text, in the
  master PRD's Open.

### Queries

A query is a window that is kept open, because that is what a list in a face is.

| name | kind | what |
|---|---|---|
| `LibraryAddRoot { path }`, `LibraryRemoveRoot { root }` | events in | |
| `LibraryPrioritize { root, dir }`, and by tracks or by query | events in | the slot |
| `LibraryQuery { id, view, parent, text, sort, first, count }` | event in | opens or replaces window `id` |
| `LibraryClose { id }` | event in | |
| `LibraryOverride { track, field, value }` | event in | writes the override |
| `LibraryResult { id, total, first, count, generation }` | event out | the window's rows are in the table |
| `LibraryChanged { generation }` | event out | something a query might show has changed |
| `LibraryScanFinished { root }` | event out | |
| `library.roots` | ktable | a row per root: path, state (walking, reading, idle, offline), counts |
| `library.scan` | resource | what is being scanned, what is in the slot, files a second |
| `library.rows` | ktable | the open windows' rows, by query and index |

- **Views**: folders, artists, albums, an album's tracks, genres, all tracks, search.
- **A window is at most a few hundred rows**, and a handful may be open. Scrolling
  moves `first`; the rows table is refilled and a `LibraryResult` says so.
- **A newer query with the same id cancels the older**, in sqlite and in the queue.
- **Open windows follow the index.** When rows they show change, they are refilled,
  at most a few times a second while a scan is running.
- **A row carries what is needed to play it**: its root and path, as the path the
  player takes.

### Changes

Each root is watched with the engine's `FsWatcher`. Its events are hints: the library
looks at the file again and believes its size and time.

- A file still being written changes many times. It is read when it has been quiet for
  a moment.
- **A root that cannot be trusted to report** (a share, or more directories than the
  OS will watch) is walked again at an interval instead: the names pass, which reads
  no file that has not changed.
- Every root is walked once at start-up for the same reason: things change while the
  program is not running.
- **A file that goes is marked missing, not deleted.** Its row leaves the views and is
  kept for a month, so a file that comes back (a share that blinked, a rename seen as
  a removal and an addition) is the same track with the same overrides.
- **A removal and an addition of the same name, size and time** within one look is a
  move: the track keeps its id and its overrides move with it.

### A root that goes away

A root whose directory is gone, cannot be listed, or is empty where it held tracks is
**offline**. Nothing under it is marked missing; its rows stay in every view, marked,
and playing one fails its queue row with the reason. It is looked at again at an
interval, and when it returns it is walked as at start-up.

The empty case is the one that matters: an unmounted disk leaves an empty directory,
and a library that believed it would forget everything.

### The corpus

The broken-file corpus started in slice 1 with FLAC. It grows here to cover tags and
MP3, and the scanner runs over all of it in one test.

- Tags: sizes that lie, frames that run past the tag, text that is not the encoding it
  claims, a tag of a hundred megabytes of art, the same field a thousand times, both
  ID3 versions at once, tags at the wrong end, a FLAC with an ID3 tag in front.
- MP3: no header, a header that lies, free format, junk between frames, cut short, a
  file that is one frame, another kind of file with the extension.
- The tree: names that are not UTF-8, a path of a thousand bytes, a directory of ten
  thousand files, a link that loops, an unreadable directory, a file that changes
  while it is read.
- Spoiled copies by the hundred, as slice 1 did for FLAC.

### The fifty thousand

A generator makes the test library: fifty thousand small tagged files (a fraction of a
second of audio each, about 50 MB in all), in a few thousand albums by several hundred
artists, in several layouts, with names in several scripts. It is made into a temporary
directory by a heavy test and is not checked in. A real library is run beside it at the
close, for whatever a generator would not think of.

## What the engine gets

- **A listing of one directory**, not a tree: names, kinds, sizes and times, hidden
  entries on request, usable from a task. The walk has to be a directory at a time for
  the priority slot to mean anything, and `io_list_files_stat` is the whole tree in
  one call. Blocks milestone 3.
- **The directory for a program's own data**, per OS (`~/.local/share/blackwing` here),
  if the engine does not have one already. The index and the overrides live there.
- **Whether a path is on a share**, so a root can be polled without being told.
  Wanted; a start-up walk makes it not needed for correctness.
- **A volume's identity in `IoStat`**, so "the root is a different disk now" can be
  told from "the root is empty". Wanted; the offline rule above works without it.
- **KR10, the timed wait**, is still wanted by `hifi`.

These go to [Korvid-Changes-For-Blackwing.md](Korvid-Changes-For-Blackwing.md) as KR11
to KR14 when this draft is agreed.

Two things need the machine and a go-ahead, not the engine: vendoring sqlite's
amalgamation and dr_mp3 are downloads, and the MP3 test files want a reference
encoder and decoder installed where they are generated.

## Out of scope

- **Cue sheets**, as files or inside a FLAC: a track that is a stretch of a file needs
  a kind of queue row the player does not have. Proposed for P1, with disc images.
- **Thumbnails** and decoding art at all: with the first face (slice 4), which also
  brings the question of a JPEG decoder to the engine.
- **Playlists** (reading M3U8), and the queue and session surviving a restart (B28).
- **The other tag formats** (MP4 atoms, RIFF and AIFF chunks): with their codecs, P1.
- **Guessing a wrong encoding** (ID3v1 in a national code page). P1.
- **Identity by audio hash** (B43), loudness (B44), tag write-back (B38). P1.
- **Any face.** `ui_dev` is slice 4, and is this slice's first real customer.
- **Paths longer than an event holds.** `PlayerEnqueue` carries 511 bytes. A longer one
  is listed, with its tags, and fails to play with that as the reason (see Open).
- Slice 2's BR22 (the device's widest word) and the hour-long run stay owed.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| BL1 | The MP3 decoder on the contract: sparse span, `Wait`, `seek`, 24 bits from floats | P0 |
| BL2 | Delay and padding from the LAME tag, and trimmed in `hifi` (slice 2's BR23) | P0 |
| BL3 | The tag reader contract: a window and memory in, fields or the next range out | P0 |
| BL4 | Readers: Vorbis comments, ID3v2.2 to 2.4 in every encoding, ID3v1, APEv2 | P0 |
| BL5 | The guess from a path | P0 |
| BL6 | The ranking, with every source's value kept | P0 |
| BL7 | Overrides: files in Blackwing's own data, mirrored by root; read, written, set aside when broken | P0 |
| BL8 | Roots named by id, files by root and path within it | P0 |
| BL9 | The names pass: a directory at a time, off the main thread, rows at once | P0 |
| BL10 | The tags pass: ranged reads at the scan's priority, skipping files whose size and time are unchanged | P0 |
| BL11 | The priority slot: a directory, tracks or a query; one slot; playing a file fills it | P0 |
| BL12 | The index: sqlite in WAL mode, a writer and a reader off the main thread, rebuilt when wrong | P0 |
| BL13 | Queries as windows: views, sort, search by prefix, cancelled by a newer one, refilled when the index changes | P0 |
| BL14 | The library's events and tables above, and nothing else, as its API | P0 |
| BL15 | Watching: the engine's watcher as hints, a quiet moment before reading, a walk at start-up | P0 |
| BL16 | Roots that cannot be trusted to report are walked at an interval | P0 |
| BL17 | Missing files are marked and kept; a root gone or emptied is offline | P0 |
| BL18 | Every failure leaves a row and a reason; readers have a ceiling on bytes and ranges | P0 |
| BL19 | The corpus for tags, MP3 and the tree, and the scanner run over all of it | P0 |
| BL20 | The generator and the heavy test over fifty thousand files | P0 |
| BL21 | The disk runs with the scan as the load | P0 |
| BL22 | `library` commands in `blackwing`: add, remove, status, find, play, override | P0 |
| BL23 | The background pass, and an MP3's true length as its first job | P1 |
| BL24 | A move seen as a removal and an addition keeps the track and its overrides | P1 |
| BL25 | Art found and recorded: inside the tags, and beside the files | P1 |
| BL26 | Sort keys: case folded, marks stripped | P1 |
| BL27 | Module pages for both modules; numbers in `docs/design/slice-3-notes.md` | P1 |

## Milestones

Each leaves the tree building and its tests passing.

1. **MP3** (BL1, BL2, and its part of BL19). In `hifi_codec` and `hifi` alone. The
   player plays an MP3 named on the command line. Independent of everything below.
2. **Tags** (BL3 to BL6, and their part of BL19). `blackwing_tags` as pure functions,
   tested on bytes with no file system and no index.
3. **The walk and the index** (BL8, BL9, BL12, BL14, BL22's first commands). A root,
   the names pass, sqlite, rows by folder. A file found by name plays. This is where
   the three-second criterion is first met.
4. **The tags pass and the slot** (BL10, BL11, BL18). Fields in the index, the scan's
   priority, the slot, the glue with the player. The disk runs under a scan.
5. **Queries** (BL13). The views, windows, search, cancelling, following the index.
   The fifty thousand, measured.
6. **Overrides** (BL7). Written, read, ranked, surviving a rebuild.
7. **Changes** (BL15 to BL17, BL24). The watcher, the walk at start-up, missing,
   offline, moves.
8. **Close** (BL19 to BL27, what is left of them). The corpus whole, a real library,
   the numbers, the pages, the master PRD corrected where this slice proved it wrong.

MP3 is first because it needs nothing from the rest and gives the tag readers their
second kind of file. Tags come before the index so that they are finished as functions
before anything stores what they return.

## Decided

Nov 23, 2026, in review of the first draft. The draft's choices stand:

- **Overrides live in Blackwing's own data, not beside the music.** This answers the
  master PRD's open question, and nothing at all is written under a root in P0.
- **The scan reads below the player's priority**, and playing fills the priority slot
  through glue in `blackwing`, so the player and the library stay strangers.
- **Missing files are kept for a month**, and an emptied root is offline.
- **A query is an open window** over a table, refilled as the index changes.
- **MP3 reports 24 bits.**
- **Cue sheets and thumbnails are not in this slice.**
- **`blackwing_library` stays a module**, which the master PRD left open.

## Open

- **The override file's format.** A small text format of our own, Lua tables, or
  whatever Korvid's settings end up using. It should be something a person can read
  and fix in an editor.
- **Whether an override file beside the music is read** when one is found there, below
  the user's own. It would make a library portable between machines without Blackwing
  ever writing into it.
- **Long paths in the player's events.** `PlayerEnqueue` holds 511 bytes and Linux
  allows 4095. The fix is the player's (a table of paths and an id in the event) and
  touches every face, so it wants deciding before slice 4.
- **What an album is.** Album artist and title, or those and the directory. Two discs
  in two directories are one album; two different pressings with the same name are
  arguably two.
- **Articles in sort keys** ("The", "Les", "Die"), and whose language decides.
- **The numbers**: the head read for tags (64 KB?), a reader's ceilings, the batch
  size and how often open windows refill (four times a second?), the interval for
  polled and offline roots (a minute?), how long a quiet moment is (a second?).
- **inotify's limit.** One watch per directory, 8192 by default on older kernels. Past
  it the root is polled; whether to say so to the user is a face's question.
- **Links.** The engine's stat follows them. A loop must end; whether a link out of
  the root is followed at all is open.
- **Which reference decoder for MP3**, and how close "within a bit" has to be.
- **A real library for the close.** Which one, and how big.
- **Requirement IDs.** `BL`, after slice 1's `BS` and slice 2's `BR`.
