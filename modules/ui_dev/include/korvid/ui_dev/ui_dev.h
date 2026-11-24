// Korvid ui_dev: the development face. A window over the player's and the library's API
// and nothing else: it copies what their tables say into its screen's model when that
// changes, and turns the screen's events into their commands. modules/ui_dev/README.md
// has the model.
//
// Nothing here is needed to use it: with the module in the engine and a window open, the
// face is up. What is here is for the application that chooses a face, and for tests.
#pragma once
#include "korvid/core/name.h"
#include "korvid/core/types.h"

namespace kv {

struct Engine;

struct UiDevConfig {
    bool enabled = true; // false: the module is in the engine and does nothing (another face is up, or none)
    bool open    = true; // false: the screen is opened by ui_dev_open, not at startup (a test that gives it files first)
};
// Before engine_init.
void ui_dev_configure(const UiDevConfig& config);

// Opens the face's screen, or opens it again from the files as they are now.
void ui_dev_open(Engine& e);
// The screen's name in Krome; null when the face is not up.
Name ui_dev_screen(Engine& e);
// The screen is built and showing what the tables say.
bool ui_dev_ready(Engine& e);

// What the face has done over the run, for the tests that count what stillness costs.
struct UiDevStats {
    u64 writes   = 0; // model values written
    u64 commands = 0; // events turned into commands
    u64 syncs    = 0; // ticks in which the tables were looked at
    u64 asleep   = 0; // ticks passed over because the window was hidden
    u32 broken   = 0; // panels standing in for a file that did not load
    bool failed  = false; // the screen itself did not load: an error screen is up in its place
};
UiDevStats ui_dev_stats(Engine& e);

} // namespace kv
