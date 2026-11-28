// The art for a track: an image beside the file, or the front cover in its tags written
// once to the cache. Blocking, for the module's own thread.
#include "korvid/blackwing_media/media.h"
#include "korvid/blackwing_tags/tags.h"
#include "korvid/hifi_codec/sparse.h"
#include "korvid/core/format.h"
#include "korvid/core/hash.h"
#include "korvid/core/memory.h"
#include "korvid/platform/io.h"
#include <string.h>

namespace kv {

namespace {

constexpr u64 kHeadBytes = 16u << 20; // a tag's art is in its first megabytes: no more is read for it

// How good a name is for a cover: higher is better, 0 is not one.
u32 cover_rank(const char* name) {
    static const char* const stems[] = {"cover", "folder", "front", "album", "albumart"};
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    usize stem = (usize)(dot - name);
    char ext[8] = {};
    for (u32 i = 0; i < 7 && dot[1 + i]; ++i) ext[i] = (char)(dot[1 + i] | 0x20);
    if (strcmp(ext, "jpg") != 0 && strcmp(ext, "jpeg") != 0 && strcmp(ext, "png") != 0) return 0;
    for (u32 k = 0; k < KV_COUNTOF(stems); ++k) {
        usize n = strlen(stems[k]);
        if (stem != n) continue;
        bool same = true;
        for (usize i = 0; i < n && same; ++i) same = (char)(name[i] | 0x20) == stems[k][i];
        if (same) return (u32)(KV_COUNTOF(stems) - k) * 2 + (ext[0] == 'j' ? 1 : 0); // a jpg over a png of the same name
    }
    return 0;
}

struct Beside {
    u32  best = 0;
    char name[256] = "";
};

bool look(void* user, const IoDirEntry& e) {
    Beside& b = *(Beside*)user;
    if (e.st.is_dir()) return true;
    u32 r = cover_rank(e.name);
    if (r > b.best && strlen(e.name) < sizeof b.name) { b.best = r; format(b.name, sizeof b.name, "%s", e.name); }
    return true;
}

struct Head {
    u8* data = nullptr;
    u64 len = 0;
    u64 cap = 0;
};

bool take(void* user, const u8* data, usize n) {
    Head& h = *(Head*)user;
    u64 room = h.cap - h.len;
    u64 k = n < room ? n : room;
    memcpy(h.data + h.len, data, (usize)k);
    h.len += k;
    return h.len < h.cap;
}

} // namespace

bool media_find_art(const char* path, const char* art_dir, char* out, u32 cap) {
    out[0] = 0;
    if (!path || !path[0]) return false;
    // Beside it.
    const char* slash = strrchr(path, '/');
    if (slash) {
        char dir[4096];
        format(dir, sizeof dir, "%.*s", (int)(slash - path), path);
        Beside b;
        io_list_dir(dir[0] ? dir : "/", 0, look, &b);
        if (b.best) { format(out, cap, "%s/%s", dir, b.name); return true; }
    }
    // In its tags: the front cover, else the first picture.
    if (!art_dir || !art_dir[0]) return false;
    IoStat st;
    if (!io_stat_file(path, &st) || st.size == 0) return false;
    char file[1024];
    u64 key = hash_str(path) ^ hash_mix(st.size);
    Head h;
    h.cap = st.size < kHeadBytes ? st.size : kHeadBytes;
    h.data = (u8*)mem_alloc((usize)h.cap);
    io_read_each(path, take, &h, 1u << 20);
    // The span says the file's length and has only its head: the reader asks for nothing
    // past it, and anything it would want there is not found.
    constexpr u32 kShift = 24;
    static_assert((1ull << kShift) == kHeadBytes, "one piece, the head");
    u64 pieces = ((st.size - 1) >> kShift) + 1;
    u8* have = (u8*)mem_alloc((usize)pieces);
    memset(have, 0, (usize)pieces);
    have[0] = h.len == h.cap ? 1 : 0;
    SparseSpan span{h.data, st.size, kShift, have};
    u8* mem = (u8*)mem_alloc(kTagReadMemory);
    TagRead r = tags_read(span, Span<u8>(mem, kTagReadMemory), true, false);
    const TagPicture* pick = nullptr;
    for (const TagPicture& p : r.pictures) {
        if (p.at + p.len > h.len || p.len == 0) continue;
        if (!pick || (p.kind == 3 && pick->kind != 3)) pick = &p;
    }
    bool ok = false;
    if (pick) {
        bool png = strstr(pick->mime, "png") || strstr(pick->mime, "PNG");
        format(file, sizeof file, "%s/%016llx.%s", art_dir, (unsigned long long)key, png ? "png" : "jpg");
        IoStat there;
        ok = io_stat_file(file, &there) && there.size == pick->len; // written already
        if (!ok) {
            io_make_dirs(art_dir);
            char tmp[1100];
            format(tmp, sizeof tmp, "%s.part", file);
            ok = io_write_file(tmp, h.data + pick->at, (usize)pick->len) && io_rename(tmp, file);
        }
        if (ok) format(out, cap, "%s", file);
    }
    mem_free(mem);
    mem_free(have);
    mem_free(h.data);
    return ok;
}

} // namespace kv
