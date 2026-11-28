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

    // Held, it stops calling back; let go, it starts again within a few periods.
    hifi_device_hold(o.device, true);
    time_sleep(0.1);                                  // what was in flight
    u32 before = atomic_load(&n.calls, MO_RELAXED);
    time_sleep(0.3);
    KV_CHECK(c, atomic_load(&n.calls, MO_RELAXED) == before);
    f64 let_go = time_now();
    hifi_device_hold(o.device, false);
    while (atomic_load(&n.calls, MO_RELAXED) == before && time_now() - let_go < 1.0) time_sleep(0.0002);
    f64 resumed = time_now() - let_go;
    KV_CHECK(c, atomic_load(&n.calls, MO_RELAXED) > before && resumed < 0.1);
    KV_INFO("test", "pipewire: held for 0.3 s with no callback; the first after letting go came in %.1f ms", resumed * 1000.0);
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

// The capture device pulled and plugged: lost, not openable, its row not present; plugged,
// present again, and a new device opens.
void test_capture_unplug(TestCtx& c) {
    HifiCaptureDesc cd;
    cd.keep_bytes = 1u << 20;
    HifiCapture* cap = hifi_capture_create(cd);
    Counter n;
    HifiDeviceDesc d;
    d.name = "capture";
    d.backend = cap;
    d.want = {48000, 2, HifiSampleFormat::S16};
    d.callback = silence;
    d.user = &n;
    n.bytes = hifi_frame_bytes(d.want);
    auto row = [&](bool* present) {
        HifiDeviceInfo list[kHifiMaxDevices];
        u32 k = hifi_devices(list, kHifiMaxDevices);
        for (u32 i = 0; i < k; ++i) if (strcmp(list[i].name, "capture") == 0) { *present = list[i].present; return true; }
        return false;
    };
    bool present = false;
    KV_CHECK(c, row(&present) && present);
    HifiDeviceOpened o = hifi_device_open(d);
    KV_CHECK(c, o.device && strcmp(o.name, "capture") == 0 && !hifi_device_lost(o.device));
    hifi_device_start(o.device);
    KV_CHECK(c, hifi_capture_pull(cap, 256) == 256);
    u64 gen = hifi_devices_generation();
    hifi_capture_unplug(cap);
    KV_CHECK(c, hifi_devices_generation() > gen);
    KV_CHECK(c, hifi_device_lost(o.device) && strstr(hifi_device_lost(o.device), "unplugged"));
    KV_CHECK(c, hifi_capture_pull(cap, 256) == 0);                // it calls back no more
    KV_CHECK(c, row(&present) && !present);
    hifi_device_close(o.device);
    HifiDeviceOpened again = hifi_device_open(d);
    KV_CHECK(c, !again.device && strstr(again.reason, "unplugged"));
    hifi_capture_plug(cap);
    KV_CHECK(c, row(&present) && present);
    again = hifi_device_open(d);
    KV_CHECK(c, again.device && !hifi_device_lost(again.device));
    if (again.device) hifi_device_close(again.device);
    hifi_capture_destroy(cap);
}

#if defined(__linux__) && !defined(__ANDROID__)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <pipewire/pipewire.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <stdio.h>
#include <unistd.h>

namespace {

// A sink of the test's own on the user's PipeWire: a null sink, which plays nowhere, made
// and destroyed by the test on a connection of its own, as a DAC is plugged and pulled.
struct NullSink {
    pw_thread_loop* loop = nullptr;
    pw_context*     context = nullptr;
    pw_core*        core = nullptr;
    pw_proxy*       node = nullptr;
    char            name[96] = "";

    bool make() {
        snprintf(name, sizeof name, "blackwing-test-sink-%d", (int)getpid());
        loop = pw_thread_loop_new("blackwing-test", nullptr);
        if (!loop || pw_thread_loop_start(loop) != 0) return false;
        pw_thread_loop_lock(loop);
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        core = context ? pw_context_connect(context, nullptr, 0) : nullptr;
        if (core) {
            pw_properties* p = pw_properties_new("factory.name", "support.null-audio-sink", PW_KEY_NODE_NAME, name,
                                                 PW_KEY_NODE_DESCRIPTION, "Blackwing's test sink", PW_KEY_MEDIA_CLASS, "Audio/Sink",
                                                 "audio.position", "FL,FR", "object.linger", "false", "priority.session", "1", nullptr);
            node = (pw_proxy*)pw_core_create_object(core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &p->dict, 0);
            pw_properties_free(p);
        }
        pw_thread_loop_unlock(loop);
        return node != nullptr;
    }
    void unmake() {
        if (!loop) return;
        pw_thread_loop_lock(loop);
        if (node) pw_proxy_destroy(node);
        node = nullptr;
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
        core = nullptr; context = nullptr;
        pw_thread_loop_unlock(loop);
        pw_thread_loop_stop(loop);
        pw_thread_loop_destroy(loop);
        loop = nullptr;
    }
};

// Whether the list has `name`, and if so whether present and the default.
bool listed(const char* name, bool* present, bool* is_default = nullptr) {
    HifiDeviceInfo list[kHifiMaxDevices];
    u32 k = hifi_devices(list, kHifiMaxDevices);
    for (u32 i = 0; i < k && i < kHifiMaxDevices; ++i) {
        if (strcmp(list[i].name, name) != 0) continue;
        *present = list[i].present;
        if (is_default) *is_default = list[i].is_default;
        return true;
    }
    return false;
}

} // namespace
#endif

