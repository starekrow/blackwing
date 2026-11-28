// blackwing: the player's executable.
//
//   blackwing [options]                  the window, with the development face
//   blackwing [options] play FILE...     plays the files in order and exits after the last
//   blackwing [options] library [DIR...] keeps the library (adding DIRs to it) until quit
//   blackwing [options] run FILE.lua [ARG...]  runs a script over the player and the library,
//                                        to the code it ends with (kv.quit, or 1 if it raises)
//   blackwing help                       the commands the engine knows
//
//   --face NAME          which face is up: dev, or none (no window). Without a command it is
//                        dev; with one it is none unless said
//   --device NAME        the output device: "default", or "capture" (no hardware)
//   --capture-to FILE    with the capture device: what reached it, as raw PCM, written at exit (no face)
//   --lead SECONDS       how far ahead of the playhead audio is kept (5)
//   --verbose            the engine's INFO lines
//   --trace-events       every event as it is delivered (dev builds), with who emitted it
//   --data DIR           where the library keeps its index and its roots (the per-user data directory),
//                        and with it the session (the user's settings directory)
//   --no-session         neither restores how it was left nor saves it
//   --run-seconds N      ends the run after N seconds (tests, measurements)
//   --cli-port N         the engine's command line on a loopback port
//   --size WxH, --renderer auto|gpu|cpu, --screenshot FILE, --borderless, --hitch-ms N,
//   --goldberg PORT, --no-goldberg       the frame loop's own, with a face
//
// `play` is sugar for the player's own commands (player add, player play), which are also
// what a terminal on stdin may type while it plays: player pause, player play, player next,
// player stop, player status, quit. Everything goes to the player as events; this file
// calls nothing of it but its configuration.
//
// Two loops, one rule: the player ticks at the clock's rate while it or the library asks
// (engine_want_ticks), and four times a second when nothing does, so a player left paused
// costs nothing: between ticks both wait in the platform, which a command, a socket or a
// library thread with news ends at once. With a face the loop is Korvid's frame loop, paced Decoupled: those
// ticks, and a frame only when a screen changed, none while the window is hidden. With
// none it is this file's own, over an engine that holds no window module at all, so it
// runs on a machine with no display.
#include "korvid/blackwing/module.h"
#include "korvid/blackwing_library/library.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/blackwing_script/host.h"
#include "korvid/blackwing_session/session.h"
#include "korvid/cli/module.h"
#include "korvid/core/atomic.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/time.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/hifi_platform/capture.h"
#include "korvid/platform/io.h"
#include "korvid/platform/settings.h"
#include "korvid/platform/module.h"
#include "korvid/ui_dev/ui_dev.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace kv {

