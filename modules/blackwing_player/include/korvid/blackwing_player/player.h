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

// A path, where the player takes or shows one: as long as the OS allows one to be (4095
// bytes on Linux, which no other system's users are likely to pass).
constexpr u32 kPlayerPathMax = 4096;
constexpr u32 kPlayerDeviceNameMax = 192; // an output device's name: hifi_platform's kHifiDeviceNameMax

// Adds a file to the queue: before row `before`, or at the end (0). The path is the OS's,
// absolute or relative to the working directory.
struct PlayerEnqueue { char path[kPlayerPathMax]; u64 before = 0; };
// An edit of the queue. Remove: row `item` leaves the list (and the table). Move: row
// `item` goes before row `before` (0: to the end); its id stays. Clear: every row goes,
// and playing stops. The place stays at the row it was at; if that row is removed, the
// row that takes its position plays. An edit after the place is heard as the edit even
// when the next row's audio is already in the lead.
enum class PlayerEditOp : u8 { Remove, Move, Clear };
struct PlayerQueueEdit { u8 op; u64 item; u64 before; };
// Plays: from the first item not yet done, or on from a pause.
struct PlayerPlay    { u8 unused = 0; };
// Holds the playhead where it is.
struct PlayerPause   { u8 unused = 0; };
// Stops. The item that was playing will start again from its beginning on the next play.
struct PlayerStop    { u8 unused = 0; };
// Ends the item that is playing, or about to, and goes on to the one after it.
struct PlayerNext    { u8 unused = 0; };
// Within the first three seconds of an item, the one before it; later, this one from its
// start.
struct PlayerPrevious { u8 unused = 0; };
// Plays this row now.
struct PlayerJump    { u64 item; };
// Goes to `frame` of a row: item 0 is the one playing. Within the row being heard, the
// old audio fades out at once and the new fades in; PlayerSeeked says when it is heard.
// Of another row, it is a jump there, starting at that frame.
struct PlayerSeek    { u64 item; u64 frame; };
// Goes `seconds` on (negative: back) from wherever the last seek was going, or from the
// frame heard, so repeats add up.
struct PlayerSeekBy  { f64 seconds; };
// Takes up a place without playing: the transport is at row `item`, at `frame`, paused,
// and nothing is heard. PlayerPlay goes on from there. What was sounding stops, as for a
// PlayerStop. A row has no rate or length until it has been heard, so whoever knows them
// may say them: `rate` and `frames` are what the transport shows for the row until it
// plays (0: not known), and the file's own take their place then. A seek while cued moves
// the place and stays cued; a stop, a skip or a jump ends it.
struct PlayerCue     { u64 item; u64 frame; u64 frames = 0; u64 rate = 0; };
// Sets a parameter of a stage of the chain (player.chain's rows, by index): volume is
// stage 0, parameter 0, in dB (0 at most); its parameter 1 is mute. What is heard changes
// by a recut, within a callback or two; PlayerParamChanged says when it is heard. A
// control that is dragged may send one per tick: only the newest value is rendered. With
// nothing reaching the ear (stopped, cued or paused) the value is the one that will be
// heard, and PlayerParamChanged says so in the next tick.
struct PlayerSetParam { u32 stage; u32 param; f64 value; };
// Sets every parameter of a stage to a preset's values (player.presets' rows, by their
// `index` within the stage): one recut renders them all, and each is said as it is heard.
struct PlayerApplyPreset { u32 stage; u32 preset; };
// Plays through another output from now: `name` as player.devices has it, or "default" to
// follow the OS's default wherever it goes. What is playing goes on there from the frame
// heard, by a jump. A device named and not there now is waited for or stood in for by the
// default, as PlayerConfig::wait_for_absent says; it is taken up when it comes.
struct PlayerSelectDevice { char name[kPlayerDeviceNameMax]; u8 alone = 0; };

// ---- notifications: events the player emits --------------------------------------------
// Each carries `seq`, which counts up by one across all of them. Events of different types
// emitted in one tick reach their handlers in the handlers' order, not the order they
// happened in; `seq` is the order they happened in. For "what is playing now", read
// player.transport rather than keeping score from events.

