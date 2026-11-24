// The Library panel: library.roots and a window of library.rows into the model; its
// buttons into the library's commands; and the queue's rows asked of the library by their
// paths, which is where their titles come from.
//
// A window onto the library is open only while its list is wanted: it is asked for where
// the list is (the rows Krome's sparse list has built), a newer ask replacing the one
// before, and it is closed when the window is hidden. When the list has been at rest for
// a moment, what it shows is what the library is told to scan next (LibraryPrioritize).
#include "face.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/arena.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"
#include "korvid/core/time.h"
#include <string.h>

namespace kv {

namespace {

constexpr f64 kRestSeconds = 0.3;  // a list this long in one place is being looked at
constexpr u32 kTitleRestTicks = 3; // the queue's view this long in one place: its paths are asked

u64 lib_hash(const char* s) {
    u64 h = 14695981039346656037ull;
    for (; *s; ++s) { h ^= (u8)*s; h *= 1099511628211ull; }
    return h ? h : 1;
}

const char* view_key(LibraryView v) {
    switch (v) {
    case LibraryView::Artists: case LibraryView::Albums: case LibraryView::AlbumTracks: return "artists";
    case LibraryView::Genres:  return "genres";
    case LibraryView::Tracks:  return "tracks";
    case LibraryView::Search:  return "search";
    default:                   return "folders";
    }
}
const char* sort_key(LibrarySort s) {
    switch (s) {
    case LibrarySort::Title:  return "title";
    case LibrarySort::Artist: return "artist";
    case LibrarySort::Album:  return "album";
    case LibrarySort::Date:   return "date";
    case LibrarySort::Path:   return "path";
    default:                  return "default";
    }
}
const char* root_state_text(u8 s) {
    switch ((LibraryRootState)s) {
    case LibraryRootState::Walking: return "walking";
    case LibraryRootState::Reading: return "reading";
    case LibraryRootState::Idle:    return "idle";
    case LibraryRootState::Offline: return "offline";
    default:                        return "?";
    }
}
const char* kind_text(u8 k) {
    switch ((LibraryRowKind)k) {
    case LibraryRowKind::Directory: return "dir";
    case LibraryRowKind::Track:     return "track";
    case LibraryRowKind::Artist:    return "artist";
    case LibraryRowKind::Album:     return "album";
    case LibraryRowKind::Genre:     return "genre";
    default:                        return "?";
    }
}

FaceCrumb& here(Face& f) { return f.crumbs[f.ncrumbs - 1]; }

// The list goes somewhere else: its place is new, and what the table holds is the old one's.
void moved(Face& f) {
    f.lib_stale = true;
    f.lib_answered = false;
    f.lib_first = 0;
    f.awake = 4;
}
void go_top(Face& f, LibraryView view, const char* name) {
    f.ncrumbs = 1;
    f.crumbs[0] = FaceCrumb{};
    f.crumbs[0].view = view;
    format(f.crumbs[0].name, sizeof f.crumbs[0].name, "%s", name);
    moved(f);
}
void go_into(Face& f, LibraryView view, u64 parent, const char* name) {
    if (f.ncrumbs == kFaceCrumbs) return;
    FaceCrumb& c = f.crumbs[f.ncrumbs++];
    c.view = view;
    c.parent = parent;
    format(c.name, sizeof c.name, "%s", name);
    moved(f);
}

// The face's window's row at an index, in the table as the handler's phase found it.
const LibraryRow* lib_row(const KTable* rows, u32 index) {
    Span<const LibraryRow> r = rows->rows_of<LibraryRow>();
    for (u32 i = 0; i < r.len; ++i) if (rows->is_alive(i) && r[i].query == kFaceLibWindow && r[i].index == index) return &r[i];
    return nullptr;
}

void emit_collect(TickContext& ctx, u32 id, u8 kind, u64 row, const char* text, LibrarySort sort) {
    LibraryCollect c;
    c.id = id; c.kind = kind; c.row = row; c.sort = sort;
    format(c.text, sizeof c.text, "%s", text);
    ctx.emit(c);
}

// What the list is of, as something to collect: a search's words, or the row it was opened
// from. False for a list no one row stands for (the roots, every artist, every track).
bool collect_here(TickContext& ctx, Face& f, u32 id) {
    if (f.lib_text[0]) { emit_collect(ctx, id, 0, 0, f.lib_text, f.lib_sort); return true; }
    const FaceCrumb& c = here(f);
    if (!c.parent) return false;
    LibraryRowKind kind = c.view == LibraryView::Folders ? LibraryRowKind::Directory : c.view == LibraryView::Albums ? LibraryRowKind::Artist :
                          c.view == LibraryView::AlbumTracks ? LibraryRowKind::Album : LibraryRowKind::Genre;
    emit_collect(ctx, id, (u8)kind, c.parent, "", f.lib_sort);
    return true;
}

// Collected to be played: the queue emptied first, and PlayerPlay once the last path is in
// (ui_dev's handler on LibraryCollected counts them).
void play_from(TickContext& ctx) { ctx.emit(PlayerQueueEdit{(u8)PlayerEditOp::Clear, 0, 0}); }

} // namespace

const char* face_title(const LibraryRow* row, const char* path, char* out, usize cap) {
    const char* name = face_file_name(path);
    // The answer is for this file when its name is this file's: a row asked for an index
    // that has since become another path is not shown under the wrong one.
    if (row && row->title[0] && strcmp(face_file_name(row->path), name) == 0) {
        if (row->artist[0]) format(out, cap, "%s - %s", row->artist, row->title);
        else                format(out, cap, "%s", row->title);
        return out;
    }
    return name;
}

void library_close(TickContext& ctx, Face& f) {
    if (f.lib_open) ctx.emit(LibraryClose{kFaceLibWindow});
    if (f.ntitles) ctx.emit(LibraryClose{kFaceTitleWindow});
    f.lib_open = false;
    f.lib_asked = FaceAsk{};
    f.ntitles = 0;
    f.titles_rest = 0;
}

void titles_sync(TickContext& ctx, Face& f) {
    f.title_now = nullptr;
    memset(f.title_rows, 0, sizeof f.title_rows);
    const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
    const KTable* q = ctx.read_ktable("player.queue");
    const KTable* lib = ctx.read_ktable("library.rows");
    if (!t || !q || !lib) return;

    // The rows wanted: those the queue's list has built, and the one the transport is at.
    u32 first = f.q_first, count = f.q_count < kFaceView - 1 ? f.q_count : kFaceView - 1;
    if (first >= t->queued) count = 0;
    else if (first + count > t->queued) count = t->queued - first;
    u64 at_id = t->item ? t->item : t->place;
    u32 at_pos = 0;
    const PlayerItem* view[kFaceView];
    const PlayerItem* at = nullptr;
    memset(view, 0, sizeof(const PlayerItem*) * (count ? count : 1));
    Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
    for (u32 i = 0; i < rows.len; ++i) {
        if (!q->is_alive(i)) continue;
        u32 k = rows[i].pos - 1 - first;
        if (k < count) view[k] = &rows[i];
        if (at_id && rows[i].id == at_id) { at = &rows[i]; at_pos = rows[i].pos; }
    }

    // Asked when the view has been where it is for a few ticks, a path at a time: an index
    // whose path is not the one asked is asked again, one no longer wanted is taken out.
    if (first != f.titles_first || count != f.titles_count) { f.titles_first = first; f.titles_count = count; f.titles_rest = 0; }
    else if (f.titles_rest < kTitleRestTicks) ++f.titles_rest;
    if (f.titles_rest < kTitleRestTicks) engine_want_ticks(*f.e);
    if (f.titles_rest == kTitleRestTicks) {
        auto wanted = [&](u32 index) -> const PlayerItem* {
            if (index - first < count) return view[index - first];
            return at && at_pos - 1 == index ? at : nullptr;
        };
        for (u32 i = 0; i < f.ntitles;) {
            const PlayerItem* it = wanted(f.titles[i].index);
            if (it && lib_hash(it->path) == f.titles[i].hash) { ++i; continue; }
            if (!it) {
                LibraryPath gone;
                gone.id = kFaceTitleWindow;
                gone.index = f.titles[i].index;
                ctx.emit(gone);
                f.titles[i] = f.titles[--f.ntitles];
                continue;
            }
            f.titles[i] = f.titles[--f.ntitles]; // another path now: asked below
        }
        auto ask = [&](const PlayerItem* it) {
            if (!it || f.ntitles == kFaceView) return;
            u32 index = it->pos - 1;
            for (u32 i = 0; i < f.ntitles; ++i) if (f.titles[i].index == index) return;
            LibraryPath p;
            p.id = kFaceTitleWindow;
            p.index = index;
            memcpy(p.path, it->path, sizeof p.path < sizeof it->path ? sizeof p.path : sizeof it->path);
            p.path[sizeof p.path - 1] = 0;
            ctx.emit(p);
            f.titles[f.ntitles++] = FaceTitle{index, lib_hash(it->path)};
            f.awake = 4;
        };
        for (u32 k = 0; k < count; ++k) ask(view[k]);
        ask(at);
    }

    // This tick's answers, by place in the view.
    Span<const LibraryRow> lr = lib->rows_of<LibraryRow>();
    for (u32 i = 0; i < lr.len; ++i) {
        if (!lib->is_alive(i) || lr[i].query != kFaceTitleWindow) continue;
        if (lr[i].index - first < count) f.title_rows[lr[i].index - first] = &lr[i];
        if (at && lr[i].index == at_pos - 1) f.title_now = &lr[i];
    }
}

void library_sync(TickContext& ctx, Face& f) {
    const KTable* roots = ctx.read_ktable("library.roots");
    const KTable* rows = ctx.read_ktable("library.rows");
    const LibraryScan* scan = ctx.resource<const LibraryScan>("library.scan");
    if (!roots || !rows) return;
    char text[512];
    MemScope scope;

    // The roots, with their states and counts, and why one is offline.
    {
        JsonBuilder b(&scope.arena->a);
        JsonRef list = b.array();
        Span<const LibraryRoot> r = roots->rows_of<LibraryRoot>();
        for (u32 id = 1, seen = 0; seen < roots->live() && id < 4096; ++id) {
            for (u32 i = 0; i < r.len; ++i) {
                if (!roots->is_alive(i) || r[i].id != id) continue;
                ++seen;
                JsonRef o = b.object();
                b.set(o, "id", b.integer(r[i].id));
                b.set(o, "path", b.string(r[i].path));
                b.set(o, "state", b.string(root_state_text(r[i].state)));
                format(text, sizeof text, "%u tracks, %u unread; %u directories, %u not listed", r[i].tracks, r[i].unread, r[i].dirs, r[i].failed);
                b.set(o, "counts", b.string(text));
                b.set(o, "reason", b.string(r[i].reason));
                b.set(o, "ink", b.string(r[i].state == (u8)LibraryRootState::Offline ? "#ff9a9a" : "#e6e9ef"));
                b.push(list, o);
            }
        }
        face_set_doc(f, "l_roots", b, list);
        b.free();
    }

    // The window: asked for where the list is, when that is not where it was last asked.
    const FaceCrumb& c = here(f);
    FaceAsk want;
    want.view = f.lib_text[0] ? LibraryView::Search : c.view;
    want.sort = f.lib_sort;
    want.parent = f.lib_text[0] ? 0 : c.parent;
    format(want.text, sizeof want.text, "%s", f.lib_text);
    want.first = f.lib_first;
    want.count = f.lib_count ? (f.lib_count < kLibraryMaxWindow ? f.lib_count : kLibraryMaxWindow) : 64; // before the list has said: a screenful
    const FaceAsk& was = f.lib_asked;
    bool same_list = want.view == was.view && want.sort == was.sort && want.parent == was.parent && strcmp(want.text, was.text) == 0;
    // Asked for wider than the list has built, by half as much again each side, so that a
    // wheel's notch is already there: a list that moves inside what was asked asks nothing.
    bool inside = same_list && want.first >= was.first && want.first + want.count <= was.first + was.count;
    if (!inside) {
        u32 pad = want.count / 2;
        u32 lo = want.first > pad ? want.first - pad : 0;
        u32 hi = want.first + want.count + pad;
        if (hi - lo > kLibraryMaxWindow) hi = lo + kLibraryMaxWindow;
        want.first = lo;
        want.count = hi - lo;
    }
    f64 now = time_now();
    if (!f.lib_open || !inside) {
        LibraryQuery q;
        q.id = kFaceLibWindow;
        q.view = want.view; q.sort = want.sort; q.parent = want.parent; q.first = want.first; q.count = want.count;
        format(q.text, sizeof q.text, "%s", want.text);
        ctx.emit(q);
        if (!same_list || !f.lib_open) f.lib_stale = true;
        f.lib_asked = want;
        f.lib_open = true;
        f.lib_answered = false;
        f.lib_ask_tick = ctx.tick;
        f.lib_moved_at = now;
        f.lib_prioritized = false;
        f.awake = 4;
        // A folder just opened is listed before the rest of the walk.
        if (!same_list && want.view == LibraryView::Folders && want.parent) { LibraryPrioritize p; p.dir = want.parent; ctx.emit(p); }
    }

    // The rows the table holds of the window, in order from the first of them.
    {
        const LibraryRow* held[kLibraryMaxWindow];
        u32 lo = ~0u, n = 0;
        Span<const LibraryRow> r = rows->rows_of<LibraryRow>();
        if (!f.lib_stale)
            for (u32 i = 0; i < r.len; ++i) if (rows->is_alive(i) && r[i].query == kFaceLibWindow && r[i].index < lo) lo = r[i].index;
        memset(held, 0, sizeof held);
        bool tracks = false;
        if (lo != ~0u)
            for (u32 i = 0; i < r.len; ++i) {
                if (!rows->is_alive(i) || r[i].query != kFaceLibWindow || r[i].index - lo >= kLibraryMaxWindow) continue;
                held[r[i].index - lo] = &r[i];
                if (r[i].index - lo + 1 > n) n = r[i].index - lo + 1;
                tracks = tracks || r[i].kind == (u8)LibraryRowKind::Track;
            }
        JsonBuilder b(&scope.arena->a);
        JsonRef list = b.array();
        for (u32 k = 0; k < n; ++k) {
            const LibraryRow* x = held[k];
            if (!x) break;
            bool track = x->kind == (u8)LibraryRowKind::Track;
            JsonRef o = b.object();
            b.set(o, "index", b.integer(x->index));
            b.set(o, "kind", b.string(kind_text(x->kind)));
            b.set(o, "title", b.string(track && x->title[0] ? x->title : x->name));
            b.set(o, "artist", b.string(x->artist));
            b.set(o, "album", b.string(x->album));
            if (track) {
                char len[32] = "";
                if (x->rate && x->frames) face_clock(len, sizeof len, x->frames, x->rate);
                format(text, sizeof text, "%s%s%s", len, len[0] && x->date[0] ? ", " : "", x->date);
            } else if (x->kind == (u8)LibraryRowKind::Artist) {
                format(text, sizeof text, "%u albums, %u tracks", x->albums, x->count);
            } else if (x->kind == (u8)LibraryRowKind::Directory) {
                text[0] = 0;
            } else {
                format(text, sizeof text, "%u tracks%s%s", x->count, x->date[0] ? ", " : "", x->date);
            }
            b.set(o, "extra", b.string(text));
            // Why it cannot be played, or its tags could not be read: where the row shows.
            format(text, sizeof text, "%s%s%s", x->missing ? "missing" : x->offline ? "its root is offline" : "", (x->missing || x->offline) && x->reason[0] ? ": " : "", x->reason);
            b.set(o, "reason", b.string(text));
            b.set(o, "ink", b.string(x->missing || x->offline || (track && x->reason[0]) ? "#ff9a9a" : "#e6e9ef"));
            b.set(o, "open", b.string(track ? "edit" : "open"));
            b.push(list, o);
        }
        face_set_doc(f, "l_rows", b, list);
        b.free();
        face_set(f, "l_first", (f64)(lo == ~0u ? 0 : lo));
        face_set(f, "l_total", (f64)(f.lib_stale ? 0 : f.lib_total));

        // At rest, and answered: what is in view is scanned next.
        if (f.lib_answered && !f.lib_prioritized && tracks) {
            if (now - f.lib_moved_at >= kRestSeconds) {
                LibraryPrioritize p;
                p.query = kFaceLibWindow;
                ctx.emit(p);
                f.lib_prioritized = true;
            } else {
                engine_want_ticks(*f.e);
            }
        }
    }

    // Where the list is, and what the library last said of it.
    usize n = 0;
    text[0] = 0;
    if (f.lib_text[0]) n = (usize)format(text, sizeof text, "search: %s", f.lib_text);
    else for (u32 i = 0; i < f.ncrumbs && n + 4 < sizeof text; ++i) n += (usize)format(text + n, sizeof text - n, "%s%s", i ? " / " : "", f.crumbs[i].name);
    face_set(f, "l_crumb", text);
    face_set(f, "l_view", f.lib_text[0] ? "search" : view_key(f.crumbs[0].view));
    face_set(f, "l_sort", sort_key(f.lib_sort));
    if (f.lib_answered) format(text, sizeof text, "%u rows, answered in %.1f ms; generation %llu%s", f.lib_total, f.lib_seconds * 1e3, (unsigned long long)f.lib_generation,
                               scan && scan->rebuilt ? "; the index was made again at start-up" : "");
    else                format(text, sizeof text, "asking%s", scan && scan->rebuilt ? "; the index was made again at start-up" : "");
    face_set(f, "l_status", text);
}

bool library_event(TickContext& ctx, Face& f, const KromeEvent& ev) {
    static const Name view = name_intern("l_view"), sort = name_intern("l_sort"), up = name_intern("l_up"), open = name_intern("l_open"),
                      queue = name_intern("l_queue"), play = name_intern("l_play"), queue_all = name_intern("l_queue_all"), play_all = name_intern("l_play_all"),
                      search = name_intern("l_search"), add_root = name_intern("l_add_root"), remove_root = name_intern("l_remove_root"),
                      edit_save = name_intern("l_edit_save"), edit_clear = name_intern("l_edit_clear"), edit_cancel = name_intern("l_edit_cancel");
    Name event{ev.event}, node{ev.node};
    const char* id = node.is_null() ? "" : name_str(node);
    u64 n = 0;

    if (event == view) {
        // A view's button: its top, and no search.
        f.lib_text[0] = 0;
        face_forget(f, "l_search");
        face_set(f, "l_search", "");
        if (!strcmp(id, "lv_artists"))     go_top(f, LibraryView::Artists, "Artists");
        else if (!strcmp(id, "lv_albums")) { go_top(f, LibraryView::Artists, "Artists"); f.crumbs[0].view = LibraryView::Albums; format(f.crumbs[0].name, sizeof f.crumbs[0].name, "Albums"); }
        else if (!strcmp(id, "lv_genres")) go_top(f, LibraryView::Genres, "Genres");
        else if (!strcmp(id, "lv_tracks")) go_top(f, LibraryView::Tracks, "Tracks");
        else                               go_top(f, LibraryView::Folders, "Folders");
        face_forget(f, "l_view"); // the press wrote the button's own value
        return true;
    }
    if (event == sort) {
        f.lib_sort = !strcmp(id, "ls_title") ? LibrarySort::Title : !strcmp(id, "ls_artist") ? LibrarySort::Artist : !strcmp(id, "ls_album") ? LibrarySort::Album :
                     !strcmp(id, "ls_date") ? LibrarySort::Date : !strcmp(id, "ls_path") ? LibrarySort::Path : LibrarySort::Default;
        face_forget(f, "l_sort");
        moved(f);
        return true;
    }
    if (event == up) {
        if (f.lib_text[0]) { f.lib_text[0] = 0; face_forget(f, "l_search"); face_set(f, "l_search", ""); moved(f); }
        else if (f.ncrumbs > 1) { --f.ncrumbs; moved(f); }
        return true;
    }
    if (event == search) {
        // The words as the edit has them now: a search while there are any.
        const char* words = krome_model_text(*f.e, f.screen, "l_search");
        format(f.lib_text, sizeof f.lib_text, "%s", words ? words : "");
        face_forget(f, "l_search");
        moved(f);
        return true;
    }
    if (event == add_root) {
        const char* path = krome_model_text(*f.e, f.screen, "l_root_path");
        if (!path || !path[0]) return true;
        LibraryAddRoot a{};
        if (strlen(path) >= kFaceTypedMax) return true; // it may have been cut (KR43): a root is not added from the front of a path
        format(a.path, sizeof a.path, "%s", path);
        ctx.emit(a);
        face_forget(f, "l_root_path");
        face_set(f, "l_root_path", "");
        return true;
    }
    if (event == remove_root && face_id(node, "lr_", &n)) { ctx.emit(LibraryRemoveRoot{(u32)n}); return true; }
    if (event == queue_all) { collect_here(ctx, f, kFaceCollect); return true; }
    if (event == play_all) {
        // Asked first, so that nothing to collect does not empty the queue.
        if (collect_here(ctx, f, kFaceCollectPlay)) play_from(ctx);
        return true;
    }
    if (event == edit_cancel) { f.edit_track = 0; face_set_bool(f, "l_edit_open", false); return true; }
    if (event == edit_save || event == edit_clear) {
        if (!f.edit_track) return true;
        LibraryOverride o;
        o.track = f.edit_track;
        format(o.field, sizeof o.field, "%s", krome_model_text(*f.e, f.screen, "l_edit_field")); // each text is the next call's to overwrite
        if (event == edit_clear) o.clear = 1;
        else format(o.value, sizeof o.value, "%s", krome_model_text(*f.e, f.screen, "l_edit_value"));
        if (o.field[0]) ctx.emit(o);
        f.edit_track = 0;
        face_set_bool(f, "l_edit_open", false);
        return true;
    }

    // A row's own buttons: the row is the window's, as the screen was showing it.
    bool is_open = event == open && face_id(node, "lo_", &n);
    bool is_queue = event == queue && face_id(node, "lq_", &n);
    bool is_play = event == play && face_id(node, "lp_", &n);
    if (!is_open && !is_queue && !is_play) return false;
    const KTable* rows = ctx.read_ktable("library.rows");
    const LibraryRow* r = rows ? lib_row(rows, (u32)n) : nullptr;
    if (!r) return true;
    if (is_queue) { emit_collect(ctx, kFaceCollect, r->kind, r->id, "", f.lib_sort); return true; }
    if (is_play)  { play_from(ctx); emit_collect(ctx, kFaceCollectPlay, r->kind, r->id, "", f.lib_sort); return true; }
    switch ((LibraryRowKind)r->kind) {
    case LibraryRowKind::Directory: f.lib_text[0] = 0; go_into(f, LibraryView::Folders, r->id, r->name); break;
    case LibraryRowKind::Artist:    f.lib_text[0] = 0; go_into(f, LibraryView::Albums, r->id, r->name); break;
    case LibraryRowKind::Album:     f.lib_text[0] = 0; go_into(f, LibraryView::AlbumTracks, r->id, r->name); break;
    case LibraryRowKind::Genre:     f.lib_text[0] = 0; go_into(f, LibraryView::Tracks, r->id, r->name); break;
    case LibraryRowKind::Track:
        // A track's field, for the user: the editor, with its title to start from.
        f.edit_track = r->id;
        face_set_bool(f, "l_edit_open", true);
        face_set(f, "l_edit_name", r->title[0] ? r->title : r->name);
        face_forget(f, "l_edit_field");
        face_set(f, "l_edit_field", "title");
        face_forget(f, "l_edit_value");
        face_set(f, "l_edit_value", r->title);
        break;
    default: break;
    }
    return true;
}

} // namespace kv
