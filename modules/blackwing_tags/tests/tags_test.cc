// The tag readers, on bytes made here: no file system and no index. A builder writes each
// kind of tag (ID3v2.2, 2.3 and 2.4 in each of their encodings, with and without
// unsynchronisation, Vorbis comments in a FLAC, APEv2, ID3v1) from one record of fields,
// and every field read back must be what was written. Then the ranking, the guess from a
// path, a file of a hundred megabytes of art in pieces, the hostile corpus, and spoiled
// copies by the hundred.
#include "korvid/blackwing_tags/tags.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/random.h"
#include "korvid/core/test.h"
#include "korvid/core/utf8.h"
#include <initializer_list>
#include <string.h>

using namespace kv;

namespace {

// ---- bytes -----------------------------------------------------------------------------

struct Buf {
    u8* p = nullptr;
    u32 n = 0, cap = 0;
    explicit Buf(u32 c = 1u << 20) : p((u8*)mem_alloc(c)), cap(c) {}
    ~Buf() { mem_free(p); }
    Buf(const Buf&) = delete;
    void put(const void* d, u32 k) { if (n + k <= cap) { memcpy(p + n, d, k); n += k; } }
    void str(const char* s) { put(s, (u32)strlen(s)); }
    void byte(u32 b) { u8 x = (u8)b; put(&x, 1); }
    void zeros(u32 k) { for (u32 i = 0; i < k && n < cap; ++i) p[n++] = 0; }
    void be32(u32 v) { byte(v >> 24); byte(v >> 16); byte(v >> 8); byte(v); }
    void be24(u32 v) { byte(v >> 16); byte(v >> 8); byte(v); }
    void le32(u32 v) { byte(v); byte(v >> 8); byte(v >> 16); byte(v >> 24); }
    void syncsafe(u32 v) { byte((v >> 21) & 0x7F); byte((v >> 14) & 0x7F); byte((v >> 7) & 0x7F); byte(v & 0x7F); }
    void append(const Buf& b) { put(b.p, b.n); }
    Span<const u8> bytes() const { return Span<const u8>(p, n); }
};

// Text in an ID3v2 encoding, from UTF-8: 0 Latin-1, 1 UTF-16 with a mark (`le` says which
// way), 2 UTF-16BE, 3 UTF-8. Code points past what Latin-1 holds become '?'.
void encode(Buf& b, u8 enc, const char* utf8, bool le = true) {
    const char* p = utf8;
    if (enc == 1) { if (le) { b.byte(0xFF); b.byte(0xFE); } else { b.byte(0xFE); b.byte(0xFF); } }
    if (enc == 3) { b.str(utf8); return; }
    while (u32 cp = utf8_next(p)) {
        if (enc == 0) { b.byte(cp < 256 ? cp : '?'); continue; }
        bool big = enc == 2 || !le;
        auto unit = [&](u32 u) { if (big) { b.byte(u >> 8); b.byte(u); } else { b.byte(u); b.byte(u >> 8); } };
        if (cp >= 0x10000) { cp -= 0x10000; unit(0xD800 + (cp >> 10)); unit(0xDC00 + (cp & 0x3FF)); }
        else unit(cp);
    }
}
void terminator(Buf& b, u8 enc) { b.byte(0); if (enc == 1 || enc == 2) b.byte(0); }

// ---- what is written ---------------------------------------------------------------------

struct Song {
    const char* title = "Straße nach Ünïcødé ☃ 𝄞";
    const char* artist = "Björk";
    const char* artist2 = "Guðmundsdóttir";
    const char* album = "Médúlla";
    const char* album_artist = "Björk & Friends";
    const char* track = "03"; const char* tracks = "12";
    const char* disc = "1";   const char* discs = "2";
    const char* date = "2004-08-30";
    const char* genre = "Electronic";
    const char* composer = "Jónsi";
    const char* title_sort = "Strasse";
    const char* artist_sort = "Bjork";
    const char* gain = "-6.54 dB";
    const char* mb_album = "8ed4a4d8-2f74-4e8f-9a0e-5f6c0e5d6a01";
    const char* mb_track = "c3b3a2f4-1d2e-4b5a-8c7d-0e1f2a3b4c5d";
};
const Song kSong;
// The same in Latin-1's reach, for the encodings that are only that.
Song latin_song() {
    Song s;
    s.title = "Straße nach Ünïcødé"; s.album_artist = "Björk & Friends";
    return s;
}

struct Expect { TagField f; const char* v; };

// ---- the builders ------------------------------------------------------------------------

struct Id3Opts {
    u32  major = 4;
    u8   enc = 3;
    bool le = true;           // UTF-16's mark
    bool unsync = false;      // tag-level (2.2, 2.3) or every frame (2.4)
    bool ext = false;         // an extended header
    bool footer = false;      // 2.4's footer
    bool itunes = false;      // 2.4 frame sizes written as plain 32-bit numbers
    u32  art = 0;             // bytes of picture, 0 for none
    u32  padding = 64;
};

// A text frame body: the encoding, then the values with terminators between.
void text_body(Buf& b, u8 enc, std::initializer_list<const char*> values, bool le) {
    b.byte(enc);
    bool first = true;
    for (const char* v : values) { if (!first) terminator(b, enc); encode(b, enc, v, le && enc == 1 ? true : le); first = false; }
}

void frame(Buf& tag, const Id3Opts& o, const char* id, const Buf& body) {
    const u8* data = body.p;
    u32 n = body.n;
    Buf un(body.n * 2 + 16);
    if (o.unsync && o.major == 4) {
        for (u32 i = 0; i < n; ++i) { un.byte(data[i]); if (data[i] == 0xFF && (i + 1 == n || data[i + 1] == 0 || (data[i + 1] & 0xE0) == 0xE0)) un.byte(0); }
        data = un.p; n = un.n;
    }
    if (o.major == 2) { tag.put(id, 3); tag.be24(n); }
    else {
        tag.put(id, 4);
        if (o.major == 4 && !o.itunes) tag.syncsafe(n); else tag.be32(n);
        tag.byte(0); tag.byte(o.unsync && o.major == 4 ? 0x02 : 0);
    }
    tag.put(data, n);
}

void text_frame(Buf& tag, const Id3Opts& o, const char* id, std::initializer_list<const char*> values) {
    Buf b(4096);
    // Before 2.4 a frame has one value: several are joined as "a/b" by convention.
    if (o.major < 4 && values.size() > 1) {
        char joined[512] = "";
        for (const char* v : values) { if (joined[0]) strcat(joined, "/"); strcat(joined, v); }
        text_body(b, o.enc, {joined}, o.le);
    } else {
        text_body(b, o.enc, values, o.le);
    }
    frame(tag, o, id, b);
}

void txxx(Buf& tag, const Id3Opts& o, const char* desc, const char* value) {
    Buf b(1024);
    b.byte(o.enc);
    encode(b, o.enc, desc, o.le); terminator(b, o.enc); encode(b, o.enc, value, o.le);
    frame(tag, o, o.major == 2 ? "TXX" : "TXXX", b);
}

// An ID3v2 tag of `s` (fields named for the version), into `out`. Where the picture's bytes
// begin, relative to the tag's start, goes in *art_at.
void id3v2(Buf& out, const Id3Opts& o, const Song& s, u32* art_at = nullptr) {
    Buf tag(o.art + (1u << 16));
    bool v2 = o.major == 2;
    if (o.ext) {
        if (o.major == 3) { tag.be32(6); tag.zeros(6); }
        else { tag.syncsafe(6); tag.byte(1); tag.byte(0); }
    }
    text_frame(tag, o, v2 ? "TT2" : "TIT2", {s.title});
    if (o.major == 4) text_frame(tag, o, "TPE1", {s.artist, s.artist2});
    else text_frame(tag, o, v2 ? "TP1" : "TPE1", {s.artist});
    text_frame(tag, o, v2 ? "TAL" : "TALB", {s.album});
    text_frame(tag, o, v2 ? "TP2" : "TPE2", {s.album_artist});
    char tr[16]; format(tr, sizeof tr, "%s/%s", s.track, s.tracks);
    char dc[16]; format(dc, sizeof dc, "%s/%s", s.disc, s.discs);
    text_frame(tag, o, v2 ? "TRK" : "TRCK", {tr});
    text_frame(tag, o, v2 ? "TPA" : "TPOS", {dc});
    text_frame(tag, o, v2 ? "TYE" : o.major == 3 ? "TYER" : "TDRC", {o.major == 4 ? s.date : "2004"});
    text_frame(tag, o, v2 ? "TCO" : "TCON", {o.major == 4 ? "52" : "(52)"}); // 52: Electronic, by ID3v1's number
    text_frame(tag, o, v2 ? "TCM" : "TCOM", {s.composer});
    text_frame(tag, o, v2 ? "TCP" : "TCMP", {"1"});
    text_frame(tag, o, v2 ? "TST" : "TSOT", {s.title_sort});
    text_frame(tag, o, v2 ? "TSP" : "TSOP", {s.artist_sort});
    txxx(tag, o, "REPLAYGAIN_TRACK_GAIN", s.gain);
    txxx(tag, o, "MusicBrainz Album Id", s.mb_album);
    {
        Buf b(256);
        b.str("http://musicbrainz.org"); b.byte(0); b.str(s.mb_track);
        frame(tag, o, v2 ? "UFI" : "UFID", b);
    }
    if (o.art) {
        Buf b(o.art + 64);
        b.byte(0);
        u32 head;
        if (v2) { b.str("JPG"); b.byte(3); b.str("cover"); b.byte(0); head = 1 + 3 + 1 + 6; }
        else { b.str("image/jpeg"); b.byte(0); b.byte(3); b.str("cover"); b.byte(0); head = 1 + 11 + 1 + 6; }
        for (u32 i = 0; i < o.art; ++i) b.byte((i * 7 + 3) & 0xFE); // no FF in it: unsynchronisation leaves it alone
        u32 at = (u32)(10 + tag.n + (o.major == 2 ? 6 : 10) + head);
        frame(tag, o, v2 ? "PIC" : "APIC", b);
        if (art_at) *art_at = at;
    }
    text_frame(tag, o, v2 ? "TCO" : "TCOM", {}); // an empty frame: nothing comes of it
    tag.zeros(o.padding);

    Buf body(tag.n * 2 + 16);
    if (o.unsync && o.major < 4) {
        for (u32 i = 0; i < tag.n; ++i) { body.byte(tag.p[i]); if (tag.p[i] == 0xFF) body.byte(0); }
    } else {
        body.append(tag);
    }
    out.str("ID3"); out.byte(o.major); out.byte(0);
    out.byte((o.unsync ? 0x80 : 0) | (o.ext ? 0x40 : 0) | (o.footer ? 0x10 : 0));
    out.syncsafe(body.n);
    out.append(body);
    if (o.footer) { out.str("3DI"); out.byte(o.major); out.byte(0); out.byte(0x10); out.syncsafe(body.n); }
}

void expect_id3(const Id3Opts& o, const Song& s, Expect* e, u32* n) {
    u32 k = 0;
    e[k++] = {TagField::Title, s.title};
    e[k++] = {TagField::Artist, s.artist};
    if (o.major == 4) e[k++] = {TagField::Artist, s.artist2};
    e[k++] = {TagField::Album, s.album};
    e[k++] = {TagField::AlbumArtist, s.album_artist};
    e[k++] = {TagField::TrackNumber, "3"}; e[k++] = {TagField::TrackTotal, "12"};
    e[k++] = {TagField::DiscNumber, "1"};  e[k++] = {TagField::DiscTotal, "2"};
    e[k++] = {TagField::Date, o.major == 4 ? s.date : "2004"};
    e[k++] = {TagField::Genre, "Electronic"};
    e[k++] = {TagField::Composer, s.composer};
    e[k++] = {TagField::Compilation, "1"};
    e[k++] = {TagField::TitleSort, s.title_sort};
    e[k++] = {TagField::ArtistSort, s.artist_sort};
    e[k++] = {TagField::TrackGain, s.gain};
    e[k++] = {TagField::MbAlbumId, s.mb_album};
    e[k++] = {TagField::MbTrackId, s.mb_track};
    *n = k;
}

// A FLAC's front: "fLaC", STREAMINFO, a Vorbis comment block, a picture, then a frame's worth of nothing.
void flac(Buf& out, const Song& s, u32 art = 0, u32* art_at = nullptr) {
    out.str("fLaC");
    out.byte(0); out.be24(34); out.zeros(34);
    Buf c(8192);
    const char* vendor = "reference libFLAC 1.4.3";
    c.le32((u32)strlen(vendor)); c.str(vendor);
    char lines[24][160];
    u32 nl = 0;
    format(lines[nl++], 160, "TITLE=%s", s.title);
    format(lines[nl++], 160, "ARTIST=%s", s.artist);
    format(lines[nl++], 160, "artist=%s", s.artist2); // keys are case-insensitive
    format(lines[nl++], 160, "ALBUM=%s", s.album);
    format(lines[nl++], 160, "ALBUMARTIST=%s", s.album_artist);
    format(lines[nl++], 160, "TRACKNUMBER=%s/%s", s.track, s.tracks);
    format(lines[nl++], 160, "DISCNUMBER=%s", s.disc);
    format(lines[nl++], 160, "DISCTOTAL=%s", s.discs);
    format(lines[nl++], 160, "DATE=%s", s.date);
    format(lines[nl++], 160, "GENRE=%s", s.genre);
    format(lines[nl++], 160, "COMPOSER=%s", s.composer);
    format(lines[nl++], 160, "COMPILATION=1");
    format(lines[nl++], 160, "TITLESORT=%s", s.title_sort);
    format(lines[nl++], 160, "ARTISTSORT=%s", s.artist_sort);
    format(lines[nl++], 160, "REPLAYGAIN_TRACK_GAIN=%s", s.gain);
    format(lines[nl++], 160, "MUSICBRAINZ_ALBUMID=%s", s.mb_album);
    format(lines[nl++], 160, "MUSICBRAINZ_TRACKID=%s", s.mb_track);
    format(lines[nl++], 160, "COMMENT=not a field this keeps");
    c.le32(nl);
    for (u32 i = 0; i < nl; ++i) { c.le32((u32)strlen(lines[i])); c.str(lines[i]); }
    out.byte(art ? 4 : 0x84); out.be24(c.n); out.append(c);
    if (art) {
        Buf p(art + 128);
        p.be32(3); p.be32(9); p.str("image/png"); p.be32(5); p.str("front");
        p.be32(500); p.be32(500); p.be32(24); p.be32(0); p.be32(art);
        u32 head = p.n;
        for (u32 i = 0; i < art; ++i) p.byte(i * 13);
        out.byte(0x86); out.be24(p.n);
        if (art_at) *art_at = out.n + head;
        out.append(p);
    }
    out.byte(0xFF); out.byte(0xF8); out.zeros(200); // where the audio would be
}

void expect_flac(const Song& s, Expect* e, u32* n) {
    u32 k = 0;
    e[k++] = {TagField::Title, s.title};
    e[k++] = {TagField::Artist, s.artist}; e[k++] = {TagField::Artist, s.artist2};
    e[k++] = {TagField::Album, s.album};
    e[k++] = {TagField::AlbumArtist, s.album_artist};
    e[k++] = {TagField::TrackNumber, "3"}; e[k++] = {TagField::TrackTotal, "12"};
    e[k++] = {TagField::DiscNumber, "1"};  e[k++] = {TagField::DiscTotal, "2"};
    e[k++] = {TagField::Date, s.date};
    e[k++] = {TagField::Genre, s.genre};
    e[k++] = {TagField::Composer, s.composer};
    e[k++] = {TagField::Compilation, "1"};
    e[k++] = {TagField::TitleSort, s.title_sort};
    e[k++] = {TagField::ArtistSort, s.artist_sort};
    e[k++] = {TagField::TrackGain, s.gain};
    e[k++] = {TagField::MbAlbumId, s.mb_album};
    e[k++] = {TagField::MbTrackId, s.mb_track};
    *n = k;
}

void ape(Buf& out, const Song& s, bool header = true, u32 art = 0) {
    Buf items(art + 4096);
    u32 count = 0;
    auto item = [&](const char* key, const char* v, u32 flags = 0, u32 vlen = ~0u) {
        u32 n = vlen == ~0u ? (u32)strlen(v) : vlen;
        items.le32(n); items.le32(flags); items.str(key); items.byte(0); items.put(v, n); ++count;
    };
    item("Title", s.title);
    char two[128]; u32 tn = (u32)format(two, sizeof two, "%s", s.artist); two[tn] = 0; memcpy(two + tn + 1, s.artist2, strlen(s.artist2));
    item("Artist", two, 0, tn + 1 + (u32)strlen(s.artist2)); // two values, split by a NUL
    item("Album", s.album);
    item("Album Artist", s.album_artist);
    char tr[16]; format(tr, sizeof tr, "%s/%s", s.track, s.tracks);
    item("Track", tr);
    item("Year", "2004");
    item("Genre", s.genre);
    item("Composer", s.composer);
    if (art) {
        Buf b(art + 16);
        b.str("cover.jpg"); b.byte(0);
        for (u32 i = 0; i < art; ++i) b.byte(i);
        items.le32(b.n); items.le32(2); items.str("Cover Art (Front)"); items.byte(0); items.append(b); ++count;
    }
    u32 size = items.n + 32;
    if (header) { out.str("APETAGEX"); out.le32(2000); out.le32(size); out.le32(count); out.le32(0xA0000000u); out.zeros(8); }
    out.append(items);
    out.str("APETAGEX"); out.le32(2000); out.le32(size); out.le32(count); out.le32(0x80000000u); out.zeros(8);
}

void id3v1(Buf& out, const char* title, const char* artist, u32 track, u32 genre) {
    u8 t[128] = {};
    memcpy(t, "TAG", 3);
    memcpy(t + 3, title, strlen(title) < 30 ? strlen(title) : 30);
    memcpy(t + 33, artist, strlen(artist) < 30 ? strlen(artist) : 30);
    memcpy(t + 63, "Album One", 9);
    memcpy(t + 93, "1999", 4);
    memcpy(t + 97, "a comment", 9);
    t[125] = 0; t[126] = (u8)track; t[127] = (u8)genre;
    out.put(t, 128);
}

// ---- reading -----------------------------------------------------------------------------

struct Reader {
    u8* mem = (u8*)mem_alloc(kTagReadMemory);
    ~Reader() { mem_free(mem); }
    TagRead read(Span<const u8> b) { return tags_read(sparse_whole(b), Span<u8>(mem, kTagReadMemory)); }
    TagRead path(const char* p) { return tags_from_path(p, Span<u8>(mem, kTagReadMemory)); }
};

// Whether `r` has exactly the values `e` from source `s`, in that order within each field.
bool matches(const TagRead& r, TagSource s, const Expect* e, u32 n, const char* what) {
    bool ok = true;
    for (u32 i = 0; i < n; ++i) {
        u32 seen = 0, want = 0;
        for (u32 j = 0; j < i; ++j) want += e[j].f == e[i].f;
        const TagValue* hit = nullptr;
        for (const TagValue& v : r.values)
            if (v.source == s && v.field == e[i].f && seen++ == want) { hit = &v; break; }
        if (!hit || strcmp(hit->text, e[i].v) != 0 || hit->len != strlen(e[i].v)) {
            KV_ERROR("test", "%s: %s is \"%s\", not \"%s\"", what, tag_field_name(e[i].f), hit ? hit->text : "(none)", e[i].v);
            ok = false;
        }
    }
    u32 total = 0;
    for (const TagValue& v : r.values) total += v.source == s;
    if (total != n) { KV_ERROR("test", "%s: %u values from %s, %u expected", what, total, tag_source_name(s), n); ok = false; }
    return ok;
}

bool all_utf8(const TagRead& r) {
    for (const TagValue& v : r.values)
        if (!utf8_valid(v.text, v.len) || v.text[v.len] != 0 || strlen(v.text) != v.len) return false;
    return true;
}

// ---- a file in pieces, made as it is asked for ----------------------------------------------

// A file of `size` bytes in reserved address space: `front` at its start, `back` at its
// end, and between them bytes made by a formula. A piece is committed only when given,
// and the test knows which were.
struct Sparse {
    SparseSpan span;
    u8* base = nullptr; usize reserved = 0;
    u8* have = nullptr; u64 pieces = 0, given = 0;
    const Buf* front = nullptr; const Buf* back = nullptr;
    void make(u64 size, u32 shift, const Buf& f, const Buf& b) {
        front = &f; back = &b;
        span.size = size; span.piece_shift = shift;
        pieces = sparse_pieces(span);
        reserved = os_page_align((usize)pieces << shift);
        base = (u8*)os_reserve(reserved);
        have = (u8*)mem_alloc((usize)pieces);
        memset(have, 0, (usize)pieces);
        span.bytes = base; span.have = have;
    }
    u8 byte_at(u64 i) const {
        if (i < front->n) return front->p[i];
        if (i >= span.size - back->n) return back->p[i - (span.size - back->n)];
        return (u8)(i * 7 + 3) & 0xFE;
    }
    void give(u64 k) {
        if (k >= pieces || have[k]) return;
        u64 from = k << span.piece_shift, len = span.size - from < (1ull << span.piece_shift) ? span.size - from : 1ull << span.piece_shift;
        os_commit(base + from, os_page_align((usize)len));
        for (u64 i = 0; i < len; ++i) base[from + i] = byte_at(from + i);
        atomic_store(&have[k], (u8)1, MO_RELEASE);
        ++given;
    }
    void free() { os_release(base, reserved); mem_free(have); }
};

} // namespace

