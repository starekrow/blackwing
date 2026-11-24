# blackwing_library

The library: you add a directory and that is the whole of setting it up. Each root is
walked a directory at a time, off the main thread, and every audio file in it is a row
in an index as soon as its directory is listed, playable, with a title, artist and album
guessed from its path. Queries are windows kept open over the index, answered off the main
thread. Its API is events and tables, as the player's is.

**Status:** slice 3, milestone 7: roots, the names pass, the tags pass, the priority slot,
the index, windows over every view, the user's overrides, and changes: the watcher,
missing files, moves, roots that are polled and roots that go offline.

## How it works

**Six threads**: the five below, and a watch thread with the engine's `FsWatcher` (see
Changes).

```
main thread (library.pump)      scan thread        tagger thread          writer thread         reader thread
  commands in; tables, events     walks, a dir       reads tags and         the index's one       read-only: windows,
  out; the tagger's I/O           at a time          audio, sparse spans    writing connection    newest first
     +-- roots, slot dirs ------->|                       |                      |                     |
     +-- roots, the slot ------------------------------------------------------>|                     |
     |                            +-- rows ---------------------------------->| commits, 4 a second |
     |                            |                       |<-- tracks to read --+                     |
     |<-- ranges to read -------------------------------- +                      |                     |
     +-- bytes ------------------------------------------>|-- what was found -->|                     |
     |<--------------------------------------------- counts, generation --------+                     |
     +-- windows asked ------------------------------------------------------------------------------>|
     |<-------------------------------------------------------------------------------- rows --------+
```

They speak through inboxes under one mutex, each with its own condition variable; nobody
holds the mutex while it works (the writer counts before it takes the lock to say so). The
main thread never touches the index, and touches the disk only by putting the tagger's
reads into the engine's I/O queue and taking them out.

**Roots and paths.** A root is a directory, named by an id that lasts for its life,
across runs. A file is named by its root and its path within it, never by an absolute
path. The list of roots is the one thing here that is not a cache: `roots.txt` beside the
index (an id, a tab and a path a line), written whole under a temporary name and renamed.

