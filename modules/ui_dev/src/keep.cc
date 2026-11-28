// The face's part of the session. The session keeps values for whoever asks, by an
// owner's name and a key, and says them back at the next start; it does not know what a
// face is. This face is "ui_dev", and keeps:
//
//   w, h        the window's size, in the units a window is opened in
//   x, y        its place, in desktop pixels, where the platform lets a program place it
//   tab         which inspector is up
//   lib_sort    how the library's list is sorted
//   lib_text    what is searched for
//   lib_where   where the list is: each step of the way there, as "view parent name"
//
// A value is asked to be kept when it is not the one last asked. The session saves a few
// seconds after the last change, so a window dragged to its size is one write.
#include "face.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr const char* kOwner = "ui_dev";
enum KeepKey : u32 { KeepW, KeepH, KeepX, KeepY, KeepTab, KeepSort, KeepText, KeepWhere, KeepCount };
constinit const char* const k_keep_keys[KeepCount] = {"w", "h", "x", "y", "tab", "lib_sort", "lib_text", "lib_where"};
static_assert(KeepCount <= sizeof(((Face*)nullptr)->kept_sent) / sizeof(u64), "Face::kept_sent holds a hash per key");

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

// The window put back where it was, and kept where it can be seen. A place on a display
// that is no longer there (a monitor unplugged since) lands where the OS puts it; if less
// than a corner of the window is then in the work area of the display it is most on, it is
// moved into it.
void place_window(Face& f, HostWindow* w) {
    if (f.place == Face::Place::Asked && f.place_have == 3) {
        f.place = w->move_to(f.place_x, f.place_y) ? Face::Place::Moved : Face::Place::None;
        KV_INFO("ui_dev", "the window put back at %d, %d%s", f.place_x, f.place_y, f.place == Face::Place::Moved ? "" : ": the platform does not let a program place it");
        f.place_asked = time_now();
        return;
    }
    if (f.place != Face::Place::Moved) return;
    i32 x = 0, y = 0;
    if (!w->position(&x, &y)) { f.place = Face::Place::None; return; }
    if (!(x == f.place_x && y == f.place_y) && time_now() - f.place_asked < 1.0) return; // not landed yet: a second at most
    f.place = Face::Place::None;
    rect2 work;
    if (!w->display_bounds(&work) || work.w <= 0.0f || work.h <= 0.0f) { KV_INFO("ui_dev", "the window is at %d, %d; its display's work area is not known", x, y); return; }
    const f32 need = 64.0f; // pixels of the window that must be in the work area, each way
    f32 l = (f32)x, t = (f32)y, r = l + (f32)w->width(), b = t + (f32)w->height();
    f32 seen_w = (r < work.x + work.w ? r : work.x + work.w) - (l > work.x ? l : work.x);
    f32 seen_h = (b < work.y + work.h ? b : work.y + work.h) - (t > work.y ? t : work.y);
    if (seen_w >= need && seen_h >= need && t >= work.y) return; // its top in the work area: it can be dragged
    f32 nx = l, ny = t;
    if (nx + (f32)w->width() > work.x + work.w) nx = work.x + work.w - (f32)w->width();
    if (nx < work.x) nx = work.x;
    if (ny + (f32)w->height() > work.y + work.h) ny = work.y + work.h - (f32)w->height();
    if (ny < work.y) ny = work.y;
    KV_INFO("ui_dev", "the window landed at %d, %d, out of sight of its display's work area (%.0f, %.0f, %.0f x %.0f): moved to %.0f, %.0f", x, y, (f64)work.x, (f64)work.y, (f64)work.w, (f64)work.h, (f64)nx, (f64)ny);
    w->move_to((i32)nx, (i32)ny);
}

const char* tab_name(FaceTab t) { return t == FaceTab::Scan ? "scan" : t == FaceTab::Events ? "events" : t == FaceTab::Tables ? "tables" : t == FaceTab::Media ? "media" : "chain"; }

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
        place_window(f, w);
        i32 x = 0, y = 0;
        if (f.place == Face::Place::None && w->position(&x, &y)) { // not while one is being put back
            keep(ctx, f, KeepX, (f64)x, "");
            keep(ctx, f, KeepY, (f64)y, "");
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
    if (!strcmp(kept.key, "x") || !strcmp(kept.key, "y")) {
        if (kept.number < -1.0e6 || kept.number > 1.0e6) return;
        if (kept.key[0] == 'x') { f.place_x = (i32)kept.number; f.place_have |= 1; }
        else                    { f.place_y = (i32)kept.number; f.place_have |= 2; }
        if (f.place_have == 3 && f.place == Face::Place::None) f.place = Face::Place::Asked;
    } else if (!strcmp(kept.key, "tab")) {
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
