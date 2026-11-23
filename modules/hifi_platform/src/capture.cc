// The capture device. One HifiCapture, at most one device open on it at a time; a mutex
// keeps a pull, the wall-clock thread and an open or close apart. The mutex is the
// device's own business, as a driver's would be: the callback itself is run with the
// thread marked realtime, so a dev build aborts if what it calls allocates.
#include "korvid/hifi_platform/capture.h"
#include "korvid/core/atomic.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#include "backend.h"
#include <new>
#include <string.h>

namespace kv {

struct HifiCapture {
    HifiCaptureDesc desc;
    OsMutex         lock;
    u8*             data    = nullptr;
    usize           len     = 0;
    u64             dropped = 0;
    HifiCaptureSegment segments[64];
    u32             nsegments = 0;
    u32             opens     = 0;
    u32             holds     = 0;
    bool            held      = false;
    u8*             spill     = nullptr; // where frames go when data is full, and thrown away
    usize           spill_bytes = 0;

    // The device open on it, if any.
    bool               open    = false;
    bool               started = false;
    HifiDeviceFormat   format;
    HifiDeviceCallback callback = nullptr;
    void*              user     = nullptr;
    OsThread           thread;
    u32                quit = 0; // atomic
};

namespace {

// Under the lock: the callback for `frames` frames, into the store or the spill.
void capture_run(HifiCapture& c, u32 frames) {
    usize fb = hifi_frame_bytes(c.format);
    while (frames) {
        usize room = (c.desc.keep_bytes - c.len) / fb;
        u32 n = frames;
        u8* dst;
        if (room > 0 && c.nsegments > 0) {
            if (n > room) n = (u32)room;
            dst = c.data + c.len;
        } else {
            if (n > c.spill_bytes / fb) n = (u32)(c.spill_bytes / fb);
            dst = c.spill;
            c.dropped += n;
        }
        c.callback(c.user, dst, n);
        if (dst != c.spill) { c.len += n * fb; c.segments[c.nsegments - 1].bytes += n * fb; }
        frames -= n;
    }
}

void capture_thread(void* arg) {
    HifiCapture& c = *(HifiCapture*)arg;
    thread_set_realtime(true);
    f64 period = (f64)c.desc.period_frames / (f64)c.format.rate;
    f64 next = time_now();
    while (!atomic_load(&c.quit, MO_ACQUIRE)) {
        os_mutex_lock(&c.lock);
        capture_run(c, c.desc.period_frames);
        os_mutex_unlock(&c.lock);
        next += period;
        f64 wait = next - time_now();
        if (wait > 0.0) time_sleep(wait);
        else if (wait < -0.25) next = time_now(); // fell far behind (a debugger, a suspended machine): do not race to catch up
    }
    thread_set_realtime(false);
}

bool capture_has(const char* name) { return strcmp(name, "capture") == 0; }

void* capture_open(const HifiDeviceDesc& desc, HifiDeviceOpened& out) {
    HifiCapture* c = (HifiCapture*)desc.backend;
    if (!c) { out.reason = "the capture device was given nowhere to capture to"; return nullptr; }
    os_mutex_lock(&c->lock);
    if (c->open) { os_mutex_unlock(&c->lock); out.reason = "the capture device is already open"; return nullptr; }
    c->open     = true;
    c->started  = false;
    c->format   = desc.want;
    c->callback = desc.callback;
    c->user     = desc.user;
    c->quit     = 0;
    ++c->opens;
    if (c->nsegments < 64) {
        HifiCaptureSegment& s = c->segments[c->nsegments++];
        s.format = desc.want;
        s.offset = c->len;
        s.bytes  = 0;
    }
    os_mutex_unlock(&c->lock);
    out.format = desc.want; // anything asked, as asked
    out.shared = false;
    return c;
}

void capture_start(void* impl) {
    HifiCapture& c = *(HifiCapture*)impl;
    os_mutex_lock(&c.lock);
    c.started = true;
    os_mutex_unlock(&c.lock);
    if (c.desc.wall_clock) os_thread_start(&c.thread, capture_thread, &c);
}

u32 capture_delay(void* impl) { return ((HifiCapture*)impl)->desc.delay_frames; }

void capture_hold(void* impl, bool hold) {
    HifiCapture& c = *(HifiCapture*)impl;
    os_mutex_lock(&c.lock);
    if (hold && !c.held) ++c.holds;
    c.held = hold;
    os_mutex_unlock(&c.lock);
}

void capture_close(void* impl) {
    HifiCapture& c = *(HifiCapture*)impl;
    if (c.thread.handle) {
        atomic_store(&c.quit, 1u, MO_RELEASE);
        os_thread_join(&c.thread);
    }
    os_mutex_lock(&c.lock); // a pull in progress finishes first
    c.open = c.started = false;
    c.held = false;
    c.callback = nullptr;
    os_mutex_unlock(&c.lock);
}

} // namespace

constinit const HifiBackend k_capture_backend = {
    .name = "capture", .has = capture_has, .open = capture_open, .start = capture_start, .delay = capture_delay,
    .hold = capture_hold, .close = capture_close,
};

HifiCapture* hifi_capture_create(const HifiCaptureDesc& desc) {
    HifiCapture* c = new (mem_alloc(sizeof(HifiCapture))) HifiCapture();
    c->desc = desc;
    if (c->desc.period_frames == 0) c->desc.period_frames = 512;
    os_mutex_init(&c->lock);
    c->data        = (u8*)mem_alloc(desc.keep_bytes ? desc.keep_bytes : 1);
    c->spill_bytes = 4096 * 8 * 4;
    c->spill       = (u8*)mem_alloc(c->spill_bytes);
    return c;
}

void hifi_capture_destroy(HifiCapture* c) {
    if (!c) return;
    os_mutex_destroy(&c->lock);
    mem_free(c->data);
    mem_free(c->spill);
    mem_free(c);
}

u32 hifi_capture_pull(HifiCapture* c, u32 frames) {
    if (c->desc.wall_clock) return 0;
    os_mutex_lock(&c->lock);
    u32 pulled = 0;
    if (c->open && c->started) {
        thread_set_realtime(true);
        capture_run(*c, frames);
        thread_set_realtime(false);
        pulled = frames;
    }
    os_mutex_unlock(&c->lock);
    return pulled;
}

u32 hifi_capture_segments(HifiCapture* c, HifiCaptureSegment* out, u32 cap) {
    os_mutex_lock(&c->lock);
    u32 n = c->nsegments;
    for (u32 i = 0; i < n && i < cap; ++i) out[i] = c->segments[i];
    os_mutex_unlock(&c->lock);
    return n;
}

const u8* hifi_capture_data(HifiCapture* c) { return c->data; }

u64 hifi_capture_dropped(HifiCapture* c) {
    os_mutex_lock(&c->lock);
    u64 d = c->dropped;
    os_mutex_unlock(&c->lock);
    return d;
}

bool hifi_capture_held(HifiCapture* c) {
    os_mutex_lock(&c->lock);
    bool h = c->held;
    os_mutex_unlock(&c->lock);
    return h;
}

u32 hifi_capture_holds(HifiCapture* c) {
    os_mutex_lock(&c->lock);
    u32 n = c->holds;
    os_mutex_unlock(&c->lock);
    return n;
}

u32 hifi_capture_opens(HifiCapture* c) {
    os_mutex_lock(&c->lock);
    u32 n = c->opens;
    os_mutex_unlock(&c->lock);
    return n;
}

} // namespace kv
