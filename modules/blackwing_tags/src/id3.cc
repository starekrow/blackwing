// ID3v2.2, 2.3 and 2.4, and ID3v1. Frames are walked by their headers; a frame whose
// field is not wanted, and the picture inside an APIC, are stepped over by their sizes.
#include "reader.h"
#include <string.h>

namespace kv::tags {

namespace {

// FF 00 back to FF; returns the length.
u64 resync(const u8* in, u64 n, u8* out) {
    u64 k = 0;
    for (u64 i = 0; i < n; ++i) {
        out[k++] = in[i];
        if (in[i] == 0xFF && i + 1 < n && in[i + 1] == 0) ++i;
    }
    return k;
}

enum class Kind : u8 { Skip, Text, Numbers, Genre, Txxx, Ufid, Picture };
struct FrameKind { const char* id; Kind kind; TagField field, total; };
constexpr FrameKind kFrames[] = {
    {"TIT2", Kind::Text, TagField::Title, TagField::Count},      {"TPE1", Kind::Text, TagField::Artist, TagField::Count},
    {"TALB", Kind::Text, TagField::Album, TagField::Count},      {"TPE2", Kind::Text, TagField::AlbumArtist, TagField::Count},
    {"TRCK", Kind::Numbers, TagField::TrackNumber, TagField::TrackTotal},
    {"TPOS", Kind::Numbers, TagField::DiscNumber, TagField::DiscTotal},
    {"TDRC", Kind::Text, TagField::Date, TagField::Count},       {"TYER", Kind::Text, TagField::Date, TagField::Count},
    {"TCON", Kind::Genre, TagField::Genre, TagField::Count},     {"TCOM", Kind::Text, TagField::Composer, TagField::Count},
    {"TCMP", Kind::Text, TagField::Compilation, TagField::Count},
    {"TSOT", Kind::Text, TagField::TitleSort, TagField::Count},  {"TSOP", Kind::Text, TagField::ArtistSort, TagField::Count},
    {"TSOA", Kind::Text, TagField::AlbumSort, TagField::Count},  {"TSO2", Kind::Text, TagField::AlbumArtistSort, TagField::Count},
    {"TSOC", Kind::Text, TagField::ComposerSort, TagField::Count},
    {"TXXX", Kind::Txxx, TagField::Count, TagField::Count},      {"UFID", Kind::Ufid, TagField::MbTrackId, TagField::Count},
    {"APIC", Kind::Picture, TagField::Count, TagField::Count},
    // ID3v2.2's three-letter names.
    {"TT2", Kind::Text, TagField::Title, TagField::Count},       {"TP1", Kind::Text, TagField::Artist, TagField::Count},
    {"TAL", Kind::Text, TagField::Album, TagField::Count},       {"TP2", Kind::Text, TagField::AlbumArtist, TagField::Count},
    {"TRK", Kind::Numbers, TagField::TrackNumber, TagField::TrackTotal},
    {"TPA", Kind::Numbers, TagField::DiscNumber, TagField::DiscTotal},
    {"TYE", Kind::Text, TagField::Date, TagField::Count},        {"TCO", Kind::Genre, TagField::Genre, TagField::Count},
    {"TCM", Kind::Text, TagField::Composer, TagField::Count},    {"TCP", Kind::Text, TagField::Compilation, TagField::Count},
    {"TST", Kind::Text, TagField::TitleSort, TagField::Count},   {"TSP", Kind::Text, TagField::ArtistSort, TagField::Count},
    {"TSA", Kind::Text, TagField::AlbumSort, TagField::Count},   {"TS2", Kind::Text, TagField::AlbumArtistSort, TagField::Count},
    {"TSC", Kind::Text, TagField::ComposerSort, TagField::Count},
    {"TXX", Kind::Txxx, TagField::Count, TagField::Count},       {"UFI", Kind::Ufid, TagField::MbTrackId, TagField::Count},
    {"PIC", Kind::Picture, TagField::Count, TagField::Count},
};

const FrameKind* kind_of(const u8* id, u32 n) {
    for (const FrameKind& k : kFrames)
        if (strlen(k.id) == n && memcmp(k.id, id, n) == 0) return &k;
    return nullptr;
}

bool frame_id(const u8* id, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) return false;
    return true;
}

// The length of the string at p (up to n) in encoding `enc`, before its terminator, and
// where the next one starts.
u64 term(u8 enc, const u8* p, u64 n, u64* next) {
    if (enc == 1 || enc == 2) {
        for (u64 i = 0; i + 1 < n; i += 2)
            if (p[i] == 0 && p[i + 1] == 0) { *next = i + 2; return i; }
        *next = n;
        return n;
    }
    const u8* z = (const u8*)memchr(p, 0, (usize)n);
    if (!z) { *next = n; return n; }
    *next = (u64)(z - p) + 1;
    return (u64)(z - p);
}

void text(Out& o, u8 enc, TagField f, const u8* p, u64 n) {
    if (enc == 1) o.utf16(TagSource::Id3v2, f, p, n, false); // a mark says which; without one, as most writers mean it
    else if (enc == 2) o.utf16(TagSource::Id3v2, f, p, n, true);
    else if (enc == 3) o.utf8(TagSource::Id3v2, f, p, n);
    else o.latin1(TagSource::Id3v2, f, p, n);              // 0, and anything else a frame claims
}

// A value into a small buffer as UTF-8, for what is looked at before it is kept.
u32 small(u8 enc, const u8* p, u64 n, char* out, u32 cap) {
    Out t;
    t.text = out; t.cap = cap;
    TagValue v;
    t.values = &v;
    text(t, enc, TagField::Title, p, n);
    if (!t.nvalues) { out[0] = 0; return 0; }
    return v.len;
}

// Each value of a text frame's body (encoding byte first): ID3v2.4 puts several in one,
// split by the encoding's terminator.
void each_value(Out& o, const FrameKind& k, const u8* p, u64 n) {
    if (n < 1) return;
    u8 enc = p[0];
    ++p; --n;
    for (u32 count = 0; n && count < kMaxPerField; ++count) {
        u64 next = 0;
        u64 len = term(enc, p, n, &next);
        if (k.kind == Kind::Text) {
            text(o, enc, k.field, p, len);
        } else {
            char buf[512];
            u32 m = small(enc, p, len, buf, sizeof buf);
            if (k.kind == Kind::Numbers) o.numbers(TagSource::Id3v2, k.field, k.total, buf, m);
            else genres(o, TagSource::Id3v2, buf, m);
        }
        p += next; n -= next;
    }
}

void txxx(Out& o, const u8* p, u64 n) {
    if (n < 1) return;
    u8 enc = p[0];
    u64 next = 0;
    u64 len = term(enc, p + 1, n - 1, &next);
    char desc[128];
    u32 m = small(enc, p + 1, len, desc, sizeof desc);
    TagField f = field_for_key(desc, m);
    if (f == TagField::Count) return;
    const u8* v = p + 1 + next;
    u64 vn = n - 1 - next;
    FrameKind k{"TXXX", Kind::Text, f, TagField::Count};
    if (f == TagField::TrackNumber) k = FrameKind{"TXXX", Kind::Numbers, f, TagField::TrackTotal};
    if (f == TagField::DiscNumber) k = FrameKind{"TXXX", Kind::Numbers, f, TagField::DiscTotal};
    // The value is in the same encoding; put the byte back in front for each_value.
    u8 buf[1024];
    if (vn + 1 > sizeof buf) { text(o, enc, f, v, vn); return; }
    buf[0] = enc;
    memcpy(buf + 1, v, (usize)vn);
    each_value(o, k, buf, vn + 1);
}

void ufid(Out& o, const u8* p, u64 n) {
    static const char kOwner[] = "http://musicbrainz.org";
    if (n > sizeof kOwner && memcmp(p, kOwner, sizeof kOwner) == 0) o.latin1(TagSource::Id3v2, TagField::MbTrackId, p + sizeof kOwner, n - sizeof kOwner);
}

// Where a tag's frames come from: the file, or the scratch when the whole tag had to be
// undone first.
struct Frames {
    File* f = nullptr;
    const u8* mem = nullptr;
    u64 base = 0, size = 0; // in the file: [base, base + size); in memory: [0, size)
    const u8* get(u64 at, u64 len) {
        if (at > size || len > size - at) return nullptr;
        return mem ? mem + at : f->get(base + at, len);
    }
};

} // namespace

