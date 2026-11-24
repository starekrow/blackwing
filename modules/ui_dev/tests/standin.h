// A stand-in for the player: a module named "player" that registers the player's event
// types, tables and resources from its schema, publishes values a test sets by hand, and
// writes down the commands it is sent. With it in the engine in the player's place, the
// face cannot tell: it reads tables and emits commands, and that is all a player is to it.
#pragma once
#include "korvid/blackwing_player/player.h"
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(standin_player_module);

// A command as it arrived: its type's name, and what it carried.
struct StandinCommand { const char* type; u64 a; u64 b; f64 value; char text[512]; };

struct StandinPlayer {
    PlayerTransport transport;   // what player.transport says: the test's to set
    PlayerOutput    output;
    StandinCommand  commands[256];
    u32             ncommands = 0;
};
StandinPlayer* standin_player(Engine& e);
// How many of a type have arrived, and forgetting them.
u32  standin_count(Engine& e, const char* type);
void standin_clear(Engine& e);

} // namespace kv