namespace {

constinit u32 g_interrupted = 0; // atomic: set by SIGINT
constinit bool g_keep = false;    // the library is kept: the end of the queue is not the end of the run
constexpr u32 kFindWindow = 0xB1AC0001; // the window `library find` fills
constinit bool g_find_said = true;       // the last find's answer has been printed: refills of its window are not

// What the command line asked for, done in the first tick, once every module is up: the
// engine's own commands, a line each.
constinit char** g_start_args = nullptr;
constinit int    g_start_count = 0;
constinit bool   g_start_library = false, g_start_play = false, g_started = false;
constinit int    g_code = 0; // the exit status, where the run's end is the engine's to reach

void on_sigint(int) { atomic_store(&g_interrupted, 1u, MO_RELAXED); }

void print_line(void* user, const char* line) {
    (void)user;
    fputs(line, stdout);
    fputc('\n', stdout);
}
// For requests whose answer nobody wants ("queued 1"); a failure still prints, on stderr.
// ---- the module: what the player says, said on stdout ---------------------------------

void on_started(TickContext&, Span<const PlayerTrackStarted> events) {
    for (const PlayerTrackStarted& e : events) {
        f64 seconds = e.rate ? (f64)e.frames / e.rate : 0.0;
        printf("playing %llu: %u Hz, %u ch, %u bit, %.1f s\n", (unsigned long long)e.item, e.rate, e.channels, e.bits, seconds);
    }
    fflush(stdout);
}
void on_seeked(TickContext& ctx, Span<const PlayerSeeked> events) {
    const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
    for (const PlayerSeeked& e : events) {
        u32 rate = t && t->item == e.item ? t->rate : 0;
        if (rate) printf("at %.1f s of %llu\n", (f64)e.frame / rate, (unsigned long long)e.item);
        else      printf("at frame %llu of %llu\n", (unsigned long long)e.frame, (unsigned long long)e.item);
    }
    fflush(stdout);
}
void on_ended(TickContext&, Span<const PlayerTrackEnded> events) {
    for (const PlayerTrackEnded& e : events)
        if (e.damaged) printf("ended %llu early: %s\n", (unsigned long long)e.item, e.reason);
    fflush(stdout);
}
void on_failed(TickContext&, Span<const PlayerTrackFailed> events) {
    for (const PlayerTrackFailed& e : events) printf("cannot play %llu: %s\n", (unsigned long long)e.item, e.reason);
    fflush(stdout);
}
void on_underrun(TickContext&, Span<const HifiUnderrun> events) {
    for (const HifiUnderrun& e : events) printf("underrun (%u)\n", e.count);
    fflush(stdout);
}
void on_queue_ended(TickContext& ctx, Span<const PlayerQueueEnded>) { if (!g_keep) engine_request_quit(*ctx.engine); }

// ---- the library: what it says ---------------------------------------------------------------
// (What the player and the library say to each other is blackwing_glue's.)

void on_scan_finished(TickContext&, Span<const LibraryScanFinished> events) {
    for (const LibraryScanFinished& e : events) printf("walked root %u: %u tracks in %.1f s\n", e.root, e.tracks, e.seconds);
    fflush(stdout);
}
// `library find`'s answer, when it comes: a numbered list `library play` takes a number from.
void on_result(TickContext& ctx, Span<const LibraryResult> events) {
    for (const LibraryResult& e : events) {
        if (e.id != kFindWindow || g_find_said) continue;
        g_find_said = true;
        const KTable* t = ctx.read_ktable("library.rows");
        printf("%u found%s\n", e.total, e.total > e.count ? " (the first shown)" : "");
        if (!t) continue;
        Span<const LibraryRow> rows = t->rows_of<LibraryRow>();
        for (u32 k = 0; k < e.count; ++k)
            for (u32 i = 0; i < rows.len; ++i)
                if (t->is_alive(i) && rows[i].query == kFindWindow && rows[i].index == k)
                    printf("%3u. %s%s%s%s%s\n", k + 1, rows[i].title[0] ? rows[i].title : rows[i].name, rows[i].artist[0] ? " - " : "",
                           rows[i].artist, rows[i].album[0] ? " - " : "", rows[i].album);
    }
    fflush(stdout);
}

// The first tick: the command line's files or directories, as the engine's own commands.
// Every tick: an interrupt ends the run as closing the window does.
void blackwing_tick(TickContext& ctx) {
    Engine& e = *ctx.engine;
    if (!g_started) {
        g_started = true;
        // As events, not command lines: a path is whatever the shell passed, any length the OS takes.
        bool failed = false;
        for (int i = 0; i < g_start_count && !failed; ++i) {
            const char* arg = g_start_args[i];
            if (strlen(arg) >= kPlayerPathMax) { fprintf(stderr, "blackwing: this path is longer than a path can be: %s\n", arg); failed = true; break; }
            if (g_start_library) { LibraryAddRoot r{}; format(r.path, sizeof r.path, "%s", arg); e.events.emit(r); }
            else                 { PlayerEnqueue q{}; format(q.path, sizeof q.path, "%s", arg); e.events.emit(q); }
        }
        if (!failed && g_start_play) e.events.emit(PlayerPlay{});
        if (failed) { g_code = 2; engine_request_quit(e); }
        engine_want_ticks(e); // the commands are events: the player acts on them in the next tick, which should not wait for a heartbeat
    }
    if (atomic_load(&g_interrupted, MO_RELAXED) && !e.quit_requested) {
        fputs("\ninterrupted\n", stdout);
        g_code = 130;
        engine_request_quit(e);
    }
}

constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::PreSim, .fn = blackwing_tick, .main_thread = true, .runs = RunWhen::Always},
};