// Every version in each of its encodings, with and without unsynchronisation and an
// extended header: every field as written.
void test_tags_id3v2(TestCtx& c) {
    c.begin("tags id3v2");
    Reader rd;
    u32 runs = 0;
    for (u32 major : {2u, 3u, 4u}) {
        for (u8 enc : {(u8)0, (u8)1, (u8)2, (u8)3}) {
            if (major < 4 && enc > 1) continue; // 2.2 and 2.3 have Latin-1 and UTF-16 with a mark
            for (u32 variant = 0; variant < 4; ++variant) {
                Id3Opts o;
                o.major = major; o.enc = enc;
                o.unsync = variant & 1;
                o.ext = (variant & 2) && major > 2;
                o.le = !(variant & 2);
                o.footer = major == 4 && variant == 3;
                o.art = variant == 2 ? 5000 : 0;
                Song s = enc == 0 ? latin_song() : kSong;
                Buf b;
                u32 art_at = 0;
                id3v2(b, o, s, &art_at);
                b.byte(0xFF); b.byte(0xFB); b.zeros(500); // then an MP3's frames
                TagRead r = rd.read(b.bytes());
                Expect e[32]; u32 n;
                expect_id3(o, s, e, &n);
                char what[64];
                format(what, sizeof what, "ID3v2.%u, encoding %u, variant %u", major, enc, variant);
                KV_CHECK(c, r.status == TagStatus::Done && matches(r, TagSource::Id3v2, e, n, what) && all_utf8(r));
                KV_CHECK(c, r.sources == (1u << (u32)TagSource::Id3v2));
                if (r.problem[0]) KV_ERROR("test", "%s: %s", what, r.problem);
                KV_CHECK(c, r.problem[0] == 0);
                if (o.art && !o.unsync) {
                    KV_CHECK(c, r.pictures.len == 1 && r.pictures[0].at == art_at && r.pictures[0].len == o.art && r.pictures[0].kind == 3);
                    KV_CHECK(c, r.pictures.len == 1 && strcmp(r.pictures[0].mime, major == 2 ? "JPG" : "image/jpeg") == 0);
                }
                ++runs;
            }
        }
    }
    // iTunes' 2.4 with 2.3's sizes, frames long enough that the two readings differ.
    {
        Id3Opts o; o.itunes = true; o.art = 300;
        Song s; s.title = "A title long enough that its frame's size is more than one hundred and twenty-seven bytes, which is where syncsafe and plain part company";
        Buf b; id3v2(b, o, s);
        TagRead r = rd.read(b.bytes());
        const TagValue* t = tags_find(r.values, TagField::Title, TagSource::Id3v2);
        KV_CHECK(c, t && strcmp(t->text, s.title) == 0 && tags_find(r.values, TagField::MbTrackId, TagSource::Id3v2) && r.pictures.len == 1);
    }
    KV_INFO("test", "tags id3v2: %u tags", runs);
}

