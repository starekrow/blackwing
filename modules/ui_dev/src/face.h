// ui_dev's state, and what its files share: the model writer, which writes a value only
// when it is not what the screen already shows, and the panels' two halves (a table read
// into the model, an event turned into a command).
#pragma once
#include "korvid/core/engine.h"
#include "korvid/core/name.h"
#include "korvid/ui_dev/ui_dev.h"

namespace kv {

constexpr u32 kFaceKeys = 512; // model keys the writer remembers

// One model key as the screen shows it: a hash of the value last written.
struct FaceKey { const char* key = nullptr; u64 shown = 0; };

struct Face {
    Engine* e = nullptr;
    Name    screen;            // Krome's name for the shell; null while none is open
    bool    ready    = false;  // built, and written once from the tables
    bool    hidden   = false;  // the window is not seen: nothing is read, written or asked
    bool    fresh    = true;   // everything is to be written at the next sync (just built, or seen again)
    u32     awake    = 0;      // ticks still to ask for, after a command: its answer is a tick or two away
    u32     attempts = 0;      // times the shell was opened this run
    u32     stubbed  = 0;      // a bit per panel standing in for a file that did not load
    FaceKey keys[kFaceKeys];
    u32     nkeys = 0;
    UiDevStats stats;
};

// ---- the model writer (model.cc) -----------------------------------------------------------
// Each writes the key if its value is not the one last written, and asks for a frame when
// it did. `key` is a string that outlives the face (a literal).
void face_set(Face& f, const char* key, const char* value);
void face_set(Face& f, const char* key, f64 value);
void face_set_bool(Face& f, const char* key, bool value);
// Forgets what was written: the next write of every key goes through.
void face_forget(Face& f);

// ---- the panels ----------------------------------------------------------------------------
// Each reads its tables through the context and writes its part of the model; and takes
// an event of the screen's by name, saying whether it was its own.
void transport_sync(TickContext& ctx, Face& f);
bool transport_event(TickContext& ctx, Face& f, Name event, f64 value);

} // namespace kv
