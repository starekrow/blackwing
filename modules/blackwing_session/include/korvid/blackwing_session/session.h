// Korvid blackwing_session: the session. What was queued, where it had got to and how the
// chain was set are saved when they change and at exit, and restored, paused, at the next
// start. The module reads the player's tables to save and emits the player's commands to
// restore: nothing of the player's is reached any other way, and the player keeps no file.
//
// Anyone may have values of their own kept with it, which is how a face's part of the
// session (which panel was showing, where its list was) is kept without the session
// knowing any face:
//
//   SessionKeep k; format(k.owner, sizeof k.owner, "ui_dev"); format(k.key, sizeof k.key, "tab");
//   format(k.text, sizeof k.text, "events"); ctx.emit(k);          // kept, and saved with the rest
//   ... at the next start, once: SessionKept{"ui_dev", "tab", 0, "events"}
//
// modules/blackwing_session/README.md has the model.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

// ---- commands -------------------------------------------------------------------------------

// Keeps a value for `owner` under `key`: a number and a text, either or both. The newest
// for an owner and a key replaces the one before. It is saved with the session, a few
// seconds after the last change.
struct SessionKeep { char owner[16] = ""; char key[32] = ""; f64 number = 0.0; char text[256] = ""; };

// ---- notifications --------------------------------------------------------------------------

// A value kept in the session that was read at start-up: said once each, in the first
// tick, whoever its owner is. An owner takes what is its own.
struct SessionKept { char owner[16]; char key[32]; f64 number; char text[256]; };

// ---- state -----------------------------------------------------------------------------------

enum class SessionPhase : u8 {
    Off,       // nothing is restored or saved
    Restoring, // the saved queue is being enqueued, a few dozen rows a tick
    Ready,     // restored (or there was nothing to restore): changes are being watched
    Count
};

// The resource "session.status".
struct SessionStatus {
    u8   phase       = 0;  // a SessionPhase
    u8   set_aside   = 0;  // the file there was would not parse: it was set aside, and the start was an empty one
    u8   dirty       = 0;  // something has changed since the last save
    u8   writing     = 0;  // a save is in the I/O queue
    u32  queue_total = 0;      // rows the saved queue had
    u32  queue_restored = 0;   // of those, enqueued so far
    u32  saves       = 0;      // files written this run
    u64  save_bytes  = 0;      // the last one's size
    f64  save_seconds = 0.0;   // from asking for the last one to its being in place
    char reason[96]  = "";     // why the file was set aside
};

// ---- setting up -------------------------------------------------------------------------------

struct SessionConfig {
    bool        enabled = false;       // off: the module is in the engine, takes SessionKeep, and reads and writes nothing
    const char* dir     = nullptr;     // where session.json is; null or "": the user's settings directory (platform_user_dir)
    f64  change_seconds   = 3.0;       // a change is saved once nothing more has changed for this long: a drag is one write
    f64  position_seconds = 10.0;      // while playing, the position is saved this often: what a kill can lose
    u32  rows_per_tick    = 64;        // the saved queue is enqueued this many rows a tick
    bool restore_device   = true;      // false: the output device chosen is kept but not put back (one was named on the command line)
};
// Before engine_init.
void session_configure(const SessionConfig& config);

// The resource, for code outside the schedule. Null if the module is not running.
const SessionStatus* session_status(Engine& e);

// For an executable that has no engine yet: a number someone kept in the session whose
// file is in `dir` (the window's size, to open it at), or `fallback`. Reads the file.
f64 session_peek(const char* dir, const char* owner, const char* key, f64 fallback);
// Where the session file is, for a directory (or for the user's settings directory when
// `dir` is null or "", which needs the engine): "<dir>/session.json".
void session_file_in(const char* dir, char* out, usize cap);

} // namespace kv
