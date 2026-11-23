// The read's frame: memory laid out, the file asked through a budget, the sources looked
// for in their places, values written as UTF-8. And the ranking, and the names.
#include "reader.h"
#include "korvid/core/format.h"
#include "korvid/core/utf8.h"
#include <stdarg.h>
#include <string.h>

namespace kv {
namespace tags {

const u8* File::get(u64 at, u64 len) {
    if (at > span.size || len > span.size - at) return nullptr;
    if (spent) return nullptr;
    asked += len;
    if (asked > kMaxAsked) { spent = true; return nullptr; }
    ByteRange miss = sparse_missing(span, at, len);
    if (miss.len) {
        if (!last && !waiting) { waiting = true; need = miss; }
        return nullptr;
    }
    return span.bytes + at;
}

void Out::fail(const char* fmt, ...) {
    if (problem[0] || !problem_buf) return;
    va_list ap;
    va_start(ap, fmt);
    vformat(problem_buf, kProblem, fmt, ap);
    va_end(ap);
    problem = problem_buf;
}

bool Out::begin() {
    if (nvalues == kMaxValues) { fail("the file has more than %u tag values; the rest are not kept", kMaxValues); return false; }
    start = used;
    open = true;
    return true;
}

void Out::put(u32 cp) {
    if (!open || cp == 0) return;
    char b[KV_UTF8_MAX_BYTES];
    u32 n = utf8_encode(cp, b);
    if (used + n + 1 > cap) { fail("the tags hold more text than is kept"); return; }
    memcpy(text + used, b, n);
    used += n;
}

void Out::end(TagSource s, TagField f, bool keep_empty) {
    if (!open) return;
    open = false;
    // Trimmed: spaces and control characters at either end.
    u32 a = start, b = used;
    while (a < b && (u8)text[a] <= ' ') ++a;
    while (b > a && (u8)text[b - 1] <= ' ') --b;
    if (a == b && !keep_empty) { used = start; return; }
    u32 per = 0;
    for (u32 i = 0; i < nvalues; ++i) per += values[i].field == f && values[i].source == s;
    if (per >= kMaxPerField) {
        fail("a field is given more than %u times; the rest are not kept", kMaxPerField);
        used = start;
        return;
    }
    if (b + 1 > cap) { used = start; return; }
    memmove(text + start, text + a, b - a);
    u32 len = b - a;
    text[start + len] = 0;
    used = start + len + 1;
    TagValue& v = values[nvalues++];
    v.field = f; v.source = s; v.len = len; v.text = text + start;
}

void Out::utf8(TagSource s, TagField f, const u8* p, u64 n) {
    if (!begin()) return;
    for (u64 at = 0; at < n;) {
        u32 step = 0;
        u32 cp = utf8_decode((const char*)p + at, (usize)(n - at), &step);
        put(cp);
        at += step ? step : 1;
    }
    end(s, f);
}

void Out::latin1(TagSource s, TagField f, const u8* p, u64 n) {
    if (!begin()) return;
    for (u64 i = 0; i < n; ++i) put(p[i]);
    end(s, f);
}

void Out::utf16(TagSource s, TagField f, const u8* p, u64 n, bool be) {
    if (!begin()) return;
    u64 i = 0;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
    else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
    auto unit = [&](u64 k) -> u32 { return be ? ((u32)p[k] << 8) | p[k + 1] : ((u32)p[k + 1] << 8) | p[k]; };
    for (; i + 1 < n; i += 2) {
        u32 u = unit(i);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            u32 lo = unit(i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) { put(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)); i += 2; continue; }
        }
        put(u >= 0xD800 && u < 0xE000 ? KV_UTF8_REPLACEMENT : u);
    }
    if (i < n) put(KV_UTF8_REPLACEMENT); // an odd byte at the end
    end(s, f);
}

namespace {

bool digits(const char* p, u32 n) {
    if (n == 0) return false;
    for (u32 i = 0; i < n; ++i) if (p[i] < '0' || p[i] > '9') return false;
    return true;
}

// The number in [p, p + n), without its leading zeros, or the text as it is if it is not one.
void number(Out& o, TagSource s, TagField f, const char* p, u32 n) {
    while (n && *p == ' ') { ++p; --n; }
    while (n && p[n - 1] == ' ') --n;
    if (digits(p, n)) while (n > 1 && *p == '0') { ++p; --n; }
    o.utf8(s, f, (const u8*)p, n);
}

} // namespace