constinit const HandlerDesc k_handlers[] = {
    {.name = ".started",     KV_ON(PlayerTrackStarted, on_started),   .phase = Phase::PostSim},
    {.name = ".ended",       KV_ON(PlayerTrackEnded, on_ended),       .phase = Phase::PostSim},
    {.name = ".seeked",      KV_ON(PlayerSeeked, on_seeked),          .phase = Phase::PostSim, .reads = "player.transport"},
    {.name = ".failed",      KV_ON(PlayerTrackFailed, on_failed),     .phase = Phase::PostSim},
    {.name = ".underrun",    KV_ON(HifiUnderrun, on_underrun),        .phase = Phase::PostSim},
    {.name = ".queue_ended", KV_ON(PlayerQueueEnded, on_queue_ended), .phase = Phase::PostSim},
    {.name = ".scanned",     KV_ON(LibraryScanFinished, on_scan_finished), .phase = Phase::PostSim},
    {.name = ".found",       KV_ON(LibraryResult, on_result),         .phase = Phase::PostSim, .reads = "library.rows"},
};

void cmd_find(CliContext& ctx, Span<const char* const> args) {
    if (args.len == 0) { ctx.fail("library find <words>..."); return; }
    LibraryQuery q;
    q.id = kFindWindow;
    q.view = LibraryView::Search;
    q.count = 20;
    usize n = 0;
    for (const char* a : args) n += (usize)format(q.text + n, sizeof q.text - n, "%s%s", n ? " " : "", a);
    ctx.engine->events.emit(q);
    g_find_said = false;
}

// Plays the n-th track `library find` listed: the player is told its path, and to play it now.
void cmd_play(CliContext& ctx, Span<const char* const> args) {
    u32 n = args.len == 1 ? (u32)atoi(args[0]) : 0;
    if (n == 0) { ctx.fail("library play <number from library find>"); return; }
    KTable* t = engine_ktable(*ctx.engine, "library.rows");
    if (!t) { ctx.fail("the library is not running"); return; }
    Span<LibraryRow> rows = t->rows_of<LibraryRow>();
    for (u32 i = 0; i < rows.len; ++i) {
        if (!t->is_alive(i) || rows[i].query != kFindWindow || rows[i].index != n - 1) continue;
        if (!rows[i].path[0]) { ctx.fail("%s cannot be played: %s", rows[i].name, rows[i].reason); return; }
        PlayerEnqueue q{};
        memcpy(q.path, rows[i].path, sizeof q.path);
        ctx.engine->events.emit(q);
        ctx.engine->events.emit(PlayerPlay{});
        ctx.print("queued %s", rows[i].title[0] ? rows[i].title : rows[i].name);
        return;
    }
    ctx.fail("there is no %u in the last find", n);
}

// Sets a field of the n-th track the last find listed, for the user: `library override 3
// title The Real Title`; no value hides the field; `--clear` gives the file's own back.
void cmd_override(CliContext& ctx, Span<const char* const> args) {
    u32 n = args.len >= 2 ? (u32)atoi(args[0]) : 0;
    if (n == 0) { ctx.fail("library override <number from library find> <field> [value...|--clear]"); return; }
    KTable* t = engine_ktable(*ctx.engine, "library.rows");
    if (!t) { ctx.fail("the library is not running"); return; }
    Span<LibraryRow> rows = t->rows_of<LibraryRow>();
    for (u32 i = 0; i < rows.len; ++i) {
        if (!t->is_alive(i) || rows[i].query != kFindWindow || rows[i].index != n - 1 || rows[i].kind != (u8)LibraryRowKind::Track) continue;
        LibraryOverride o;
        o.track = rows[i].id;
        format(o.field, sizeof o.field, "%s", args[1]);
        usize k = 0;
        for (u32 a = 2; a < args.len; ++a) {
            if (strcmp(args[a], "--clear") == 0) { o.clear = 1; continue; }
            k += (usize)format(o.value + k, sizeof o.value - k, "%s%s", k ? " " : "", args[a]);
        }
        ctx.engine->events.emit(o);
        ctx.print(o.clear ? "%s of %s: its own again" : o.value[0] ? "%s of %s: \"%s\"" : "%s of %s: hidden", o.field,
                  rows[i].title[0] ? rows[i].title : rows[i].name, o.value);
        return;
    }
    ctx.fail("there is no %u in the last find", n);
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "library find", .help = "<words>... tracks whose title, artist, album or path has words beginning so", .fn = cmd_find},
    {.name = "library play", .help = "<number> queues a track the last find listed, and plays", .fn = cmd_play},
    {.name = "library override", .help = "<number> <field> [value...|--clear] sets a field of a found track for you; no value hides it", .fn = cmd_override},
};

} // namespace

