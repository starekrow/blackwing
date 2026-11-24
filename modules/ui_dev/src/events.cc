// The Events panel: every command and notification of the player and the library as it is
// delivered, with no handler written for any of them. The engine hands each delivered
// event to one hook (engine_set_event_hook, dev builds), with its type's name and field
// table; the face keeps the last few hundred of the two modules' in a ring and shows the
// newest. A type added next slice shows with no change here.
//
// Live, the list is as many of the newest as fit, newest last. Held, nothing more is
// kept, and the list is everything in the ring, to be read back. A filter keeps the lines
// that have the words typed.
#include "face.h"
#include "korvid/core/arena.h"
#include "korvid/core/format.h"
#include <string.h>

namespace kv {

namespace {

// The player's and the library's event types, by the names they share.
bool ours(const char* type) { return !strncmp(type, "Player", 6) || !strncmp(type, "Library", 7) || !strncmp(type, "Hifi", 4); }

const FaceEvent& kept(const Face& f, u32 back) { return f.events[(f.ev_head + kFaceEvents - 1 - back) % kFaceEvents]; } // 0: the newest

} // namespace

void events_hook(void* user, const DeliveredEvent& ev) {
    Face& f = *(Face*)user;
    if (!f.ev_on || !f.events || !ours(ev.type_name)) return;
    FaceEvent& x = f.events[f.ev_head];
    f.ev_head = (f.ev_head + 1) % kFaceEvents;
    if (f.ev_count < kFaceEvents) ++f.ev_count;
    x.n = ++f.ev_seen;
    char what[200];
    engine_describe_event(*f.e, ev.type, ev.data, what, sizeof what); // cut to fit: a path may be four thousand bytes
    format(x.text, sizeof x.text, "%s%s%s", what, ev.emitter && ev.emitter[0] ? "  by " : "", ev.emitter ? ev.emitter : "");
    f.ev_dirty = true;
    f.awake = 2; // delivered at a boundary: shown by the next sync
    engine_want_ticks(*f.e);
}

void events_sync(TickContext& ctx, Face& f) {
    (void)ctx;
    bool up = f.tab == FaceTab::Events;
    f.ev_on = up && !f.ev_held && f.ev_hooked;
    if (!up) return;
    char text[160];
    if (!f.ev_hooked) format(text, sizeof text, "the engine hands events over in dev builds only");
    else format(text, sizeof text, "%llu seen, %u kept%s", (unsigned long long)f.ev_seen, f.ev_count, f.ev_held ? "; held" : "");
    face_set(f, "e_summary", text);
    face_set_bool(f, "e_held", f.ev_held);

    // Live: as many of the newest as fit. Held: all that are kept.
    u32 fit = kFaceEvents;
    if (!f.ev_held) {
        rect2 r{};
        fit = krome_find_rect(*f.e, f.screen, "e_list", &r) && r.h >= 36.0f ? (u32)(r.h / 18.0f) : 12;
    }
    const FaceEvent* show[kFaceEvents];
    u32 n = 0;
    for (u32 back = 0; back < f.ev_count && n < fit; ++back) {
        const FaceEvent& x = kept(f, back);
        if (f.ev_filter[0] && !strstr(x.text, f.ev_filter)) continue;
        show[n++] = &x;
    }
    MemScope scope;
    JsonBuilder b(&scope.arena->a);
    JsonRef list = b.array();
    for (u32 i = n; i > 0; --i) { // oldest first: newest last
        JsonRef o = b.object();
        format(text, sizeof text, "%llu", (unsigned long long)show[i - 1]->n);
        b.set(o, "n", b.string(text));
        b.set(o, "text", b.string(show[i - 1]->text));
        b.push(list, o);
    }
    face_set_doc(f, "e_rows", b, list);
    b.free();
    f.ev_dirty = false;
}

bool events_event(TickContext& ctx, Face& f, const KromeEvent& ev) {
    (void)ctx;
    static const Name hold = name_intern("e_hold"), clear = name_intern("e_clear"), filter = name_intern("e_filter");
    Name event{ev.event};
    if (event == hold) {
        f.ev_held = !f.ev_held;
        face_forget(f, "e_held"); // the press flipped the model's own
        return true;
    }
    if (event == clear) { f.ev_count = 0; f.ev_head = 0; f.ev_dirty = true; return true; }
    if (event == filter) {
        const char* words = krome_model_text(*f.e, f.screen, "e_filter");
        format(f.ev_filter, sizeof f.ev_filter, "%s", words ? words : "");
        face_forget(f, "e_filter");
        return true;
    }
    return false;
}

} // namespace kv