// The sinks PipeWire has, one made and destroyed under a stream: listed, opened by name,
// lost when it goes (and not moved to another sink), listed as gone, not openable.
void test_pipewire_devices(TestCtx& c) {
#if defined(__linux__) && !defined(__ANDROID__)
    HifiDeviceInfo list[kHifiMaxDevices];
    u32 k = hifi_devices(list, kHifiMaxDevices);
    u32 sinks = 0, defaults = 0;
    for (u32 i = 0; i < k && i < kHifiMaxDevices; ++i) {
        if (strcmp(list[i].backend, "pipewire") != 0) continue;
        ++sinks;
        if (list[i].is_default) ++defaults;
        KV_INFO("test", "pipewire: %s%s, \"%s\", %u channels%s", list[i].name, list[i].is_default ? " (the default)" : "",
                list[i].label, (u32)list[i].channels, list[i].can_exclusive ? ", can be opened alone" : "");
    }
    if (!sinks) { KV_INFO("test", "pipewire devices: skipped, PipeWire lists no sink"); return; }
    KV_CHECK(c, defaults <= 1);

    NullSink sink;
    if (!sink.make()) { KV_INFO("test", "pipewire devices: skipped, a null sink could not be made"); sink.unmake(); return; }
    bool present = false;
    u64 gen = hifi_devices_generation();
    f64 t0 = time_now();
    while (!(listed(sink.name, &present) && present) && time_now() - t0 < 3.0) time_sleep(0.01);
    KV_CHECK(c, listed(sink.name, &present) && present);
    KV_CHECK(c, hifi_devices_generation() > gen);
    KV_INFO("test", "pipewire: a sink made was listed in %.0f ms", (time_now() - t0) * 1000.0);

    Counter n;
    HifiDeviceDesc d;
    d.name = sink.name;
    d.want = {48000, 2, HifiSampleFormat::S16};
    d.callback = silence;
    d.user = &n;
    n.bytes = hifi_frame_bytes(d.want);
    HifiDeviceOpened o = hifi_device_open(d);
    if (!o.device) KV_INFO("test", "pipewire: the test sink would not open: %s", o.reason);
    KV_CHECK(c, o.device && strcmp(o.name, sink.name) == 0);
    if (o.device) {
        hifi_device_start(o.device);
        time_sleep(0.2);
        KV_CHECK(c, !hifi_device_lost(o.device));
        // Pulled.
        sink.unmake();
        f64 t1 = time_now();
        while (!hifi_device_lost(o.device) && time_now() - t1 < 3.0) time_sleep(0.001);
        f64 took = time_now() - t1;
        KV_CHECK(c, hifi_device_lost(o.device) != nullptr && took < 1.0);
        KV_INFO("test", "pipewire: a sink destroyed under a stream was lost in %.1f ms: %s", took * 1000.0,
                hifi_device_lost(o.device) ? hifi_device_lost(o.device) : "(not lost)");
        u32 calls = atomic_load(&n.calls, MO_RELAXED);
        time_sleep(0.2);
        KV_CHECK(c, atomic_load(&n.calls, MO_RELAXED) == calls); // not moved to another sink: no more callbacks
        u32 d1 = hifi_device_delay(o.device);
        time_sleep(0.05);
        KV_CHECK(c, hifi_device_delay(o.device) == d1);           // its delay stopped where it was
        hifi_device_close(o.device);
    } else {
        sink.unmake();
    }
    t0 = time_now();
    while (listed(sink.name, &present) && present && time_now() - t0 < 2.0) time_sleep(0.01);
    KV_CHECK(c, listed(sink.name, &present) && !present);
    HifiDeviceOpened again = hifi_device_open(d);
    KV_CHECK(c, !again.device && again.reason[0]);
    if (again.device) hifi_device_close(again.device);
#else
    (void)c;
#endif
}
