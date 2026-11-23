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
};

extern const HifiBackend k_capture_backend;
#if defined(__linux__) && !defined(__ANDROID__)
extern const HifiBackend k_pipewire_backend; // src/linux/pipewire.cc
#endif

} // namespace kv
