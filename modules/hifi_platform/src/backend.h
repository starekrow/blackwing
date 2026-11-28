// Korvid hifi_platform: what device.cc asks of a backend. One record per backend; a
// HifiDevice is a backend and that backend's own state.
#pragma once
#include "korvid/hifi_platform/device.h"

namespace kv {

struct HifiBackend {
    const char* name;
    // Whether this backend has a device called `name`.
    bool  (*has)(const char* name);
    // Opens it: the backend's state, or null with `out.reason` set. Fills out.format and out.shared.
    void* (*open)(const HifiDeviceDesc& desc, HifiDeviceOpened& out);
    void  (*start)(void* impl);
    u32   (*delay)(void* impl);
    void  (*hold)(void* impl, bool hold); // may be null: the device goes on calling back
    void  (*close)(void* impl);
    const char* (*lost)(void* impl);                       // hifi_device_lost
    u32   (*list)(HifiDeviceInfo* out, u32 cap);           // may be null: the devices it has now, present or gone
    // May be null. The device to open alone for `name` ("default" resolved): its kernel
    // card and device on Linux. False when it has none.
    bool  (*alone_target)(const char* name, i32* card, i32* device);
};

// A backend's list changed: the generation moves on and the watcher is called. Any thread.
void hifi_devices_changed();

extern const HifiBackend k_capture_backend;
#if defined(__linux__) && !defined(__ANDROID__)
extern const HifiBackend k_pipewire_backend; // src/linux/pipewire.cc
extern const HifiBackend k_alsa_backend;     // src/linux/alsa.cc: the kernel's PCM device, alone
// Opens card/device alone for `desc`: the backend's state, or null with `why` set.
void* alsa_open_alone(i32 card, i32 device, const HifiDeviceDesc& desc, HifiDeviceOpened& out, char* why, u32 why_cap);
#endif

} // namespace kv
