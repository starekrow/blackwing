// blackwing: the player's executable, headless for now.
//
//   blackwing [options] play FILE...     plays the files in order and exits after the last
//   blackwing help                       the commands the engine knows
//
//   --device NAME        the output device: "default", or "capture" (no hardware)
//   --capture-to FILE    with the capture device: what reached it, as raw PCM, written at exit
//   --lead SECONDS       how far ahead of the playhead audio is kept (5)
//   --verbose            the engine's INFO lines
//
// `play` is sugar for the player's own commands (player add, player play), which are also
// what a terminal on stdin may type while it plays: player pause, player play, player next,
// player stop, player status, quit. Everything goes to the player as events; this file
// calls nothing of it but its configuration.
//
// The loop is this file's own. It ticks the engine at its clock's rate while something is
// playing, and four times a second when nothing is, so a player left paused costs nothing.
#include "korvid/blackwing/module.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/cli/module.h"
#include "korvid/core/atomic.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/time.h"
#include "korvid/hifi_platform/capture.h"
#include "korvid/platform/module.h"
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

void on_sigint(int) { atomic_store(&g_interrupted, 1u, MO_RELAXED); }

void print_line(void* user, const char* line) {
    (void)user;
    fputs(line, stdout);
    fputc('\n', stdout);
}
// For requests whose answer nobody wants ("queued 1"); a failure still prints, on stderr.
void quiet_line(void*, const char*) {}

// ---- the module: what the player says, said on stdout ---------------------------------

void on_started(TickContext&, Span<const PlayerTrackStarted> events) {
    for (const PlayerTrackStarted& e : events) {
        f64 seconds = e.rate ? (f64)e.frames / e.rate : 0.0;
        printf("playing %llu: %u Hz, %u ch, %u bit, %.1f s\n", (unsigned long long)e.item, e.rate, e.channels, e.bits, seconds);
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
void on_queue_ended(TickContext& ctx, Span<const PlayerQueueEnded>) { engine_request_quit(*ctx.engine); }

constinit const HandlerDesc k_handlers[] = {
    {.name = ".started",     KV_ON(PlayerTrackStarted, on_started),   .phase = Phase::PostSim},
    {.name = ".ended",       KV_ON(PlayerTrackEnded, on_ended),       .phase = Phase::PostSim},
    {.name = ".failed",      KV_ON(PlayerTrackFailed, on_failed),     .phase = Phase::PostSim},
    {.name = ".underrun",    KV_ON(HifiUnderrun, on_underrun),        .phase = Phase::PostSim},
    {.name = ".queue_ended", KV_ON(PlayerQueueEnded, on_queue_ended), .phase = Phase::PostSim},
};

} // namespace

KV_MODULE_DEFINE(blackwing_module) = {
    .name     = "blackwing",
    .handlers = k_handlers,
};

namespace {

void usage() {
    fputs("usage: blackwing [--device NAME] [--capture-to FILE] [--lead SECONDS] [--verbose] play FILE...\n"
          "       blackwing help\n", stderr);
}

// What the capture device was given, to a file: each device's frames as they were, one
// after another. Raw PCM in the device's format, which the run's output names.
bool write_capture(HifiCapture* cap, const char* path) {
    HifiCaptureSegment seg[64];
    u32 n = hifi_capture_segments(cap, seg, 64);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "blackwing: cannot write %s\n", path); return false; }
    for (u32 i = 0; i < n && i < 64; ++i) {
        fwrite(hifi_capture_data(cap) + seg[i].offset, 1, seg[i].bytes, f);
        printf("captured: %zu bytes of %u Hz, %u ch, %s\n", seg[i].bytes, seg[i].format.rate, seg[i].format.channels,
               hifi_sample_format_name(seg[i].format.sample));
    }
    fclose(f);
    if (u64 dropped = hifi_capture_dropped(cap)) printf("captured: %llu frames did not fit and were dropped\n", (unsigned long long)dropped);
    return true;
}

