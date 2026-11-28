// ui_dev: the development face. One Krome screen (assets/shell.kim) of panels, each a
// definition in a file of its own. This file is the face's frame: it opens the screen,
// stands in for a panel whose file does not load, sleeps while the window is hidden, and
// hands each tick to the panels' two halves: a table read into the model (.sync), an
// event of the screen's turned into a command (.events).
//
// It reads the player's and the library's tables through the context, by name, and emits
// their commands. It calls nothing of either: their headers are declarations.
#include "korvid/ui_dev/module.h"
#include "face.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/kim/kim.h"
#include "korvid/krome/krome.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/module.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constinit UiDevConfig g_ui_dev_config = {};

constexpr const char* kShellFile = "ui_dev/shell.kimb";
constexpr const char* kShellScreen = "shell";
constexpr const char* kFallback = "ui_dev/failed";
// The panels, by the name of their files (ui_dev/<name>.kim, .lua) and of their
// definition (<name>-panel). The shell names the same eight.
constinit const char* const k_panels[] = {"transport", "library", "queue", "output", "chain", "scan", "events", "tables"};
constexpr u32 kPanels = KV_COUNTOF(k_panels);
// A command's answer is a tick or two away: the face asks for this many ticks after it
// emits one, so the answer is not left waiting for the loop's next heartbeat.
constexpr u32 kAwakeTicks = 4;

Face* face(Engine& e) { return engine_module_state_as<Face>(e, "ui_dev"); }

// Text that can sit inside a quoted string of markup or Lua and be shown as it is: no
// quote, no backslash, no brace (a brace in a text reads the model), no line break.
void plain(char* out, usize cap, const char* in) {
    usize n = 0;
    for (; *in && n + 1 < cap; ++in) {
        char c = *in;
        if (c == '"' || c == '\\') c = '\'';
        else if (c == '{') c = '(';
        else if (c == '}') c = ')';
        else if ((unsigned char)c < 0x20) c = ' ';
        out[n++] = c;
    }
    out[n] = 0;
}

void open_shell(Face& f) {
    Engine& e = *f.e;
    if (!f.screen.is_null()) krome_close(e, f.screen);
    krome_close(e, name_intern(kFallback));
    krome_clear_errors(e);
    f.screen = krome_open(e, kShellFile, kShellScreen);
    f.wanted = false;
    f.ready = false;
    f.fresh = true;
    f.seek_drag = f.chain_drag = false;
    f.q_first = f.q_count = 0;
    f.lib_first = f.lib_count = 0;
    f.lib_stale = true;
    f.edit_track = 0;
    f.stats.failed = false;
    ++f.attempts;
    face_forget(f);
}

// A panel's file did not load: a definition of the same name that says so, and a script
// that does nothing, given to Krome in the files' place.
void stub_panel(Face& f, u32 panel, const char* why) {
    char text[640], said[320];
    plain(said, sizeof said, why);
    format(text, sizeof text, "use \"krome/std\"\nuse \"ui_dev/widgets\"\ndef %s-panel {\n    panel-broken \"%s\" \"%s\"\n}\n", k_panels[panel], k_panels[panel], said);
    char path[96];
    format(path, sizeof path, "ui_dev/%s.kim", k_panels[panel]);
    Array<u8> out{};
    KimErrors errors;
    if (kim_compile(text, strlen(text), path, out, errors)) {
        format(path, sizeof path, "ui_dev/%s.kimb", k_panels[panel]);
        krome_provide(*f.e, path, out.data, out.len);
    }
    out.free();
    format(path, sizeof path, "ui_dev/%s.lua", k_panels[panel]);
    static const char k_nothing[] = "return { model = {}, on = {} }\n";
    krome_provide(*f.e, path, k_nothing, sizeof k_nothing - 1);
    f.stubbed |= 1u << panel;
    ++f.stats.broken;
    KV_WARN("ui_dev", "the %s panel did not load: %s", k_panels[panel], why);
}

