// The Chain panel: each stage, flat or not, every parameter with its value heard, and
// the presets; and a slider's movement into PlayerSetParam.
//
// The stages' controls are not written in a file. They are a definition, chain-stages,
// that this file writes as markup from player.params and player.presets and hands to
// Krome in the place of assets/chain_stages.kim, which only says there is none yet: a slider for a parameter with a range, a switch for
// one that is 0 or 1, a button for each preset. A stage the face has never heard of gets
// its controls from its own description, and a parameter added to one appears.
//
// Each parameter has two model keys: c_v_S_P, the value heard, as text; c_k_S_P, where
// its slider's knob is, from 0 to 1. A dragged slider's knob is the pointer's, so its c_k
// is left alone until it is let go; its c_v goes on saying what is heard.
#include "face.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/array.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"
#include "korvid/kim/kim.h"
#include <stdarg.h>
#include <string.h>

namespace kv {

namespace {

constexpr const char* kChainFile = "ui_dev/chain_stages.kimb";

u64 chain_mix(u64 h, const void* p, usize n) {
    const u8* b = (const u8*)p;
    for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

bool is_switch(const PlayerParam& p) { return p.lo == 0.0 && p.hi == 1.0 && p.step == 1.0; }

// A parameter's row among the table's, by stage and index; the parameters in that order.
const PlayerParam* chain_param(const KTable* params, u32 stage, u32 index) {
    Span<const PlayerParam> rows = params->rows_of<PlayerParam>();
    for (u32 i = 0; i < rows.len; ++i) if (params->is_alive(i) && rows[i].stage == stage && rows[i].index == index) return &rows[i];
    return nullptr;
}

void put(Array<char>& out, const char* fmt, ...) KV_PRINTF(2, 3);
void put(Array<char>& out, const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    int n = vformat(line, sizeof line, fmt, args);
    va_end(args);
    if (n <= 0) return;
    usize len = (usize)n < sizeof line ? (usize)n : sizeof line - 1;
    memcpy(out.push_n((u32)len), line, len);
}

// A name as markup can quote it.
void chain_plain(char* out, usize cap, const char* in) {
    usize n = 0;
    for (; *in && n + 1 < cap; ++in) out[n++] = (*in == '"' || *in == '\\' || *in == '{' || *in == '}' || (unsigned char)*in < 0x20) ? '_' : *in;
    out[n] = 0;
}

void provide(Face& f, const Array<char>& text) {
    Array<u8> out{};
    KimErrors errors;
    if (kim_compile(text.data, text.len, "ui_dev/chain_stages.kim", out, errors)) {
        krome_provide(*f.e, kChainFile, out.data, out.len);
    } else {
        char why[320];
        errors.format_one(0, why, sizeof why);
        KV_ERROR("ui_dev", "the chain's controls did not compile: %s", why);
    }
    out.free();
}

// The markup, from the tables.
void chain_write(Face& f, const KTable* chain, const KTable* params, const KTable* presets) {
    Array<char> out{};
    put(out, "use \"krome/std\"\nuse \"ui_dev/widgets\"\n\ndef chain-stages {\n    col gap=6 w=fill {\n");
    Span<const PlayerStage> stages = chain->rows_of<PlayerStage>();
    Span<const PlayerPreset> pre = presets ? presets->rows_of<PlayerPreset>() : Span<const PlayerPreset>{};
    for (u32 s = 0; s < 8; ++s) {
        const PlayerStage* st = nullptr;
        for (u32 i = 0; i < stages.len; ++i) if (chain->is_alive(i) && stages[i].index == s) st = &stages[i];
        if (!st) continue;
        char name[64];
        chain_plain(name, sizeof name, st->name);
        put(out, "        row gap=8 middle w=fill {\n            text \"%u %s\" size=13\n            text \"{c_flat_%u}\" [dev-title]\n        }\n", s, name, s);
        // Its presets, a button each.
        bool any = false;
        for (u32 k = 0; k < 64; ++k) {
            const PlayerPreset* p = nullptr;
            for (u32 i = 0; i < pre.len; ++i) if (presets->is_alive(i) && pre[i].stage == s && pre[i].index == k) p = &pre[i];
            if (!p) break;
            if (!any) put(out, "        row gap=4 middle w=fill {\n            text \"presets\" [dev-title] w=56\n");
            any = true;
            chain_plain(name, sizeof name, p->name);
            put(out, "            hit [dev-btn] id=cp_%u_%u -> c_preset {\n                text \"%s\" size=11\n            }\n", s, k, name);
        }
        if (any) put(out, "        }\n");
        // Its parameters, a line each.
        for (u32 j = 0; j < kFaceParams; ++j) {
            const PlayerParam* p = chain_param(params, s, j);
            if (!p) break;
            chain_plain(name, sizeof name, p->name);
            put(out, "        row gap=8 middle w=fill h=20 {\n            text \"%s\" [dev-cell] w=64\n", name);
            if (is_switch(*p)) put(out, "            hit [dev-btn] id=ct_%u_%u -> c_toggle {\n                text \"{c_v_%u_%u}\" size=11\n            }\n            spacer\n", s, j, s, j);
            else               put(out, "            dev-slider id=cs_%u_%u @c_k_%u_%u w=fill\n            text \"{c_v_%u_%u}\" [dev-cell] w=84 right\n", s, j, s, j, s, j);
            put(out, "        }\n");
        }
    }
    put(out, "    }\n}\n");
    provide(f, out);
    out.free();
}

// A value as its readout shows it: as many decimals as its step has, and its unit.
void value_text(char* out, usize cap, const PlayerParam& p) {
    if (is_switch(p)) { format(out, cap, "%s", p.value != 0.0 ? "on" : "off"); return; }
    const char* fmt = p.step >= 1.0 ? "%.0f%s%s" : p.step >= 0.1 ? "%.1f%s%s" : p.step > 0.0 ? "%.2f%s%s" : "%.1f%s%s";
    format(out, cap, fmt, p.value, p.unit[0] ? " " : "", p.unit);
}

} // namespace

void chain_sync(TickContext& ctx, Face& f) {
    const KTable* chain = ctx.read_ktable("player.chain");
    const KTable* params = ctx.read_ktable("player.params");
    const KTable* presets = ctx.read_ktable("player.presets");
    if (!chain || !params) return;
    Span<const PlayerStage> stages = chain->rows_of<PlayerStage>();
    Span<const PlayerParam> rows = params->rows_of<PlayerParam>();

    // What the controls are made from: when it is not what they were made from, they are
    // made again, and Krome builds the screen over the new definition.
    u64 sig = 14695981039346656037ull;
    for (u32 i = 0; i < stages.len; ++i)
        if (chain->is_alive(i)) { sig = chain_mix(sig, &stages[i].index, sizeof(u32)); sig = chain_mix(sig, stages[i].name, strlen(stages[i].name)); }
    for (u32 i = 0; i < rows.len; ++i) {
        if (!params->is_alive(i)) continue;
        const PlayerParam& p = rows[i];
        sig = chain_mix(sig, &p.stage, sizeof(u32) * 2);
        f64 shape[3] = {p.lo, p.hi, p.step};
        sig = chain_mix(sig, shape, sizeof shape);
        sig = chain_mix(sig, p.name, strlen(p.name));
    }
    if (presets) {
        Span<const PlayerPreset> pre = presets->rows_of<PlayerPreset>();
        for (u32 i = 0; i < pre.len; ++i)
            if (presets->is_alive(i)) { sig = chain_mix(sig, &pre[i].stage, sizeof(u32) * 2); sig = chain_mix(sig, pre[i].name, strlen(pre[i].name)); }
    }
    if (!sig) sig = 1;
    if (sig != f.chain_sig) {
        chain_write(f, chain, params, presets);
        f.chain_sig = sig;
        f.chain_drag = false;
        face_forget(f);
        engine_want_frame(*f.e);
    }

    u32 n = 0, unflat = 0;
    for (u32 i = 0; i < stages.len; ++i) {
        if (!chain->is_alive(i) || stages[i].index >= KV_COUNTOF(f.stage_keys)) continue;
        ++n;
        unflat += !stages[i].flat;
        char* key = f.stage_keys[stages[i].index];
        format(key, sizeof f.stage_keys[0], "c_flat_%u", stages[i].index);
        face_set(f, key, stages[i].flat ? "flat" : "not flat");
    }
    char text[64];
    if (unflat) format(text, sizeof text, "%u stages, %u not flat", n, unflat);
    else        format(text, sizeof text, "%u stages, all flat", n);
    face_set(f, "c_summary", text);

    u32 slot = 0;
    for (u32 i = 0; i < rows.len && slot < kFaceParams; ++i) {
        if (!params->is_alive(i)) continue;
        const PlayerParam& p = rows[i];
        char* vkey = f.chain_keys[slot][0];
        char* kkey = f.chain_keys[slot][1];
        ++slot;
        format(vkey, 16, "c_v_%u_%u", p.stage, p.index);
        format(kkey, 16, "c_k_%u_%u", p.stage, p.index);
        value_text(text, sizeof text, p);
        face_set(f, vkey, text);
        if (is_switch(p)) continue;
        if (f.chain_drag && f.chain_drag_stage == p.stage && f.chain_drag_param == p.index) continue; // the knob is the pointer's
        f64 k = p.hi > p.lo ? (p.value - p.lo) / (p.hi - p.lo) : 0.0;
        face_set(f, kkey, k < 0.0 ? 0.0 : k > 1.0 ? 1.0 : k);
    }
}

bool chain_event(TickContext& ctx, Face& f, const KromeEvent& ev) {
    static const Name slide = name_intern("slide"), slide_end = name_intern("slide_end"), toggle = name_intern("c_toggle"), preset = name_intern("c_preset");
    Name event{ev.event}, node{ev.node};
    u64 a = 0, b = 0;
    if (event == preset && face_id(node, "cp_", &a, &b)) { ctx.emit(PlayerApplyPreset{(u32)a, (u32)b}); return true; }
    const KTable* params = ctx.read_ktable("player.params");
    if (event == toggle && face_id(node, "ct_", &a, &b)) {
        const PlayerParam* p = params ? chain_param(params, (u32)a, (u32)b) : nullptr;
        if (p) ctx.emit(PlayerSetParam{(u32)a, (u32)b, p->value != 0.0 ? 0.0 : 1.0});
        return true;
    }
    if ((event == slide || event == slide_end) && face_id(node, "cs_", &a, &b)) {
        if (event == slide_end) { f.chain_drag = false; return true; }
        f.chain_drag = true;
        f.chain_drag_stage = (u32)a;
        f.chain_drag_param = (u32)b;
        const PlayerParam* p = params ? chain_param(params, (u32)a, (u32)b) : nullptr;
        if (!p) return true;
        // Where the knob is, as the parameter's value: within its range, on its step.
        f64 k = ev.value < 0.0 ? 0.0 : ev.value > 1.0 ? 1.0 : ev.value;
        f64 v = p->lo + k * (p->hi - p->lo);
        if (p->step > 0.0) v = p->lo + (f64)(i64)((v - p->lo) / p->step + 0.5) * p->step;
        ctx.emit(PlayerSetParam{(u32)a, (u32)b, v});
        return true;
    }
    return false;
}

} // namespace kv
