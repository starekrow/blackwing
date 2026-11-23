// The guess from a path: the file's name and the directories above it, in the layouts
// people use. The lowest rank, so it is only seen where nothing better says otherwise.
#include "reader.h"
#include <string.h>

namespace kv {
namespace tags {
namespace {

struct Str {
    const char* p = nullptr;
    u32 n = 0;
    bool empty() const { return n == 0; }
};

Str trim(Str s) {
    while (s.n && (s.p[0] == ' ' || s.p[0] == '_' || s.p[0] == '.' || s.p[0] == '-')) { ++s.p; --s.n; }
    while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '_')) --s.n;
    return s;
}

bool digit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

// " - " in s: where, or -1.
i32 dash(Str s) {
    for (u32 i = 0; i + 3 <= s.n; ++i)
        if (s.p[i] == ' ' && s.p[i + 1] == '-' && s.p[i + 2] == ' ') return (i32)i;
    return -1;
}

bool same(Str a, Str b) {
    if (a.n != b.n) return false;
    for (u32 i = 0; i < a.n; ++i) if (lower(a.p[i]) != lower(b.p[i])) return false;
    return true;
}

// "CD2", "Disc 1", "disk_03": the number, or 0.
u32 disc_dir(Str s) {
    static const char* const kWords[] = {"cd", "disc", "disk"};
    for (const char* w : kWords) {
        u32 n = (u32)strlen(w);
        if (s.n <= n) continue;
        bool match = true;
        for (u32 i = 0; i < n && match; ++i) match = lower(s.p[i]) == w[i];
        if (!match) continue;
        u32 i = n;
        while (i < s.n && (s.p[i] == ' ' || s.p[i] == '_' || s.p[i] == '-')) ++i;
        if (i == s.n || s.n - i > 3) continue;
        u32 v = 0;
        for (; i < s.n; ++i) { if (!digit(s.p[i])) { v = 0; break; } v = v * 10 + (u32)(s.p[i] - '0'); }
        if (v) return v;
    }
    return 0;
}

// A year at either end of an album's directory: "Album (2003)", "Album [2003]",
// "2003 - Album", "[2003] Album". Takes it off and says it.
Str year(Str s, Str* y) {
    auto is_year = [](const char* p) {
        return digit(p[0]) && digit(p[1]) && digit(p[2]) && digit(p[3]) && (p[0] == '1' || p[0] == '2');
    };
    if (s.n >= 7 && (s.p[s.n - 1] == ')' || s.p[s.n - 1] == ']') && (s.p[s.n - 6] == '(' || s.p[s.n - 6] == '[') && is_year(s.p + s.n - 5)) {
        *y = Str{s.p + s.n - 5, 4};
        return trim(Str{s.p, s.n - 6});
    }
    if (s.n >= 7 && (s.p[0] == '(' || s.p[0] == '[') && (s.p[5] == ')' || s.p[5] == ']') && is_year(s.p + 1)) {
        *y = Str{s.p + 1, 4};
        return trim(Str{s.p + 6, s.n - 6});
    }
    if (s.n >= 7 && is_year(s.p) && (s.p[4] == ' ' || s.p[4] == '-' || s.p[4] == '.') ) {
        Str rest = trim(Str{s.p + 4, s.n - 4});
        if (!rest.empty()) { *y = Str{s.p, 4}; return rest; }
    }
    return s;
}

} // namespace
} // namespace tags

using namespace tags;

