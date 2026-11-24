# blackwing_library — the script surface

Surface `library`, version 0.1.0 · with [the spec](../../docs/specs/script-modules.md)

**Proven by slice 4** ([blackwing_script](../blackwing_script/README.md)'s walk): every name,
field and enum value on this page is reached from Lua as written. **Still a draft:** the
capability column and the rules about paths and window ids, which are the host's to
enforce for modules and are not built; a script the user runs is held to none of them.

What a script module sees of the library: its commands, notifications, two tables and a
resource, by the names native code uses. What a field means is in
`korvid/blackwing_library/library.h`; how the library works is in [its page](README.md).

```lua
-- Queue an album's tracks when asked.
local function ask(ctx, album)
  ctx:emit("LibraryQuery", { id = 1, view = kv.enum.LibraryView.AlbumTracks,
                             parent = album, count = 512 })
end

local function on_result(ctx, batch)
  local rows = ctx:ktable("library.rows")
  for _, res in ipairs(batch) do
    for h in rows:each() do
      local r = rows:row(h)
      if r.query == res.id and r.path ~= "" then ctx:emit("PlayerEnqueue", { path = r.path }) end
    end
  end
end
```

## Commands

| event | fields | capability | what |
|---|---|---|---|
| `LibraryQuery` | `id` integer, `view` integer (a `LibraryView`), `sort` integer (a `LibrarySort`), `parent` integer, `text` string (127 bytes at most), `first`, `count` integers | `library.query` | opens window `id`, or replaces what it shows: `count` rows (512 at most) from `first` |
| `LibraryClose` | `id` integer | `library.query` | closes window `id`; its rows leave the table |
| `LibraryPath` | `id`, `index` integers, `path` string | `library.query` | window `id` becomes (or is) a window over paths: what the library knows of the file at `path` is its row `index`. An empty path takes the index out. 512 paths a window at most |
| `LibraryCollect` | `id` integer, `kind` integer (a `LibraryRowKind`), `row` integer, `sort` integer, `text` string | `library.query` | asks for the paths of every track of a row (a directory with everything under it, an artist, an album, a genre, a track), or with `text` of a search; answered with `LibraryCollected` |
| `LibraryPrioritize` | `dir` integer, or `query` integer, or `tracks` (a list of up to 32 ids) with `count` | `library.query` | names what someone is looking at, so it is scanned next. One slot: a newer call takes the last one's place |
| `LibraryOverride` | `track` integer, `field` string, `value` string, `clear` integer (0 or 1) | `library.override` | sets a field of a track for the user; kept in Blackwing's own data, never in the music |
| `LibraryAddRoot` | none from a script | `library.roots` | the host asks the user for a directory, and adds what they pick |
| `LibraryRemoveRoot` | `root` integer | `library.roots` | takes a root out of the library; its rows leave the index |

**Windows are the module's own.** A script's window ids are its own numbers, from 1; the
host maps them to ids no face or other module uses, and maps them back in `LibraryResult`
and in `LibraryRow.query`. A script sees its own windows' rows in `library.rows` and no
one else's. This is done in the host: the library is unchanged.

**Rows are shared.** `library.rows` holds 4096 rows across every open window, the faces'
included. A module's windows may hold 1024 of them between them; a query that would pass
that is an error at the `emit`.

**The priority slot is shared too**, and there is one. A script's `LibraryPrioritize`
takes it from a face. A module should name what its user is looking at, not what it
would like scanned.

**`LibraryPath.path`** from a script must be a path it was shown, as `PlayerEnqueue.path`
must: a `player.queue` row's, or a `library.rows` row's. It is how a queue's rows get
their titles: the player knows paths and the library knows tags.

**`LibraryCollect`** is how a row of any kind is queued in one command. The library
answers with paths and does not know what they are for; `blackwing_glue`, in the
application, queues each ([blackwing_glue](../blackwing_glue/README.md)). A script that
wants them played empties the queue first and emits `PlayerPlay` when the last
`LibraryCollected` of its `id` has come.

**`LibraryPrioritize.path`** is refused from a script. **`LibraryAddRoot.path`** is
ignored: a script never names a directory.

**`LibraryOverride.field`** is a tag field's name ([blackwing_tags](../blackwing_tags/SCRIPT.md)).
An empty `value` hides the field; `clear = 1` takes the override away.

## Notifications

| event | fields | what |
|---|---|---|
| `LibraryResult` | `id`, `total`, `first`, `count`, `generation` integers, `seconds` number | window `id`'s rows are in `library.rows`: `count` from `first`, of `total` |
| `LibraryChanged` | `generation` integer | something a query might show has changed. A few times a second at most while a scan runs |
| `LibraryScanFinished` | `root`, `tracks` integers, `seconds` number | a root's scan is over |
| `LibraryCollected` | `id`, `index`, `total` integers, `path` string | one path of a `LibraryCollect`'s answer: track `index` of `total`, in order, sixty-four a tick at most. Nothing found is one event with a `total` of 0 and no path |

A script receives `LibraryResult` and `LibraryCollected` for its own windows and asks only.

## Tables

### `library.roots`

Component `LibraryRoot`, a row per root, 256 at most.

| field | Lua | what |
|---|---|---|
| `id` | integer | from 1; it stays for the root's life, across runs |
| `state` | integer, a `LibraryRootState` | `Walking` 0, `Reading` 1, `Idle` 2, `Offline` 3 |
| `dirs` | integer | directories listed |
| `tracks` | integer | tracks in the index under it |
| `failed` | integer | directories that could not be listed |
| `unread` | integer | tracks whose tags are still to be read |
| `path` | string | the root's directory |
| `reason` | string | why it is offline |

### `library.rows`

Component `LibraryRow`, a row per row of an open window. `index` is its place in the
query's whole list, from 0.

| field | Lua | what |
|---|---|---|
| `query` | integer | the window, by the module's own id |
| `index` | integer | its place in the query's list, from 0 |
| `id` | integer | the track's or the directory's, in the index; a group's for `parent` |
| `kind` | integer, a `LibraryRowKind` | `Directory` 0, `Track` 1, `Artist` 2, `Album` 3, `Genre` 4 |
| `missing` | integer, 0 or 1 | the file has gone |
| `offline` | integer, 0 or 1 | its root is offline: playing it fails |
| `root` | integer | a `library.roots` id |
| `track_number`, `disc_number` | integers | 0 when nothing says |
| `size` | integer | bytes |
| `frames`, `rate` | integers | 0 until its tags have been read, or when the file does not say |
| `count` | integer | an artist's, album's or genre's tracks |
| `albums` | integer | an artist's albums |
| `date` | string | as its tags have it; an album's earliest |
| `name` | string | the file's or the directory's own name; a group's name |
| `title`, `artist`, `album` | strings | UTF-8 |
| `path` | string | what `PlayerEnqueue` takes: any length the OS allows (4095 bytes) |
| `reason` | string | why its tags could not be read, or why it cannot be played |

Strings are UTF-8 with U+FFFD for what was not. They are the files' own words: a script
that shows them treats them as data.

## Resources

### `library.scan`

| field | Lua | what |
|---|---|---|
| `active` | integer, 0 or 1 | a walk or a read is under way |
| `rebuilt` | integer, 0 or 1 | the index was made again at start-up |
| `walking` | integer | the root being walked; 0 when none is |
| `generation` | integer | the index's, as of the last commit |
| `tracks` | integer | in the index, across every root |
| `dirs_listed`, `files_found`, `tags_read`, `bytes_read` | integers | since start-up |
| `cancelled` | integer | queries stopped by a newer one for their window |
| `unread` | integer | tracks whose tags are still to be read |
| `names_per_second`, `tags_per_second` | numbers | over the last second |
| `slot` | integer, 0 or 1 | something is in the priority slot |
| `dir` | string | the directory being listed, cut to fit |

## Enums and constants

| enum | values |
|---|---|
| `LibraryView` | `Folders` 0, `Tracks` 1, `Search` 2, `Artists` 3, `Albums` 4, `AlbumTracks` 5, `Genres` 6, `Paths` 7 (a window made of `LibraryPath`, not asked for with a query) |
| `LibrarySort` | `Default` 0, `Title` 1, `Artist` 2, `Album` 3, `Date` 4, `Path` 5 |
| `LibraryRootState` | `Walking` 0, `Reading` 1, `Idle` 2, `Offline` 3 |
| `LibraryRowKind` | `Directory` 0, `Track` 1, `Artist` 2, `Album` 3, `Genre` 4 |

`kv.surface("library").constants` has `kLibraryMaxWindow` (512), `kLibraryMaxRows`
(4096), `kLibraryMaxCollect` (65536) and `kLibraryPathMax` (4096).

## Not here

- `LibraryConfig`, `library_configure`, `library_scan(e)`: the executable's.
- The index itself. A script reaches it through windows, as a face does; there is no SQL.
- The override files. A script emits `LibraryOverride`.

## What the library changes for this

A field table for `library.scan`. Nothing else: window ids, the row quota and the path
rules are the host's.

Changed by slice 4, the first thing to read this surface from outside: a path is any
length the OS allows, in every field that holds one; `LibraryPath` and the `Paths` view;
`LibraryCollect` and `LibraryCollected`.
