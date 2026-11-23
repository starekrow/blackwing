// The PipeWire backend: the device named "default" (or "pipewire") on Linux, a playback
// stream on the default sink, asking for the source's own rate, channels and samples.
//
// One connection for the process. The first open makes a pw_thread_loop, a context and a
// connection to the daemon, and they stay until the process ends; each device is a stream
// on them. A context loads PipeWire's client modules, and module-rt brings in libdbus to
// talk to rtkit: each unload of that strands libdbus's static state (about 3.7 KB, which
// LeakSanitizer sees), and the player opens a device at every change of format. Kept, the
// context also keeps its data thread, so the realtime grant is had once and not per track.
// A connection that the daemon dropped is made again at the next open.
//
// Threads. The thread loop runs PipeWire's main loop (a stream's state and format arrive
// there); open, start and close take its lock. Streams are RT_PROCESS, so `process` runs
// on the context's data thread, which PipeWire puts in the realtime class where the system
// allows (module-rt, through rtkit or RLIMIT_RTPRIO). That thread is marked realtime for
// Korvid's allocators for as long as the callback runs.
//
// Shared. PipeWire's graph mixes and may resample to its own rate on the way to the
// hardware, so this device is always shared: what reaches the speaker is not proved to
// be the file's samples, however well it sounds.
//
// The delay. PipeWire reports, per stream, the frames queued in the stream and not yet
// taken (`queued`), those inside its resampler (`buffered`), and the graph's delay from
// there to the hardware (`delay`, in ticks of the graph's rate). Their sum, taken just
// after a buffer is queued, is the delay to the ear as of that moment; between callbacks
// it shrinks with the wall clock, as the device plays it out.
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/thread.h"
#include "../backend.h"
#include <errno.h>
#include <new>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace kv {

namespace {

constexpr f64 kOpenTimeoutSeconds = 5.0;
constexpr u32 kPeriodFrames       = 1024; // what the stream asks the graph for: about 21 ms at 48 kHz

u64 mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

// The process's connection. Static, so Korvid's leak count at exit does not see it.
struct PwSession {
    pw_thread_loop* loop    = nullptr;
    pw_context*     context = nullptr;
    pw_core*        core    = nullptr;
    spa_hook        core_listener{};
    pw_core_events  core_events{};
    bool            broken  = false; // the daemon went away: connect again at the next open (under the loop's lock)
};
constinit PwSession g_pw{};
constinit u32       g_pw_making = 0; // atomic: a spin lock around making the session

struct PwDevice {
    pw_stream*         stream = nullptr;
    spa_hook           listener{};
    pw_stream_events   events{};
    HifiDeviceFormat   format;
    HifiDeviceCallback callback = nullptr;
    void*              user     = nullptr;
    u32                frame_bytes = 0;

    // Main loop thread to the opener, under the loop's lock.
    bool        negotiated = false;
    bool        failed     = false;
    const char* why        = "";

    // The data thread to any thread. The delay is a snapshot (frames, and when) under a
    // sequence count: odd while it is being written.
    u32 seq         = 0;
    u32 snap_delay  = 0;
    u64 snap_ns     = 0;
    u32 quit        = 0;

    // What the data thread saw, read at close (atomics, relaxed: the data thread is done
    // with the stream when pw_stream_destroy returns, by a hand-off TSan cannot see).
    u64 callbacks   = 0;
    u64 worst_ns    = 0;
    u32 most_frames = 0;
    u32 most_delay  = 0;
    i32 policy      = -1; // the data thread's scheduling policy (as of its last look), and its priority or nice
    i32 priority    = 0;
    i32 nice        = 0;
};

spa_audio_format spa_format_of(HifiSampleFormat f) {
    switch (f) {
    case HifiSampleFormat::S16:   return SPA_AUDIO_FORMAT_S16_LE;
    case HifiSampleFormat::S24:   return SPA_AUDIO_FORMAT_S24_32_LE; // 24 bits in the low end of an i32, as ours are
    case HifiSampleFormat::S32:   return SPA_AUDIO_FORMAT_S32_LE;
    case HifiSampleFormat::F32:   return SPA_AUDIO_FORMAT_F32_LE;
    case HifiSampleFormat::Count: break;
    }
    return SPA_AUDIO_FORMAT_UNKNOWN;
}

// FLAC's channel orders (and WAVE's), which are what sources carry.
void set_positions(spa_audio_info_raw& info) {
    static const u32 k_orders[8][8] = {
        {SPA_AUDIO_CHANNEL_MONO},
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR},
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC},
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR},
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR},
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL,
         SPA_AUDIO_CHANNEL_RR},
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RC,
         SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR},
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL,
         SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR},
    };
    for (u32 i = 0; i < info.channels; ++i) info.position[i] = k_orders[info.channels - 1][i];
}