bool read_id3v2(File& f, Out& o, u64 at, u64* length) {
    *length = 0;
    const u8* h = f.get(at, 10);
    if (!h) return !f.waiting;
    if (memcmp(h, "ID3", 3) != 0) return true;
    const u32 major = h[3], flags = h[5];
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) { o.fail("an ID3v2 tag's size is not a size"); return true; }
    o.sources |= 1u << (u32)TagSource::Id3v2;
    u64 size = syncsafe(h + 6);
    *length = 10 + size + (major == 4 && (flags & 0x10) ? 10u : 0u);
    if (major < 2 || major > 4) { o.fail("ID3v2.%u is not a version this reads", major); return true; }
    if (major == 2 && (flags & 0x40)) { o.fail("a compressed ID3v2.2 tag is not read"); return true; }
    if (size > f.size() - at - 10) { o.fail("an ID3v2 tag says it is longer than the file"); size = f.size() - at - 10; }

    Frames w;
    w.f = &f; w.base = at + 10; w.size = size;
    const bool whole_unsync = (flags & 0x80) && major < 4;
    if (whole_unsync) {
        // Before 2.4 the sizes are of the undone bytes: the whole tag must be undone to walk it.
        u64 n = size;
        if (n > kScratch) { o.fail("an unsynchronised ID3v2 tag of %llu bytes: only the first %u are read", (unsigned long long)n, kScratch); n = kScratch; }
        const u8* raw = f.get(at + 10, n);
        if (!raw) { if (f.waiting) return false; o.fail("an unsynchronised ID3v2 tag could not be read"); return true; }
        w.size = resync(raw, n, o.scratch);
        w.mem = o.scratch;
    }

    // The extended header.
    u64 pos = 0;
    if (major >= 3 && (flags & 0x40)) {
        const u8* e = w.get(0, 4);
        if (!e) { if (f.waiting) return false; o.fail("an ID3v2 extended header past the tag's end"); return true; }
        pos = major == 3 ? 4 + (u64)be32(e) : (u64)syncsafe(e);
    }

    const u32 idn = major == 2 ? 3 : 4, hn = major == 2 ? 6 : 10;
    for (u32 count = 0; pos + hn <= w.size; ++count) {
        if (count == kMaxItems) { o.fail("an ID3v2 tag with more than %u frames: the rest are not read", kMaxItems); break; }
        const u8* fh = w.get(pos, hn);
        if (!fh) { if (f.waiting) return false; o.fail("an ID3v2 frame could not be read"); break; }
        if (fh[0] == 0) break; // padding
        if (!frame_id(fh, idn)) { o.fail("an ID3v2 frame header is not one"); break; }
        u64 fsize = major == 2 ? be24(fh + 3) : major == 3 ? be32(fh + 4) : syncsafe(fh + 4);
        if (major == 4 && be32(fh + 4) != fsize) {
            // iTunes wrote 2.4 sizes as 2.3's. Where the syncsafe reading lands on no frame
            // and the plain one does, the plain one is meant.
            u64 plain = be32(fh + 4);
            auto lands = [&](u64 n) {
                u64 next = pos + hn + n;
                if (next == w.size) return true;
                if (next + 4 > w.size) return false;
                const u8* id = w.get(next, 4);
                return id && (id[0] == 0 || frame_id(id, 4));
            };
            bool ss = lands(fsize);
            if (f.waiting) return false;
            if (!ss && lands(plain)) fsize = plain;
            if (f.waiting) return false;
        }
        if (fsize > w.size - pos - hn) { o.fail("an ID3v2 frame runs past the end of its tag"); break; }
        const u32 fflags = major >= 3 ? be16(fh + 8) : 0;
        const FrameKind* k = kind_of(fh, idn);
        u64 data = pos + hn, dn = fsize;
        pos += hn + fsize;
        if (!k) continue;

        bool unsync = false;
        if (major == 3) {
            if (fflags & 0x00C0) { o.fail("a compressed or encrypted ID3v2 frame is not read"); continue; }
            if (fflags & 0x0020) { if (dn < 1) continue; ++data; --dn; } // grouping
        } else if (major == 4) {
            if (fflags & 0x000C) { o.fail("a compressed or encrypted ID3v2 frame is not read"); continue; }
            if (fflags & 0x0040) { if (dn < 1) continue; ++data; --dn; }  // grouping
            if (fflags & 0x0001) { if (dn < 4) continue; data += 4; dn -= 4; } // data length
            unsync = (fflags & 0x0002) || (flags & 0x80);
        }

        if (k->kind == Kind::Picture) {
            // Its head only: what it is, and where its bytes are.
            u64 hn2 = dn < 1024 ? dn : 1024;
            const u8* p = w.get(data, hn2);
            if (!p) { if (f.waiting) return false; continue; }
            u8 head[1024];
            u64 m = unsync ? resync(p, hn2, head) : hn2;
            if (!unsync) memcpy(head, p, (usize)hn2);
            if (m < 2) continue;
            TagPicture pic;
            pic.source = TagSource::Id3v2;
            u64 i = 1;
            u8 enc = head[0];
            if (major == 2) {
                if (m < 5) continue;
                memcpy(pic.mime, head + 1, 3); pic.mime[3] = 0;
                i = 4;
            } else {
                u64 next = 0;
                u64 len = term(0, head + 1, m - 1, &next);
                if (next >= m - 1) continue;
                u64 c = len < sizeof pic.mime - 1 ? len : sizeof pic.mime - 1;
                memcpy(pic.mime, head + 1, (usize)c); pic.mime[c] = 0;
                i = 1 + next;
            }
            if (i >= m) continue;
            pic.kind = head[i++];
            u64 next = 0;
            term(enc, head + i, m - i, &next);
            if (i + next >= m && m == hn2 && hn2 < dn) continue; // a description longer than its head
            i += next;
            if (whole_unsync || unsync) continue; // its bytes in the file are not the picture's: not recorded
            pic.at = w.base + data + i;
            pic.len = dn - i;
            o.picture(pic);
            continue;
        }

        if (dn > kMaxItemBytes) { o.fail("an ID3v2 frame of %llu bytes is larger than is read", (unsigned long long)dn); continue; }
        const u8* p = w.get(data, dn);
        if (!p) { if (f.waiting) return false; o.fail("an ID3v2 frame could not be read"); continue; }
        if (unsync && !whole_unsync) { dn = resync(p, dn, o.scratch); p = o.scratch; }
        switch (k->kind) {
        case Kind::Text: case Kind::Numbers: case Kind::Genre: each_value(o, *k, p, dn); break;
        case Kind::Txxx: txxx(o, p, dn); break;
        case Kind::Ufid: ufid(o, p, dn); break;
        default: break;
        }
    }
    return true;
}

