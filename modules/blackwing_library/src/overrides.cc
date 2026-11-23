// Override files: read, written whole, set aside when they will not parse; and the merge.
#include "overrides.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include <stdlib.h>
#include <string.h>

namespace kv::lib {

namespace {

constexpr usize kMaxFile = 1u << 20; // an override file larger than this is not one a person wrote

const char* kHeader =
    "# Blackwing's overrides for the files in this directory. Each [name] is a file; each line\n"
    "# under it a field and the value that wins over the file's own tags. An empty value hides\n"
    "# the field. Remove a line to have the file's own value back. The fields: title, artist,\n"
    "# album, albumartist, tracknumber, tracktotal, discnumber, disctotal, date, genre,\n"
    "# composer, compilation, and the sort names (titlesort, artistsort, albumsort,\n"
    "# albumartistsort, composersort).\n";

void trim(const char*& b, const char*& e) {
    while (b < e && (*b == ' ' || *b == '\t')) ++b;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) --e;
}

} // namespace

void override_path(char* out, u32 cap, const char* data_dir, const char* root_path, const char* rel_dir) {
    const char* r = root_path[0] == '/' ? root_path + 1 : root_path;
    format(out, cap, rel_dir[0] ? "%s/overrides/%s/%s/overrides.txt" : "%s/overrides/%s%s/overrides.txt", data_dir, r, rel_dir);
}

bool override_load(const char* path, OverrideFile* out, char* problem, u32 cap) {
    out->clear();
    problem[0] = 0;
    IoBytes b;
    i32 err = 0;
    if (!io_read_now(path, &b, nullptr, &err)) return true; // none: no overrides
    if (b.size > kMaxFile) { format(problem, cap, "it is %zu bytes, more than an override file is", b.size); b.free(); return false; }
    const char* p = (const char*)b.data;
    const char* end = p + b.size;
    u32 file = ~0u;
    u32 line = 0;
    bool good = true;
    while (p < end && good) {
        const char* nl = (const char*)memchr(p, '\n', (usize)(end - p));
        const char* e = nl ? nl : end;
        ++line;
        const char* s = p, *t = e;
        trim(s, t);
        p = nl ? nl + 1 : end;
        if (s == t || *s == '#') continue;
        if (*s == '[') {
            const char* close = t;
            while (close > s && close[-1] != ']') --close;
            if (close <= s + 1) { format(problem, cap, "line %u: a [file name] without its ']'", line); good = false; break; }
            file = out->s.add(s + 1, (u32)(close - 1 - (s + 1)));
            continue;
        }
        const char* eq = (const char*)memchr(s, '=', (usize)(t - s));
        if (!eq) { format(problem, cap, "line %u: neither a [file name] nor a field = value", line); good = false; break; }
        if (file == ~0u) { format(problem, cap, "line %u: a field before any [file name]", line); good = false; break; }
        const char* kb = s, *ke = eq, *vb = eq + 1, *ve = t;
        trim(kb, ke);
        trim(vb, ve);
        char key[64];
        format(key, sizeof key, "%.*s", (int)(ke - kb), kb);
        TagField f;
        if (!tag_field_from_name(key, &f)) { format(problem, cap, "line %u: '%s' is not a field", line, key); good = false; break; }
        out->entries.push(OverrideFile::Entry{file, (u8)f, out->s.add(vb, (u32)(ve - vb))});
    }
    b.free();
    if (!good) out->clear();
    return good;
}

bool override_save(const char* path, const OverrideFile& f) {
    Array<char> text;
    auto put = [&](const char* s, u32 n) { memcpy(text.push_n(n), s, n); };
    put(kHeader, (u32)strlen(kHeader));
    // Each file once, its lines in the order they were given.
    Array<u32> done;
    for (const OverrideFile::Entry& a : f.entries) {
        bool seen = false;
        for (u32 d : done) seen |= strcmp(f.s.at(d), f.s.at(a.file)) == 0;
        if (seen) continue;
        done.push(a.file);
        put("[", 1); put(f.s.at(a.file), (u32)strlen(f.s.at(a.file))); put("]\n", 2);
        for (const OverrideFile::Entry& b : f.entries) {
            if (strcmp(f.s.at(b.file), f.s.at(a.file)) != 0) continue;
            const char* name = tag_field_name((TagField)b.field);
            const char* v = f.s.at(b.value);
            put(name, (u32)strlen(name));
            put(v[0] ? " = " : " =", v[0] ? 3 : 2);
            put(v, (u32)strlen(v));
            put("\n", 1);
        }
    }
    done.free();
    char dir[kPathMax], tmp[kPathMax + 8];
    format(dir, sizeof dir, "%s", path);
    if (char* slash = strrchr(dir, '/')) *slash = 0;
    io_make_dirs(dir);
    format(tmp, sizeof tmp, "%s.new", path);
    bool ok = f.entries.len ? io_write_now(tmp, text.data, text.len) && io_rename(tmp, path) : (io_remove(path), true);
    text.free();
    return ok;
}

void override_set_aside(const char* path, const char* problem) {
    char aside[kPathMax + 64];
    format(aside, sizeof aside, "%s.broken-%llu", path, (unsigned long long)(time_now() * 1000.0));
    io_rename(path, aside);
    KV_WARN("library", "%s will not parse (%s); it is set aside as %s, and the overrides in it are not used", path, problem, aside);
}

void override_set(OverrideFile& f, const char* file, TagField field, const char* value, bool clear) {
    u32 k = 0;
    bool placed = false;
    for (u32 i = 0; i < f.entries.len; ++i) {
        OverrideFile::Entry e = f.entries[i];
        bool mine = e.field == (u8)field && strcmp(f.s.at(e.file), file) == 0;
        if (mine && !clear && !placed) { e.value = f.s.add(value); placed = true; f.entries[k++] = e; continue; }
        if (mine) continue;
        f.entries[k++] = e;
    }
    f.entries.len = k;
    if (!clear && !placed) f.entries.push(OverrideFile::Entry{f.s.add(file), (u8)field, f.s.add(value)});
}

u32 override_values(const OverrideFile& f, const char* file, TagValue* out, u32 cap) {
    u32 n = 0;
    for (const OverrideFile::Entry& e : f.entries) {
        if (n == cap || strcmp(f.s.at(e.file), file) != 0) continue;
        const char* v = f.s.at(e.value);
        out[n++] = TagValue{(TagField)e.field, TagSource::Override, (u32)strlen(v), v};
    }
    return n;
}

void merge(Span<const TagValue> values, Merged* out) {
    TagMerge m;
    tags_merge(values, &m);
    auto get = [&](TagField f) -> const char* {
        if (m.source[(u32)f] == TagSource::Count) return "";
        const TagValue* v = tags_find(values, f, m.source[(u32)f]);
        return v ? v->text : "";
    };
    out->title = get(TagField::Title);
    out->artist = get(TagField::Artist);
    out->album = get(TagField::Album);
    out->albumartist = get(TagField::AlbumArtist);
    out->date = get(TagField::Date);
    out->genre = get(TagField::Genre);
    out->titlesort = get(TagField::TitleSort);
    out->artistsort = get(TagField::ArtistSort);
    out->albumsort = get(TagField::AlbumSort);
    out->albumartistsort = get(TagField::AlbumArtistSort);
    out->trackno = (u32)strtoul(get(TagField::TrackNumber), nullptr, 10);
    out->discno = (u32)strtoul(get(TagField::DiscNumber), nullptr, 10);
}

} // namespace kv::lib
