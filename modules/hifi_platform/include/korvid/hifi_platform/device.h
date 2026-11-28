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
    const char*        name     = "default"; // "default" (the OS's default output, as it is now), "capture", or a name from hifi_devices
    HifiDeviceFormat   want;                 // what the caller would like
    HifiDeviceCallback callback = nullptr;
    void*              user     = nullptr;
    void*              backend  = nullptr;   // for "capture": the HifiCapture* to play into
    bool               alone    = false;     // open it alone, if it can be (can_exclusive): no other program's sound, the file's rate where the device has it, its word or the device's widest. Refused, it is opened shared, and `alone_why` says why
};

// hifi_device_open's answer.
struct HifiDeviceOpened {
    HifiDevice*      device = nullptr; // null when it failed
    HifiDeviceFormat format;           // what was granted
    bool             shared = false;   // other streams are mixed into this device, or its format is converted on the way
    const char*      reason = "";      // when it failed: why, in words; a constant
    char             name[192] = "";   // the device opened, by its name in hifi_devices: "default" resolved to the one it is now
    bool             alone = false;    // it is open alone
    char             alone_why[96] = ""; // asked for alone and opened shared: why
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
// Null while the device plays; once it has gone (unplugged, its server gone, taken by
// another program), why, in words, and it never calls back again. Its delay is then the
// one it had when it went: frames given to it since were not heard. Any thread, not the
// callback's.
const char* hifi_device_lost(HifiDevice* d);
// How the callback's thread is scheduled, in words ("SCHED_FIFO 20, from rtkit"); "" when it
// is not realtime; null when the backend does not say (PipeWire's own thread, the capture
// device's) or has not decided yet. Any thread.
const char* hifi_device_realtime(HifiDevice* d);

// ---- the devices there are -------------------------------------------------------------

constexpr u32 kHifiDeviceNameMax = 192;

// An output the OS offers, as hifi_devices lists it.
struct HifiDeviceInfo {
    char name[kHifiDeviceNameMax] = ""; // what hifi_device_open takes: the backend's own, stable across runs where the OS has one (PipeWire's node.name)
    char label[128]  = "";              // what it is called, for a person
    char backend[16] = "";              // "pipewire", "capture"
    bool is_default  = false;           // the OS's default output now
    bool present     = true;            // false: it has gone; it stays listed for the process's life, so it can be said what went
    bool shared      = true;            // opened, other programs' streams are mixed in
    bool can_exclusive = false;         // it could be opened alone (slice 5, milestone 7)
    u16  channels    = 0;               // as the OS says; 0 when it does not
    i32  alsa_card   = -1;              // Linux: the kernel's card and device behind it, which alone opens; -1 when there is none
    i32  alsa_device = -1;
};

constexpr u32 kHifiMaxDevices = 64;

// Copies the list (up to `cap`) and returns how many there are. The first call starts the
// backends' watches and waits (a second at most) for their first answer. Not for a
// realtime thread.
u32  hifi_devices(HifiDeviceInfo* out, u32 cap);
// Moves on whenever the list, a device's presence or the default changes. Any thread.
u64  hifi_devices_generation();
// Called, on a backend's thread, after each change: one watcher for the process, which
// should do no more than wake whoever reads the list. Null stops it.
void hifi_devices_watch(void (*fn)(void* user), void* user);

} // namespace kv
