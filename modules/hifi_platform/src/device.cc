// The device interface over the backends linked in: the capture device everywhere, and
// each platform's own: PipeWire on Linux.
#include "korvid/hifi_platform/device.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "backend.h"

namespace kv {

struct HifiDevice {
    const HifiBackend* backend = nullptr;
    void*              impl    = nullptr;
};

namespace {

constinit const HifiBackend* const k_backends[] = {
    &k_capture_backend,
#if defined(__linux__) && !defined(__ANDROID__)
    &k_pipewire_backend,
#endif
};

constinit u64   g_generation = 1;     // atomic
constinit void (*g_watch)(void*) = nullptr;
constinit void* g_watch_user = nullptr;
constinit u32   g_watch_lock = 0;     // atomic: a spin lock around the watcher's two words

} // namespace

void hifi_devices_changed() {
    atomic_add_fetch(&g_generation, (u64)1, MO_ACQ_REL);
    u32 zero = 0;
    while (!atomic_compare_exchange(&g_watch_lock, &zero, 1u, false, MO_ACQUIRE, MO_RELAXED)) zero = 0;
    void (*fn)(void*) = g_watch;
    void* user = g_watch_user;
    if (fn) fn(user); // under the spin lock, so a watcher being taken away is not called after
    atomic_store(&g_watch_lock, 0u, MO_RELEASE);
}

u64 hifi_devices_generation() { return atomic_load(&g_generation, MO_ACQUIRE); }

void hifi_devices_watch(void (*fn)(void* user), void* user) {
    u32 zero = 0;
    while (!atomic_compare_exchange(&g_watch_lock, &zero, 1u, false, MO_ACQUIRE, MO_RELAXED)) zero = 0;
    g_watch = fn;
    g_watch_user = user;
    atomic_store(&g_watch_lock, 0u, MO_RELEASE);
}

u32 hifi_devices(HifiDeviceInfo* out, u32 cap) {
    u32 n = 0;
    for (const HifiBackend* b : k_backends) {
        if (!b->list) continue;
        HifiDeviceInfo some[kHifiMaxDevices];
        u32 got = b->list(some, kHifiMaxDevices);
        for (u32 i = 0; i < got && i < kHifiMaxDevices; ++i, ++n) if (n < cap) out[n] = some[i];
    }
    return n;
}

const char* hifi_sample_format_name(HifiSampleFormat f) {
    switch (f) {
    case HifiSampleFormat::S16:   return "s16";
    case HifiSampleFormat::S24:   return "s24";
    case HifiSampleFormat::S32:   return "s32";
    case HifiSampleFormat::F32:   return "f32";
    case HifiSampleFormat::Count: break;
    }
    return "?";
}

HifiDeviceOpened hifi_device_open(const HifiDeviceDesc& desc) {
    HifiDeviceOpened out;
    if (!desc.callback) { out.reason = "a device needs a callback"; return out; }
    if (desc.want.rate == 0 || desc.want.channels == 0) { out.reason = "a device needs a rate and a channel count"; return out; }
    for (const HifiBackend* b : k_backends) {
        const char* name = desc.name ? desc.name : "default";
        if (!b->has(name)) continue;
#if defined(__linux__) && !defined(__ANDROID__)
        // Alone: the kernel's device behind it, if it has one; else, or refused, shared.
        if (desc.alone) {
            i32 card = -1, dev = -1;
            char why[96] = "";
            if (b->alone_target && b->alone_target(name, &card, &dev)) {
                void* impl = alsa_open_alone(card, dev, desc, out, why, sizeof why);
                if (impl) {
                    if (!out.name[0]) format(out.name, sizeof out.name, "%s", name);
                    HifiDevice* d = (HifiDevice*)mem_alloc(sizeof(HifiDevice));
                    d->backend = &k_alsa_backend;
                    d->impl    = impl;
                    out.device = d;
                    out.alone  = true;
                    return out;
                }
                format(out.alone_why, sizeof out.alone_why, "%s", why);
            }
        }
#endif
        void* impl = b->open(desc, out);
        if (desc.alone && !out.alone && !out.alone_why[0]) format(out.alone_why, sizeof out.alone_why, "this device cannot be opened alone");
        if (!impl) { if (!out.reason[0]) out.reason = "the device could not be opened"; return out; }
        if (!out.name[0]) format(out.name, sizeof out.name, "%s", desc.name ? desc.name : "default");
        HifiDevice* d = (HifiDevice*)mem_alloc(sizeof(HifiDevice));
        d->backend = b;
        d->impl    = impl;
        out.device = d;
        return out;
    }
    out.reason = "there is no such output device";
    return out;
}

void hifi_device_start(HifiDevice* d) { if (d) d->backend->start(d->impl); }
u32  hifi_device_delay(HifiDevice* d) { return d ? d->backend->delay(d->impl) : 0; }
void hifi_device_hold(HifiDevice* d, bool hold) { if (d && d->backend->hold) d->backend->hold(d->impl, hold); }
const char* hifi_device_lost(HifiDevice* d) { return d && d->backend->lost ? d->backend->lost(d->impl) : nullptr; }
void hifi_device_close(HifiDevice* d) {
    if (!d) return;
    d->backend->close(d->impl);
    mem_free(d);
}

} // namespace kv
