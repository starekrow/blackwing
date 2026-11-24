// The Transport panel: player.transport into the model, and the panel's presses and its
// bar into the player's commands.
//
// The position is the one thing that changes all the time, and it is written at the rate
// it can be seen to change: the readout shows whole seconds, so its text changes once a
// second; the bar is so many pixels wide, so its value is rounded to one of them. The
// writer finds the same value and does nothing in between.
#include "face.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"

namespace kv {

namespace {

const char* transport_state_text(u8 s) {
    switch ((PlayerState)s) {
    case PlayerState::Stopped:  return "stopped";
    case PlayerState::Starting: return "starting";
    case PlayerState::Playing:  return "playing";
    case PlayerState::Paused:   return "paused";
    default:                    return "?";
    }
}

} // namespace

void transport_sync(TickContext& ctx, Face& f) {
    const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
    if (!t) return;
    char text[96];
    face_set(f, "t_state", transport_state_text(t->state));
    f.playing = t->state == (u8)PlayerState::Playing || t->state == (u8)PlayerState::Starting;
    face_clock(text, sizeof text, t->position, t->rate);
    face_set(f, "t_pos", text);
    face_clock(text, sizeof text, t->frames, t->rate);
    face_set(f, "t_len", text);
    f.bar_frames = t->frames;

    // The row the transport is at: the one heard, or the one about to be.
    u64 at = t->item ? t->item : t->place;
    const PlayerItem* row = nullptr;
    if (const KTable* q = ctx.read_ktable("player.queue")) {
        Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
        for (u32 i = 0; i < rows.len && at; ++i)
            if (q->is_alive(i) && rows[i].id == at) { row = &rows[i]; break; }
    }
    if (row) format(text, sizeof text, "%u of %u", row->pos, t->queued);
    else     format(text, sizeof text, t->queued ? "past the end of %u" : "nothing queued", t->queued);
    face_set(f, "t_row", text);
    char title[600];
    face_set(f, "t_title", row ? face_title(f.title_now, row->path, title, sizeof title) : "");

    // A seek on its way to the ear: where it was sent.
    u32 rate = t->rate ? t->rate : row ? row->rate : 0;
    if (t->seek_item) {
        char to[32];
        face_clock(to, sizeof to, t->seek_frame, rate);
        format(text, sizeof text, "seeking to %s", to);
        face_set(f, "t_seeking", text);
    } else {
        face_set(f, "t_seeking", "");
    }

    // The bar: where the pointer has it while it is held; where a seek is going while one
    // is on its way; else the position, to a pixel of the bar.
    if (!f.seek_drag) {
        f64 k = 0.0;
        u64 frames = t->frames;
        u64 pos = t->seek_item && t->seek_item == at ? t->seek_frame : t->position;
        if (frames) {
            rect2 r{};
            f64 steps = krome_find_rect(*f.e, f.screen, "seek", &r) && r.w > 16.0f ? (f64)r.w : 256.0;
            k = (f64)(pos < frames ? pos : frames) / (f64)frames;
            k = (f64)(u64)(k * steps) / steps;
        }
        face_set(f, "t_bar", k);
    }
}

bool transport_event(TickContext& ctx, Face& f, const KromeEvent& ev) {
    static const Name play = name_intern("play"), pause = name_intern("pause"), stop = name_intern("stop"), next = name_intern("next"),
                      previous = name_intern("previous"), back = name_intern("back"), on = name_intern("on"), slide = name_intern("slide"),
                      slide_end = name_intern("slide_end"), seek = name_intern("seek");
    Name event{ev.event};
    static const Name toggle = name_intern("toggle");
    if (event == toggle)   { if (f.playing) ctx.emit(PlayerPause{}); else ctx.emit(PlayerPlay{}); return true; } // Space: the other of the two
    if (event == play)     { ctx.emit(PlayerPlay{}); return true; }
    if (event == pause)    { ctx.emit(PlayerPause{}); return true; }
    if (event == stop)     { ctx.emit(PlayerStop{}); return true; }
    if (event == next)     { ctx.emit(PlayerNext{}); return true; }
    if (event == previous) { ctx.emit(PlayerPrevious{}); return true; }
    if (event == back)     { ctx.emit(PlayerSeekBy{-10.0}); return true; }
    if (event == on)       { ctx.emit(PlayerSeekBy{10.0}); return true; }
    if (ev.node != seek.hash) return false;
    if (event == slide) {
        // The bar, pressed or dragged: a seek each time it moves. The player renders the newest.
        f.seek_drag = true;
        if (f.bar_frames) {
            f64 k = ev.value < 0.0 ? 0.0 : ev.value > 1.0 ? 1.0 : ev.value;
            ctx.emit(PlayerSeek{0, (u64)(k * (f64)f.bar_frames)});
        }
        return true;
    }
    if (event == slide_end) { f.seek_drag = false; return true; }
    return false;
}

} // namespace kv
