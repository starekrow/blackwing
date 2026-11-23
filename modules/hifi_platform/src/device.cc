// The device interface over the backends linked in: the capture device everywhere, and
// each platform's own (none yet; PipeWire is slice 1's milestone 4).
#include "korvid/hifi_platform/device.h"
#include "korvid/core/memory.h"
#include "backend.h"

namespace kv {

struct HifiDevice {
    const HifiBackend* backend = nullptr;
    void*              impl    = nullptr;
};

namespace {

constinit const HifiBackend* const k_backends[] = {&k_capture_backend};

} // namespace

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
        if (!b->has(desc.name ? desc.name : "default")) continue;
        void* impl = b->open(desc, out);
        if (!impl) { if (!out.reason[0]) out.reason = "the device could not be opened"; return out; }
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
void hifi_device_close(HifiDevice* d) {
    if (!d) return;
    d->backend->close(d->impl);
    mem_free(d);
}

} // namespace kv
