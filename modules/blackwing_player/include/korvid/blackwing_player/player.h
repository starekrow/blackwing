// Korvid blackwing_player: the player's whole API, which is data. Events it consumes,
// events it emits, a table of what is queued and two resources that say where things stand.
// Nothing here is a function a face calls.
//
//   ctx.emit(PlayerEnqueue{"/music/a.flac"});      // a unit that declares .emits; or
//   e.events.emit(PlayerPlay{});                    // code outside the schedule: a command, a test
//   ...
//   const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
//   for (const PlayerItem& it : ctx.read_ktable("player.queue")->rows_of<PlayerItem>()) ...
//
// A command emitted in one tick is acted on in the next. Every time in what comes back is
// the device's: PlayerTrackStarted is emitted in the tick after the track's first frame is
// heard, and `position` counts frames heard, never frames decoded.
// modules/blackwing_player/README.md has the model.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

// ---- commands: events the player consumes ----------------------------------------------

// Adds a file to the end of the queue. The path is the OS's, absolute or relative to the
// working directory.
struct PlayerEnqueue { char path[512]; };
// Plays: from the first item not yet done, or on from a pause.
struct PlayerPlay    { u8 unused = 0; };
// Holds the playhead where it is.
struct PlayerPause   { u8 unused = 0; };
// Stops. The item that was playing will start again from its beginning on the next play.
struct PlayerStop    { u8 unused = 0; };
// Ends the item that is playing, or about to, and goes on to the one after it.
struct PlayerNext    { u8 unused = 0; };

// ---- notifications: events the player emits --------------------------------------------
// Each carries `seq`, which counts up by one across all of them. Events of different types
// emitted in one tick reach their handlers in the handlers' order, not the order they
// happened in; `seq` is the order they happened in. For "what is playing now", read
// player.transport rather than keeping score from events.

// The item's first frame has been heard.
struct PlayerTrackStarted { u64 item; u32 rate; u16 channels; u16 bits; u64 frames; u32 seq; };
// Its last frame has been heard. `damaged`: the file was cut short or spoiled, and
// `reason` says how; what was heard was good.
struct PlayerTrackEnded   { u64 item; u64 frames; u32 seq; bool damaged; char reason[96]; };
// The item could not be played at all. It was never Started.
struct PlayerTrackFailed  { u64 item; u32 seq; char reason[96]; };
// Every item in the queue is done or failed, and nothing is sounding. Once per run of the queue.
struct PlayerQueueEnded   { u32 seq; };
// The lead ran dry with more expected: `count` times since the last tick.
struct HifiUnderrun       { u32 count; u32 seq; };

// ---- state: what anyone may read ---------------------------------------------------------

enum class PlayerItemState : u8 {
    Waiting, // queued; not read yet
    Reading, // its first pieces are being read
    Ready,   // its first pieces are in, and a decoder takes it; the rest arrive as it plays
    Playing, // its first frame has been heard and its last has not
    Done,    // heard to its end, or skipped
    Failed,  // could not be played: `reason`
    Count
};
// "waiting", "reading", ...
const char* player_item_state_name(PlayerItemState s);

// A row of the ktable "player.queue": the component PlayerItem, one row per item ever
// queued, in order. Rows are not removed when an item is done.
struct PlayerItem {
    u64  id;          // from 1, in the order queued
    u8   state;       // a PlayerItemState
    u8   damaged;     // Done, but the file ended early or was spoiled: `reason`
    u16  channels;    // the format, known once the item has Started
    u16  bits;
    u32  rate;
    u64  frames;      // its length as its header says; 0 until Started, or when it does not say
    u64  size;        // the file's bytes; 0 until it has been looked at
    u64  bytes_read;  // of those, how many are in memory now
    char path[512];
    char reason[96];  // why it Failed, or how it was damaged
};

enum class PlayerState : u8 {
    Stopped,  // not playing and not meaning to
    Starting, // meaning to play; nothing is being heard yet (reading, decoding, a gap between devices)
    Playing,  // an item is being heard
    Paused,
    Count
};
// "stopped", "starting", "playing", "paused".
const char* player_state_name(PlayerState s);

// The resource "player.transport".
struct PlayerTransport {
    u8   state    = 0;  // a PlayerState
    u8   pad[7]   = {};
    u64  item     = 0;  // the item being heard; 0 when none is
    u64  position = 0;  // frames of it heard
    u64  frames   = 0;  // its length; 0 when its header does not say
    u32  rate     = 0;  // frames per second of the item being heard
    u32  queued   = 0;  // items ever queued
    u32  left     = 0;  // items not yet Done or Failed
    u32  pad2     = 0;
};

// The resource "player.output": the device and the lead, as hifi reports them.
struct PlayerOutput {
    u8   open      = 0; // a device is open
    u8   shared    = 0; // it mixes other streams in, or converts on the way
    u8   exact     = 0; // every sample reaching it is the file's own
    u8   sample    = 0; // a HifiSampleFormat
    u16  channels  = 0;
    u16  pad       = 0;
    u32  rate      = 0;
    u32  lead_frames = 0; // decoded and ready, not yet taken by the device
    u32  underruns = 0;   // ever
    u32  pad2      = 0;
    u64  bytes_held = 0;  // of the files, in memory now across every item: at most PlayerConfig::memory_bytes
};

// ---- setting up ----------------------------------------------------------------------------

struct PlayerConfig {
    const char* device         = "default";  // hifi_platform's device name
    void*       device_backend = nullptr;    // for "capture": the HifiCapture*
    f64         lead_seconds   = 5.0;
    usize       memory_bytes   = 256u << 20; // the files' bytes held at once, across every item
    u32         piece_bytes    = 256u << 10; // what a file is read in: a power of two, a page or more
};
// Before engine_init: what the module opens at startup.
void player_configure(const PlayerConfig& config);

// The two resources, for code outside the schedule (a main loop deciding how long to
// sleep, a test). Units read them through the context. Null if the module is not running.
const PlayerTransport* player_transport(Engine& e);
const PlayerOutput*    player_output(Engine& e);

} // namespace kv
