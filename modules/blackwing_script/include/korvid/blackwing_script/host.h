// Korvid blackwing_script: the script host. A Lua script over the player's and the
// library's API: every command, notification, table and resource by the name native code
// uses, with the engine's mapping of types and its errors (modules/script in the engine's
// checkout). The binding is made from the two modules' own tables (their schema.h): no
// event or row is described a second time here.
//
//   ScriptHostConfig c;
//   c.file = "walk.lua";
//   script_host_configure(c);            // before engine_init
//   ... the loop ...                     // the script's handlers and systems run after each tick's PostSim
//   int code = script_host_exit_code();  // what the script ended the run with
//
// The script returns the record docs/specs/script-modules.md describes (handlers and
// systems, each with its `reads` and `emits`) and is handed the spec's `ctx`. This slice
// it is one script the user runs, in process and trusted: the command line's equal.
// modules/blackwing_script/README.md has what is built of the spec and what is not.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

struct ScriptHostConfig {
    const char* file = nullptr;        // the script to run; null: the module is in the engine and does nothing
    const char* const* args = nullptr; // what followed it on the command line: kv.args
    u32         nargs = 0;
    bool        ends_run = true;       // a script that raises, or calls kv.quit, ends the run (engine_request_quit)
};
// Before engine_init.
void script_host_configure(const ScriptHostConfig& config);

enum class ScriptHostState : u8 {
    None,     // no script was given
    Running,
    Done,     // it called kv.quit
    Failed,   // it would not load, or it raised
};
struct ScriptHostStatus {
    ScriptHostState state = ScriptHostState::None;
    int  code = 0;            // what it quit with; 1 when it failed
    u64  slices = 0;          // times its units were run
    u64  emitted = 0;         // events it emitted
    char error[1024] = "";    // why it failed: the message, with its file and line
};
ScriptHostStatus script_host_status(Engine& e);
// The last run's exit code, for after the engine has gone: what the script quit with, 1 if
// it failed, 0 if there was none.
int script_host_exit_code();

} // namespace kv