// The item's first frame has been heard.
struct PlayerTrackStarted { u64 item; u32 rate; u16 channels; u16 bits; u64 frames; u32 seq; };
// A seek's target frame has been heard. Not a new PlayerTrackStarted: the item goes on.
struct PlayerSeeked       { u64 item; u64 frame; u32 seq; };
// A parameter's new value is what is heard. Changes that came faster than they could
// each be rendered are said once, at the value heard.
struct PlayerParamChanged { u32 stage; u32 param; f64 value; u32 seq; };
// Its last frame has been heard. `damaged`: the file was cut short or spoiled, and
// `reason` says how; what was heard was good.
struct PlayerTrackEnded   { u64 item; u64 frames; u32 seq; bool damaged; char reason[96]; };
// The item could not be played at all. It was never Started.
struct PlayerTrackFailed  { u64 item; u32 seq; char reason[96]; };
// Every item in the queue is done or failed, and nothing is sounding. Once per run of the queue.
struct PlayerQueueEnded   { u32 seq; };
// The lead ran dry with more expected: `count` times since the last tick.
struct HifiUnderrun       { u32 count; u32 seq; };
// What became of an output device, by player.devices' name for it. `reason` in words.
enum class PlayerDeviceChange : u8 {
    Added,   // a device there was not before
    Gone,    // a device went away (the one playing is also said as Lost)
    Back,    // one that had gone came back
    Default, // the OS's default is now this one
    Lost,    // the one playing went: the transport is paused at what was heard, and nothing plays elsewhere until asked
    Moved,   // the music is on this one now: chosen, the default followed, or stood in for one not there
    Count
};
struct PlayerDeviceChanged { u8 kind; char name[kPlayerDeviceNameMax]; char reason[96]; u32 seq; };

// ---- state: what anyone may read ---------------------------------------------------------

// What is true of a row now. A row before the place is done: heard, or skipped; its
// state is then idle (or failed), and `damaged` and `reason` say how it ended.
enum class PlayerItemState : u8 {
    Idle,    // nothing of it read
    Reading, // its first pieces are being read
    Ready,   // its first pieces are in, and a decoder takes it; the rest arrive as it plays
    Playing, // its first frame has been heard and its last has not
    Failed,  // could not be played: `reason`
    Count
};
// "idle", "reading", ...
const char* player_item_state_name(PlayerItemState s);

// A row of the ktable "player.queue": the component PlayerItem, one row per item in the
// list. The table's order is not the list's: `pos` is. A row removed from the list
// leaves the table.
struct PlayerItem {
    u64  id;          // from 1, in the order added; it stays through moves
    u32  pos;         // its place in the list, from 1
    u8   state;       // a PlayerItemState
    u8   damaged;     // done, but the file ended early or was spoiled: `reason`
    u16  channels;    // the format, known once the item has Started
    u16  bits;
    u32  rate;
    u64  frames;      // its length as its header says; 0 until Started, or when it does not say
    u64  size;        // the file's bytes; 0 until it has been looked at
    u64  bytes_read;  // of those, how many are in memory now
    char reason[96];  // why it Failed, or how it was damaged
    char path[kPlayerPathMax]; // last: a row's other fields are its first two hundred bytes
};

enum class PlayerState : u8 {
    Stopped,  // not playing and not meaning to
    Starting, // meaning to play; nothing is being heard yet (reading, decoding, a gap between devices)
    Playing,  // an item is being heard
    Paused,   // held: by a pause, or by a cue (a place taken up and not yet played)
    Count
};
// "stopped", "starting", "playing", "paused".
const char* player_state_name(PlayerState s);

// The resource "player.transport".
struct PlayerTransport {
    u8   state    = 0;  // a PlayerState
    u8   pad[7]   = {};
    u64  item     = 0;  // the item being heard, or cued; 0 when none is (between the old audio and the new, after a seek or a skip)
    u64  position = 0;  // the frame of it being heard: where it started, or was sent to, and the frames heard since; a cue's frame
    u64  frames   = 0;  // its length; 0 when its header does not say
    u32  rate     = 0;  // frames per second of the item being heard
    u32  queued   = 0;  // rows in the list
    u32  left     = 0;  // items not yet done or failed
    u32  pad2     = 0;
    u64  place    = 0;  // the row the transport is at: the one playing, or about to; 0 past the end
    u32  place_pos = 0; // its pos (the list's length and one, past the end): the rows before it are done
    u32  pad3     = 0;
    u64  seek_item  = 0; // a seek on its way to the ear: its row (0: none), and the frame it was sent to
    u64  seek_frame = 0;
};

// A row of the ktable "player.chain": the component PlayerStage, one row per stage of the
// chain, in order, fixed at startup. Its parameters are the values heard.
struct PlayerStage {
    u32  index;
    u8   flat;          // with these parameters it passes audio unchanged
    u8   params_len;
    u16  pad;
    char name[32];
    char param_names[48][16];
    f64  params[48];
};

