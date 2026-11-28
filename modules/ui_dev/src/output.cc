// The Output panel: player.output, and why what reaches the device is not the file's own
// samples when it is not. The reason is worked out here from what is published: the
// device is shared, a stage is not flat (which), a join is under way, the device's word
// is narrower than the file's. If the tables could not say, they would be missing
// something.
#include "face.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"
#include "korvid/core/arena.h"
#include <string.h>

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

    // The devices, and which is chosen.
    if (!strcmp(o->chosen, "default") || !o->chosen[0]) format(text, sizeof text, "following the default%s", o->lost ? "; the one playing went" : "");
    else format(text, sizeof text, "%s%s%s", o->chosen, o->standing_in ? ", absent: the default stands in" : "", o->lost ? "; it went while playing" : "");
    if (!o->open && o->none_why[0]) format(text + strlen(text), sizeof text - strlen(text), " (%s)", o->none_why);
    face_set(f, "o_chosen", text);
    const KTable* devs = ctx.read_ktable("player.devices");
    MemScope scope;
    JsonBuilder b(&scope.arena->a);
    JsonRef list = b.array();
    u32 k = 0;
    if (devs) {
        Span<const PlayerDevice> rows = devs->rows_of<PlayerDevice>();
        for (u32 i = 0; i < rows.len && k < KV_COUNTOF(f.dev_names); ++i) {
            if (!devs->is_alive(i)) continue;
            const PlayerDevice& d = rows[i];
            format(f.dev_names[k], sizeof f.dev_names[k], "%s", d.name);
            JsonRef row = b.object();
            char id[16];
            format(id, sizeof id, "%u", k);
            b.set(row, "k", b.string(id));
            b.set(row, "label", b.string(d.label[0] ? d.label : d.name));
            b.set(row, "backend", b.string(d.backend));
            char flags[96];
            format(flags, sizeof flags, "%s%s%s%s", d.open ? "playing " : "", d.is_default ? "default " : "", d.chosen ? "chosen " : "", d.present ? "" : "gone");
            b.set(row, "flags", b.string(flags));
            b.set(row, "ink", b.string(!d.present ? "#8b95a7" : d.open ? "#7aa7ff" : "#e6e9ef")); // std's dark theme, as the queue's
            b.push(list, row);
            ++k;
        }
    }
    f.ndev = k;
    face_set_doc(f, "o_devices", b, list);
    b.free();
    face_set(f, "o_ndev", (f64)k);
}

bool output_event(TickContext& ctx, Face& f, const KromeEvent& ev) {
    static const Name use = name_intern("o_use"), follow = name_intern("o_follow");
    Name event{ev.event}, node{ev.node};
    u64 k = 0;
    if (event == follow) {
        PlayerSelectDevice s{};
        format(s.name, sizeof s.name, "default");
        ctx.emit(s);
        return true;
    }
    if (event == use && face_id(node, "od_", &k)) {
        if (k >= f.ndev) return true;
        PlayerSelectDevice s{};
        format(s.name, sizeof s.name, "%s", f.dev_names[k]);
        ctx.emit(s);
        return true;
    }
    return false;
}

} // namespace kv
