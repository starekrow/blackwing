// The test library's generator: a tree of small, tagged, playable FLAC files, made into a
// directory by the tests. Each file is a tenth of a second of silence (one FLAC frame of
// constant subframes, CRCs and all) behind a Vorbis comment block, about 300 bytes. The
// tree has a few layouts and names in several scripts, deterministic from its seed.
#pragma once
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/platform/io.h"
#include <string.h>

namespace kvtest {

using namespace kv;

inline u8 crc8(const u8* p, u32 n) {
    u8 c = 0;
    for (u32 i = 0; i < n; ++i) { c ^= p[i]; for (u32 k = 0; k < 8; ++k) c = (u8)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1); }
    return c;
}
inline u16 crc16(const u8* p, u32 n) {
    u16 c = 0;
    for (u32 i = 0; i < n; ++i) { c ^= (u16)(p[i] << 8); for (u32 k = 0; k < 8; ++k) c = (u16)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1); }
    return c;
}

struct Bytes {
    u8 b[8192]; u32 n = 0;
    void put(const void* d, u32 k) { if (n + k <= sizeof b) { memcpy(b + n, d, k); n += k; } }
    void byte(u32 v) { u8 x = (u8)v; put(&x, 1); }
    void be24(u32 v) { byte(v >> 16); byte(v >> 8); byte(v); }
    void le32(u32 v) { byte(v); byte(v >> 8); byte(v >> 16); byte(v >> 24); }
    void str(const char* s) { put(s, (u32)strlen(s)); }
};

struct Tags { const char* title; const char* artist; const char* album; u32 track; u32 tracks; u32 disc; const char* date; const char* genre; };

// A FLAC file: 4096 frames of 16-bit stereo silence at 44.1 kHz, and these tags.
inline void flac_file(Bytes& f, const Tags& t) {
    f.n = 0;
    f.str("fLaC");
    // STREAMINFO: blocks of 4096, 44100 Hz, 2 channels, 16 bits, 4096 frames, no MD5.
    f.byte(0x00); f.be24(34);
    f.byte(0x10); f.byte(0x00); f.byte(0x10); f.byte(0x00); // min and max block
    f.be24(0); f.be24(0);                                   // min and max frame: not said
    u64 packed = ((u64)44100 << 44) | ((u64)1 << 41) | ((u64)15 << 36) | 4096;
    for (int i = 7; i >= 0; --i) f.byte((u32)(packed >> (i * 8)));
    for (int i = 0; i < 16; ++i) f.byte(0);
    // VORBIS_COMMENT, the last block.
    Bytes c;
    c.le32(9); c.str("blackwing");
    char lines[8][320];
    u32 nl = 0;
    format(lines[nl++], 320, "TITLE=%s", t.title);
    format(lines[nl++], 320, "ARTIST=%s", t.artist);
    format(lines[nl++], 320, "ALBUM=%s", t.album);
    format(lines[nl++], 320, "TRACKNUMBER=%u", t.track);
    format(lines[nl++], 320, "TRACKTOTAL=%u", t.tracks);
    format(lines[nl++], 320, "DISCNUMBER=%u", t.disc);
    format(lines[nl++], 320, "DATE=%s", t.date);
    format(lines[nl++], 320, "GENRE=%s", t.genre);
    c.le32(nl);
    for (u32 i = 0; i < nl; ++i) { c.le32((u32)strlen(lines[i])); c.str(lines[i]); }
    f.byte(0x84); f.be24(c.n); f.put(c.b, c.n);
    // One frame: fixed blocking, 4096 (code 12), 44.1 kHz (code 9), left/right, 16 bits,
    // frame 0; a CRC-8; two constant subframes of 0; a CRC-16.
    u32 at = f.n;
    f.byte(0xFF); f.byte(0xF8); f.byte(0xC9); f.byte(0x18); f.byte(0x00);
    f.byte(crc8(f.b + at, 5));
    for (u32 ch = 0; ch < 2; ++ch) { f.byte(0x00); f.byte(0x00); f.byte(0x00); }
    u16 crc = crc16(f.b + at, f.n - at);
    f.byte(crc >> 8); f.byte(crc);
}

// Names in a few scripts, so sorting and search meet more than ASCII.
constexpr const char* kWords[] = {
    "Blue", "Night", "River", "Morning", "Glass", "Echo", "Northern", "Silver", "Paper", "Ocean",
    "Café", "Señor", "Über", "Ångström", "Œuvre", "Москва", "Ночь", "Река", "東京", "夜明け",
    "Αθήνα", "Νύχτα", "Kraków", "Žena", "Çay", "Ísland", "Fjørd", "Déjà", "Naïve", "Smörgås",
};
constexpr u32 kNWords = sizeof kWords / sizeof kWords[0];

struct Made { u64 files = 0, bytes = 0, dirs = 0; };

inline u32 mix(u32 x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }

// Album a's artist, title, year and directory, as make_library makes them.
struct Album { char artist[160]; char title[200]; u32 year; u32 layout; char dir[2048]; };
inline void album(const char* root, u32 a, u64 seed, Album* out) {
    u32 ar = a / 9; // nine albums to an artist
    format(out->artist, sizeof out->artist, "%s %s %u", kWords[ar % kNWords], kWords[(ar / kNWords + 3) % kNWords], ar);
    u32 w1 = mix(a * 2 + (u32)seed) % kNWords, w2 = mix(a * 2 + 1 + (u32)seed * 7919) % kNWords;
    format(out->title, sizeof out->title, "%s %s", kWords[w1], kWords[w2]);
    out->year = 1960 + a % 60;
    out->layout = a % 4;
    if (out->layout == 0)      format(out->dir, sizeof out->dir, "%s/%s/%s", root, out->artist, out->title);
    else if (out->layout == 1) format(out->dir, sizeof out->dir, "%s/%s - %s", root, out->artist, out->title);
    else if (out->layout == 2) format(out->dir, sizeof out->dir, "%s/%s/%s (%u)/CD1", root, out->artist, out->title, out->year);
    else                       format(out->dir, sizeof out->dir, "%s/Compilations/%s %u", root, out->title, a);
}

// Makes `n` files under `root` (which exists), `per_album` to an album, in a few layouts.
// The i-th file's title has the word "Track" and its number, so one can be found by name:
// "Zq<number>" is in exactly one title.
inline Made make_library(const char* root, u32 n, u32 per_album = 12, u64 seed = 1) {
    Made m;
    Bytes f;
    char path[2600], title[200];
    u32 albums = (n + per_album - 1) / per_album;
    for (u32 a = 0; a < albums; ++a) {
        Album al;
        album(root, a, seed, &al);
        io_make_dirs(al.dir);
        ++m.dirs;
        for (u32 t = 0; t < per_album && (u64)a * per_album + t < n; ++t) {
            u32 i = a * per_album + t;
            format(title, sizeof title, "%s Track Zq%u", kWords[(i * 7) % kNWords], i);
            char yr[8];
            format(yr, sizeof yr, "%u", al.year);
            Tags tg{title, al.artist, al.title, t + 1, per_album, 1, yr, "Electronic"};
            flac_file(f, tg);
            if (al.layout == 3) format(path, sizeof path, "%s/%02u - %s - %s.flac", al.dir, t + 1, al.artist, title);
            else                format(path, sizeof path, "%s/%02u - %s.flac", al.dir, t + 1, title);
            io_write_now(path, f.b, f.n);
            ++m.files;
            m.bytes += f.n;
        }
    }
    return m;
}

} // namespace kvtest