void test_tags_vorbis(TestCtx& c) {
    c.begin("tags vorbis");
    Reader rd;
    Buf b; u32 art_at = 0;
    flac(b, kSong, 20000, &art_at);
    TagRead r = rd.read(b.bytes());
    Expect e[32]; u32 n;
    expect_flac(kSong, e, &n);
    KV_CHECK(c, r.status == TagStatus::Done && matches(r, TagSource::Vorbis, e, n, "FLAC") && all_utf8(r) && !r.problem[0]);
    KV_CHECK(c, r.pictures.len == 1 && r.pictures[0].source == TagSource::Vorbis && r.pictures[0].at == art_at && r.pictures[0].len == 20000);
    KV_CHECK(c, r.pictures.len == 1 && r.pictures[0].kind == 3 && strcmp(r.pictures[0].mime, "image/png") == 0);

    // A FLAC with an ID3v2 tag in front: both are read, and the Vorbis comments win the tie.
    Buf t;
    Id3Opts o;
    Song other = kSong; other.title = "The ID3 title";
    id3v2(t, o, other);
    t.append(b);
    r = rd.read(t.bytes());
    KV_CHECK(c, r.sources == ((1u << (u32)TagSource::Vorbis) | (1u << (u32)TagSource::Id3v2)));
    KV_CHECK(c, matches(r, TagSource::Vorbis, e, n, "FLAC behind ID3v2"));
    TagMerge m; tags_merge(r.values, &m);
    KV_CHECK(c, m.source[(u32)TagField::Title] == TagSource::Vorbis);
    KV_CHECK(c, tag_source_rank(TagSource::Vorbis) == tag_source_rank(TagSource::Id3v2));
}