// ---- the main loop's thread ------------------------------------------------------------

void on_state(void* data, pw_stream_state, pw_stream_state state, const char* error) {
    PwDevice& d = *(PwDevice*)data;
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        if (!d.negotiated) {
            d.failed = true;
            d.why = "PipeWire refused the playback stream";
        } else if (!atomic_load(&d.quit, MO_ACQUIRE)) {
            KV_WARN("hifi_platform", "pipewire: the stream stopped: %s", error ? error : "no reason given");
        }
    }
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) d.negotiated = true;
    pw_thread_loop_signal(g_pw.loop, false);
}

void on_param(void* data, u32 id, const spa_pod* param) {
    PwDevice& d = *(PwDevice*)data;
    if (id != SPA_PARAM_Format || !param) return;
    spa_audio_info_raw got;
    memset(&got, 0, sizeof got);
    if (spa_format_audio_raw_parse(param, &got) < 0 || got.rate != d.format.rate || got.channels != d.format.channels ||
        got.format != spa_format_of(d.format.sample)) {
        // One format was offered, so anything else is a refusal.
        d.failed = true;
        d.why = "PipeWire would not take the stream's format";
    }
    pw_thread_loop_signal(g_pw.loop, false);
}

// ---- the data thread -------------------------------------------------------------------

void on_process(void* data) {
    PwDevice& d = *(PwDevice*)data;
    pw_buffer* b = pw_stream_dequeue_buffer(d.stream);
    if (!b) return;
    spa_data& sd = b->buffer->datas[0];
    if (!sd.data) { pw_stream_queue_buffer(d.stream, b); return; }
    u32 frames = sd.maxsize / d.frame_bytes;
    if (b->requested && b->requested < frames) frames = (u32)b->requested;

    // The policy as it stands: at the first callback, and again later, since PipeWire may
    // be granted the realtime class by rtkit after the stream has started.
    u64 n = atomic_load(&d.callbacks, MO_RELAXED);
    if (n == 0 || n == 64 || n == 1024) {
        sched_param sp;
        atomic_store(&d.policy, (i32)(sched_getscheduler(0) & ~SCHED_RESET_ON_FORK), MO_RELAXED);
        atomic_store(&d.priority, sched_getparam(0, &sp) == 0 ? (i32)sp.sched_priority : 0, MO_RELAXED);
        atomic_store(&d.nice, (i32)getpriority(PRIO_PROCESS, (id_t)gettid()), MO_RELAXED);
    }
    u64 t0 = mono_ns();
    if (frames && !atomic_load(&d.quit, MO_ACQUIRE)) {
        thread_set_realtime(true);
        d.callback(d.user, sd.data, frames);
        thread_set_realtime(false);
    } else {
        memset(sd.data, 0, (usize)frames * d.frame_bytes);
    }
    u64 spent = mono_ns() - t0;
    if (spent > atomic_load(&d.worst_ns, MO_RELAXED)) atomic_store(&d.worst_ns, spent, MO_RELAXED);
    if (frames > atomic_load(&d.most_frames, MO_RELAXED)) atomic_store(&d.most_frames, frames, MO_RELAXED);
    atomic_store(&d.callbacks, n + 1, MO_RELAXED);

    sd.chunk->offset = 0;
    sd.chunk->stride = (i32)d.frame_bytes;
    sd.chunk->size   = frames * d.frame_bytes;
    pw_stream_queue_buffer(d.stream, b);

    // The delay to the ear, now that this buffer is queued.
    pw_time t;
    memset(&t, 0, sizeof t);
    u64 delay = 0;
    if (pw_stream_get_time_n(d.stream, &t, sizeof t) == 0) {
        delay = t.queued / d.frame_bytes + t.buffered;
        if (t.rate.denom && t.delay > 0)
            delay += (u64)t.delay * t.rate.num * d.format.rate / t.rate.denom;
    } else {
        delay = frames;
    }
    u32 delay32 = (u32)(delay > 0xffffffffull ? 0xffffffffull : delay);
    if (delay32 > atomic_load(&d.most_delay, MO_RELAXED)) atomic_store(&d.most_delay, delay32, MO_RELAXED);
    u32 s = atomic_load(&d.seq, MO_RELAXED);
    atomic_store(&d.seq, s + 1, MO_RELAXED);
    atomic_fence(MO_RELEASE);
    atomic_store(&d.snap_delay, delay32, MO_RELAXED);
    atomic_store(&d.snap_ns, mono_ns(), MO_RELAXED);
    atomic_store(&d.seq, s + 2, MO_RELEASE);
}


