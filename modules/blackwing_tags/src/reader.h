// What the readers share: the file, read through a budget, and the values they write,
// in the memory the caller gave.
#pragma once
#include "korvid/blackwing_tags/tags.h"

namespace kv::tags {

inline u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
inline u32 be24(const u8* p) { return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; }
inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
inline u32 le32(const u8* p) { return ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0]; }
inline u32 syncsafe(const u8* p) { return ((u32)(p[0] & 0x7F) << 21) | ((u32)(p[1] & 0x7F) << 14) | ((u32)(p[2] & 0x7F) << 7) | (p[3] & 0x7F); }

constexpr u32 kMaxValues    = 1024;
constexpr u32 kMaxPerField  = 64;         // values of one field from one source
constexpr u32 kMaxPictures  = 16;
constexpr u32 kMaxItems     = 2048;       // frames in an ID3v2 tag, items in APE, comments, FLAC blocks
constexpr u64 kMaxItemBytes = 256u << 10; // the largest frame or item read (a picture's is stepped over)
constexpr u64 kMaxAsked     = 4u << 20;   // all the bytes one read may ask for
constexpr u32 kScratch      = 1u << 20;   // an unsynchronised ID3v2.2 or 2.3 tag is undone here, up to this

// The file, as the readers see it. get() is the only way in: it answers with the bytes,
// or null and the range to wait for (once, the first missing one), and counts what is
// asked for against the budget.
struct File {
    const SparseSpan& span;
    bool      last = false;
    bool      waiting = false;  // a range is wanted: the read stops and says so
    ByteRange need;
    u64       asked = 0;
    bool      spent = false;    // the budget is gone

    File(const SparseSpan& s, bool l) : span(s), last(l) {}
    u64 size() const { return span.size; }
    // [at, at + len) of the file, or null: past the end, over budget, or not there yet.
    const u8* get(u64 at, u64 len);
};

// Where values go: arrays and text in the caller's memory.
struct Out {
    TagValue*   values = nullptr; u32 nvalues = 0;
    TagPicture* pictures = nullptr; u32 npictures = 0;
    char*       text = nullptr; u32 used = 0, cap = 0;
    u8*         scratch = nullptr;
    u32         sources = 0;
    const char* problem = "";
    char*       problem_buf = nullptr; // kProblem bytes in the caller's memory; none, and problems are not said
    static constexpr u32 kProblem = 160;

    // Says what went wrong, if nothing has yet.
    void fail(const char* fmt, ...);
    // A value, from bytes in one of the encodings. Leading and trailing NULs and spaces
    // are trimmed; an empty value is not kept (except an override's, which means "hide").
    void utf8(TagSource s, TagField f, const u8* p, u64 n);
    void latin1(TagSource s, TagField f, const u8* p, u64 n);
    void utf16(TagSource s, TagField f, const u8* p, u64 n, bool big_endian);
    // "3", "03", "3/12": the number to `num` and the total, if there is one, to `total`.
    void numbers(TagSource s, TagField num, TagField total, const char* text, u32 len);
    void picture(const TagPicture& p);

    // The value being built: begin, add code points, end.
    bool begin();
    void put(u32 cp);
    void end(TagSource s, TagField f, bool keep_empty = false);
    u32  start = 0;
    bool open = false;
};

// The readers. Each returns false when it stopped to wait (f.waiting).
bool read_id3v2(File& f, Out& o, u64 at, u64* length);      // the tag at `at`; its length, header and footer included
bool read_flac(File& f, Out& o, u64 at);                    // metadata blocks after "fLaC" at `at`
bool read_ape(File& f, Out& o, u64 end, u64* length);       // the APEv2 tag whose footer ends at `end`
void read_id3v1(Out& o, const u8* tag);                     // the last 128 bytes, "TAG" first

// A Vorbis comment's or an APE item's key, case-insensitively: its field (Count if none).
// TRACKNUMBER and DISCNUMBER may carry totals, which the caller splits.
TagField field_for_key(const char* key, u32 len);
// "(13)", "13", "(13)Pop", "RX", or a name: genres as names.
void genres(Out& o, TagSource s, const char* text, u32 len);
// An ID3v1 genre's name; null past the list.
const char* id3v1_genre(u32 i);

} // namespace kv::tags
