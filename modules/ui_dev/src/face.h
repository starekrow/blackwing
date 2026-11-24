// ui_dev's state, and what its files share: the model writer, which writes a value only
// when it is not what the screen already shows, and the panels' two halves (a table read
// into the model, an event turned into a command).
#pragma once
#include "korvid/core/engine.h"
#include "korvid/core/json.h"
#include "korvid/core/name.h"
#include "korvid/krome/krome.h"
#include "korvid/ui_dev/ui_dev.h"

namespace kv {

constexpr u32 kFaceKeys = 768;   // model keys the writer remembers
constexpr u32 kFaceParams = 192; // parameters the Chain panel has keys for

// One model key as the screen shows it: a hash of the value last written.
struct FaceKey { const char* key = nullptr; u64 shown = 0; };

struct Face {
    Engine* e = nullptr;
    Name    screen;            // Krome's name for the shell; null while none is open
    bool    wanted   = false;  // the shell is to be opened at the next tick
    bool    ready    = false;  // built, and written once from the tables
    bool    hidden   = false;  // the window is not seen: nothing is read, written or asked
    bool    fresh    = true;   // everything is to be written at the next sync (just built, or seen again)
    u32     awake    = 0;      // ticks still to ask for, after a command: its answer is a tick or two away
    u32     attempts = 0;      // times the shell was opened this run
    u32     stubbed  = 0;      // a bit per panel standing in for a file that did not load
    FaceKey keys[kFaceKeys];
    u32     nkeys = 0;
    UiDevStats stats;

    // Transport: the bar is the pointer's while it is held; the length the bar was last
    // drawn against, which is what a place on it means.
    bool    seek_drag = false;
    u64     bar_frames = 0;
    // Queue: the rows its list has built, as it last said.
    u32     q_first = 0, q_count = 0;
    // Chain: what its stages' definition was written from; the parameter being dragged.
    u64     chain_sig = 0;
    bool    chain_drag = false;
    u32     chain_drag_stage = 0, chain_drag_param = 0;
    char    chain_keys[kFaceParams][2][16]; // "c_v_S_P", "c_k_S_P": the writer keeps the pointers
    char    stage_keys[8][16];              // "c_flat_S"
};

// ---- the model writer (model.cc) -----------------------------------------------------------
// Each writes the key if its value is not the one last written, and asks for a frame when
// it did. `key` is a string that outlives the face (a literal, or one the face holds).
void face_set(Face& f, const char* key, const char* value);
void face_set(Face& f, const char* key, f64 value);
void face_set_bool(Face& f, const char* key, bool value);
// A list or a table: the document under `root` of a builder, written if its bytes are new.
void face_set_doc(Face& f, const char* key, JsonBuilder& b, JsonRef root);
// Forgets what was written: the next write of every key goes through. Of one key: the
// screen changed it itself (an edit's text).
void face_forget(Face& f);
void face_forget(Face& f, const char* key);

// ---- what the panels share (ui_dev.cc) -----------------------------------------------------
// A node's id as "<prefix><number>" or "<prefix><number>_<number>": the numbers.
bool face_id(Name node, const char* prefix, u64* a, u64* b = nullptr);
// "3:07", "1:02:03"; frames at a rate of 0 are 0:00.
void face_clock(char* out, usize cap, u64 frames, u32 rate);
// What to call a file when nothing better is known: its name, less its directories.
const char* face_file_name(const char* path);

// ---- the panels ----------------------------------------------------------------------------
// Each reads its tables through the context and writes its part of the model; and is
// offered each event of the screen's, saying whether it was its own (and emitted for it).
void transport_sync(TickContext& ctx, Face& f);
bool transport_event(TickContext& ctx, Face& f, const KromeEvent& ev);
void queue_sync(TickContext& ctx, Face& f);
bool queue_event(TickContext& ctx, Face& f, const KromeEvent& ev);
void output_sync(TickContext& ctx, Face& f);
void chain_sync(TickContext& ctx, Face& f);
bool chain_event(TickContext& ctx, Face& f, const KromeEvent& ev);

} // namespace kv
