# blackwing_tags — the script surface

Surface `tags`, version 0.1.0 · draft, with [the spec](../../docs/specs/script-modules.md)

What a script module sees of the tags: the fields and sources by name, the ranking, the
guess from a path, and the reader itself for a state that holds a file. Tags are
functions of bytes, so everything here is a pure function: it opens nothing and changes
nothing. What each does is in `korvid/blackwing_tags/tags.h`; the model is in
[blackwing_tags' page](README.md).

No capability. A script changes what a track is called with `LibraryOverride`
([the library's surface](../blackwing_library/SCRIPT.md)), never here.

```lua
local guess = kv.tags.from_path("Artist/Album (2003)/03 - Title.flac")
for _, v in ipairs(guess.values) do
  print(kv.tags.field_name(v.field), v.text)          -- "artist", "Artist" ...
end
```

## `kv.tags`

| call | native | what |
|---|---|---|
| `kv.tags.field_name(field)` | `tag_field_name` | a field's name, as override files and the index write it: `"title"`, `"albumartist"`, `"tracknumber"` |
| `kv.tags.field_from_name(name)` | `tag_field_from_name` | the field, case-insensitively; `nil` for a name that is not one. What `LibraryOverride.field` takes |
| `kv.tags.source_name(source)` | `tag_source_name` | `"override"`, `"vorbis"`, `"id3v2"`, `"ape"`, `"id3v1"`, `"path"` |
| `kv.tags.source_rank(source)` | `tag_source_rank` | 1 to 5, best first |
| `kv.tags.from_path(path)` | `tags_from_path` | guesses from a file's path within its root, `/` between names. A `TagRead`, always `Done` |
| `kv.tags.merge(values)` | `tags_merge` | which source each field is taken from: a table from field to source, with no entry where no source has a value or the user's override hides it |
| `kv.tags.find(values, field, source)` | `tags_find` | the first value of `field` from `source`, or `nil` |
| `kv.tags.read(file, last, tail)` | `tags_read` | reads every tag in a file. In a decoder's state only: nothing else holds a file |

`from_path` takes a string and returns a guess. It does not touch the file system, and a
path is not opened because it was parsed.

`kv.tags.read` works in memory of the host's, not the state's, and its ceilings are the
native ones: 2048 frames or items in a tag, 256 KB in one, 4 MB asked for in all, 1024
values. When it returns `Wait`, the decoder returns that `need` and reads again once the
bytes have come.

## Records

As tables with the native records' field names.

| record | fields |
|---|---|
| `TagRead` | `status`, a `TagStatus`; `need = { at, len }` with `Wait`; `values`, a list of `TagValue`, every source's, in the order found; `pictures`, a list of `TagPicture`; `sources`, an integer with a bit per `TagSource` found; `problem`, the first thing that could not be read, `""` when nothing |
| `TagValue` | `field`, a `TagField`; `source`, a `TagSource`; `text`, a UTF-8 string, with U+FFFD for what could not be decoded |
| `TagPicture` | `source`; `kind` (3: the front cover, 0: other); `at`, `len`: its bytes in the file; `mime` |

A field may have several values (two artists, three genres); all are kept. A picture is
found, not decoded: a script gets where it is, and reads it with `file:bytes` if it
holds the file.

## Enums

| enum | values |
|---|---|
| `TagField` | `Title` 0, `Artist` 1, `Album` 2, `AlbumArtist` 3, `TrackNumber` 4, `TrackTotal` 5, `DiscNumber` 6, `DiscTotal` 7, `Date` 8, `Genre` 9, `Composer` 10, `Compilation` 11, `TitleSort` 12, `ArtistSort` 13, `AlbumSort` 14, `AlbumArtistSort` 15, `ComposerSort` 16, `TrackGain` 17, `TrackPeak` 18, `AlbumGain` 19, `AlbumPeak` 20, `MbTrackId` 21, `MbAlbumId` 22, `MbArtistId` 23, `MbAlbumArtistId` 24, `MbReleaseGroupId` 25 |
| `TagSource` | `Override` 0 (rank 1), `Vorbis` 1 (2), `Id3v2` 2 (2), `Ape` 3 (3), `Id3v1` 4 (4), `Path` 5 (5) |
| `TagStatus` | `Done` 0, `Wait` 1 |

A script names a field by `kv.enum.TagField.Title`, not by 0: values are added at the
end by minor versions, and a script ignores the ones it does not know.

## Not here

- A track's tags by its id. The library's windows carry what the index holds (`title`,
  `artist`, `album`, `date`, the numbers); every source's values for a track are not
  published yet.
- A tag reader as a contract. The sources are fixed (`TagSource`); a script cannot add
  one.
- Writing tags. Nothing in Blackwing writes into a file in P0.

## What blackwing_tags changes for this

Nothing.