int run(int argc, char** argv) {
    const char* device = "default";
    const char* capture_to = nullptr;
    f64 lead = 5.0;
    bool verbose = false;
    int first = 1;
    for (; first < argc && strncmp(argv[first], "--", 2) == 0; ++first) {
        if (strcmp(argv[first], "--device") == 0 && first + 1 < argc) device = argv[++first];
        else if (strcmp(argv[first], "--capture-to") == 0 && first + 1 < argc) capture_to = argv[++first];
        else if (strcmp(argv[first], "--lead") == 0 && first + 1 < argc) lead = atof(argv[++first]);
        else if (strcmp(argv[first], "--verbose") == 0) verbose = true;
        else { fprintf(stderr, "blackwing: unknown option %s\n", argv[first]); usage(); return 2; }
    }
    if (first >= argc) { usage(); return 2; }
    const char* command = argv[first];
    bool help = strcmp(command, "help") == 0;
    bool play = strcmp(command, "play") == 0;
    if ((!help && !play) || (play && first + 1 >= argc) || lead <= 0.0) { usage(); return 2; }
    if (capture_to) device = "capture";

    LogLevel was = log_level();
    if (!verbose && was < LogLevel::Warn) log_set_level(LogLevel::Warn);

    HifiCapture* capture = nullptr;
    if (strcmp(device, "capture") == 0) {
        HifiCaptureDesc cd;
        cd.wall_clock = true;
        cd.keep_bytes = capture_to ? (usize)1 << 30 : 0; // a gigabyte if it is wanted, none of it if not
        capture = hifi_capture_create(cd);
    }
    PlayerConfig pc;
    pc.device = device;
    pc.device_backend = capture;
    pc.lead_seconds = lead;
    player_configure(pc);

    CliConfig cli;
#if !defined(_WIN32)
    cli.stdin_ = isatty(0) != 0; // a terminal may type commands while it plays; a pipe is left alone
    cli.no_stdin = !cli.stdin_;
#else
    cli.no_stdin = true;
#endif
    cli_configure(cli);
    PlatformMounts mounts;
    mounts.none = true; // files are named by the OS's paths, not found in asset mounts
    platform_configure_mounts(mounts);

    int code = 0;
    Engine e;
    if (!engine_init(e, kv_module_list())) {
        code = 1;
    } else {
        bool failed = false;
        if (help) {
            cli_run(e, "help", print_line, nullptr);
        } else {
            for (int i = first + 1; i < argc && !failed; ++i) {
                char line[1200];
                if (strchr(argv[i], '"') || strlen(argv[i]) > 1000) { fprintf(stderr, "blackwing: cannot name this file on the command line: %s\n", argv[i]); failed = true; break; }
                format(line, sizeof line, "player add \"%s\"", argv[i]);
                failed = !cli_run(e, line, quiet_line, nullptr) || failed;
            }
            if (!failed) failed = !cli_run(e, "player play", quiet_line, nullptr);
            if (failed) code = 2;
#if !defined(_WIN32)
            signal(SIGINT, on_sigint);
#endif
            f64 last = time_now();
            while (!failed && !e.quit_requested && !atomic_load(&g_interrupted, MO_RELAXED)) {
                f64 now = time_now();
                platform_pump(e);
                engine_frame(e, now - last);
                last = now;
                const PlayerTransport* t = player_transport(e);
                bool busy = t && (t->state == (u8)PlayerState::Playing || t->state == (u8)PlayerState::Starting);
                time_sleep(busy ? e.clock.tick_dt : 0.25);
            }
            if (atomic_load(&g_interrupted, MO_RELAXED)) { fputs("\ninterrupted\n", stdout); code = 130; }
            const PlayerOutput* o = player_output(e);
            if (o && o->underruns) printf("%u underruns\n", o->underruns);
        }
        engine_shutdown(e); // the player closes its device here, so the capture is whole
    }
    if (capture) {
        if (capture_to && code != 1 && !write_capture(capture, capture_to) && code == 0) code = 1;
        hifi_capture_destroy(capture);
    }
    log_set_level(was);
    return code;
}

} // namespace

} // namespace kv

int main(int argc, char** argv) {
    int code = kv::run(argc, argv);
    kv::names_shutdown();
    kv::u64 live = kv::mem_live_allocations();
    if (live && (KV_DEV || KV_DEBUG)) {
        KV_ERROR("blackwing", "%llu allocations still live at exit", (unsigned long long)live);
        if (code == 0) code = 3;
    }
    kv::mem_shutdown();
    return code;
}