void Out::numbers(TagSource s, TagField num, TagField total, const char* t, u32 len) {
    const char* slash = (const char*)memchr(t, '/', len);
    if (!slash || total == TagField::Count) { number(*this, s, num, t, len); return; }
    u32 a = (u32)(slash - t);
    number(*this, s, num, t, a);
    number(*this, s, total, slash + 1, len - a - 1);
}

void Out::picture(const TagPicture& p) {
    if (npictures < kMaxPictures) pictures[npictures++] = p;
}

namespace {

struct Key { const char* name; TagField field; };
constexpr Key kKeys[] = {
    {"TITLE", TagField::Title}, {"ARTIST", TagField::Artist}, {"ALBUM", TagField::Album},
    {"ALBUMARTIST", TagField::AlbumArtist}, {"ALBUM ARTIST", TagField::AlbumArtist}, {"ALBUM_ARTIST", TagField::AlbumArtist},
    {"TRACKNUMBER", TagField::TrackNumber}, {"TRACK", TagField::TrackNumber},
    {"TRACKTOTAL", TagField::TrackTotal}, {"TOTALTRACKS", TagField::TrackTotal},
    {"DISCNUMBER", TagField::DiscNumber}, {"DISC", TagField::DiscNumber},
    {"DISCTOTAL", TagField::DiscTotal}, {"TOTALDISCS", TagField::DiscTotal},
    {"DATE", TagField::Date}, {"YEAR", TagField::Date},
    {"GENRE", TagField::Genre}, {"COMPOSER", TagField::Composer}, {"COMPILATION", TagField::Compilation},
    {"TITLESORT", TagField::TitleSort}, {"ARTISTSORT", TagField::ArtistSort}, {"ALBUMSORT", TagField::AlbumSort},
    {"ALBUMARTISTSORT", TagField::AlbumArtistSort}, {"COMPOSERSORT", TagField::ComposerSort},
    {"REPLAYGAIN_TRACK_GAIN", TagField::TrackGain}, {"REPLAYGAIN_TRACK_PEAK", TagField::TrackPeak},
    {"REPLAYGAIN_ALBUM_GAIN", TagField::AlbumGain}, {"REPLAYGAIN_ALBUM_PEAK", TagField::AlbumPeak},
    {"MUSICBRAINZ_TRACKID", TagField::MbTrackId}, {"MUSICBRAINZ_ALBUMID", TagField::MbAlbumId},
    {"MUSICBRAINZ_ARTISTID", TagField::MbArtistId}, {"MUSICBRAINZ_ALBUMARTISTID", TagField::MbAlbumArtistId},
    {"MUSICBRAINZ_RELEASEGROUPID", TagField::MbReleaseGroupId},
    // ID3v2's TXXX descriptions as Picard writes them, spaces and all.
    {"MUSICBRAINZ ALBUM ID", TagField::MbAlbumId}, {"MUSICBRAINZ ARTIST ID", TagField::MbArtistId},
    {"MUSICBRAINZ ALBUM ARTIST ID", TagField::MbAlbumArtistId}, {"MUSICBRAINZ RELEASE GROUP ID", TagField::MbReleaseGroupId},
};

char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

bool same_name(const char* key, u32 len, const char* name) {
    u32 i = 0;
    for (; i < len && name[i]; ++i) if (upper(key[i]) != name[i]) return false;
    return i == len && name[i] == 0;
}

} // namespace

TagField field_for_key(const char* key, u32 len) {
    for (const Key& k : kKeys) if (same_name(key, len, k.name)) return k.field;
    return TagField::Count;
}

