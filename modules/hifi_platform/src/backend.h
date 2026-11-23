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
    void  (*close)(void* impl);
};

extern const HifiBackend k_capture_backend;

} // namespace kv