// ---- the backend -----------------------------------------------------------------------

bool pw_has(const char* name) { return strcmp(name, "default") == 0 || strcmp(name, "pipewire") == 0; }

void on_core_error(void*, u32 id, int, int res, const char* message) {
    if (id == PW_ID_CORE && res == -EPIPE) {
        g_pw.broken = true;
        KV_WARN("hifi_platform", "pipewire: the connection to the daemon was lost: %s", message ? message : "no reason given");
    }
    pw_thread_loop_signal(g_pw.loop, false);
}

// The session, made or mended. Returns with the loop locked when it is usable, and false,
// with the reason and the loop unlocked, when not.
bool session_lock(const char** why) {
    u32 zero = 0;
    while (!atomic_compare_exchange(&g_pw_making, &zero, 1u, false, MO_ACQUIRE, MO_RELAXED)) { zero = 0; sched_yield(); }
    bool ok = true;
    if (!g_pw.loop) {
        pw_init(nullptr, nullptr);
        pw_thread_loop* loop = pw_thread_loop_new("blackwing-pipewire", nullptr);
        if (!loop || pw_thread_loop_start(loop) != 0) {
            if (loop) pw_thread_loop_destroy(loop);
            *why = "PipeWire's loop could not be started";
            ok = false;
        } else {
            g_pw.loop = loop;
        }
    }
    if (ok) {
        pw_thread_loop_lock(g_pw.loop);
        if (!g_pw.context) g_pw.context = pw_context_new(pw_thread_loop_get_loop(g_pw.loop), nullptr, 0);
        if (!g_pw.context) { *why = "PipeWire's context could not be made"; ok = false; }
        if (ok && g_pw.core && g_pw.broken) {
            spa_hook_remove(&g_pw.core_listener);
            pw_core_disconnect(g_pw.core);
            g_pw.core = nullptr;
        }
        if (ok && !g_pw.core) {
            g_pw.broken = false;
            g_pw.core = pw_context_connect(g_pw.context, nullptr, 0);
            if (!g_pw.core) {
                *why = "PipeWire is not running, or would not take a connection";
                ok = false;
            } else {
                memset(&g_pw.core_listener, 0, sizeof g_pw.core_listener);
                memset(&g_pw.core_events, 0, sizeof g_pw.core_events);
                g_pw.core_events.version = PW_VERSION_CORE_EVENTS;
                g_pw.core_events.error   = on_core_error;
                pw_core_add_listener(g_pw.core, &g_pw.core_listener, &g_pw.core_events, nullptr);
            }
        }
        if (!ok) pw_thread_loop_unlock(g_pw.loop);
    }
    atomic_store(&g_pw_making, 0u, MO_RELEASE);
    return ok;
}

// Under the loop's lock: ends the stream; after this no `process` of it runs.
void end_stream(PwDevice* d) {
    atomic_store(&d->quit, 1u, MO_RELEASE);
    if (d->stream) pw_stream_destroy(d->stream); // removes it from the data thread, waiting for that
    d->stream = nullptr;
}

void free_device(PwDevice* d) {
    d->~PwDevice();
    mem_free(d);
}

