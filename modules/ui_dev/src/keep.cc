// The face's part of the session. The session keeps values for whoever asks, by an
// owner's name and a key, and says them back at the next start; it does not know what a
// face is. This face is "ui_dev", and keeps:
//
//   w, h        the window's size, in the units a window is opened in
//   tab         which inspector is up
//   lib_sort    how the library's list is sorted
//   lib_text    what is searched for
//   lib_where   where the list is: each step of the way there, as "view parent name"
//
// A value is asked to be kept when it is not the one last asked. The session saves a few
// seconds after the last change, so a window dragged to its size is one write.
#include "face.h"
#include "korvid/core/format.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr const char* kOwner = "ui_dev";
enum KeepKey : u32 { KeepW, KeepH, KeepTab, KeepSort, KeepText, KeepWhere, KeepCount };
constinit const char* const k_keep_keys[KeepCount] = {"w", "h", "tab", "lib_sort", "lib_text", "lib_where"};

u64 keep_hash(f64 number, const char* text) {
    u64 h = 14695981039346656037ull;
    const u8* b = (const u8*)&number;
    for (usize i = 0; i < sizeof number; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    for (; *text; ++text) { h ^= (u8)*text; h *= 1099511628211ull; }
    return h ? h : 1;
}

void keep(TickContext& ctx, Face& f, KeepKey key, f64 number, const char* text) {
    u64 h = keep_hash(number, text);
    if (f.kept_sent[key] == h) return;
    f.kept_sent[key] = h;
    SessionKeep k;
    format(k.owner, sizeof k.owner, "%s", kOwner);
    format(k.key, sizeof k.key, "%s", k_keep_keys[key]);
    k.number = number;
    format(k.text, sizeof k.text, "%s", text);
    ctx.emit(k);
}

const char* tab_name(FaceTab t) { return t == FaceTab::Scan ? "scan" : t == FaceTab::Events ? "events" : t == FaceTab::Tables ? "tables" : "chain"; }

} // namespace

void keep_sync(TickContext& ctx, Face& f) {
    // The window's size as it would be asked for: its pixels over its scale.
    if (HostWindow* w = platform_window(*f.e)) {
        f32 scale = w->pixel_scale();
        if (!(scale > 0.0f)) scale = 1.0f;
        if (w->width() && w->height()) {
            keep(ctx, f, KeepW, (f64)(u32)((f32)w->width() / scale + 0.5f), "");
            keep(ctx, f, KeepH, (f64)(u32)((f32)w->height() / scale + 0.5f), "");
        }
    }
    if (!f.tab_restore[0]) keep(ctx, f, KeepTab, 0.0, tab_name(f.tab)); // not while one is still to be put up
    keep(ctx, f, KeepSort, (f64)(u8)f.lib_sort, "");
    keep(ctx, f, KeepText, 0.0, f.lib_text);
    // Where the list is. What does not fit is left off its end: the list opens a step or two up.
    char where[256];
    usize n = 0;
    where[0] = 0;
    for (u32 i = 0; i < f.ncrumbs; ++i) {
        char step[160];
        int len = format(step, sizeof step, "%u %llu %.60s\n", (u32)(u8)f.crumbs[i].view, (unsigned long long)f.crumbs[i].parent, f.crumbs[i].name);
        if (len < 0 || n + (usize)len >= sizeof where) break;
        memcpy(where + n, step, (usize)len + 1);
        n += (usize)len;
    }
    keep(ctx, f, KeepWhere, 0.0, where);
}

void keep_restore(Face& f, const SessionKept& kept) {
    if (strcmp(kept.owner, kOwner) != 0) return;
    // What comes back is what is kept: it is not asked for again.
    for (u32 k = 0; k < KeepCount; ++k) if (strcmp(kept.key, k_keep_keys[k]) == 0) f.kept_sent[k] = keep_hash(kept.number, kept.text);
    if (!strcmp(kept.key, "tab")) {
        if (!strcmp(kept.text, "scan") || !strcmp(kept.text, "events") || !strcmp(kept.text, "tables") || !strcmp(kept.text, "chain")) format(f.tab_restore, sizeof f.tab_restore, "%s", kept.text);
    } else if (!strcmp(kept.key, "lib_sort")) {
        if (kept.number >= 0.0 && kept.number < (f64)(u8)LibrarySort::Count) f.lib_sort = (LibrarySort)(u8)kept.number;
        f.lib_stale = true;
    } else if (!strcmp(kept.key, "lib_text")) {
        format(f.lib_text, sizeof f.lib_text, "%s", kept.text);
        face_forget(f, "l_search");
        f.lib_stale = true;
    } else if (!strcmp(kept.key, "lib_where")) {
        u32 n = 0;
        FaceCrumb crumbs[kFaceCrumbs];
        for (const char* p = kept.text; *p && n < kFaceCrumbs;) {
            char* end = nullptr;
            unsigned long view = strtoul(p, &end, 10);
            if (end == p || *end != ' ' || view >= (unsigned long)(u8)LibraryView::Count) break;
            p = end + 1;
            unsigned long long parent = strtoull(p, &end, 10);
            if (end == p || *end != ' ') break;
            p = end + 1;
            const char* line = strchr(p, '\n');
            if (!line) break;
            FaceCrumb& c = crumbs[n++];
            c.view = (LibraryView)(u8)view;
            c.parent = parent;
            format(c.name, sizeof c.name, "%.*s", (int)(line - p), p);
            p = line + 1;
        }
        if (n) { for (u32 i = 0; i < n; ++i) f.crumbs[i] = crumbs[i]; f.ncrumbs = n; f.lib_first = 0; f.lib_stale = true; }
    }
}

} // namespace kv