// The shell did not build. Each mistake that names a panel's file makes that panel a
// stand-in, and the shell is opened again; a mistake that names none (the shell's own, or
// the prelude's) leaves a screen that lists them, and nothing else of the face.
void shell_failed(Face& f) {
    Engine& e = *f.e;
    const KimErrors* errors = krome_errors(e);
    u32 stubs = 0;
    for (u32 p = 0; p < kPanels; ++p) {
        if (f.stubbed & (1u << p)) continue;
        char mark[64];
        format(mark, sizeof mark, "ui_dev/%s.", k_panels[p]);
        for (u32 i = 0; errors && i < errors->count; ++i) {
            const KimError& x = errors->list[i];
            if (!(x.file && strstr(x.file, mark)) && !strstr(x.text, mark)) continue;
            char why[320];
            errors->format_one(i, why, sizeof why);
            stub_panel(f, p, why);
            ++stubs;
            break;
        }
    }
    if (stubs && f.attempts <= kPanels + 1) { open_shell(f); return; }

    // No panel to blame: the errors, where the face would be.
    char src[4096];
    usize n = (usize)format(src, sizeof src, "return col { bg = \"#10131a\", color = \"#e6e9ef\", pad = 16, gap = 6, w = \"fill\", h = \"fill\",\n"
                                             "  text { \"The development face did not load. The player is not affected.\", size = 16 },\n");
    for (u32 i = 0; errors && i < errors->count && n + 420 < sizeof src; ++i) {
        char why[320], said[320];
        errors->format_one(i, why, sizeof why);
        plain(said, sizeof said, why);
        KV_ERROR("ui_dev", "%s", why);
        n += (usize)format(src + n, sizeof src - n, "  text { \"%s\", size = 13, color = \"#ff9a9a\" },\n", said);
    }
    if (!errors || !errors->count) n += (usize)format(src + n, sizeof src - n, "  text { \"%s would not open.\", size = 13, color = \"#ff9a9a\" },\n", kShellFile);
    format(src + n, sizeof src - n, "}\n");
    krome_close(e, f.screen);
    f.screen = Name{};
    krome_open_source(e, kFallback, src);
    f.stats.failed = true;
}

// ---- the units -----------------------------------------------------------------------------

// The screen's events, by name: each panel is offered it, and the first to know it emits
// its command. The command's answer is a tick or two away, so the face asks for those.
void on_event(TickContext& ctx, Span<const KromeEvent> events) {
    Face& f = ctx.state_as<Face>();
    if (f.screen.is_null()) return;
    for (const KromeEvent& ev : events) {
        if (ev.screen != f.screen.hash) continue;
        if (transport_event(ctx, f, ev) || queue_event(ctx, f, ev) || chain_event(ctx, f, ev) || library_event(ctx, f, ev) || events_event(ctx, f, ev) ||
            tables_event(ctx, f, ev)) {
            ++f.stats.commands;
            f.awake = kAwakeTicks;
            engine_want_ticks(*ctx.engine);
        }
    }
}

// Which rows a list of the screen's has built: kept for the panel that fills it.
void on_view(TickContext& ctx, Span<const KromeView> views) {
    static const Name q_view = name_intern("q_view"), l_rows = name_intern("l_rows"), x_view = name_intern("x_view");
    Face& f = ctx.state_as<Face>();
    for (const KromeView& v : views) {
        if (f.screen.is_null() || v.screen != f.screen.hash) continue;
        if (v.event == q_view.hash) { f.q_first = v.first; f.q_count = v.count; }
        if (v.event == l_rows.hash) { f.lib_first = v.first; f.lib_count = v.count; }
        if (v.event == x_view.hash) { f.x_first = v.first; f.x_count = v.count; }
        f.awake = kAwakeTicks;
    }
}

