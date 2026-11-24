// The Output panel: player.output, and why what reaches the device is not the file's own
// samples when it is not. The reason is worked out here from what is published: the
// device is shared, a stage is not flat (which), a join is under way, the device's word
// is narrower than the file's. If the tables could not say, they would be missing
// something.
#include "face.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"

namespace kv {

void output_sync(TickContext& ctx, Face& f) {
    const PlayerOutput* o = ctx.resource<const PlayerOutput>("player.output");
    const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
    if (!o || !t) return;
    char text[256];
    face_set(f, "o_exact", !o->open ? "no device open" : o->exact ? "exact" : "not exact");

    // Why not, every reason there is.
    usize n = 0;
    text[0] = 0;
    auto say = [&](const char* fmt, const char* a = "") {
        if (n + 1 >= sizeof text) return;
        if (n) n += (usize)format(text + n, sizeof text - n, "; ");
        if (n + 1 < sizeof text) n += (usize)format(text + n, sizeof text - n, fmt, a);
        if (n >= sizeof text) n = sizeof text - 1;
    };
    if (o->open && !o->exact) {
        if (o->shared) say("the device is shared: it mixes, or converts on the way");
        if (const KTable* chain = ctx.read_ktable("player.chain")) {
            Span<const PlayerStage> stages = chain->rows_of<PlayerStage>();
            for (u32 i = 0; i < stages.len; ++i)
                if (chain->is_alive(i) && !stages[i].flat) say("the %s stage is not flat", stages[i].name);
        }
        if (o->joining) say("a join is under way");
        if (const KTable* q = ctx.read_ktable("player.queue")) {
            Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
            for (u32 i = 0; i < rows.len && t->item; ++i)
                if (q->is_alive(i) && rows[i].id == t->item && rows[i].bits > o->bits) {
                    char bits[64];
                    format(bits, sizeof bits, "%u bits in the file, %u in the device's", rows[i].bits, o->bits);
                    say("the device's word is narrower: %s", bits);
                }
        }
        if (!n) say("the tables do not say");
    }
    face_set(f, "o_why", text);

    if (o->open) format(text, sizeof text, "%u Hz, %u ch, %u-bit%s, %s", o->rate, o->channels, o->bits, o->float_word ? " float" : "", o->shared ? "shared" : "unshared");
    else         format(text, sizeof text, "none");
    face_set(f, "o_device", text);
    // The lead moves by a callback's worth all the time: shown to a tenth of a second.
    format(text, sizeof text, "%.1f s", o->rate ? (f64)o->lead_frames / (f64)o->rate : 0.0);
    face_set(f, "o_lead", text);
    format(text, sizeof text, "%u", o->underruns);
    face_set(f, "o_underruns", text);
    format(text, sizeof text, "%.1f MB", (f64)o->bytes_held / 1048576.0);
    face_set(f, "o_held", text);
    if (o->recuts) format(text, sizeof text, "%.1f ms to the ear, %.1f ms to render; %u so far", o->recut_seconds * 1e3, o->recut_render_seconds * 1e3, o->recuts);
    else           format(text, sizeof text, "none yet");
    face_set(f, "o_recut", text);
    if (o->jump_seconds > 0.0) format(text, sizeof text, "%.1f ms", o->jump_seconds * 1e3);
    else                       format(text, sizeof text, "none yet");
    face_set(f, "o_jump", text);
}

} // namespace kv
