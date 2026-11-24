// The Transport panel: player.transport into the model, and the panel's presses into the
// player's commands.
//
// The position is the one thing that changes all the time, and it is written at the rate
// it can be seen to change: the readout shows whole seconds, so it is written once a
// second, by the model writer finding the same text and doing nothing in between.
#include "face.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/format.h"

namespace kv {

namespace {

// "3:07", "1:02:03"; frames at a rate of 0 are 0:00.
void clock_text(char* out, usize cap, u64 frames, u32 rate) {
    u64 s = rate ? frames / rate : 0;
    if (s >= 3600) format(out, cap, "%llu:%02llu:%02llu", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    else           format(out, cap, "%llu:%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
}

const char* state_text(u8 s) {
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
    char text[32];
    face_set(f, "t_state", state_text(t->state));
    clock_text(text, sizeof text, t->position, t->rate);
    face_set(f, "t_pos", text);
    clock_text(text, sizeof text, t->frames, t->rate);
    face_set(f, "t_len", text);
}

bool transport_event(TickContext& ctx, Face& f, Name event, f64) {
    (void)f;
    static const Name play = name_intern("play"), pause = name_intern("pause");
    if (event == play)  { ctx.emit(PlayerPlay{}); return true; }
    if (event == pause) { ctx.emit(PlayerPause{}); return true; }
    return false;
}

} // namespace kv
