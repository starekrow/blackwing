// Korvid hifi_platform: an audio output device, opened at a format, that pulls frames
// from a callback.
//
//   HifiDeviceDesc d;
//   d.name = "capture"; d.backend = capture;        // or "default", once a backend has one
//   d.want = {44100, 2, HifiSampleFormat::S16};
//   d.callback = fill; d.user = me;
//   HifiDeviceOpened o = hifi_device_open(d);
//   if (!o.device) ... o.reason
//   ... o.format is what was granted, which may not be what was asked
//   hifi_device_start(o.device);
//   ... fill(me, out, frames) is called on the backend's realtime thread
//   hifi_device_hold(o.device, true);                // nothing to play: it may stop calling back
//   hifi_device_hold(o.device, false);               // and go on
//   hifi_device_close(o.device);                     // no callback runs after this returns
//
// The callback runs on a thread marked realtime (korvid/core/thread.h): it must not
// allocate, lock, log or block. `hifi` is the caller of this header; players and faces
// are not. modules/hifi_platform/README.md has the rest.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// How one sample is stored in a device's buffer.
enum class HifiSampleFormat : u8 {
    S16, // i16
    S24, // i32 holding 24 significant bits, sign-extended: the value is in [-2^23, 2^23)
    S32, // i32
    F32, // f32 in [-1, 1)
    Count
};

// Bytes one sample takes: 2 for S16, 4 for the rest.
constexpr u32 hifi_sample_bytes(HifiSampleFormat f) { return f == HifiSampleFormat::S16 ? 2u : 4u; }
// Significant bits a format carries exactly: 16, 24, 32, and 24 for F32.
constexpr u32 hifi_sample_bits(HifiSampleFormat f) {
    return f == HifiSampleFormat::S16 ? 16u : f == HifiSampleFormat::S32 ? 32u : 24u;
}
// "s16", "s24", "s32", "f32".
const char* hifi_sample_format_name(HifiSampleFormat f);

// What a device's stream is: interleaved frames at a rate.
struct HifiDeviceFormat {
    u32              rate     = 0;
    u16              channels = 0;
    HifiSampleFormat sample   = HifiSampleFormat::S16;
};
constexpr u32 hifi_frame_bytes(const HifiDeviceFormat& f) { return hifi_sample_bytes(f.sample) * f.channels; }

// Fills `out` with `frames` frames in the device's format. Called on the backend's
// realtime thread, and never after hifi_device_close has returned.
typedef void (*HifiDeviceCallback)(void* user, void* out, u32 frames);

struct HifiDevice;

struct HifiDeviceDesc {
    const char*        name     = "default"; // "capture", or a backend's device
    HifiDeviceFormat   want;                 // what the caller would like
    HifiDeviceCallback callback = nullptr;
    void*              user     = nullptr;
    void*              backend  = nullptr;   // for "capture": the HifiCapture* to play into
};

// hifi_device_open's answer.
struct HifiDeviceOpened {
    HifiDevice*      device = nullptr; // null when it failed
    HifiDeviceFormat format;           // what was granted
    bool             shared = false;   // other streams are mixed into this device, or its format is converted on the way
    const char*      reason = "";      // when it failed: why, in words; a constant
};

// Opens a device. It does not call back until hifi_device_start. May allocate and block;
// not for a realtime thread.
HifiDeviceOpened hifi_device_open(const HifiDeviceDesc& desc);
// Starts the callbacks.
void hifi_device_start(HifiDevice* d);
// Frames between the callback and the listener's ear, as of now: a frame written in the
// next callback is heard this many frames of the device's clock later. Any thread.
u32  hifi_device_delay(HifiDevice* d);
// Holds the device (true) or lets it go on (false). A held device may stop calling back,
// so a stream with nothing to play costs nothing; it is for when the frames already given
// have been heard. A backend that cannot stop goes on calling back. Not for a realtime
// thread; any other.
void hifi_device_hold(HifiDevice* d, bool hold);
// Stops the callbacks, waits for one that is running, and closes the device.
void hifi_device_close(HifiDevice* d);

} // namespace kv