void test_tags_ape_id3v1(TestCtx& c) {
    c.begin("tags ape and id3v1");
    Reader rd;
    for (bool header : {true, false}) {
        Buf b;
        b.byte(0xFF); b.byte(0xFB); b.zeros(3000); // an MP3's frames
        ape(b, kSong, header, 3000);
        id3v1(b, "Old Title", "Old Artist", 7, 17);
        TagRead r = rd.read(b.bytes());
        const Expect e[] = {
            {TagField::Title, kSong.title}, {TagField::Artist, kSong.artist}, {TagField::Artist, kSong.artist2},
            {TagField::Album, kSong.album}, {TagField::AlbumArtist, kSong.album_artist},
            {TagField::TrackNumber, "3"}, {TagField::TrackTotal, "12"}, {TagField::Date, "2004"},
            {TagField::Genre, kSong.genre}, {TagField::Composer, kSong.composer},
        };
        KV_CHECK(c, matches(r, TagSource::Ape, e, sizeof e / sizeof e[0], "APEv2"));
        const Expect v1[] = {
            {TagField::Title, "Old Title"}, {TagField::Artist, "Old Artist"}, {TagField::Album, "Album One"},
            {TagField::Date, "1999"}, {TagField::TrackNumber, "7"}, {TagField::Genre, "Rock"},
        };
        KV_CHECK(c, matches(r, TagSource::Id3v1, v1, sizeof v1 / sizeof v1[0], "ID3v1"));
        KV_CHECK(c, r.pictures.len == 1 && r.pictures[0].source == TagSource::Ape && r.pictures[0].len == 3000 && r.pictures[0].kind == 3);
        KV_CHECK(c, r.pictures.len == 1 && r.pictures[0].at + 3000 == b.n - 128 - 32 && !r.problem[0] && all_utf8(r));
    }
    // ID3v1 in Latin-1, padded with spaces; ID3v1.0 (no track); a genre past the list.
    {
        Buf b; b.zeros(200);
        u8 t[128] = {};
        memcpy(t, "TAG", 3);
        memcpy(t + 3, "Caf\xe9 No\xebl                    ", 30);
        memcpy(t + 97, "a comment that fills the field", 30);
        t[127] = 250;
        b.put(t, 128);
        TagRead r = rd.read(b.bytes());
        const Expect e[] = {{TagField::Title, "Café Noël"}};
        KV_CHECK(c, matches(r, TagSource::Id3v1, e, 1, "ID3v1.0"));
    }
}