TagRead tags_from_path(const char* path, Span<u8> memory) {
    Out o;
    {
        constexpr u32 kAlign = 16;
        usize skew = (usize)(-(intptr_t)memory.data) & (kAlign - 1);
        usize need = skew + sizeof(TagValue) * 16 + Out::kProblem + 4096;
        if (!memory.data || memory.len < need) { TagRead r; r.problem = "the tag reader was given too little memory"; return r; }
        // Only a few values and the path's own length of text: no scratch needed.
        u8* p = memory.data + skew;
        o.values = (TagValue*)p; p += sizeof(TagValue) * 16;
        o.problem_buf = (char*)p; p += Out::kProblem;
        o.text = (char*)p;
        o.cap = (u32)(memory.data + memory.len - p);
    }
    o.sources = 1u << (u32)TagSource::Path;

    // The last four names: the file, and up to three directories above it.
    Str part[4];
    u32 n = 0;
    const char* end = path + strlen(path);
    const char* e = end;
    while (e > path && n < 4) {
        const char* b = e;
        while (b > path && b[-1] != '/') --b;
        if (e > b) part[n++] = Str{b, (u32)(e - b)};
        e = b > path ? b - 1 : b;
    }
    if (n == 0) { TagRead r; r.sources = o.sources; return r; }

    // The name, without its extension, with underscores for spaces where it has none.
    Str name = part[0];
    for (u32 i = name.n; i-- > 1;) {
        if (name.p[i] == '.') { if (name.n - i <= 6) name.n = i; break; }
    }
    char nb[1024];
    u32 nn = name.n < sizeof nb ? name.n : (u32)sizeof nb;
    memcpy(nb, name.p, nn);
    if (!memchr(nb, ' ', nn)) for (u32 i = 0; i < nn; ++i) if (nb[i] == '_') nb[i] = ' ';
    Str s{nb, nn};

    Str d1 = n > 1 ? part[1] : Str{}, d2 = n > 2 ? part[2] : Str{}, d3 = n > 3 ? part[3] : Str{};
    u32 disc = disc_dir(d1);
    if (disc) { d1 = d2; d2 = d3; }

    Str album, dir_artist, date;
    if (!d1.empty()) {
        Str a = year(d1, &date);
        i32 k = dash(a);
        if (k > 0) { dir_artist = trim(Str{a.p, (u32)k}); album = year(trim(Str{a.p + k + 3, a.n - (u32)k - 3}), &date); }
        else { album = a; dir_artist = d2; }
    }

    // The name: "1-03 Title", "03 - Title", "03. Title", "03 Artist - Title", "Artist - Title".
    Str track, title = trim(s), name_artist;
    u32 i = 0;
    if (s.n >= 4 && digit(s.p[0]) && (s.p[1] == '-' || s.p[1] == '.') && digit(s.p[2]) && digit(s.p[3]) && (s.n == 4 || !digit(s.p[4]))) {
        if (!disc) disc = (u32)(s.p[0] - '0');
        track = Str{s.p + 2, 2};
        i = 4;
    } else {
        while (i < s.n && i < 4 && digit(s.p[i])) ++i;
        if (i >= 1 && i <= 3 && (i == s.n || s.p[i] == ' ' || s.p[i] == '.' || s.p[i] == '-' || s.p[i] == '_' || s.p[i] == ')')) track = Str{s.p, i};
        else i = 0;
    }
    if (!track.empty()) {
        Str rest = trim(Str{s.p + i, s.n - i});
        if (rest.p < s.p + s.n && rest.p[0] == ')') rest = trim(Str{rest.p + 1, rest.n - 1});
        title = rest.empty() ? title : rest;
    }
    i32 k = dash(title);
    if (k > 0) {
        Str left = trim(Str{title.p, (u32)k}), right = trim(Str{title.p + k + 3, title.n - (u32)k - 3});
        if (!right.empty()) {
            if (!dir_artist.empty() && same(left, dir_artist)) title = right;                           // the artist again
            else if (dir_artist.empty() || !track.empty()) { name_artist = left; title = right; }      // a compilation's, or no directory to say
            // otherwise a title with a dash in it
        }
    }

    auto put = [&](TagField f, Str v) { if (!v.empty()) o.utf8(TagSource::Path, f, (const u8*)v.p, v.n); };
    put(TagField::Title, title);
    put(TagField::Artist, !name_artist.empty() ? name_artist : dir_artist);
    put(TagField::AlbumArtist, dir_artist);
    put(TagField::Album, album);
    if (!track.empty()) o.numbers(TagSource::Path, TagField::TrackNumber, TagField::Count, track.p, track.n);
    if (disc) {
        char b[8];
        u32 m = 0;
        if (disc >= 100) b[m++] = (char)('0' + disc / 100 % 10);
        if (disc >= 10) b[m++] = (char)('0' + disc / 10 % 10);
        b[m++] = (char)('0' + disc % 10);
        o.utf8(TagSource::Path, TagField::DiscNumber, (const u8*)b, m);
    }
    put(TagField::Date, date);

    TagRead r;
    r.values = Span<const TagValue>(o.values, o.nvalues);
    r.sources = o.sources;
    r.problem = o.problem;
    return r;
}

} // namespace kv
