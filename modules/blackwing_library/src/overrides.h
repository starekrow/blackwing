// The user's overrides: the one thing the library keeps that is not a cache. A small text
// file per directory, in Blackwing's own data, in a tree that mirrors each root by its path:
//
//   <data>/overrides/home/me/Music/Artist/Album/overrides.txt
//
//   # Blackwing's overrides for the files in this directory. ...
//   [03 - Title.flac]
//   title = The Real Title
//   artist =
//
// A [name] is a file of the directory; each line under it a field (blackwing_tags' names:
// title, artist, albumartist, tracknumber ...) and the value that wins over every source.
// An empty value hides the field. A field given twice is two values. Removing a line
// gives the file its own value back. Nothing is ever written under a root.
#pragma once
#include "lib.h"
#include "korvid/blackwing_tags/tags.h"

namespace kv::lib {

struct OverrideFile {
    struct Entry { u32 file; u8 field; u32 value; };
    Strings      s;
    Array<Entry> entries;
    void clear() { s.clear(); entries.clear(); }
    void free() { s.free(); entries.free(); }
};

// Where the overrides for a directory of a root are kept.
void override_path(char* out, u32 cap, const char* data_dir, const char* root_path, const char* rel_dir);

// Reads one. False, with a line saying why in `problem`, when it will not parse; a file
// that is not there is an empty one.
bool override_load(const char* path, OverrideFile* out, char* problem, u32 cap);
// Writes one whole, under a temporary name and renamed, making its directories.
bool override_save(const char* path, const OverrideFile& f);
// Puts a file that will not parse aside, under a new name that nothing will write over.
void override_set_aside(const char* path, const char* problem);

// `field` of `file` is `value` (one value, in place of any it had); or, with `clear`, it
// has no override at all.
void override_set(OverrideFile& f, const char* file, TagField field, const char* value, bool clear);
// The overrides of `file`, as values of TagSource::Override (pointing into `f`).
u32  override_values(const OverrideFile& f, const char* file, TagValue* out, u32 cap);

// The merge of every source's values: what a track's own columns say.
struct Merged {
    const char* title = ""; const char* artist = ""; const char* album = ""; const char* albumartist = "";
    const char* date = ""; const char* genre = "";
    const char* titlesort = ""; const char* artistsort = ""; const char* albumsort = ""; const char* albumartistsort = "";
    u32 trackno = 0, discno = 0;
};
void merge(Span<const TagValue> values, Merged* out);

} // namespace kv::lib