// The library's answer for the face's window: how long the list is, and that it has come.
void on_result(TickContext& ctx, Span<const LibraryResult> results) {
    Face& f = ctx.state_as<Face>();
    for (const LibraryResult& r : results) {
        if (r.id != kFaceLibWindow) continue;
        if (ctx.tick <= f.lib_ask_tick) continue; // an older ask's, said in the tick the newer one was sent
        f.lib_total = r.total;
        f.lib_seconds = r.seconds;
        f.lib_generation = r.generation;
        f.lib_answered = true;
        f.lib_stale = false;
        f.awake = kAwakeTicks;
    }
}

// What the session kept for this face at the last run: taken back.
void on_kept(TickContext& ctx, Span<const SessionKept> kept) {
    Face& f = ctx.state_as<Face>();
    for (const SessionKept& k : kept) keep_restore(f, k);
    f.awake = kAwakeTicks;
}

// What was collected to be played is all in: play, a tick or two on, once whoever forwards
// the paths to the player has forwarded the last.
void on_collected(TickContext& ctx, Span<const LibraryCollected> paths) {
    Face& f = ctx.state_as<Face>();
    for (const LibraryCollected& p : paths) {
        if (p.id != kFaceCollectPlay && p.id != kFaceCollect) continue;
        f.awake = kAwakeTicks;
        if (p.id == kFaceCollectPlay && p.total && p.index + 1 == p.total) f.play_in = 3;
    }
}

// A frame was drawn. Whatever Krome said in it (a press, a list's new view) is an event
// that the next tick delivers, and under a loop that ticks only when asked the next tick
// may be a quarter second away. One is asked for: a frame costs a tick.
void ui_dev_frame(TickContext& ctx) {
    Face& f = ctx.state_as<Face>();
    if (!f.screen.is_null() || f.wanted) engine_want_ticks(*ctx.engine);
}

// Hidden, the face reads nothing and writes nothing; seen again, it writes everything.
void on_visibility(TickContext& ctx, Span<const WindowVisibility> events) {
    Face& f = ctx.state_as<Face>();
    for (const WindowVisibility& v : events) {
        if (f.hidden && v.visible) f.fresh = true;
        if (!f.hidden && !v.visible) { library_close(ctx, f); f.ev_on = false; } // no window onto the library while nobody looks, and no event kept
        f.hidden = !v.visible;
    }
}

void ui_dev_sync(TickContext& ctx) {
    Face& f = ctx.state_as<Face>();
    Engine& e = *ctx.engine;
    if (f.awake) { --f.awake; engine_want_ticks(e); }
    if (f.play_in) { engine_want_ticks(e); if (--f.play_in == 0) ctx.emit(PlayerPlay{}); }
    if (f.wanted) open_shell(f);
    if (f.screen.is_null()) return;
    if (f.hidden) { ++f.stats.asleep; return; }
    if (!f.ready) {
        if (!krome_ready(e, f.screen)) {
            // Loading, or failed: failed when nothing is still on its way and a mistake was found.
            const KimErrors* errors = krome_errors(e);
            if (errors && errors->total && krome_settled(e)) shell_failed(f);
            return;
        }
        f.ready = true;
        f.fresh = true;
    }
    if (f.fresh) { face_forget(f); f.fresh = false; }
    ++f.stats.syncs;
    // A tab the session kept, put up: the model's own, which the tabs read.
    if (f.tab_restore[0]) {
        face_forget(f, "r_tab");
        face_set(f, "r_tab", f.tab_restore);
        f.tab_restore[0] = 0;
        return; // read back next tick, as the screen has it
    }
    // Which inspector is up: the model's own, since the tabs write it themselves.
    const char* tab = krome_model_text(e, f.screen, "r_tab");
    f.tab = !strcmp(tab, "scan") ? FaceTab::Scan : !strcmp(tab, "events") ? FaceTab::Events : !strcmp(tab, "tables") ? FaceTab::Tables : FaceTab::Chain;
    titles_sync(ctx, f);
    transport_sync(ctx, f);
    library_sync(ctx, f);
    queue_sync(ctx, f);
    output_sync(ctx, f);
    chain_sync(ctx, f);
    scan_sync(ctx, f);
    events_sync(ctx, f);
    tables_sync(ctx, f);
    keep_sync(ctx, f);
}