**The names pass.** `io_list_dir` lists one directory, with sizes and times, and the walk
is breadth first: each directory's subdirectories join the end of the walk, and its audio
files (by extension: `.flac`, `.mp3`, in any case) become rows with a guess from their
paths (`blackwing_tags`' `tags_from_path`: title, artist, album, track and disc numbers).
Rows go to the writer in batches of up to 2000 records, or every tenth of a second. A
directory appears in its parent's listing as soon as the parent is listed, before it is
itself. Hidden entries are skipped, and links to directories are not followed, so a walk
cannot loop or leave its root. A directory that cannot be listed is marked, with the reason
in words, and the walk goes on; a root that cannot be listed is offline.

**The tags pass.** The writer knows which tracks want reading: a new row, one whose size
or time differ from the last look, or one never read at its size and time (an earlier run
stopped first). It hands them to the tagger in the order the walk found them, after the
commit that made them rows. The tagger keeps up to 24 tracks under way. Each is a sparse
span (the decoders' kind) of 16 KB pieces: its first 64 KB and last 16 KB are asked for at
once (a FLAC's first only: its tags are at the front), then whatever range a reader
needs. The main thread puts the reads into the I/O queue at
`Low`, below the player's, at most 48 at once, within the queue's own budget, so a
playing track's pieces go first and a read under way is not stopped. `blackwing_tags`
reads every tag; the decoder that would play the file reads its rate, channels, depth and
length from the same pieces (its probe is shown the bytes past any ID3v2 tags, several in
a row, and the zeros after them that they did not count, so a tag of megabytes of art is
not read for it). After eight waits beyond its first reads, a track
is what could be read of it. What is found goes to the writer every tenth of a second:
every source's values (the guess from the path among them) into `tagvals`, the merge into
the track's own columns and the search table, the audio, and a reason when something
could not be read. The walk's rows go in a commit of their own before the tags', so a file
is findable by name as soon as it can be.

**The priority slot.** `LibraryPrioritize` names what someone is looking at: a file or
directory by its OS path, a directory by id, an open window, or tracks by id. There is
one slot, and a newer call replaces it. The writer resolves it: the tracks that want
reading go to the tagger as the slot's, ahead of the rest, with their reads at `Normal`; a
directory the walk has not listed is listed by the scan thread now, ahead of the walk
(which steps over it when it comes to it), or as soon as its root's walk begins, and its
tracks join the slot's as they become rows. When the slot's work is done, the tagger goes back to where it was. Playing a file
prioritizes it: `blackwing` hears `PlayerEnqueue` and says so to the library.

**Overrides.** The user's corrections are the one thing here that is not a cache. They
live in Blackwing's own data, one small text file per directory, in a tree that mirrors
each root by its path (`<data>/overrides/home/me/Music/Artist/Album/overrides.txt`), so
nothing is ever written under a root and a root removed and added again keeps them:

```
# Blackwing's overrides for the files in this directory. ...
[03 - Title.flac]
title = The Real Title
album =
```

A `[name]` is a file of the directory; each line under it a field (blackwing_tags' names)
and the value that wins over every source. An empty value hides the field; a field given
twice is two values; removing a line gives the file its own value back. `LibraryOverride`
sets or clears one: the writer reads the directory's file, changes the line, writes the
file whole under a temporary name and renames it, and merges the track again from the
other sources' values it keeps. Every read of a track's tags merges its directory's
overrides, so they come back when the index is deleted and made again. At start-up (and
when a root is added) each of the root's override files whose time differs from when it
was last taken in is read again and its directory's tracks merged again: a file edited by
hand while the program was not running counts. A file that will not parse is renamed
aside (`overrides.txt.broken-<time>`), never written over, with a warning naming the line,
and its overrides are not used.

**Changes.** Each root that is not offline is watched with the engine's `FsWatcher`, on
a thread of its own, and so is the overrides tree. Its events are hints: a path it names is
looked at again once it has been quiet for a moment (a second; a file being written
changes many times), and what is there is believed. A file there is a row with its size and
time (read again if they changed); a file not there is gone; a directory gone takes what
was under it; lost events are a walk of the root; an override file changed is taken in.
Every root is also walked at start-up, since things change while the program is not
running, and a walk marks what it did not see.

- **A file that goes is missing, not deleted.** Its row, its values and its overrides are
  kept, it leaves every view and the search, and it comes back as itself if the file does.
  Missing for a month, it is forgotten (at start-up).
- **A move keeps the track.** A new path whose name, size and time are those of exactly one
  missing track of the root is that track moved: it keeps its id, its values, and its
  overrides, which move from one directory's file to the other's. A rename to another name
  is a new track (and the old one missing).
- **A root that cannot be trusted to report is polled**: one on a share (`FsWatcher` polls
  it under `Auto`, KR12), one the OS will not watch (its watches run out; it is added again
  with `Poll`), or every root with `poll_roots`; every `poll_seconds` (a minute).
- **A root that goes away is offline, not forgotten**: its directory is gone or cannot be
  listed, or it is empty where it held tracks (an unplugged disk leaves its mount point
  so; with KR13's volume the reason says whether it is another disk). Nothing under it is
  marked missing; its rows stay in every view with `offline` set. Every root is looked at
  again every `offline_seconds` (a minute): an offline one that has something in it is
  walked again (and nothing is read again that did not change), and one that is empty is
  walked, and the walk says offline. Removals the watcher reports from a root that is now
  empty are not believed file by file for the same reason.

**The index** is sqlite (vendored, built as C), in WAL mode, at `<data>/index.sqlite`,
with tables for the roots, the directories, the tracks (with the merge of their tags and
their audio), every source's values (`tagvals`), and an FTS5 table over the tracks' title,
artist, album and path. Paths are kept as bytes (a name that is not UTF-8
still opens the file); names are kept as UTF-8 with U+FFFD for what is not. The writer
takes the batches as they come, waits up to a quarter of a second for more so a commit
carries several, and each commit is a new generation. A file of another schema version,
or one sqlite calls not a database or corrupt, is deleted and made again with a warning in
the log, and every root walked into it again; `library.scan`'s `rebuilt` says so.

**Windows.** `LibraryQuery` opens window `id` or changes what it shows: a view, a parent
or search text, an order, and a range of rows. The reader answers the newest ask for each
window into `library.rows`, and `LibraryResult` says it is there and how long the index
took. A newer ask for a window takes the place of an older one: one not yet begun is
dropped, and one being answered is stopped inside sqlite (a progress handler sees the
newer ask and interrupts). While a scan commits, open windows are asked again, at most four
times a second, so they follow the index.

| view | `parent` | what |
|---|---|---|
| `Folders` | a directory (0: the roots) | its subdirectories by name, then its tracks by name |
| `Tracks` | 0, or a genre | every track (a genre's), by artist, album, disc and number |
| `Search` | | tracks with words beginning with each word of `text`, in title, artist, album or path, best first; case and marks do not matter ("cafe" finds "Café") |
| `Artists` | | album artists (the artist where there is none), with their tracks and albums |
| `Albums` | 0, or an artist | albums by title, with their artist ("Various Artists" when their tracks differ), tracks and earliest date |
| `AlbumTracks` | an album | its tracks by disc and number |
| `Genres` | | genres, with their tracks |

An album is its album artist and title, or, with no album artist, its title in its
directory; an artist is the album artist, or the artist. Neither is stored: they are
groupings of tracks, made when asked, and named by a hash of their keys (an Artists or
Albums row's `id`, which an Albums or AlbumTracks query takes as `parent`). The track views
take `LibrarySort`: by title, artist, album, date or path. Names sort by a key with their
case folded and their marks stripped (Latin, Greek, Cyrillic; other scripts by code
point), so "Ångström" sorts beside "angle"; leading articles are not moved.

**Every window is a walk of an index.** Each order a track window takes has an index that
is that order to its last column, and a window's ids are read from it first (the offset
skips index entries, not rows), then its rows by id. A grouping is made whole over an
index that covers it, and a search's matches are put in order whole; either is kept by the
reader for as long as the index's generation lasts, so every window of it after the first
is a slice.

A row carries what is needed to play it: `path` is the root's path and the file's within
it, which `PlayerEnqueue` takes (empty, with a reason, when it is longer than that holds).

## Using it

```cpp
LibraryConfig lc;
lc.data_dir = "/where/the/index/goes";   // or null: the per-user data directory, library/
library_configure(lc);                   // before engine_init

e.events.emit(LibraryAddRoot{"/music"});
LibraryQuery q;
q.id = 1; q.view = LibraryView::Search; q.count = 50;
format(q.text, sizeof q.text, "blue night");
e.events.emit(q);
// ... on LibraryResult{1, total, first, count}: the rows of library.rows whose query is 1
```

The module's commands: `library add DIR...`, `library remove ROOT`, `library status`.
`blackwing` adds `library find WORDS...` and `library play N`, the glue between the
library and the player.

## Quick reference

Header `korvid/blackwing_library/library.h`:

| name | kind | what |
|---|---|---|
| `LibraryAddRoot { path }`, `LibraryRemoveRoot { root }` | events in | a directory in, a root out |
| `LibraryQuery { id, view, sort, parent, text, first, count }` | event in | opens or changes window `id` |
| `LibraryClose { id }` | event in | its rows leave the table |
| `LibraryPrioritize { path, dir, query, count, tracks }` | event in | the slot: scanned next |
| `LibraryOverride { track, field, value, clear }` | event in | the user's value for a field of a track, kept in Blackwing's data |
| `LibraryResult { id, total, first, count, generation, seconds }` | event out | the window's rows are in `library.rows`; how long the index took |
| `LibraryChanged { generation }` | event out | the index has moved on |
| `LibraryScanFinished { root, tracks, seconds }` | event out | a root's scan is over: walked (in `seconds`) and every track read |
| `LibraryView` | enum | `Folders`, `Tracks`, `Search`, `Artists`, `Albums`, `AlbumTracks`, `Genres` |
| `LibrarySort` | enum | `Default`, `Title`, `Artist`, `Album`, `Date`, `Path` |
| `LibraryRoot` | component | a row of `library.roots`: `id`, `state`, `dirs`, `tracks`, `failed`, `unread`, `path`, `reason` |
| `LibraryRootState` | enum | `Walking`, `Reading`, `Idle`, `Offline` |
| `LibraryRow` | component | a row of `library.rows`: `query`, `index`, `id`, `kind` (directory, track, artist, album, genre), names, numbers, `count`, `albums`, `date`, `path`, `reason` |
| `LibraryScan` | resource | `library.scan`: `active`, `rebuilt`, `walking`, `generation`, `tracks`, counts, `tags_read`, `unread`, `cancelled`, `names_per_second`, `tags_per_second`, `slot`, `dir` |
| `kLibraryMaxWindow`, `kLibraryMaxRows` | constants | 512 rows a window, 4096 in all |
| `LibraryConfig`, `library_configure` | record, function | `data_dir`, `quiet_seconds` (1), `offline_seconds` (60), `poll_seconds` (60), `poll_roots`; before `engine_init` |
| `library_scan(e)` | function | the resource, for code outside the schedule |

Header `korvid/blackwing_library/module.h`: `blackwing_library_module`, the record.

## Behaviour worth knowing

- **What comes back arrives in a later tick.** A window is answered on the reader's thread;
  `LibraryResult` comes in the tick after the answer.
- **The walk is fast, and the index follows it.** On the built-in disk fifty thousand files
  are listed in about 0.5 s and are rows within about 1.8 s; their tags are read at about
  5,500 files a second with the cache warm, and in 16 s for all of them with it cold. A
  track is findable once its batch is committed.
- **A window of 100 anywhere in fifty thousand rows takes the index 0 to 11 ms**, in every
  view and order; the first window of a grouping or a search makes it whole (artists 8 ms,
  albums 10 ms), and the next ones are slices. A search that matches every track (a word in
  all fifty thousand titles) takes 35 ms to rank them for its first window.
- **An override is seen from the next generation**: in every view, in search (by the new
  value, and still by the path), and in the groupings. The audio file is not touched, nor
  its time.
- **An override file is read whole**, up to 1 MB; it is one directory's.
- **A change on a local disk is in the index within a second or so**: a file added or
  changed 0.6 s, moved 0.4 s, removed 0.6 s, a directory removed 0.35 s, with the quiet
  moment at 0.3 s (in the tests); with the default second, add 0.7 s.
- **A root is watched, then walked**, so a file made between the walk's listing of its
  directory and that directory's watch is not missed. Watching a large root costs its first
  walk that time (fifty thousand files: found by name at 2.5 s instead of 1.6).
- **The watcher's reports through a link to a directory are dropped**: the engine's watcher
  follows such links (KR16), and the walk does not, so a file seen through one is not the
  root's.
- **What is kept goes with the generation**: while a scan commits, an open grouping or
  search is made again with each refill.
- **A root is walking, then reading, then idle**, and `LibraryScanFinished` comes when it is
  idle: walked and every track read. A track that could not be read is read, with its
  reason.
- **A file whose size and time are unchanged is not read again**, at the next start or the
  next walk.
- **Every root is walked at start-up**, since things change while the program is not
  running. A file whose size and time are unchanged keeps its row and its id.
- **Links to directories are not followed**; links to files are listed, as their targets.
- **A root is offline when its directory cannot be listed**; its rows stay. (That a root
  emptied is offline too is milestone 7's.)
- **Removing a root deletes its rows from the index**; its id is not used again.
- **The index is a cache.** Deleting it costs a walk of every root.

## Tests

`kormake test blackwing_library` runs the module in a real engine over a tree made in a
temporary directory: sixty generated FLAC files in four layouts and names in several
scripts, beside a text file, an upper-case `.MP3`, a hidden file, a name that is not UTF-8,
a link back to the root, and a directory that may not be read. The walk lists every audio
file (and not the rest), marks the locked directory, and does not follow the link; the
roots, a root's directories and a directory's rows come back in order; a title is found by
a word in it, by a word in Cyrillic, and by "cafe" for "Café"; a window of every track
from row 10; a closed window's rows go. Every track's tags are read: the title is the
Vorbis comment's, the rate and length the decoder's; a one-byte `.MP3` is a row with "too
short to be audio". The next start finds the roots, walks them again, reads no file again,
and keeps every id; a second root, a root added twice, a root removed; the index replaced
by text is made again from the roots; a root that is not there is offline. The groupings: artists with their tracks and albums, an artist's
albums, an album's tracks by number, the genres and a genre's tracks; five titles in one
album sorted by title as "angle", "Ångström", "apple", "Été", "Zebra". Then the slot: eight thousand files, and the
directory of an album the walk comes to near its end prioritized by its path as the scan
starts; its track is seen read with more than 2,000 still to read (about 5,500, typically).
Then overrides: a title set and an album hidden show in search and
in the albums; the file in Blackwing's data says so in words; the audio file's bytes and
time are unchanged; the index deleted and made again keeps them; the file edited by hand
while stopped is taken in at the next start; the root removed and added again keeps
them; a cleared title is the file's own again; a file that will not parse is set aside.
Then changes, with the quiet moment at 0.3 s: a file added,
its title changed, moved with an override on it (the same id and the override after), removed
and put back (the same id), a directory removed, each within two seconds; a file removed and
one added while stopped, found by the start-up walk; the root emptied (offline, "empty", its
rows still found and marked offline) and filled again (idle, nothing read again); and a root
polled every half second finding a new file. Then a wide search replaced at once by a narrow one for the same window, forty times over
four thousand files: only the narrow one is ever shown (and nearly every wide one is
stopped in sqlite). Last, the corpus, scanned whole: the codec's good and broken files,
three hundred spoiled copies of a FLAC and an MP3, hostile tags as files (a field a
thousand times, text not in its encoding, a hundred megabytes of art in a sparse file; the
rest of the hostile tags are `blackwing_tags`' own tests, on bytes), and the odd tree (a path
past a thousand bytes, ten thousand files in one directory, a link that loops, a locked
directory, a name that is not UTF-8, a file written while it is read). Every one of its
10,351 audio files is one row, with its tags or a reason; 12 MB is read of it, the art not
at all; the scan takes about four seconds.
Clean under ASan, UBSan and TSan.

`kormake test blackwing_library --heavy [-- DIR [DATA]]` makes fifty thousand files (once,
under `build/library-heavy` unless a directory is given; the index under `DIR/data`, or
`DATA`) and measures, from `LibraryAddRoot` on
an empty index, when two chosen tracks are found by name and heard through the player; a
folder of twenty prioritized as the tags pass starts, and how soon its tags are in; the
walk and the tags pass; the index's size; the second start; windows of 100 at the start,
middle and end of every view and order; and the disk run: the files
out of the page cache and a long track played from the same disk through a whole scan,
with no underrun. `-- --real MUSIC DATA` instead reads a real library, read only (its
files out of the page cache first, the index in `DATA`), and says how long the walk and
the reads took, how much was read, and why any file has no audio.

## Not in this module

The readers themselves (`blackwing_tags`). The player (`blackwing`
glues them: `library play`). Any face.

## See also

- [Slice 3](../../docs/PRDs/Blackwing-Slice-3-PRD.md): the plan this module follows.
- [blackwing_tags](../blackwing_tags/README.md): the readers and the guess from a path.
- [src/vendor](src/vendor/README.md): where sqlite came from.