KV_MODULE_DEFINE(blackwing_module) = {
    .name     = "blackwing",
    .systems  = k_systems,
    .handlers = k_handlers,
    .commands = k_commands,
};

namespace {

void usage() {
    fputs("usage: blackwing [options]                  the window, with the development face\n"
          "       blackwing [options] play FILE...\n"
          "       blackwing [options] library [DIR...]\n"
          "       blackwing [options] run FILE.lua [ARG...]\n"
          "       blackwing help\n"
          "options: --face dev|none, --device NAME, --capture-to FILE, --lead SECONDS, --data DIR, --verbose, --trace-events,\n"
          "         --run-seconds N, --cli-port N; with a face: --size WxH, --renderer auto|gpu|cpu, --screenshot FILE,\n"
          "         --borderless, --hitch-ms N, --goldberg PORT, --no-goldberg\n", stderr);
}

// Whether there is a display to open a window on. Where the OS always has one, yes.
bool have_display() {
#if defined(__linux__)
    const char* x = getenv("DISPLAY");
    const char* w = getenv("WAYLAND_DISPLAY");
    return (x && x[0]) || (w && w[0]);
#else
    return true;
#endif
}

// Where the session is, before there is an engine to ask: the directory named, or the
// user's settings directory as the engine would name it. Only the window's size is read
// from there this early; where the rule is not known (it is the engine's, per OS), no
// size is read and the window opens at its own.
bool session_dir(const char* data_dir, char* out, usize cap) {
    if (data_dir && data_dir[0]) { format(out, cap, "%s", data_dir); return true; }
#if defined(__linux__)
    const char* config = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    if (config && config[0] == '/') { format(out, cap, "%s/blackwing/blackwing", config); return true; }
    if (home && home[0]) { format(out, cap, "%s/.config/blackwing/blackwing", home); return true; }
#endif
    (void)out; (void)cap;
    return false;
}

// The engine with no face: the modules a machine with no display can start. The frame
// loop, the window, Krome and the faces are linked in and left out.
constinit const char* const k_headless[] = {"platform", "cli", "player", "library", "glue", "session", "script", "blackwing"};
constinit const ModuleDesc* g_headless[KV_COUNTOF(k_headless)] = {};
Span<const ModuleDesc* const> headless_modules() {
    u32 n = 0;
    for (const ModuleDesc* m : kv_module_list())
        for (const char* name : k_headless)
            if (strcmp(m->name, name) == 0 && n < KV_COUNTOF(g_headless)) g_headless[n++] = m;
    return Span<const ModuleDesc* const>(g_headless, n);
}

// What the capture device was given, to a file: each device's frames as they were, one
// after another. Raw PCM in the device's format, which the run's output names.
bool write_capture(HifiCapture* cap, const char* path) {
    HifiCaptureSegment seg[64];
    u32 n = hifi_capture_segments(cap, seg, 64);
    IoFileWriter f;
    if (!f.open(path)) { fprintf(stderr, "blackwing: cannot write %s: %s\n", path, os_error_text(f.error)); return false; }
    for (u32 i = 0; i < n && i < 64; ++i) {
        f.write(hifi_capture_data(cap) + seg[i].offset, seg[i].bytes);
        printf("captured: %zu bytes of %u Hz, %u ch, %s\n", seg[i].bytes, seg[i].format.rate, seg[i].format.channels,
               hifi_sample_format_name(seg[i].format.sample));
    }
    if (!f.close()) { fprintf(stderr, "blackwing: %s was not written whole: %s\n", path, os_error_text(f.error)); return false; }
    if (u64 dropped = hifi_capture_dropped(cap)) printf("captured: %llu frames did not fit and were dropped\n", (unsigned long long)dropped);
    return true;
}

// The frame loop's options that pass through to it as they are, and whether each takes a value.
struct PassOption { const char* name; bool value; };
constinit const PassOption k_pass[] = {
    {"--size", true}, {"--renderer", true}, {"--screenshot", true}, {"--screenshot-frame", true}, {"--hitch-ms", true},
    {"--goldberg", true}, {"--no-goldberg", false}, {"--borderless", false}, {"--no-vsync", false},
};

constinit const FrameLoopConfig k_window = {
    .title  = "Blackwing",
    .width  = 1280,
    .height = 800,
    .cpu_scale = 1,           // every pixel: text
    .exit_on_escape = false,  // Escape closes a menu, not the player
    .report_every_seconds = 0,
    .pacing = Pacing::Decoupled,
};

int run(int argc, char** argv, bool* engine_owns_exit) {
    const char* device = "default";
    bool device_named = false;
    const char* capture_to = nullptr;
    const char* data_dir = nullptr;
    const char* face = nullptr;
    const char* run_seconds = nullptr;
    const char* cli_port = nullptr;
    f64 lead = 5.0;
    bool verbose = false, trace = false, no_session = false, sized = false;
    // What the frame loop is given, should there be a face: its own options, as they came.
    static char* loop_argv[48];
    int loop_argc = 0;
    loop_argv[loop_argc++] = argv[0];
    int first = 1;
    for (; first < argc && strncmp(argv[first], "--", 2) == 0; ++first) {
        const PassOption* pass = nullptr;
        for (const PassOption& o : k_pass) if (strcmp(argv[first], o.name) == 0) pass = &o;
        if (pass && strcmp(pass->name, "--size") == 0) sized = true;
        if (pass && (!pass->value || first + 1 < argc)) {
            if (loop_argc + 2 < (int)KV_COUNTOF(loop_argv)) { loop_argv[loop_argc++] = argv[first]; if (pass->value) loop_argv[loop_argc++] = argv[++first]; }
        }
        else if (strcmp(argv[first], "--device") == 0 && first + 1 < argc) { device = argv[++first]; device_named = true; }
        else if (strcmp(argv[first], "--capture-to") == 0 && first + 1 < argc) capture_to = argv[++first];
        else if (strcmp(argv[first], "--lead") == 0 && first + 1 < argc) lead = atof(argv[++first]);
        else if (strcmp(argv[first], "--data") == 0 && first + 1 < argc) data_dir = argv[++first];
        else if (strcmp(argv[first], "--face") == 0 && first + 1 < argc) face = argv[++first];
        else if (strcmp(argv[first], "--run-seconds") == 0 && first + 1 < argc) run_seconds = argv[++first];
        else if (strcmp(argv[first], "--cli-port") == 0 && first + 1 < argc) cli_port = argv[++first];
        else if (strcmp(argv[first], "--no-session") == 0) no_session = true;
        else if (strcmp(argv[first], "--verbose") == 0) verbose = true;
        else if (strcmp(argv[first], "--trace-events") == 0) trace = true;
        else { fprintf(stderr, "blackwing: unknown option %s\n", argv[first]); usage(); return 2; }
    }
    const char* command = first < argc ? argv[first] : "";
    bool help = strcmp(command, "help") == 0;
    bool play = strcmp(command, "play") == 0;
    bool library = strcmp(command, "library") == 0;
    bool script = strcmp(command, "run") == 0;
    bool bare = !command[0];
    if ((!bare && !help && !play && !library && !script) || ((play || script) && first + 1 >= argc) || lead <= 0.0) { usage(); return 2; }
    if (!face) face = bare ? "dev" : "none";
    bool window = strcmp(face, "dev") == 0;
    if (!window && strcmp(face, "none") != 0) { fprintf(stderr, "blackwing: there is no face called %s (dev, none)\n", face); return 2; }
    if (help) window = false;
    if (window && capture_to) { fputs("blackwing: --capture-to writes at exit, which a run with a face does not reach; use --device capture, or --face none\n", stderr); return 2; }
    // No display: say so, and run with no face if there is something to do without one.
    if (window && !have_display()) {
        if (bare) { fputs("blackwing: there is no display to open a window on (play, library and help need none)\n", stderr); return 1; }
        fputs("blackwing: there is no display; running with no face\n", stderr);
        window = false;
    }
    g_keep = library || window || script; // a face, a kept library or a script: the end of the queue is not the end of the run
    g_start_args = argv + first + 1;
    g_start_count = bare || help || script ? 0 : argc - first - 1;
    // A script: the file, and what follows it as its arguments. It ends the run itself.
    ScriptHostConfig sc;
    if (script) {
        sc.file = argv[first + 1];
        sc.args = (const char* const*)(argv + first + 2);
        sc.nargs = (u32)(argc - first - 2);
    }
    script_host_configure(sc);
    // The session: a sitting has one, a command does not. With a face, or a library kept
    // open, how it was left comes back and is kept; `play` and `run` neither restore nor save.
    SessionConfig session;
    session.enabled = !no_session && !play && !script && !help && (window || library);
    session.dir = data_dir; // beside the library's, when a directory was named; else the user's settings directory
    session.restore_device = !device_named && !capture_to; // a device named on the command line is this run's
    session_configure(session);
    g_start_library = library;
    g_start_play = play;
    if (capture_to) device = "capture";

    LogLevel was = log_level();
    if (!verbose && was < LogLevel::Warn) log_set_level(LogLevel::Warn);

    HifiCapture* capture = nullptr;
    if (capture_to) {
        HifiCaptureDesc cd;
        cd.wall_clock = true;
        cd.keep_bytes = (usize)1 << 30; // a gigabyte of what reached it
        capture = hifi_capture_create(cd);
    }
    PlayerConfig pc;
    pc.device = device;
    pc.device_backend = capture; // none: the capture device, asked for by name, is the player's own
    pc.lead_seconds = lead;
    // The settings, read before the engine starts: `absent_device` is "default" (a device
    // chosen and not there: the default plays meanwhile) or "wait" (nothing plays until it
    // comes). The store's file is the engine's settings.json.
    {
        char settings_at[4200];
        if (session_dir(data_dir, settings_at, sizeof settings_at)) {
            if (SettingsStore* st = settings_open_dir(settings_at)) {
                const char* absent = settings_string(st, "absent_device", "default");
                pc.wait_for_absent = strcmp(absent, "wait") == 0;
                if (strcmp(absent, "wait") != 0 && strcmp(absent, "default") != 0)
                    fprintf(stderr, "blackwing: the setting absent_device is \"%s\", which is neither \"default\" nor \"wait\": the default is used\n", absent);
                settings_close(st);
            }
        }
    }
    player_configure(pc);
    LibraryConfig lc;
    lc.data_dir = data_dir;
    library_configure(lc);
#if !defined(_WIN32)
    signal(SIGINT, on_sigint);
    bool terminal = isatty(0) != 0; // a terminal may type commands while it plays; a pipe is left alone
#else
    bool terminal = false;
#endif

    if (window) {
        // The frame loop's run: it makes the engine from the whole module list, opens the
        // window and the renderer, loops, and shuts everything down, memory included.
        static char quiet[] = "--quiet", stdin_flag[] = "--cli-stdin", trace_flag[] = "--trace-events", port_flag[] = "--cli-port", run_flag[] = "--run-seconds";
        // The window at the size it was left at, unless one was asked for: the face kept it
        // in the session, which is read here because the window opens before any module can.
        static char size_flag[] = "--size", size_text[32];
        char settings[4200];
        if (session.enabled && !sized && session_dir(data_dir, settings, sizeof settings)) {
            f64 w = session_peek(settings, "ui_dev", "w", 0.0), h = session_peek(settings, "ui_dev", "h", 0.0);
            if (w >= 320.0 && h >= 240.0 && w <= 16384.0 && h <= 16384.0) {
                format(size_text, sizeof size_text, "%ux%u", (u32)w, (u32)h);
                loop_argv[loop_argc++] = size_flag;
                loop_argv[loop_argc++] = size_text;
            }
        }
        if (!verbose && !trace) loop_argv[loop_argc++] = quiet;
        if (terminal) loop_argv[loop_argc++] = stdin_flag;
        if (trace) loop_argv[loop_argc++] = trace_flag;
        if (cli_port) { loop_argv[loop_argc++] = port_flag; loop_argv[loop_argc++] = (char*)cli_port; }
        if (run_seconds) { loop_argv[loop_argc++] = run_flag; loop_argv[loop_argc++] = (char*)run_seconds; }
        loop_argv[loop_argc] = nullptr;
        *engine_owns_exit = true;
        int code = frameloop_main(loop_argc, loop_argv, k_window);
        return code ? code : g_code ? g_code : script_host_exit_code();
    }

    UiDevConfig no_face;
    no_face.enabled = false;
    ui_dev_configure(no_face);
    CliConfig cli;
    cli.stdin_ = terminal;
    cli.no_stdin = !terminal;
    if (cli_port) cli.port = (u16)atoi(cli_port);
    cli_configure(cli);
    PlatformMounts mounts;
    mounts.none = true; // files are named by the OS's paths, not found in asset mounts
    platform_configure_mounts(mounts);

    int code = 0;
    Engine e;
    e.trace.print = trace;
    if (trace && !KV_DEV) KV_WARN("blackwing", "--trace-events: the event trace exists in dev builds only");
    if (!engine_init(e, headless_modules())) {
        code = 1;
    } else {
        // The trace is INFO lines. Turned on after the engine is up, they come almost alone.
        if (trace && log_level() > LogLevel::Info) log_set_level(LogLevel::Info);
        if (help) {
            cli_run(e, "help", print_line, nullptr);
        } else {
            // The next tick at the clock's rate while a module asked for it (the player
            // while it plays, the library while it scans), and four times a second
            // otherwise. Between ticks the loop waits in the platform (Korvid's KR26), not
            // a sleep: a command on stdin or the CLI's port, or a library thread with
            // something for the main thread (platform_wake), ends the wait at once and the
            // pass after it ticks; a read on its way shortens it (platform_idle_time).
            constexpr f64 k_heartbeat = 0.25;
            f64 limit = run_seconds ? atof(run_seconds) : -1.0;
            f64 start = time_now(), last = start;
            IoQueue* io = platform_io(e);
            bool woke = true; // the first pass ticks: the command line's files are its commands
            while (!e.quit_requested) {
                platform_pump(e);
                f64 now = time_now();
                if (limit >= 0.0 && now - start >= limit) break;
                f64 next = last + (e.ticks_wanted ? e.clock.tick_dt : k_heartbeat);
                bool tick = woke || now >= next || (io && io->completed_count());
                if (engine_wake(e)) tick = true; // the CLI's wakers state what they watch here
                if (!tick) {
                    f64 wait = next - now;
                    if (limit >= 0.0 && start + limit - now < wait) wait = start + limit - now;
                    woke = platform_wait(e, platform_idle_time(e, wait)) == PlatformWoken::Wake;
                    continue;
                }
                woke = false;
                engine_frame(e, now - last);
                last = now;
            }
            code = g_code ? g_code : script_host_exit_code();
            const PlayerOutput* o = player_output(e);
            if (o && o->underruns) printf("%u underruns\n", o->underruns);
        }
        if (trace && !verbose) log_set_level(LogLevel::Warn); // the trace is over; shutdown's lines are not wanted
        engine_shutdown(e); // the player closes its device here, so the capture is whole
    }
    if (capture) {
        if (code != 1 && !write_capture(capture, capture_to) && code == 0) code = 1;
        hifi_capture_destroy(capture);
    }
    log_set_level(was);
    return code;
}

} // namespace

} // namespace kv

int main(int argc, char** argv) {
    bool engine_owns_exit = false; // the frame loop's run: names and memory are shut down by it
    int code = kv::run(argc, argv, &engine_owns_exit);
    if (engine_owns_exit) return code;
    kv::names_shutdown();
    kv::u64 live = kv::mem_live_allocations();
    if (live && (KV_DEV || KV_DEBUG)) {
        KV_ERROR("blackwing", "%llu allocations still live at exit", (unsigned long long)live);
        if (code == 0) code = 3;
    }
    kv::mem_shutdown();
    return code;
}