// Each rank beating the one below it, the override above all, and an empty override hiding.
void test_tags_rank(TestCtx& c) {
    c.begin("tags rank");
    Reader rd;
    KV_CHECK(c, tag_source_rank(TagSource::Override) == 1 && tag_source_rank(TagSource::Ape) == 3 &&
                tag_source_rank(TagSource::Id3v1) == 4 && tag_source_rank(TagSource::Path) == 5);
    Buf b;
    Id3Opts o; Song s2 = kSong; s2.title = "v2";
    id3v2(b, o, s2);
    b.byte(0xFF); b.byte(0xFB); b.zeros(1000);
    Song s3 = kSong; s3.title = "ape";
    ape(b, s3);
    id3v1(b, "v1", "v1 artist", 1, 0);
    TagRead r = rd.read(b.bytes());
    KV_CHECK(c, r.sources == ((1u << 2) | (1u << 3) | (1u << 4)));

    // Everything read, plus a guess from the path and the user's overrides, in one list.
    TagValue all[512];
    u32 n = 0;
    for (const TagValue& v : r.values) all[n++] = v;
    u8* pm = (u8*)mem_alloc(1 << 16);
    TagRead p = tags_from_path("Some Artist/Some Album/07 - path title.flac", Span<u8>(pm, 1 << 16));
    for (const TagValue& v : p.values) all[n++] = v;
    auto winner = [&](u32 count, TagField f) { TagMerge m; tags_merge(Span<const TagValue>(all, count), &m); return m.source[(u32)f]; };
    auto without = [&](TagSource s) { u32 k = 0; for (u32 i = 0; i < n; ++i) if (all[i].source != s) all[k++] = all[i]; n = k; };

    KV_CHECK(c, winner(n, TagField::Title) == TagSource::Id3v2);
    KV_CHECK(c, winner(n, TagField::Composer) == TagSource::Id3v2);
    without(TagSource::Id3v2);
    KV_CHECK(c, winner(n, TagField::Title) == TagSource::Ape && strcmp(tags_find(Span<const TagValue>(all, n), TagField::Title, TagSource::Ape)->text, "ape") == 0);
    KV_CHECK(c, winner(n, TagField::TrackNumber) == TagSource::Ape);
    without(TagSource::Ape);
    KV_CHECK(c, winner(n, TagField::Title) == TagSource::Id3v1 && winner(n, TagField::TrackNumber) == TagSource::Id3v1);
    KV_CHECK(c, winner(n, TagField::Album) == TagSource::Id3v1);
    without(TagSource::Id3v1);
    KV_CHECK(c, winner(n, TagField::Title) == TagSource::Path && winner(n, TagField::TrackNumber) == TagSource::Path);
    KV_CHECK(c, winner(n, TagField::Composer) == TagSource::Count);

    // Overrides: a value wins over everything; an empty one hides the field.
    all[n++] = TagValue{TagField::Title, TagSource::Override, 4, "Mine"};
    all[n++] = TagValue{TagField::Album, TagSource::Override, 0, ""};
    KV_CHECK(c, winner(n, TagField::Title) == TagSource::Override);
    KV_CHECK(c, winner(n, TagField::Album) == TagSource::Count);
    KV_CHECK(c, winner(n, TagField::Artist) == TagSource::Path);
    mem_free(pm);

    // The names, both ways.
    for (u32 f = 0; f < (u32)TagField::Count; ++f) {
        TagField back = TagField::Count;
        KV_CHECK(c, tag_field_from_name(tag_field_name((TagField)f), &back) && back == (TagField)f);
    }
    TagField x;
    KV_CHECK(c, tag_field_from_name("AlbumArtist", &x) && x == TagField::AlbumArtist && !tag_field_from_name("nonsense", &x));
}