bool ui_dev_startup(Engine& e, ModuleInstance& self) {
    Face* f = arena_push<Face>(self.arena);
    if (!f) return false;
    self.set_state(f);
    f->e = &e;
    format(f->crumbs[0].name, sizeof f->crumbs[0].name, "Folders");
    // Opened in the first tick, not here: the files it is given by hand are compiled, and
    // every module it reads is up by then.
    f->wanted = g_ui_dev_config.enabled && g_ui_dev_config.open;
    if (f->wanted) engine_want_ticks(e);
    // The Events panel's ring, and the engine's hook (dev builds; there is one, and a face
    // that is up takes it).
    if (g_ui_dev_config.enabled) {
        f->events = (FaceEvent*)mem_alloc(sizeof(FaceEvent) * kFaceEvents);
        for (u32 i = 0; i < kFaceEvents; ++i) f->events[i] = FaceEvent{};
        f->ev_hooked = engine_set_event_hook(e, events_hook, f);
    }
    return true;
}

void ui_dev_shutdown(Engine& e, ModuleInstance& self) {
    Face* f = self.state_as<Face>();
    if (!f) return;
    if (f->ev_hooked) engine_set_event_hook(e, nullptr, nullptr);
    f->ev_hooked = f->ev_on = false;
    if (f->events) mem_free(f->events);
    f->events = nullptr;
}

// The face from the engine's command line: a control pressed by its id, a model value
// read. For driving a real window from a script (the slice's measurements were), and for
// asking a running face what it shows.
void cmd_press(CliContext& ctx, Span<const char* const> args) {
    Face* f = face(*ctx.engine);
    if (args.len != 1 || !f || f->screen.is_null()) { ctx.fail("face press <a control's id>"); return; }
    if (!krome_press(*ctx.engine, f->screen, args[0])) { ctx.fail("there is no control '%s' on the face, or it does nothing", args[0]); return; }
    engine_want_ticks(*ctx.engine);
    engine_want_frame(*ctx.engine);
}
// The wheel turned over a control: a list scrolled, as a hand would scroll it.
void cmd_wheel(CliContext& ctx, Span<const char* const> args) {
    Face* f = face(*ctx.engine);
    rect2 r{};
    if (args.len != 2 || !f || f->screen.is_null() || !krome_find_rect(*ctx.engine, f->screen, args[0], &r)) { ctx.fail("face wheel <a control's id> <notches, negative down>"); return; }
    input_pointer_override(*ctx.engine, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0, (f32)atof(args[1]));
    engine_want_ticks(*ctx.engine);
    engine_want_frame(*ctx.engine);
}
void cmd_show(CliContext& ctx, Span<const char* const> args) {
    Face* f = face(*ctx.engine);
    if (args.len != 1 || !f || f->screen.is_null()) { ctx.fail("face show <a model key>"); return; }
    ctx.print("%s", krome_model_text(*ctx.engine, f->screen, args[0]));
}
void cmd_stats(CliContext& ctx, Span<const char* const>) {
    Face* f = face(*ctx.engine);
    if (!f) { ctx.fail("the face is not up"); return; }
    KromeStats k = krome_stats(*ctx.engine);
    ctx.print("%llu model writes, %llu commands, %llu ticks looked at, %llu asleep; %u panels standing in%s", (unsigned long long)f->stats.writes,
              (unsigned long long)f->stats.commands, (unsigned long long)f->stats.syncs, (unsigned long long)f->stats.asleep, f->stats.broken, f->stats.failed ? "; the screen did not load" : "");
    ctx.print("krome: %llu layouts, %llu paints, %llu lists handed over", (unsigned long long)k.layouts, (unsigned long long)k.paints, (unsigned long long)k.submits);
}
constinit const CliCommandDesc k_commands[] = {
    {.name = "face press", .help = "<id> presses a control of the face by its id, as a click would", .fn = cmd_press},
    {.name = "face wheel", .help = "<id> <notches> turns the wheel over a control: a list scrolled", .fn = cmd_wheel},
    {.name = "face show", .help = "<key> a value of the face's model, as the screen shows it", .fn = cmd_show},
    {.name = "face stats", .help = "what the face has done: model writes, commands, layouts, paints", .fn = cmd_stats},
};

