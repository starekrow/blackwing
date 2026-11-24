// The Queue panel: player.queue's rows, in the list's order, into the rows its list has
// in view; and its buttons into the queue's edits.
//
// The list is Krome's sparse one: it lays out as if every row were there, says which it
// has built (KromeView, kept in Face::q_first and q_count), and is handed those. A queue
// of four thousand rows is thirty in the model.
#include "face.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/arena.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 kQueueView = kFaceView - 1; // rows handed over at once, at most: the title window's, less the transport's row

const char* item_state_text(const PlayerItem& r, u32 place_pos) {
    switch ((PlayerItemState)r.state) {
    case PlayerItemState::Reading: return "reading";
    case PlayerItemState::Ready:   return "ready";
    case PlayerItemState::Playing: return "playing";
    case PlayerItemState::Failed:  return "failed";
    default:                       return r.pos < place_pos ? (r.damaged ? "damaged" : "done") : "idle";
    }
}

// The ink of a row, by what is true of it: std's dark theme's, as hex (a model value is
// not looked up in a theme).
const char* item_ink(const PlayerItem& r, u32 place_pos) {
    if (r.state == (u8)PlayerItemState::Failed || r.damaged) return "#ff9a9a";
    if (r.state == (u8)PlayerItemState::Playing) return "#7aa7ff";
    if (r.pos < place_pos) return "#8b95a7";
    return "#e6e9ef";
}

const PlayerItem* queue_row(const KTable* q, u64 id) {
    Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
    for (u32 i = 0; i < rows.len; ++i) if (q->is_alive(i) && rows[i].id == id) return &rows[i];
    return nullptr;
}
const PlayerItem* queue_at(const KTable* q, u32 pos) {
    Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
    for (u32 i = 0; i < rows.len; ++i) if (q->is_alive(i) && rows[i].pos == pos) return &rows[i];
    return nullptr;
}

} // namespace

void queue_sync(TickContext& ctx, Face& f) {
    const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
    const KTable* q = ctx.read_ktable("player.queue");
    if (!t || !q) return;
    char text[160];
    format(text, sizeof text, "%u rows, %u left", t->queued, t->left);
    face_set(f, "q_summary", text);
    face_set(f, "q_total", (f64)t->queued);

    // The rows the list has built, by their place: found in one pass of the table.
    u32 first = f.q_first, count = f.q_count < kQueueView ? f.q_count : kQueueView;
    if (first >= t->queued) count = 0;
    else if (first + count > t->queued) count = t->queued - first;
    const PlayerItem* view[kQueueView];
    memset(view, 0, sizeof(const PlayerItem*) * count);
    Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
    for (u32 i = 0; i < rows.len && count; ++i) {
        if (!q->is_alive(i)) continue;
        u32 at = rows[i].pos - 1 - first; // wraps past the view's end for a row before it
        if (at < count) view[at] = &rows[i];
    }
    MemScope scope;
    JsonBuilder b(&scope.arena->a);
    JsonRef list = b.array();
    for (u32 k = 0; k < count; ++k) {
        const PlayerItem* r = view[k];
        if (!r) break; // a row the table does not have yet: the list shows its placeholder from here on
        JsonRef o = b.object();
        format(text, sizeof text, "%llu", (unsigned long long)r->id);
        b.set(o, "id", b.string(text));
        b.set(o, "pos", b.integer(r->pos));
        char title[600];
        b.set(o, "title", b.string(face_title(k < kFaceView ? f.title_rows[k] : nullptr, r->path, title, sizeof title)));
        b.set(o, "state", b.string(item_state_text(*r, t->place_pos)));
        if (r->rate) format(text, sizeof text, "%u Hz, %u ch, %u bit", r->rate, r->channels, r->bits);
        else text[0] = 0;
        b.set(o, "format", b.string(text));
        if (r->size) format(text, sizeof text, "%.1f/%.1f MB", (f64)r->bytes_read / 1048576.0, (f64)r->size / 1048576.0);
        else text[0] = 0;
        b.set(o, "held", b.string(text));
        b.set(o, "reason", b.string(r->reason));
        b.set(o, "ink", b.string(item_ink(*r, t->place_pos)));
        b.push(list, o);
    }
    face_set_doc(f, "q_rows", b, list);
    b.free();
    face_set(f, "q_first", (f64)first);
}

bool queue_event(TickContext& ctx, Face& f, const KromeEvent& ev) {
    static const Name jump = name_intern("q_jump"), up = name_intern("q_up"), down = name_intern("q_down"), remove = name_intern("q_remove"),
                      clear = name_intern("q_clear"), add = name_intern("q_add");
    Name event{ev.event}, node{ev.node};
    u64 id = 0;
    if (event == jump && face_id(node, "qj_", &id))   { ctx.emit(PlayerJump{id}); return true; }
    if (event == remove && face_id(node, "qx_", &id)) { ctx.emit(PlayerQueueEdit{(u8)PlayerEditOp::Remove, id, 0}); return true; }
    if (event == clear) { ctx.emit(PlayerQueueEdit{(u8)PlayerEditOp::Clear, 0, 0}); return true; }
    if (event == up || event == down) {
        // A row moved by one place: before the row above it, or before the row two below
        // (to the end, when there is none).
        if (!face_id(node, event == up ? "qu_" : "qd_", &id)) return false;
        const KTable* q = ctx.read_ktable("player.queue");
        const PlayerItem* r = q ? queue_row(q, id) : nullptr;
        if (!r) return true;
        if (event == up) {
            const PlayerItem* above = r->pos > 1 ? queue_at(q, r->pos - 1) : nullptr;
            if (above) ctx.emit(PlayerQueueEdit{(u8)PlayerEditOp::Move, id, above->id});
        } else {
            if (!queue_at(q, r->pos + 1)) return true; // the last row
            const PlayerItem* below = queue_at(q, r->pos + 2);
            ctx.emit(PlayerQueueEdit{(u8)PlayerEditOp::Move, id, below ? below->id : 0});
        }
        return true;
    }
    if (event == add) {
        // The path typed in, as the edit has it; then the edit is emptied.
        const char* path = krome_model_text(*f.e, f.screen, "q_path");
        if (!path || !path[0]) return true;
        PlayerEnqueue e{};
        if (strlen(path) >= sizeof e.path) { KV_WARN("ui_dev", "the path is longer than PlayerEnqueue carries"); return true; }
        format(e.path, sizeof e.path, "%s", path);
        ctx.emit(e);
        face_forget(f, "q_path");
        face_set(f, "q_path", "");
        return true;
    }
    return false;
}

} // namespace kv
