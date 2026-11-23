// The device interface's own checks, and the system's device where there is one. The
// capture device is tested through hifi (modules/hifi/tests), which is what it is for.
//
// The PipeWire checks play silence, so they make no sound, and they are skipped, with a
// line saying so, on a machine where PipeWire is not running.
#include "korvid/hifi_platform/capture.h"
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/test.h"
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#include <string.h>

using namespace kv;

namespace {

struct Counter {
    u64 frames   = 0; // atomic
    u32 calls    = 0; // atomic
    u32 not_rt   = 0; // atomic: callbacks that ran on a thread not marked realtime
    u32 bytes    = 0; // a frame's
};

void silence(void* user, void* out, u32 frames) {
    Counter& c = *(Counter*)user;
    memset(out, 0, (usize)frames * c.bytes);
    if (!thread_is_realtime()) atomic_add_fetch(&c.not_rt, 1u, MO_RELAXED);
    atomic_add_fetch(&c.frames, (u64)frames, MO_RELAXED);
    atomic_add_fetch(&c.calls, 1u, MO_RELAXED);
}

} // namespace

void test_device_names(TestCtx& c) {
    Counter n;
    HifiDeviceDesc d;
    d.want = {48000, 2, HifiSampleFormat::S16};
    d.callback = silence;
    d.user = &n;
    d.name = "no such device";
    HifiDeviceOpened o = hifi_device_open(d);
    KV_CHECK(c, !o.device && strcmp(o.reason, "there is no such output device") == 0);
    d.name = "capture"; // with nowhere to capture to
    o = hifi_device_open(d);
    KV_CHECK(c, !o.device && o.reason[0]);
    d.name = "default";
    d.callback = nullptr;
    o = hifi_device_open(d);
    KV_CHECK(c, !o.device && o.reason[0]);
}

void test_pipewire(TestCtx& c) {
#if defined(__linux__) && !defined(__ANDROID__)
    // Each sample format, granted as asked and shared.
    const HifiSampleFormat formats[] = {HifiSampleFormat::S16, HifiSampleFormat::S24, HifiSampleFormat::S32, HifiSampleFormat::F32};
    for (HifiSampleFormat f : formats) {
        Counter n;
        HifiDeviceDesc d;
        d.name = "default";
        d.want = {44100, 2, f};
        d.callback = silence;
        d.user = &n;
        n.bytes = hifi_frame_bytes(d.want);
        HifiDeviceOpened o = hifi_device_open(d);
        if (!o.device) {
            KV_INFO("test", "pipewire: skipped, the default device would not open: %s", o.reason);
            return;
        }
        KV_CHECK(c, o.format.rate == 44100 && o.format.channels == 2 && o.format.sample == f);
        KV_CHECK(c, o.shared);
        hifi_device_close(o.device);
        KV_CHECK(c, atomic_load(&n.calls, MO_RELAXED) == 0); // never started
    }

    // Half a second of silence at the wall's pace: the frames taken follow the clock, the
    // callback is marked realtime, the delay is something, and close is the end.
    Counter n;
    HifiDeviceDesc d;
    d.name = "pipewire";
    d.want = {48000, 1, HifiSampleFormat::S24};
    d.callback = silence;
    d.user = &n;
    n.bytes = hifi_frame_bytes(d.want);
    HifiDeviceOpened o = hifi_device_open(d);
    KV_CHECK(c, o.device != nullptr);
    if (!o.device) return;
    KV_CHECK(c, hifi_device_delay(o.device) == 0); // nothing taken yet
    hifi_device_start(o.device);
    f64 t0 = time_now();
    while (atomic_load(&n.calls, MO_RELAXED) == 0 && time_now() - t0 < 2.0) time_sleep(0.001);
    KV_CHECK(c, atomic_load(&n.calls, MO_RELAXED) > 0);
    u64 first = atomic_load(&n.frames, MO_RELAXED);
    f64 t1 = time_now();
    time_sleep(0.3);
    u32 delay = hifi_device_delay(o.device);
    time_sleep(0.2);
    u64 taken = atomic_load(&n.frames, MO_RELAXED) - first;
    f64 expect = (time_now() - t1) * 48000.0;
    KV_CHECK(c, (f64)taken > expect - 4096.0 && (f64)taken < expect + 4096.0); // within a few periods of the wall
    KV_CHECK(c, delay > 0 && delay < 48000);                                     // something, and under a second
    KV_CHECK(c, atomic_load(&n.not_rt, MO_RELAXED) == 0);
    hifi_device_close(o.device);
    u32 calls = atomic_load(&n.calls, MO_RELAXED);
    time_sleep(0.05);
    KV_CHECK(c, atomic_load(&n.calls, MO_RELAXED) == calls);
    KV_INFO("test", "pipewire: %llu frames in %.2f s against %.0f expected; delay %u frames", (unsigned long long)taken,
            time_now() - t1, expect, delay);
#else
    (void)c;
#endif
}
