// Blackwing tags: what a file's bytes say about it, from every source, ranked. Tags are
// functions of bytes: a reader is handed a file as a sparse span (hifi_codec/sparse.h:
// its address range, and the pieces of it that are there) and memory to work in, and
// returns the fields it found, or the range it needs next. It opens nothing, allocates
// nothing and never asks for a file whole: the art in the middle of a tag is stepped over
// by its size, not read.
//
//   u8* memory = ...;                                   // kTagReadMemory bytes
//   TagRead r;
//   while ((r = tags_read(file, Span<u8>(memory, kTagReadMemory))).status == TagStatus::Wait)
//       ... read r.need into the span, then call again (it starts again from nothing)
//   for (const TagValue& v : r.values) ... v.field, v.source, v.text (UTF-8)
//   TagMerge m; tags_merge(r.values, &m);               // which source each field comes from
//
// Every source's values are kept, so the ranking can change without reading a file
// again: tags_merge is a function of the values. tags_from_path guesses the lowest-ranked
// source from a file's path; overrides (the user's, the best) come from the library.
// modules/blackwing_tags/README.md has the rest.
#pragma once
#include "korvid/core/types.h"
#include "korvid/hifi_codec/sparse.h"

namespace kv {

// The fields. A field may have several values (two artists, three genres); all are kept.
enum class TagField : u8 {
    Title, Artist, Album, AlbumArtist,
    TrackNumber, TrackTotal, DiscNumber, DiscTotal, // the digits, without leading zeros ("3", not "03/12")
    Date,                                           // as the tag has it: "2003", "2003-05-01"
    Genre, Composer,
    Compilation,                                    // "1" when the tag says it is one
    TitleSort, ArtistSort, AlbumSort, AlbumArtistSort, ComposerSort,
    // Kept, not yet used.
    TrackGain, TrackPeak, AlbumGain, AlbumPeak,     // ReplayGain, as written ("-6.54 dB", "0.988")
    MbTrackId, MbAlbumId, MbArtistId, MbAlbumArtistId, MbReleaseGroupId, // MusicBrainz ids
    Count
};

// Where a value came from, best first. Two sources can share a rank (a FLAC's Vorbis
// comments and an ID3v2 tag in front of it); between them the one earlier here wins.
enum class TagSource : u8 {
    Override, // the user's: rank 1, from the library's override files
    Vorbis,   // rank 2: a FLAC's metadata blocks
    Id3v2,    // rank 2: at the front of the file (or with a footer, at the back)
    Ape,      // rank 3: APEv2, at the back
    Id3v1,    // rank 4: the last 128 bytes
    Path,     // rank 5: guessed from the name and its parent directories
    Count
};

// One value. `text` is UTF-8, NUL-terminated, `len` bytes before the NUL: whatever its
// source's encoding was, and with anything that could not be decoded replaced by U+FFFD,
// never dropped whole. It lives in the memory given to the call that made it.
struct TagValue {
    TagField    field  = TagField::Count;
    TagSource   source = TagSource::Count;
    u32         len    = 0;
    const char* text   = "";
};

// A picture inside a tag: found, not decoded. Its bytes are [at, at + len) of the file.
// (One stored with ID3v2's unsynchronisation is not recorded: its bytes are not the picture's.)
struct TagPicture {
    TagSource source = TagSource::Count;
    u8        kind   = 0;     // ID3v2's and FLAC's picture type: 3 the front cover, 0 other
    u64       at = 0, len = 0;
    char      mime[32] = "";  // as the tag says: "image/jpeg"; ID3v2.2's three letters ("JPG")
};

enum class TagStatus : u8 {
    Done, // `values` is everything that could be read
    Wait, // `need` must be there; then call again
    Count
};

// tags_read's and tags_from_path's answer. Everything it points at is in the memory given.
struct TagRead {
    TagStatus              status = TagStatus::Done;
    ByteRange              need;     // with Wait
    Span<const TagValue>   values;   // every source's, in the order found
    Span<const TagPicture> pictures;
    u32                    sources = 0;   // a bit per TagSource found (1 << source), whether or not it had values
    const char*            problem = "";  // the first thing that could not be read, in words; "" when nothing
};

// The memory tags_read and tags_from_path want: values, text, and room to undo ID3v2's
// unsynchronisation.
constexpr u32 kTagReadMemory = 3u << 19; // 1.5 MB

// Reads every tag in `file`: ID3v2 at the front (and at the back, with a footer), a FLAC's
// Vorbis comments and pictures, APEv2 and ID3v1 at the back. Its first and last few
// kilobytes are usually all it needs; it asks for the rest a range at a time, and is
// called again from the start each time. With `last`, no more bytes will come: what can
// be read is, and the problem says what could not. A file with no tags is Done with no
// values. Ceilings, so no file can make it read for ever: 2048 frames or items in a tag,
// 256 KB in one, 4 MB asked for in all, 1024 values, 64 of one field from one source.
TagRead tags_read(const SparseSpan& file, Span<u8> memory, bool last = false);

// Guesses from `path`, a file's path within its root with '/' between names (any bytes:
// what is not UTF-8 becomes U+FFFD): its name and its parent directories. The common
// layouts: "Artist/Album/03 - Title.flac", "Artist - Album/03 Title.mp3", "Artist/Album
// (2003)/CD2/1-03 Title.flac", "Artist - Title.mp3", a bare name. Always Done.
TagRead tags_from_path(const char* path, Span<u8> memory);

// Which source each field is taken from: the best-ranked source with a value for it.
// TagSource::Count when none has one, or when the user's override for it is empty (an
// override with an empty value hides a field).
struct TagMerge {
    TagSource source[(u32)TagField::Count];
};
void tags_merge(Span<const TagValue> values, TagMerge* out);

// The first value of `field` from `source` in `values`, or null.
const TagValue* tags_find(Span<const TagValue> values, TagField field, TagSource source);

// A field's name, as override files and the index write it ("title", "albumartist",
// "tracknumber"); and back, case-insensitively. False for a name that is not a field.
const char* tag_field_name(TagField f);
bool        tag_field_from_name(const char* name, TagField* out);
// A source's name ("override", "vorbis", "id3v2", "ape", "id3v1", "path") and its rank (1 to 5).
const char* tag_source_name(TagSource s);
u32         tag_source_rank(TagSource s);

} // namespace kv