void* pw_open(const HifiDeviceDesc& desc, HifiDeviceOpened& out) {
    if (desc.want.channels > 8) { out.reason = "PipeWire is not asked for more than eight channels"; return nullptr; }
    const char* why = "";
    if (!session_lock(&why)) { out.reason = why; return nullptr; }
    // The loop is locked from here.
    PwDevice* d = new (mem_alloc(sizeof(PwDevice))) PwDevice();
    d->format      = desc.want;
    d->callback    = desc.callback;
    d->user        = desc.user;
    d->frame_bytes = hifi_frame_bytes(desc.want);

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                             PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, "Blackwing",
                                             PW_KEY_NODE_NAME, "blackwing", nullptr);
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", desc.want.rate);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", kPeriodFrames, desc.want.rate);
    d->stream = pw_stream_new(g_pw.core, "Blackwing", props);
    if (!d->stream) {
        pw_thread_loop_unlock(g_pw.loop);
        free_device(d);
        out.reason = "PipeWire would not make a playback stream";
        return nullptr;
    }
    d->events.version       = PW_VERSION_STREAM_EVENTS;
    d->events.state_changed = on_state;
    d->events.param_changed = on_param;
    d->events.process       = on_process;
    pw_stream_add_listener(d->stream, &d->listener, &d->events, d);

    u8 pod[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    spa_audio_info_raw info;
    memset(&info, 0, sizeof info);
    info.format   = spa_format_of(desc.want.sample);
    info.rate     = desc.want.rate;
    info.channels = desc.want.channels;
    set_positions(info);
    const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    auto flags = (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
                                   PW_STREAM_FLAG_INACTIVE);
    bool ok = pw_stream_connect(d->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) >= 0;
    why = "PipeWire would not connect the playback stream";

    // Wait for the format, or the refusal, or the time.
    if (ok) {
        timespec until;
        pw_thread_loop_get_time(g_pw.loop, &until, (i64)(kOpenTimeoutSeconds * 1e9));
        while (!d->negotiated && !d->failed && !g_pw.broken)
            if (pw_thread_loop_timed_wait_full(g_pw.loop, &until) != 0) break;
        ok = d->negotiated && !d->failed && !g_pw.broken;
        why = d->failed ? d->why : g_pw.broken ? "the connection to PipeWire was lost" : "PipeWire did not answer within five seconds";
    }
    if (!ok) end_stream(d);
    pw_thread_loop_unlock(g_pw.loop);
    if (!ok) { free_device(d); out.reason = why; return nullptr; }

    out.format = desc.want;
    out.shared = true;
    return d;
}

void pw_start(void* impl) {
    PwDevice& d = *(PwDevice*)impl;
    pw_thread_loop_lock(g_pw.loop);
    pw_stream_set_active(d.stream, true);
    pw_thread_loop_unlock(g_pw.loop);
}

u32 pw_delay(void* impl) {
    PwDevice& d = *(PwDevice*)impl;
    u32 delay, s;
    u64 at;
    do {
        s = atomic_load(&d.seq, MO_ACQUIRE);
        delay = atomic_load(&d.snap_delay, MO_RELAXED);
        at    = atomic_load(&d.snap_ns, MO_RELAXED);
        atomic_fence(MO_ACQUIRE);
    } while ((s & 1u) || s != atomic_load(&d.seq, MO_RELAXED));
    if (s == 0) return 0; // nothing has been taken yet
    u64 played = (mono_ns() - at) * d.format.rate / 1000000000ull;
    return played >= delay ? 0 : delay - (u32)played;
}

const char* policy_name(i32 p) {
    switch (p) {
    case SCHED_FIFO:  return "SCHED_FIFO";
    case SCHED_RR:    return "SCHED_RR";
    case SCHED_OTHER: return "SCHED_OTHER";
    default:          return "another policy";
    }
}

void pw_close(void* impl) {
    PwDevice* d = (PwDevice*)impl;
    pw_thread_loop_lock(g_pw.loop);
    end_stream(d); // the data thread is done with it, so what it counted can be read
    pw_thread_loop_unlock(g_pw.loop);
    u64 callbacks = atomic_load(&d->callbacks, MO_RELAXED), worst = atomic_load(&d->worst_ns, MO_RELAXED);
    u32 most = atomic_load(&d->most_frames, MO_RELAXED), most_delay = atomic_load(&d->most_delay, MO_RELAXED);
    i32 policy = atomic_load(&d->policy, MO_RELAXED), priority = atomic_load(&d->priority, MO_RELAXED);
    i32 nice = atomic_load(&d->nice, MO_RELAXED);
    u32 rate = d->format.rate;
    free_device(d);
    if (!callbacks) return;
    f64 ms = rate ? 1000.0 / rate : 0.0;
    if (policy == SCHED_FIFO || policy == SCHED_RR)
        KV_INFO("hifi_platform", "pipewire: %llu callbacks of up to %u frames (%.1f ms), the slowest %.1f us; delay up to %.1f ms; the thread ran %s at priority %d",
                (unsigned long long)callbacks, most, most * ms, worst / 1000.0, most_delay * ms, policy_name(policy), priority);
    else
        KV_INFO("hifi_platform", "pipewire: %llu callbacks of up to %u frames (%.1f ms), the slowest %.1f us; delay up to %.1f ms; the thread ran %s at nice %d, not in the realtime class",
                (unsigned long long)callbacks, most, most * ms, worst / 1000.0, most_delay * ms, policy_name(policy), nice);
}

} // namespace

constinit const HifiBackend k_pipewire_backend = {
    .name = "pipewire", .has = pw_has, .open = pw_open, .start = pw_start, .delay = pw_delay, .close = pw_close,
};

} // namespace kv
