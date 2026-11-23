// Korvid hifi_platform: the capture device, an output with no hardware that keeps what it
// is given.
//
// A HifiCapture is the place the frames go; the device named "capture" plays into the one
// passed as HifiDeviceDesc::backend. It outlives the devices opened on it, so a stream
// that changes format (close, open again) leaves one segment per device, in order.
//
// Two clocks. Driven: nothing happens until hifi_capture_pull, which runs the callback
// on the calling thread for exactly the frames asked, so a test decides when every frame
// is taken. Wall clock: a thread of its own pulls a period at a time at the stream's
// rate, as hardware would.
//
// It grants any format asked of it, unshared, and reports the delay it was told to: it is
// the device against which "exact" and "when it is heard" are checked.
#pragma once
#include "korvid/hifi_platform/device.h"

namespace kv {

struct HifiCapture;

struct HifiCaptureDesc {
    usize keep_bytes    = 64u << 20; // room for what is captured; frames past it are counted and dropped
    u32   delay_frames  = 0;         // what hifi_device_delay reports
    u32   period_frames = 512;       // wall clock: frames per callback
    bool  wall_clock    = false;     // false: driven by hifi_capture_pull
};

// One device's worth of captured frames.
struct HifiCaptureSegment {
    HifiDeviceFormat format;
    usize            offset = 0; // into hifi_capture_data
    usize            bytes  = 0;
};

HifiCapture* hifi_capture_create(const HifiCaptureDesc& desc);
// The devices opened on it must be closed first.
void hifi_capture_destroy(HifiCapture* c);

// Driven clock: runs the open, started device's callback for `frames` frames on this
// thread (marked realtime for the call) and keeps them. Returns the frames pulled: 0 when
// no device is open and started, or the clock is the wall's.
u32 hifi_capture_pull(HifiCapture* c, u32 frames);

// What has been captured: the segments so far (the count is returned, up to `cap` are
// written), and the bytes they index. Stable while no device on it is running or pulled.
u32       hifi_capture_segments(HifiCapture* c, HifiCaptureSegment* out, u32 cap);
const u8* hifi_capture_data(HifiCapture* c);
// Frames that did not fit in keep_bytes.
u64       hifi_capture_dropped(HifiCapture* c);
// Devices opened on it so far.
u32       hifi_capture_opens(HifiCapture* c);

} // namespace kv
