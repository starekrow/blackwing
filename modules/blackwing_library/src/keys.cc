// Sort keys and group identities: what the index orders by and groups on. A sort key is a
// name with its case folded and its marks stripped, for Latin, Greek and Cyrillic (fold.h);
// other scripts are kept as they are, so they sort by code point after those. Leading
// articles are not moved (the PRD's Open). A group's identity is a 64-bit hash of its key.
#include "index.h"
#include "fold.h"
#include "korvid/core/utf8.h"
#include <string.h>

namespace kv::lib {

u32 sort_key(const char* s, char* out, u32 cap) {
    u32 n = 0;
    auto put = [&](const char* p, u32 k) { if (n + k + 1 <= cap) { memcpy(out + n, p, k); n += k; } };
    for (usize at = 0, len = strlen(s); at < len;) {
        u32 step = 0;
        u32 cp = utf8_decode(s + at, len - at, &step);
        at += step ? step : 1;
        if (cp < 0x80) { char c = (char)(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp); put(&c, 1); continue; }
        if (cp >= 0x300 && cp < 0x370) continue; // a combining mark
        if (cp >= kFoldFirst && cp < kFoldEnd && kFold[cp - kFoldFirst]) { const char* f = kFold[cp - kFoldFirst]; put(f, (u32)strlen(f)); continue; }
        char b[KV_UTF8_MAX_BYTES];
        put(b, utf8_encode(cp, b));
    }
    // Leading spaces and punctuation do not sort first.
    u32 skip = 0;
    while (skip < n && (out[skip] == ' ' || out[skip] == '"' || out[skip] == '\'' || out[skip] == '(' || out[skip] == '[')) ++skip;
    if (skip) memmove(out, out + skip, n - skip);
    n -= skip;
    if (cap) out[n] = 0;
    return n;
}

u64 key_hash(const char* a, const char* b) {
    u64 h = 1469598103934665603ull;
    for (const char* p = a; *p; ++p) { h ^= (u8)*p; h *= 1099511628211ull; }
    h ^= 0x1f; h *= 1099511628211ull;
    for (const char* p = b; p && *p; ++p) { h ^= (u8)*p; h *= 1099511628211ull; }
    return h & 0x7fffffffffffffffull; // positive in sqlite's INTEGER, never 0 for a real key
}

void group_keys(const GroupIn& in, GroupOut* out) {
    sort_key(in.titlesort[0] ? in.titlesort : in.title, out->sort_title, sizeof out->sort_title);
    const char* artist = in.albumartistsort[0] ? in.albumartistsort : in.albumartist[0] ? in.albumartist : in.artistsort[0] ? in.artistsort : in.artist;
    sort_key(artist, out->sort_artist, sizeof out->sort_artist);
    sort_key(in.albumsort[0] ? in.albumsort : in.album, out->sort_album, sizeof out->sort_album);
    sort_key(in.genre, out->sort_genre, sizeof out->sort_genre);
    out->artist_hash = key_hash("artist", out->sort_artist);
    out->genre_hash = out->sort_genre[0] ? key_hash("genre", out->sort_genre) : 0;
    if (!out->sort_album[0]) out->album_hash = 0;
    else if (in.albumartist[0]) out->album_hash = key_hash(out->sort_artist, out->sort_album);
    else {
        char d[24];
        u32 k = 0;
        for (u64 v = in.dir; v || !k; v /= 10) d[k++] = (char)('0' + v % 10);
        d[k] = 0;
        out->album_hash = key_hash(d, out->sort_album); // no album artist: an album is its title in its directory
    }
}

} // namespace kv::lib