constinit const HandlerDesc k_handlers[] = {
    {.name = ".events", KV_ON(KromeEvent, on_event), .phase = Phase::PreSim,
     .reads = "^player.queue, ^player.params, ^library.rows", // as the phase found them: what the screen was showing
     .emits = "PlayerPlay, PlayerPause, PlayerStop, PlayerNext, PlayerPrevious, PlayerJump, PlayerSeek, PlayerSeekBy, PlayerSetParam, PlayerApplyPreset, PlayerQueueEdit, PlayerEnqueue, "
              "LibraryAddRoot, LibraryRemoveRoot, LibraryOverride, LibraryCollect"},
    {.name = ".view", KV_ON(KromeView, on_view), .phase = Phase::PreSim},
    {.name = ".visibility", KV_ON(WindowVisibility, on_visibility), .phase = Phase::PreSim, .emits = "LibraryClose"},
    {.name = ".result", KV_ON(LibraryResult, on_result), .phase = Phase::PostSim, .before = ".sync"},
    {.name = ".collected", KV_ON(LibraryCollected, on_collected), .phase = Phase::PostSim, .before = ".sync"},
    {.name = ".kept", KV_ON(SessionKept, on_kept), .phase = Phase::PreSim},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".sync", .phase = Phase::PostSim, .fn = ui_dev_sync,
     .reads = "player.transport, player.output, player.queue, player.chain, player.params, player.presets, player.devices, library.roots, library.rows, library.scan",
     .emits = "LibraryQuery, LibraryPrioritize, LibraryPath, PlayerPlay, SessionKeep", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".frame", .phase = Phase::Frame, .fn = ui_dev_frame, .main_thread = true},
};

} // namespace

void ui_dev_configure(const UiDevConfig& config) { g_ui_dev_config = config; }

void ui_dev_open(Engine& e) {
    Face* f = face(e);
    if (!f || !g_ui_dev_config.enabled) return;
    f->attempts = 0;
    f->wanted = true;
    engine_want_ticks(e);
}

bool face_id(Name node, const char* prefix, u64* a, u64* b) {
    const char* id = node.is_null() ? nullptr : name_str(node);
    usize n = strlen(prefix);
    if (!id || strncmp(id, prefix, n) != 0) return false;
    const char* p = id + n;
    if (*p < '0' || *p > '9') return false;
    u64 v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (u64)(*p - '0');
    *a = v;
    if (!b) return *p == 0;
    if (*p != '_' || p[1] < '0' || p[1] > '9') return false;
    v = 0;
    for (++p; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (u64)(*p - '0');
    *b = v;
    return *p == 0;
}

void face_clock(char* out, usize cap, u64 frames, u32 rate) {
    u64 s = rate ? frames / rate : 0;
    if (s >= 3600) format(out, cap, "%llu:%02llu:%02llu", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    else           format(out, cap, "%llu:%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
}

const char* face_file_name(const char* path) {
    const char* name = path;
    for (const char* p = path; *p; ++p) if ((*p == '/' || *p == '\\') && p[1]) name = p + 1;
    return name;
}

Name ui_dev_screen(Engine& e) {
    Face* f = face(e);
    return f ? f->screen : Name{};
}

bool ui_dev_ready(Engine& e) {
    Face* f = face(e);
    return f && f->ready;
}

UiDevStats ui_dev_stats(Engine& e) {
    Face* f = face(e);
    return f ? f->stats : UiDevStats{};
}

KV_MODULE_DEFINE(ui_dev_module) = {
    .name     = "ui_dev",
    .systems  = k_systems,
    .handlers = k_handlers,
    .commands = k_commands,
    .startup  = ui_dev_startup,
    .shutdown = ui_dev_shutdown,
    .memory   = {.reserve = 1024 * 1024},
};

} // namespace kv
