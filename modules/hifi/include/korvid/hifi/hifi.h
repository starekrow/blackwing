// Korvid hifi: the audio path. Sources go in; a render thread decodes them ahead of time
// into the lead; a device's realtime callback takes from the lead; and the path says, in
// frames of the device's clock, what the listener has heard.
//
//   HifiConfig cfg;                                  // the default device, a five-second lead
//   Hifi* h = hifi_create(cfg);
//   hifi_add_source(h, {id, decoder_for(head), file}); // file: a SparseSpan, filling in
//   ... each tick:
//   HifiEvent ev[16];
//   u32 n = hifi_poll(h, ev, 16);                    // Started, Ended, Failed, Released, Underrun, Want
//   HifiStatus st; hifi_status(h, &st);              // where the playhead is, how deep the lead
//   ...
//   hifi_destroy(h);
//
// Three threads. The caller's (every function here; none blocks for longer than it takes
// to pass a message). The render thread (hifi's own: decodes, converts, fills the lead).
// The device's realtime thread (the callback: takes from the lead and nothing else). The
// caller and the render thread pass messages under a mutex neither holds while working;
// the render thread and the callback share only a ring and a few counters.
//
// Time is the device's. A source has Started when its first frame has been heard, by the
// device's own account of its delay, and its position is counted from there; nothing
// reports where the decoder is. modules/hifi/README.md has the whole model.
//
// hifi does no I/O. A source is a sparse span its caller fills; when a decoder needs bytes
// that are not there yet, hifi says so with a Want, and looks again shortly.
#pragma once
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_platform/device.h"

namespace kv {

struct Hifi;

struct HifiConfig {
    const char* device         = "default"; // hifi_platform's device name
    void*       device_backend = nullptr;   // for "capture": the HifiCapture*
    f64 lead_seconds  = 5.0;   // how far ahead of the playhead finished audio is kept
    f64 start_seconds = 0.25;  // how much lead there must be before playback starts, or restarts after an underrun
    f64 ramp_seconds  = 0.005; // the fade to and from silence on pause, resume, flush and underrun
};

// One thing to play: a decoder and the file it reads. The span's bytes and map must stay
// where they are until the source's Released event. Pieces may arrive in it meanwhile; a
// piece may be given back (its byte cleared) only through hifi_read_fence.
struct HifiSource {
    u64                id      = 0; // the caller's name for it; comes back in events
    const DecoderDesc* decoder = nullptr;
    SparseSpan         file;
};

enum class HifiEventKind : u8 {
    Started,  // the source's first frame has been heard. `info` is what its decoder found
    Ended,    // its last frame has been heard. `status` is End or Damaged, `reason` why, `frames` how many it gave
    Failed,   // it could not be played at all: `reason`. Never Started
    Released, // the path will not touch the source's bytes again. Once per source, whatever became of it
    Underrun, // the lead ran dry while more was expected: `frames` is how many times since the last poll
    Want,     // the source's decoder is waiting for `need`, a range of its file. Said once per wait
    Count
};

struct HifiEvent {
    HifiEventKind kind   = HifiEventKind::Count;
    u64           source = 0;
    DecoderInfo   info;
    DecodeStatus  status = DecodeStatus::End;
    u64           frames = 0;
    ByteRange     need;
    char          reason[96] = "";
};

struct HifiStatus {
    bool             device_open = false;
    HifiDeviceFormat format;         // what the device granted
    bool             shared      = false;
    bool             exact       = false; // every sample reaching the device is the source's own: the format carries its bits, unshared
    bool             paused      = false;
    bool             held        = false; // the device is held: nothing to play, and what it had was heard
    bool             flushing    = false; // a hifi_flush has not reached the render thread yet
    u64              written     = 0; // stream frames the render thread has put in the lead, ever
    u64              taken       = 0; // stream frames the device has taken (or skipped past, after a flush)
    u64              heard       = 0; // stream frames the listener has heard
    u32              lead_frames = 0; // written - taken: what is ready and not yet taken
    u32              underruns   = 0; // ever
    u64              source      = 0; // the source being heard; 0 between sources
    u64              position    = 0; // frames of it heard so far
};

// Starts the render thread. Not paused. Null only if the thread cannot start.
Hifi* hifi_create(const HifiConfig& config);
// Stops both threads, closes the device and frees everything. Sources not yet Released
// are let go of without an event.
void  hifi_destroy(Hifi* h);

// Queues a source after those already queued. It plays when the ones before it are done,
// in the same stream if its rate and channels are the same and the device's format holds
// its bits; otherwise the lead drains and the device is opened again.
void hifi_add_source(Hifi* h, const HifiSource& source);
// Holds the playhead (after the ramp down) and lets it go again (with the ramp up). The
// lead is kept.
void hifi_pause(Hifi* h);
void hifi_play(Hifi* h);
// Drops every source and everything in the lead: what is playing ramps down and stops.
// Sources flushed get Released and nothing else. What is added afterwards starts clean.
void hifi_flush(Hifi* h);
// The bytes source `id` waits for will not come (a read failed): when it next waits, its
// decode ends there, Damaged with `reason`, or it Fails if it had not opened yet. What
// was decoded before plays.
void hifi_starve(Hifi* h, u64 id, const char* reason);

// Giving a piece back. A decoder may have seen a piece's byte set just before it was
// cleared, and still be reading it. Clear the byte, then take a fence; once
// hifi_read_passed says the fence is passed, no decoder is still reading what it saw,
// and the piece's memory can go.
u64  hifi_read_fence(Hifi* h);
bool hifi_read_passed(Hifi* h, u64 fence);

// Events since the last poll, oldest first; returns how many were written. Started and
// Ended arrive when the device's clock passes them, so poll as often as they should be
// timely.
u32  hifi_poll(Hifi* h, HifiEvent* out, u32 cap);
// Where things stand. `heard`, `source` and `position` are as of the last hifi_poll.
void hifi_status(Hifi* h, HifiStatus* out);

} // namespace kv
