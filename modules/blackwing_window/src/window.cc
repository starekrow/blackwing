// The window's side of the application, for whichever face is up. Closing the window while
// the transport plays hides it, and the music goes on with the OS's media surface live
// (slice 5, BP29); the surface's Raise (MediaRaise) brings it back as it was. Closing it
// with nothing playing ends the run, as interrupting it does; so does the queue running
// out while it is hidden. The engine then shuts down in order, so the session is saved and
// the device closed cleanly.
//
// It also counts what the window costs, since that is what slice 4 has to prove: ticks,
// frames drawn (Frame cycles), and whether the window is seen. `window status` says them;
// `window minimize` and `window restore` hide and show it, and `window stall` holds the
// main thread as a compositor that stalls a resize does, so each can be measured in the
// real thing.
#include "korvid/blackwing_window/module.h"
#include "korvid/blackwing_media/media.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/time.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/module.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

struct Window {
    u64 ticks = 0;        // sim ticks run
    u64 frames = 0;       // frames drawn: Frame cycles
    u64 hidden_ticks = 0; // ticks run while the window was not seen
    u64 hidden_frames = 0;
    u32 hides = 0;        // times it stopped being seen
    bool hidden = false;
    bool closed = false;  // closed while playing: hidden, the music going on
    u32  closes = 0;      // times that happened
    f64 since = 0.0;      // when the counts were last zeroed
};

bool transport_moving(TickContext& ctx) {
    const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
    return t && (t->state == (u8)PlayerState::Playing || t->state == (u8)PlayerState::Starting);
}

Window* window_state(Engine& e) { return engine_module_state_as<Window>(e, "window"); }

void on_exit(TickContext& ctx, Span<const ExitRequested> exits) {
    Window& w = ctx.state_as<Window>();
    for (const ExitRequested& x : exits) {
        HostWindow* hw = platform_window(*ctx.engine);
        if (x.reason == ExitReason::WindowClosed && hw && transport_moving(ctx)) {
            if (!w.closed) { hw->show(false); w.closed = true; ++w.closes; }
            continue;
        }
        engine_request_quit(*ctx.engine);
        return;
    }
}

void on_raise(TickContext& ctx, Span<const MediaRaise> raises) {
    Window& w = ctx.state_as<Window>();
    HostWindow* hw = platform_window(*ctx.engine);
    if (!raises.len || !hw) return;
    if (w.closed) { hw->show(true); w.closed = false; }
    hw->minimize(false);
}

void on_visibility(TickContext& ctx, Span<const WindowVisibility> events) {
    Window& w = ctx.state_as<Window>();
    for (const WindowVisibility& v : events) {
        if (!v.visible && !w.hidden) ++w.hides;
        w.hidden = !v.visible;
    }
}

void window_tick(TickContext& ctx) {
    Window& w = ctx.state_as<Window>();
    // Closed and hidden, and the music has ended (the queue ran out, or a stop): nothing to
    // play and nothing on screen.
    if (w.closed) {
        const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
        if (!t || t->state == (u8)PlayerState::Stopped) engine_request_quit(*ctx.engine);
    }
    ++w.ticks;
    if (w.hidden) ++w.hidden_ticks;
}

void window_frame(TickContext& ctx) {
    Window& w = ctx.state_as<Window>();
    ++w.frames;
    if (w.hidden) ++w.hidden_frames;
}

void cmd_status(CliContext& ctx, Span<const char* const> args) {
    Window* w = window_state(*ctx.engine);
    if (!w) { ctx.fail("there is no window"); return; }
    f64 seconds = time_now() - w->since;
    if (HostWindow* hw = platform_window(*ctx.engine)) ctx.print("%u x %u pixels, %.2f to a unit", hw->width(), hw->height(), (f64)hw->pixel_scale());
    ctx.print("%s%s; %llu ticks, %llu frames in %.1f s; hidden %u times: %llu ticks, %llu frames while hidden",
              w->closed ? "closed while playing, " : "", w->hidden ? "hidden" : "shown",
              (unsigned long long)w->ticks, (unsigned long long)w->frames, seconds, w->hides, (unsigned long long)w->hidden_ticks,
              (unsigned long long)w->hidden_frames);
    if (args.len == 1 && strcmp(args[0], "reset") == 0) {
        bool hidden = w->hidden;
        *w = Window{};
        w->hidden = hidden;
        w->since = time_now();
    }
}

void cmd_minimize(CliContext& ctx, Span<const char* const>) {
    HostWindow* w = platform_window(*ctx.engine);
    if (!w) { ctx.fail("there is no window"); return; }
    w->minimize(true);
}

void cmd_restore(CliContext& ctx, Span<const char* const>) {
    HostWindow* w = platform_window(*ctx.engine);
    if (!w) { ctx.fail("there is no window"); return; }
    w->minimize(false);
}

// What the window's close button sends, sent: so the close can be driven from a command
// line, where no desktop lets a script press another program's button.
void cmd_close(CliContext& ctx, Span<const char* const>) {
    if (!platform_window(*ctx.engine)) { ctx.fail("there is no window"); return; }
    ctx.engine->events.emit(CloseRequested{0});
    ctx.print("closing the window: it is hidden while the music plays, else the run ends");
}

// Holds the main thread: nothing pumps, ticks or draws for that long, as when a
// compositor stalls a resize or a title bar is dragged on an OS that blocks for it. The
// audio path has its own thread and its lead; this is how to hear whether that is enough.
void cmd_stall(CliContext& ctx, Span<const char* const> args) {
    f64 seconds = args.len == 1 ? atof(args[0]) : 0.0;
    if (seconds <= 0.0 || seconds > 30.0) { ctx.fail("window stall <seconds, up to 30>"); return; }
    time_sleep(seconds);
    ctx.print("held the main thread %.1f s", seconds);
}

bool window_startup(Engine&, ModuleInstance& self) {
    Window* w = arena_push<Window>(self.arena);
    if (!w) return false;
    self.set_state(w);
    w->since = time_now();
    return true;
}

constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::PreSim, .fn = window_tick, .reads = "player.transport", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".frame", .phase = Phase::Frame, .fn = window_frame, .main_thread = true},
};
constinit const HandlerDesc k_handlers[] = {
    {.name = ".exit", KV_ON(ExitRequested, on_exit), .phase = Phase::PreSim, .reads = "player.transport"},
    {.name = ".raise", KV_ON(MediaRaise, on_raise), .phase = Phase::PreSim},
    {.name = ".visibility", KV_ON(WindowVisibility, on_visibility), .phase = Phase::PreSim},
};
constinit const CliCommandDesc k_commands[] = {
    {.name = "window status", .help = "[reset] shown or hidden, and the ticks and frames since the counts were last reset", .fn = cmd_status},
    {.name = "window minimize", .help = "minimizes the window", .fn = cmd_minimize},
    {.name = "window restore", .help = "restores it (where the desktop lets a program)", .fn = cmd_restore},
    {.name = "window close", .help = "what the window's close button does: hidden while playing, else the run ends", .fn = cmd_close},
    {.name = "window stall", .help = "<seconds> holds the main thread: no pump, tick or frame for that long", .fn = cmd_stall},
};

} // namespace

KV_MODULE_DEFINE(blackwing_window_module) = {
    .name     = "window",
    .systems  = k_systems,
    .handlers = k_handlers,
    .commands = k_commands,
    .startup  = window_startup,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