void test_tags_path(TestCtx& c) {
    c.begin("tags path");
    Reader rd;
    struct Case { const char* path; const char* title; const char* artist; const char* album; const char* track; const char* disc; const char* date; };
    const Case cases[] = {
        {"Artist/Album/03 - Title.flac", "Title", "Artist", "Album", "3", nullptr, nullptr},
        {"Artist - Album/03 Title.mp3", "Title", "Artist", "Album", "3", nullptr, nullptr},
        {"Artist/Album (2003)/CD2/1-07 Title.flac", "Title", "Artist", "Album", "7", "2", "2003"},
        {"Artist/2003 - Album/05. Title.mp3", "Title", "Artist", "Album", "5", nullptr, "2003"},
        {"Artist/[1999] Album/disc 1/02_-_Title.flac", "Title", "Artist", "Album", "2", "1", "1999"},
        {"Various Artists/Hits/04 - Someone Else - A Song.mp3", "A Song", "Someone Else", "Hits", "4", nullptr, nullptr},
        {"Artist/Album/09 Artist - Title.mp3", "Title", "Artist", "Album", "9", nullptr, nullptr},
        {"Artist/Album/Title - Live.flac", "Title - Live", "Artist", "Album", nullptr, nullptr, nullptr},
        {"Someone - Something.mp3", "Something", "Someone", nullptr, nullptr, nullptr, nullptr},
        {"Just_a_name.mp3", "Just a name", nullptr, nullptr, nullptr, nullptr, nullptr},
        {"a/b/c/Artist/Album/12.flac", "12", "Artist", "Album", "12", nullptr, nullptr},
        {"Artist/Album/1984.mp3", "1984", "Artist", "Album", nullptr, nullptr, nullptr},
        {"Artist/Album/\xff\xfe broken.mp3", "\xef\xbf\xbd\xef\xbf\xbd broken", "Artist", "Album", nullptr, nullptr, nullptr},
    };
    for (const Case& k : cases) {
        TagRead r = rd.path(k.path);
        auto is = [&](TagField f, const char* want) {
            const TagValue* v = tags_find(r.values, f, TagSource::Path);
            bool ok = want ? v && strcmp(v->text, want) == 0 : v == nullptr;
            if (!ok) KV_ERROR("test", "%s: %s is \"%s\", not \"%s\"", k.path, tag_field_name(f), v ? v->text : "(none)", want ? want : "(none)");
            return ok;
        };
        KV_CHECK(c, is(TagField::Title, k.title) && is(TagField::Artist, k.artist) && is(TagField::Album, k.album));
        KV_CHECK(c, is(TagField::TrackNumber, k.track) && is(TagField::DiscNumber, k.disc) && is(TagField::Date, k.date));
        KV_CHECK(c, all_utf8(r) && r.sources == (1u << (u32)TagSource::Path));
    }
}

// A hundred megabytes of art in an ID3v2 tag, and frames after it, in pieces: the frames
// are read and not one piece of the art is asked for. The same with an APE tag at the back
// and an ID3v2 tag with a footer before it.
void test_tags_pieces(TestCtx& c) {
    c.begin("tags in pieces");
    const u32 art = 100u << 20;
    for (u32 shift : {12u, 16u, 18u}) {
        Id3Opts o; o.art = 0;
        // The tag's frames, then a picture header that says 100 MB, then the frames after it.
        Buf before, after;
        {
            Buf tag(1 << 16);
            text_frame(tag, o, "TIT2", {kSong.title});
            text_frame(tag, o, "TALB", {kSong.album});
            Buf head(64); head.byte(0); head.str("image/jpeg"); head.byte(0); head.byte(3); head.byte(0);
            tag.str("APIC"); tag.syncsafe(head.n + art); tag.byte(0); tag.byte(0); tag.append(head);
            u32 art_at = 10 + tag.n;
            Buf rest(1 << 16);
            text_frame(rest, o, "TPE1", {kSong.artist});
            text_frame(rest, o, "TCOM", {kSong.composer});
            rest.zeros(100);
            u32 body = tag.n + art + rest.n;
            before.str("ID3"); before.byte(4); before.byte(0); before.byte(0); before.syncsafe(body); before.append(tag);
            // The back: the frames after the art, the audio, an ID3v2 tag with a footer, an APE tag.
            after.append(rest);
            after.byte(0xFF); after.byte(0xFB); after.zeros(5000);
            Id3Opts tail; tail.footer = true;
            Song ts = kSong; ts.title = "at the back";
            id3v2(after, tail, ts);
            Song as = kSong; as.title = "ape at the back";
            ape(after, as);
            Sparse f;
            f.make((u64)before.n + art + after.n, shift, before, after);
            u8* mem = (u8*)mem_alloc(kTagReadMemory);
            TagRead r;
            u32 waits = 0;
            bool fair = true;
            while ((r = tags_read(f.span, Span<u8>(mem, kTagReadMemory))).status == TagStatus::Wait && waits < 64) {
                ++waits;
                u64 a = r.need.at, z = r.need.at + r.need.len;
                if (a < (u64)art_at + 64 + (1ull << shift) && z > (u64)art_at + (2ull << shift)) {} // reaching in from the front is fine
                if (a > (u64)art_at + (2ull << shift) && z < (u64)art_at + art - (2ull << shift)) fair = false; // inside the art
                for (u64 k = a >> shift; k <= (z - 1) >> shift; ++k) f.give(k);
            }
            u64 inside = 0;
            for (u64 k = ((u64)art_at >> shift) + 2; k + 2 < ((u64)art_at + art) >> shift; ++k) inside += f.have[k];
            KV_CHECK(c, r.status == TagStatus::Done && fair && inside == 0 && waits < 16);
            const TagValue* t = tags_find(r.values, TagField::Title, TagSource::Id3v2);
            const TagValue* a = tags_find(r.values, TagField::Composer, TagSource::Id3v2);
            const TagValue* ap = tags_find(r.values, TagField::Title, TagSource::Ape);
            u32 titles = 0;
            for (const TagValue& v : r.values) titles += v.field == TagField::Title && v.source == TagSource::Id3v2;
            KV_CHECK(c, t && strcmp(t->text, kSong.title) == 0 && a && strcmp(a->text, kSong.composer) == 0 && titles == 2);
            KV_CHECK(c, ap && strcmp(ap->text, "ape at the back") == 0);
            KV_CHECK(c, r.pictures.len == 1 && r.pictures[0].at == art_at && r.pictures[0].len == art);
            KV_INFO("test", "tags in pieces of %u: %u waits, %llu of %llu pieces read", 1u << shift, waits,
                    (unsigned long long)f.given, (unsigned long long)f.pieces);
            // With `last`, nothing more comes: what is there is read, and the rest is a problem.
            Sparse g;
            g.make(f.span.size, shift, before, after);
            g.give(0);
            TagRead l = tags_read(g.span, Span<u8>(mem, kTagReadMemory), true);
            KV_CHECK(c, l.status == TagStatus::Done && tags_find(l.values, TagField::Title, TagSource::Id3v2) && l.problem[0]);
            g.free();
            mem_free(mem);
            f.free();
        }
    }
}

