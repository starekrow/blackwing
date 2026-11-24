#include "standin.h"
#include "korvid/blackwing_player/schema.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include <string.h>

namespace kv {

namespace {

constinit u32 g_standin_queue = 0, g_standin_chain = 0;

void put(StandinPlayer& s, const char* type, u64 a = 0, u64 b = 0, f64 value = 0.0, const char* text = "") {
    if (s.ncommands == KV_COUNTOF(s.commands)) return;
    StandinCommand& c = s.commands[s.ncommands++];
    c.type = type; c.a = a; c.b = b; c.value = value;
    format(c.text, sizeof c.text, "%s", text);
}

void standin_take(TickContext& ctx) {
    StandinPlayer& s = ctx.state_as<StandinPlayer>();
    for (const PlayerEnqueue& e : ctx.received<PlayerEnqueue>()) put(s, "PlayerEnqueue", e.before, 0, 0.0, e.path);
    for (const PlayerQueueEdit& e : ctx.received<PlayerQueueEdit>()) put(s, "PlayerQueueEdit", e.item, e.before, (f64)e.op);
    for (const PlayerPlay& e : ctx.received<PlayerPlay>()) { (void)e; put(s, "PlayerPlay"); }
    for (const PlayerPause& e : ctx.received<PlayerPause>()) { (void)e; put(s, "PlayerPause"); }
    for (const PlayerStop& e : ctx.received<PlayerStop>()) { (void)e; put(s, "PlayerStop"); }
    for (const PlayerNext& e : ctx.received<PlayerNext>()) { (void)e; put(s, "PlayerNext"); }
    for (const PlayerPrevious& e : ctx.received<PlayerPrevious>()) { (void)e; put(s, "PlayerPrevious"); }
    for (const PlayerJump& e : ctx.received<PlayerJump>()) put(s, "PlayerJump", e.item);
    for (const PlayerSeek& e : ctx.received<PlayerSeek>()) put(s, "PlayerSeek", e.item, e.frame);
    for (const PlayerSeekBy& e : ctx.received<PlayerSeekBy>()) put(s, "PlayerSeekBy", 0, 0, e.seconds);
    for (const PlayerSetParam& e : ctx.received<PlayerSetParam>()) put(s, "PlayerSetParam", e.stage, e.param, e.value);
}

bool standin_startup(Engine& e, ModuleInstance& self) {
    StandinPlayer* s = arena_push<StandinPlayer>(self.arena);
    if (!s) return false;
    self.set_state(s);
    return engine_publish(e, self, ".transport", &s->transport) && engine_publish(e, self, ".output", &s->output);
}

constinit const KTableDesc k_standin_ktables[] = {
    {.name = ".queue", .components = "PlayerItem", .capacity = 4096, .id = &g_standin_queue, .cosmetic = true},
    {.name = ".chain", .components = "PlayerStage", .capacity = 8, .id = &g_standin_chain, .cosmetic = true},
};
constinit const SystemDesc k_standin_systems[] = {
    {.name = ".take", .phase = Phase::PreSim, .fn = standin_take,
     .reads = "PlayerEnqueue, PlayerPlay, PlayerPause, PlayerStop, PlayerNext, PlayerPrevious, PlayerJump, PlayerSeek, PlayerSeekBy, PlayerSetParam, PlayerQueueEdit",
     .writes = ".queue, .chain, .transport, .output", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

StandinPlayer* standin_player(Engine& e) { return engine_module_state_as<StandinPlayer>(e, "player"); }

u32 standin_count(Engine& e, const char* type) {
    StandinPlayer* s = standin_player(e);
    u32 n = 0;
    for (u32 i = 0; s && i < s->ncommands; ++i) n += strcmp(s->commands[i].type, type) == 0;
    return n;
}

void standin_clear(Engine& e) {
    if (StandinPlayer* s = standin_player(e)) s->ncommands = 0;
}

KV_MODULE_DEFINE(standin_player_module) = {
    .name       = "player",
    .events     = k_player_events,
    .components = k_player_components,
    .ktables    = k_standin_ktables,
    .resources  = k_player_resources,
    .systems    = k_standin_systems,
    .startup    = standin_startup,
    .memory     = {.reserve = 512 * 1024},
};

} // namespace kv
