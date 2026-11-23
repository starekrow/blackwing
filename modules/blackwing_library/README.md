# blackwing_library

The library: you add a directory and that is the whole of setting it up. Each root is
walked a directory at a time, off the main thread, and every audio file in it is a row
in an index as soon as its directory is listed, playable, with a title, artist and album
guessed from its path. Queries are windows kept open over the index, answered off the main
thread. Its API is events and tables, as the player's is.

**Status:** slice 3, milestone 4: roots, the names pass, the tags pass, the priority slot,
the index, windows by folder, of every track and by search. More views and sorting,
overrides and watching are the milestones after it.

## How it works

**Five threads.**

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
span (the decoders' kind) of 64 KB pieces: its head and tail are asked for at once, then
whatever range a reader needs. The main thread puts the reads into the I/O queue at
`Low`, below the player's, at most 48 at once, within the queue's own budget, so a
playing track's pieces go first and a read under way is not stopped. `blackwing_tags`
reads every tag; the decoder that would play the file reads its rate, channels, depth and
length from the same pieces (its probe is shown the bytes past any ID3v2 tag, so a tag of
megabytes of art is not read for it). After eight waits beyond its first reads, a track
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
(which steps over it when it comes to it), and its tracks join the slot's as they become
rows. When the slot's work is done, the tagger goes back to where it was. Playing a file
prioritizes it: `blackwing` hears `PlayerEnqueue` and says so to the library.

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
or search text, and a range of rows. The reader answers the newest ask for each window
(an older one not yet answered is dropped) into `library.rows`, and `LibraryResult` says
it is there. While a scan commits, open windows are asked again, at most four times a
second, so they follow the index.

| view | what |
|---|---|
| `Folders` | a directory's subdirectories by name, then its tracks by name; `parent` 0 is the roots |
| `Tracks` | every track, in root and path order |
| `Search` | tracks with words beginning with each word of `text`, in title, artist, album or path, best first; case and marks do not matter ("cafe" finds "Café") |

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
| `LibraryQuery { id, view, parent, text, first, count }` | event in | opens or changes window `id` |
| `LibraryClose { id }` | event in | its rows leave the table |
| `LibraryPrioritize { path, dir, query, count, tracks }` | event in | the slot: scanned next |
| `LibraryResult { id, total, first, count, generation }` | event out | the window's rows are in `library.rows` |
| `LibraryChanged { generation }` | event out | the index has moved on |
| `LibraryScanFinished { root, tracks, seconds }` | event out | a root's scan is over: walked (in `seconds`) and every track read |
| `LibraryView` | enum | `Folders`, `Tracks`, `Search` |
| `LibraryRoot` | component | a row of `library.roots`: `id`, `state`, `dirs`, `tracks`, `failed`, `unread`, `path`, `reason` |
| `LibraryRootState` | enum | `Walking`, `Reading`, `Idle`, `Offline` |
| `LibraryRow` | component | a row of `library.rows`: `query`, `index`, `id`, `kind`, names, numbers, `path`, `reason` |
| `LibraryScan` | resource | `library.scan`: `active`, `rebuilt`, `walking`, `generation`, `tracks`, counts, `tags_read`, `unread`, `names_per_second`, `tags_per_second`, `slot`, `dir` |
| `kLibraryMaxWindow`, `kLibraryMaxRows` | constants | 512 rows a window, 4096 in all |
| `LibraryConfig`, `library_configure` | record, function | `data_dir`, before `engine_init` |
| `library_scan(e)` | function | the resource, for code outside the schedule |

Header `korvid/blackwing_library/module.h`: `blackwing_library_module`, the record.

## Behaviour worth knowing

- **What comes back arrives in a later tick.** A window is answered on the reader's thread;
  `LibraryResult` comes in the tick after the answer.
- **The walk is fast, and the index follows it.** On the built-in disk fifty thousand files
  are listed in about 0.5 s and are rows within about 1.8 s; their tags are read at about
  5,500 files a second with the cache warm, and in 16 s for all of them with it cold. A
  track is findable once its batch is committed.
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
by text is made again from the roots; a root that is not there is offline. Then the slot: eight thousand files, and the
directory of an album the walk comes to near its end prioritized by its path as the scan
starts; its track is seen read with more than 2,000 still to read (about 5,500, typically).
Clean under ASan, UBSan and TSan.

`kormake test blackwing_library --heavy [-- DIR]` makes fifty thousand files (once, under
`build/library-heavy` unless a directory is given) and measures, from `LibraryAddRoot` on
an empty index, when two chosen tracks are found by name and heard through the player; a
folder of twenty prioritized as the tags pass starts, and how soon its tags are in; the
walk and the tags pass; the index's size; the second start; and the disk run: the files
out of the page cache and a long track played from the same disk through a whole scan,
with no underrun.

## Not in this module

The readers themselves (`blackwing_tags`). The player (`blackwing`
glues them: `library play`). Any face.

## See also

- [Slice 3](../../docs/PRDs/Blackwing-Slice-3-PRD.md): the plan this module follows.
- [blackwing_tags](../blackwing_tags/README.md): the readers and the guess from a path.
- [src/vendor](src/vendor/README.md): where sqlite came from.