// A row of the ktable "player.params": the component PlayerParam, one row per parameter
// of every stage, fixed at startup but for `value`. Enough to draw a control for a stage
// nobody has heard of: a slider from `lo` to `hi` in steps of `step`, a switch when that
// is 0 to 1 in steps of 1. `value` is the value heard, as player.chain's `params` are.
struct PlayerParam {
    u32  stage;        // its stage's index (a player.chain row's)
    u32  index;        // its own, within the stage: what PlayerSetParam's `param` is
    f64  value;        // heard
    f64  lo, hi;       // what a value is kept within
    f64  initial;      // what a new chain starts with
    f64  step;         // the smallest change worth offering; 0: any
    char name[16];
    char unit[8];      // "dB", "Hz"; "" when it has none
};

// A row of the ktable "player.presets": the component PlayerPreset, one row per preset a
// stage carries, fixed at startup. PlayerApplyPreset{stage, index} applies it.
struct PlayerPreset {
    u32  stage;
    u32  index;        // within the stage, from 0
    char name[32];
};

// A row of the ktable "player.devices": the component PlayerDevice, one per output the
// OS offers, and the capture device where there is one. A device that goes stays, not
// present, for the run's life.
struct PlayerDevice {
    char name[kPlayerDeviceNameMax];  // what PlayerSelectDevice takes: stable across runs where the OS has a name for it
    char label[128];                  // what it is called, for a person
    char backend[16];                 // "pipewire", "capture"
    u8   is_default;                  // the OS's default now
    u8   present;                     // 0: it has gone
    u8   shared;                      // other programs' sound is mixed into it
    u8   can_exclusive;               // it can be opened alone
    u8   open;                        // the music is on it
    u8   chosen;                      // it is the one chosen (or, choosing "default", the default)
    u16  channels;                    // as the OS says; 0 when it does not
};

// The resource "player.output": the device and the lead, as hifi reports them.
struct PlayerOutput {
    u8   open      = 0; // a device is open
    u8   shared    = 0; // it mixes other streams in, or converts on the way
    u8   exact     = 0; // every sample reaching it is the file's own: unshared, its bits held, every stage flat, no join under way
    u8   sample    = 0; // a HifiSampleFormat
    u16  channels  = 0;
    u8   bits      = 0; // the significant bits its word carries exactly: 16, 24 or 32 (24 for a float word)
    u8   float_word = 0; // its word is a float
    u32  rate      = 0;
    u32  lead_frames = 0; // decoded and ready, not yet taken by the device
    u32  underruns = 0;   // ever
    u8   joining   = 0;   // a recut or a jump is being joined
    u8   pad3[3]   = {};
    u64  bytes_held = 0;
    f64  recut_seconds = 0.0; // the last recut: from the change reaching hifi to the join's first frame leaving the callback
    u32  recuts    = 0;   // ever
    u32  resampled_from = 0; // the device would not take the file's rate: it is resampled from this one to `rate`; 0 when it is not
    f64  recut_render_seconds = 0.0; // the last recut's rendering of the whole lead
    f64  jump_seconds = 0.0;         // the last seek or skip: from reaching hifi to its first frame leaving the callback  // of the files, in memory now across every item: at most PlayerConfig::memory_bytes
    char device[kPlayerDeviceNameMax] = ""; // the device open, by player.devices' name; empty when none is
    char chosen[kPlayerDeviceNameMax] = ""; // what was chosen: "default", or a name
    u8   following  = 0;  // the default is chosen, and followed
    u8   standing_in = 0; // the device chosen is not there: the default stands in for it
    u8   lost       = 0;  // the device playing went: paused, until asked to play (anywhere)
    u8   pad5       = 0;
    char none_why[96] = ""; // no device open while one is wanted: why
    u8   alone      = 0;  // the device is open alone: no other program's sound on it
    u8   alone_asked = 0; // alone was chosen
    u8   released   = 0;  // paused alone past exclusive_hold: the device let go, opened again at the next play
    u8   pad6       = 0;
    char alone_why[96] = ""; // alone was chosen and the device is shared: why
};

// ---- setting up ----------------------------------------------------------------------------

struct PlayerConfig {
    const char* device         = "default";  // hifi_platform's device name
    bool        device_alone   = false;      // and alone
    void*       device_backend = nullptr;    // for "capture": the HifiCapture*; null: one of the player's own, on the wall clock, keeping nothing
    f64         lead_seconds   = 5.0;
    bool        wait_for_absent = false;     // a device chosen and not there: true waits for it; false plays on the default meanwhile
    f64         exclusive_hold = 60.0;       // a device open alone and paused this long is let go, so other programs can sound; less than 0: never
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