// Sizes that lie, frames past their tag, text that is not its encoding, a field a thousand
// times, both ID3 versions at once, more frames than are read: each ends as values and a
// problem, never a crash, never more than the ceilings.
void test_tags_hostile(TestCtx& c) {
    c.begin("tags hostile");
    Reader rd;
    auto check = [&](const char* what, const Buf& b, bool want_problem) {
        TagRead r = rd.read(b.bytes());
        bool ok = r.status == TagStatus::Done && all_utf8(r) && (!want_problem || r.problem[0]);
        if (!ok) KV_ERROR("test", "%s: %s", what, r.problem);
        KV_CHECK(c, ok);
        KV_INFO("test", "tags hostile: %s: %u values; %s", what, r.values.len, r.problem[0] ? r.problem : "no problem");
        return r;
    };
    // A tag that says it is longer than the file.
    { Buf b; Id3Opts o; id3v2(b, o, kSong); b.p[6] = 0x7F; check("a tag longer than its file", b, true); }
    // A frame whose size runs past its tag.
    {
        Buf b; Id3Opts o; id3v2(b, o, kSong);
        b.p[10 + 4] = 0x7F; // TIT2's size
        TagRead r = check("a frame past its tag", b, true);
        KV_CHECK(c, r.values.len == 0);
    }
    // A frame header in the middle that is not one: what came before it is kept.
    {
        Buf b; Id3Opts o; id3v2(b, o, kSong);
        u32 second = 10 + 10 + (b.p[10 + 7] | (b.p[10 + 6] << 7));
        b.p[second] = '?';
        TagRead r = check("a frame header that is not one", b, true);
        KV_CHECK(c, r.values.len == 1 && r.values[0].field == TagField::Title);
    }
    // Text that is not the encoding it claims: odd UTF-16, bad UTF-8, a lone surrogate, an encoding of 9.
    {
        Buf tag; Id3Opts o;
        Buf a(64); a.byte(1); a.byte(0xFF); a.byte(0xFE); a.byte('A'); a.byte(0); a.byte('B'); frame(tag, o, "TIT2", a);
        Buf d(64); d.byte(3); d.str("ok \xc3\x28 \xff\xfe \xe2\x82"); frame(tag, o, "TALB", d);
        Buf e(64); e.byte(2); e.byte(0xD8); e.byte(0x00); e.byte(0); e.byte('x'); frame(tag, o, "TPE1", e);
        Buf g(64); g.byte(9); g.str("Caf\xe9"); frame(tag, o, "TCOM", g);
        Buf b; b.str("ID3"); b.byte(4); b.byte(0); b.byte(0); b.syncsafe(tag.n); b.append(tag);
        TagRead r = check("text not its encoding", b, false);
        const TagValue* t = tags_find(r.values, TagField::Title, TagSource::Id3v2);
        const TagValue* al = tags_find(r.values, TagField::Album, TagSource::Id3v2);
        const TagValue* ar = tags_find(r.values, TagField::Artist, TagSource::Id3v2);
        const TagValue* co = tags_find(r.values, TagField::Composer, TagSource::Id3v2);
        KV_CHECK(c, t && strcmp(t->text, "A\xef\xbf\xbd") == 0);                       // "A", then the odd byte
        KV_CHECK(c, al && strncmp(al->text, "ok ", 3) == 0 && strstr(al->text, "\xef\xbf\xbd")); // replaced, not dropped
        KV_CHECK(c, ar && strcmp(ar->text, "\xef\xbf\xbdx") == 0);
        KV_CHECK(c, co && strcmp(co->text, "Café") == 0);
    }
    // The same field a thousand times: 64 kept.
    {
        Buf tag(1 << 20); Id3Opts o;
        for (u32 i = 0; i < 1000; ++i) { char t[32]; format(t, sizeof t, "Artist %u", i); text_frame(tag, o, "TPE1", {t}); }
        Buf b(1 << 20); b.str("ID3"); b.byte(4); b.byte(0); b.byte(0); b.syncsafe(tag.n); b.append(tag);
        TagRead r = check("a field a thousand times", b, true);
        KV_CHECK(c, r.values.len == 64);
    }
    // More frames than are read.
    {
        Buf tag(1 << 20); Id3Opts o;
        for (u32 i = 0; i < 3000; ++i) text_frame(tag, o, "TXYZ", {"x"});
        text_frame(tag, o, "TIT2", {"late"});
        Buf b(1 << 20); b.str("ID3"); b.byte(4); b.byte(0); b.byte(0); b.syncsafe(tag.n); b.append(tag);
        TagRead r = check("three thousand frames", b, true);
        KV_CHECK(c, r.values.len == 0);
    }
    // Frames that together are more than the reader asks for: it stops at its budget.
    {
        Buf tag(8u << 20); Id3Opts o;
        Buf big(200u << 10); big.byte(3); for (u32 i = 0; i < (200u << 10) - 1; ++i) big.byte('a' + i % 26);
        for (u32 i = 0; i < 30; ++i) frame(tag, o, "TIT2", big);
        Buf b(8u << 20); b.str("ID3"); b.byte(4); b.byte(0); b.byte(0); b.syncsafe(tag.n); b.append(tag);
        TagRead r = check("six megabytes of titles", b, true);
        KV_CHECK(c, r.values.len < 30); // the text it keeps runs out first, then the budget
    }
    // Both ID3 versions at once, an APE tag with lying sizes, a FLAC block past its file.
    {
        Buf b; Id3Opts o; id3v2(b, o, kSong); b.zeros(100); id3v1(b, "v1", "v1", 2, 3);
        TagRead r = check("ID3v2 and ID3v1", b, false);
        KV_CHECK(c, r.sources == ((1u << 2) | (1u << 4)));
    }
    {
        Buf b; b.zeros(100); ape(b, kSong); b.p[b.n - 32 + 12] = 0xFF; b.p[b.n - 32 + 15] = 0x7F;
        check("an APE tag longer than its file", b, true);
        Buf d; d.zeros(100); ape(d, kSong); d.p[d.n - 32 + 16] = 200; // says 200 items
        check("an APE tag with fewer items than it says", d, true);
    }
    {
        Buf b; flac(b, kSong); b.p[4 + 4 + 34 + 1] = 0x7F; check("a FLAC block past its file", b, true);
        Buf d; flac(d, kSong); d.p[4 + 4 + 34 + 4 + 23 + 4] = 0xFF; check("a Vorbis comment count that lies", d, true);
    }
    // Unsynchronised 2.3 tags, and the wrong versions.
    {
        Buf b; Id3Opts o; o.major = 3; o.unsync = true; o.art = 2000; id3v2(b, o, kSong);
        TagRead r = check("an unsynchronised 2.3 tag with art", b, false);
        KV_CHECK(c, tags_find(r.values, TagField::MbTrackId, TagSource::Id3v2) && r.pictures.len == 0);
        Buf d; d.str("ID3"); d.byte(9); d.byte(0); d.byte(0); d.syncsafe(20); d.zeros(20);
        check("ID3v2.9", d, true);
        Buf e; e.str("ID3"); e.byte(4); e.byte(0); e.byte(0); e.byte(0x80); e.zeros(20);
        check("a size that is not syncsafe", e, true);
    }
    // Nothing at all, and too little memory.
    {
        Buf b; b.zeros(10);
        KV_CHECK(c, rd.read(b.bytes()).values.len == 0 && !rd.read(b.bytes()).problem[0]);
        u8 small[1000];
        TagRead r = tags_read(sparse_whole(b.bytes()), Span<u8>(small, sizeof small));
        KV_CHECK(c, r.status == TagStatus::Done && r.problem[0]);
        KV_CHECK(c, tags_read(sparse_whole(Span<const u8>()), Span<u8>(rd.mem, kTagReadMemory)).values.len == 0);
    }
}

