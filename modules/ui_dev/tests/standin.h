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
// Rows of the stand-in's tables, made by hand. A queue row: `pos` is its place, its id the
// same number unless given. A parameter and a preset of a stage.
PlayerItem*  standin_item(Engine& e, u32 pos, const char* path, PlayerItemState state = PlayerItemState::Idle, u64 id = 0);
PlayerItem*  standin_find(Engine& e, u64 id);
void         standin_remove(Engine& e, u64 id);
PlayerStage* standin_stage(Engine& e, u32 index, const char* name);
PlayerParam* standin_param(Engine& e, u32 stage, u32 index, const char* name, f64 lo, f64 hi, f64 step, const char* unit, f64 value);
void         standin_preset(Engine& e, u32 stage, u32 index, const char* name);
// The last command of a type that arrived; null when none has.
const StandinCommand* standin_last(Engine& e, const char* type);
// How many of a type have arrived, and forgetting them.
u32  standin_count(Engine& e, const char* type);
void standin_clear(Engine& e);

} // namespace kv