void genres(Out& o, TagSource s, const char* t, u32 n) {
    // ID3's own forms: "(13)", "(13)(17)", "(13)Pop" (a refinement: the words win), "13", "RX", "CR".
    u32 i = 0;
    bool any = false;
    while (i < n && t[i] == '(') {
        u32 j = i + 1;
        while (j < n && t[j] != ')') ++j;
        if (j >= n) break;
        const char* in = t + i + 1;
        u32 len = j - i - 1;
        i = j + 1;
        if (len == 2 && in[0] == 'R' && in[1] == 'X') { o.utf8(s, TagField::Genre, (const u8*)"Remix", 5); any = true; continue; }
        if (len == 2 && in[0] == 'C' && in[1] == 'R') { o.utf8(s, TagField::Genre, (const u8*)"Cover", 5); any = true; continue; }
        if (!digits(in, len) || len > 3) { i -= len + 2; break; } // not a reference: words in brackets
        u32 g = 0;
        for (u32 k = 0; k < len; ++k) g = g * 10 + (u32)(in[k] - '0');
        if (i >= n || t[i] == '(') {
            if (const char* name = id3v1_genre(g)) { o.utf8(s, TagField::Genre, (const u8*)name, strlen(name)); any = true; }
        }
    }
    if (i < n) {
        const char* rest = t + i;
        u32 len = n - i;
        if (digits(rest, len) && len <= 3) {
            u32 g = 0;
            for (u32 k = 0; k < len; ++k) g = g * 10 + (u32)(rest[k] - '0');
            if (const char* name = id3v1_genre(g)) { o.utf8(s, TagField::Genre, (const u8*)name, strlen(name)); return; }
        }
        if (len == 2 && rest[0] == 'R' && rest[1] == 'X') { o.utf8(s, TagField::Genre, (const u8*)"Remix", 5); return; }
        if (len == 2 && rest[0] == 'C' && rest[1] == 'R') { o.utf8(s, TagField::Genre, (const u8*)"Cover", 5); return; }
        o.utf8(s, TagField::Genre, (const u8*)rest, len);
    } else if (!any && n) {
        o.utf8(s, TagField::Genre, (const u8*)t, n);
    }
}

namespace {

constexpr u32 kAlign = 16;

// Lays the caller's memory out as an Out: values, pictures, the scratch, then text.
bool layout(Out& o, Span<u8> memory) {
    usize skew = (usize)(-(intptr_t)memory.data) & (kAlign - 1);
    usize need = skew + sizeof(TagValue) * kMaxValues + sizeof(TagPicture) * kMaxPictures + kScratch + Out::kProblem + 4096;
    if (!memory.data || memory.len < need) return false;
    u8* p = memory.data + skew;
    o.problem_buf = (char*)p;     p += Out::kProblem;
    o.values = (TagValue*)p;      p += sizeof(TagValue) * kMaxValues;
    o.pictures = (TagPicture*)p;  p += sizeof(TagPicture) * kMaxPictures;
    o.scratch = p;                p += kScratch;
    o.text = (char*)p;
    o.cap = (u32)(memory.data + memory.len - p);
    return true;
}

TagRead finish(Out& o) {
    TagRead r;
    r.values = Span<const TagValue>(o.values, o.nvalues);
    r.pictures = Span<const TagPicture>(o.pictures, o.npictures);
    r.sources = o.sources;
    r.problem = o.problem;
    return r;
}

TagRead waiting(const File& f) {
    TagRead r;
    r.status = TagStatus::Wait;
    r.need = f.need;
    return r;
}

} // namespace
} // namespace tags

using namespace tags;