// Good tags with bits flipped and cut anywhere, many times over, whole and in pieces:
// every value valid UTF-8 of the length it says, every wait for a range in the file whose
// front is missing, and every read ends.
void test_tags_spoiled(TestCtx& c) {
    c.begin("tags spoiled");
    Buf good(1 << 20);
    {
        Id3Opts o; o.art = 3000;
        id3v2(good, o, kSong);
        Id3Opts o3; o3.major = 3; o3.enc = 1;
        id3v2(good, o3, kSong);
        flac(good, kSong, 2000);
        ape(good, kSong, true, 1000);
        id3v1(good, "v1", "v1", 1, 1);
    }
    u8* work = (u8*)mem_alloc(good.n);
    u8* mem = (u8*)mem_alloc(kTagReadMemory);
    Rng rng = Rng::seeded(20261123, 21);
    u32 bad = 0, values = 0, problems = 0;
    for (u32 round = 0; round < 600; ++round) {
        memcpy(work, good.p, good.n);
        u32 len = good.n;
        if (round % 4 == 3) len = rng.next() % (good.n + 1);
        else for (u32 k = 0, flips = 1 + rng.next() % 16; k < flips; ++k) work[rng.next() % good.n] ^= (u8)(1u << (rng.next() % 8));
        Buf front(len + 1); front.put(work, len);
        Buf none(1);
        Sparse f;
        f.make(len, 12, front, none);
        TagRead r;
        u32 waits = 0;
        bool fair = true;
        while ((r = tags_read(f.span, Span<u8>(mem, kTagReadMemory), waits >= 40)).status == TagStatus::Wait) {
            ++waits;
            if (r.need.len == 0 || r.need.at >= len || len - r.need.at < r.need.len || f.have[r.need.at >> 12]) { fair = false; break; }
            u64 z = r.need.at + r.need.len;
            if (round % 2) f.give(r.need.at >> 12);
            else for (u64 k = r.need.at >> 12; k <= (z - 1) >> 12; ++k) f.give(k);
            if (waits > 100) { fair = false; break; }
        }
        TagRead whole = tags_read(sparse_whole(Span<const u8>(work, len)), Span<u8>(mem, kTagReadMemory));
        if (!fair || !all_utf8(r) || r.values.len > 1024 || whole.status != TagStatus::Done || !all_utf8(whole)) ++bad;
        values += whole.values.len;
        problems += whole.problem[0] != 0;
        f.free();
    }
    KV_CHECK(c, bad == 0);
    KV_INFO("test", "tags spoiled: 600 files, %u values read, %u with a problem", values, problems);
    mem_free(work);
    mem_free(mem);
}