void read_id3v1(Out& o, const u8* t) {
    auto field = [&](TagField f, u32 at, u32 n) {
        const u8* p = t + at;
        u32 len = 0;
        while (len < n && p[len]) ++len;
        o.latin1(TagSource::Id3v1, f, p, len);
    };
    field(TagField::Title, 3, 30);
    field(TagField::Artist, 33, 30);
    field(TagField::Album, 63, 30);
    field(TagField::Date, 93, 4);
    if (t[125] == 0 && t[126] != 0) { // ID3v1.1: the comment's last byte is the track
        char n[4];
        u32 k = 0, v = t[126];
        if (v >= 100) n[k++] = (char)('0' + v / 100);
        if (v >= 10) n[k++] = (char)('0' + v / 10 % 10);
        n[k++] = (char)('0' + v % 10);
        o.utf8(TagSource::Id3v1, TagField::TrackNumber, (const u8*)n, k);
    }
    if (const char* g = id3v1_genre(t[127])) o.utf8(TagSource::Id3v1, TagField::Genre, (const u8*)g, strlen(g));
}

namespace {
constexpr const char* kGenres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk",
    "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes",
    "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
    "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass",
    "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic",
    "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove",
    "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle",
    "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore",
    "Terror", "Indie", "BritPop", "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop", "Synthpop", "Abstract", "Art Rock",
    "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM", "Eclectic", "Electro",
    "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient", "Industro-Goth", "Jam Band", "Krautrock",
    "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz", "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock",
    "Trop Rock", "World Music", "Neoclassical", "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep",
    "Garage Rock", "Psybient",
};
} // namespace

const char* id3v1_genre(u32 i) { return i < sizeof kGenres / sizeof kGenres[0] ? kGenres[i] : nullptr; }

} // namespace kv::tags