TagRead tags_read(const SparseSpan& span, Span<u8> memory, bool last) {
    Out o;
    if (!layout(o, memory)) { TagRead r; r.problem = "the tag reader was given too little memory"; return r; }
    File f(span, last);
    const u64 size = span.size;

    // The front: ID3v2 tags, one after another, then a FLAC's metadata blocks.
    u64 pos = 0;
    for (u32 k = 0; k < 4 && size - pos >= 10; ++k) {
        const u8* h = f.get(pos, 10);
        if (!h) { if (f.waiting) return waiting(f); break; }
        if (memcmp(h, "ID3", 3) != 0) break;
        u64 len = 0;
        if (!read_id3v2(f, o, pos, &len)) return waiting(f);
        if (len == 0 || len > size - pos) break;
        pos += len;
    }
    if (size - pos >= 4) {
        const u8* h = f.get(pos, 4);
        if (!h && f.waiting) return waiting(f);
        if (h && memcmp(h, "fLaC", 4) == 0 && !read_flac(f, o, pos)) return waiting(f);
    }

    // The back: ID3v1, then APEv2 before it, then an ID3v2 tag with a footer before those.
    u64 end = size;
    if (end >= pos + 128) {
        const u8* t = f.get(end - 128, 128);
        if (!t && f.waiting) return waiting(f);
        if (t && memcmp(t, "TAG", 3) == 0) { o.sources |= 1u << (u32)TagSource::Id3v1; read_id3v1(o, t); end -= 128; }
    }
    if (end >= pos + 32) {
        const u8* t = f.get(end - 32, 32);
        if (!t && f.waiting) return waiting(f);
        if (t && memcmp(t, "APETAGEX", 8) == 0) {
            u64 len = 0;
            if (!read_ape(f, o, end, &len)) return waiting(f);
            if (len <= end - pos) end -= len;
        }
    }
    if (end >= pos + 20) {
        const u8* t = f.get(end - 10, 10);
        if (!t && f.waiting) return waiting(f);
        if (t && memcmp(t, "3DI", 3) == 0 && !((t[6] | t[7] | t[8] | t[9]) & 0x80)) {
            u64 body = syncsafe(t + 6);
            if (body + 20 <= end - pos) {
                u64 len = 0;
                if (!read_id3v2(f, o, end - 20 - body, &len)) return waiting(f);
            } else {
                o.fail("an ID3v2 footer at the end says its tag is longer than the file");
            }
        }
    }
    if (f.spent) o.fail("the tags are larger than the reader reads (%llu bytes)", (unsigned long long)kMaxAsked);
    return finish(o);
}

void tags_merge(Span<const TagValue> values, TagMerge* out) {
    for (u32 f = 0; f < (u32)TagField::Count; ++f) out->source[f] = TagSource::Count;
    bool shown[(u32)TagField::Count] = {}; // an override with a value, not only empty ones
    for (const TagValue& v : values) {
        u32 f = (u32)v.field;
        if (f >= (u32)TagField::Count || (u32)v.source >= (u32)TagSource::Count) continue;
        if (v.source == TagSource::Override) { if (v.len) shown[f] = true; }
        if ((u32)v.source < (u32)out->source[f]) out->source[f] = v.source;
    }
    for (u32 f = 0; f < (u32)TagField::Count; ++f)
        if (out->source[f] == TagSource::Override && !shown[f]) out->source[f] = TagSource::Count; // hidden
}

const TagValue* tags_find(Span<const TagValue> values, TagField field, TagSource source) {
    for (const TagValue& v : values) if (v.field == field && v.source == source) return &v;
    return nullptr;
}

namespace {
constexpr const char* kFieldNames[] = {
    "title", "artist", "album", "albumartist", "tracknumber", "tracktotal", "discnumber", "disctotal",
    "date", "genre", "composer", "compilation",
    "titlesort", "artistsort", "albumsort", "albumartistsort", "composersort",
    "trackgain", "trackpeak", "albumgain", "albumpeak",
    "mb_trackid", "mb_albumid", "mb_artistid", "mb_albumartistid", "mb_releasegroupid",
};
static_assert(sizeof kFieldNames / sizeof kFieldNames[0] == (usize)TagField::Count);
constexpr const char* kSourceNames[] = {"override", "vorbis", "id3v2", "ape", "id3v1", "path"};
static_assert(sizeof kSourceNames / sizeof kSourceNames[0] == (usize)TagSource::Count);
constexpr u32 kRanks[] = {1, 2, 2, 3, 4, 5};
} // namespace

const char* tag_field_name(TagField f) { return (u32)f < (u32)TagField::Count ? kFieldNames[(u32)f] : "?"; }

bool tag_field_from_name(const char* name, TagField* out) {
    for (u32 f = 0; f < (u32)TagField::Count; ++f) {
        const char* a = kFieldNames[f];
        const char* b = name;
        while (*a && (*b == *a || (*b >= 'A' && *b <= 'Z' && *b + 32 == *a))) { ++a; ++b; }
        if (!*a && !*b) { *out = (TagField)f; return true; }
    }
    return false;
}

const char* tag_source_name(TagSource s) { return (u32)s < (u32)TagSource::Count ? kSourceNames[(u32)s] : "?"; }
u32 tag_source_rank(TagSource s) { return (u32)s < (u32)TagSource::Count ? kRanks[(u32)s] : 0; }

} // namespace kv
