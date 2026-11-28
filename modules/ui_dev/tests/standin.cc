#include "standin.h"
#include "korvid/blackwing_library/schema.h"
#include "korvid/blackwing_player/schema.h"
#include "korvid/core/memory.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include <string.h>

namespace kv {

namespace {

constinit u32 g_standin_queue = 0, g_standin_chain = 0, g_standin_params = 0, g_standin_presets = 0, g_standin_devices = 0;

StandinCommand* put(StandinPlayer& s, const char* type, u64 a = 0, u64 b = 0, f64 value = 0.0, const char* text = "") {
    static StandinCommand spare;
    if (s.ncommands == kStandinCommands) return &spare;
    StandinCommand& c = s.commands[s.ncommands++];
    c = StandinCommand{};
    c.type = type; c.a = a; c.b = b; c.value = value;
    format(c.text, sizeof c.text, "%s", text);
    return &c;
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
    for (const PlayerCue& e : ctx.received<PlayerCue>()) put(s, "PlayerCue", e.item, e.frame, (f64)e.rate);
    for (const PlayerSetParam& e : ctx.received<PlayerSetParam>()) put(s, "PlayerSetParam", e.stage, e.param, e.value);
    for (const PlayerApplyPreset& e : ctx.received<PlayerApplyPreset>()) put(s, "PlayerApplyPreset", e.stage, e.preset);
}

bool standin_startup(Engine& e, ModuleInstance& self) {
    StandinPlayer* s = arena_push<StandinPlayer>(self.arena);
    if (!s) return false;
    self.set_state(s);
    s->commands = (StandinCommand*)mem_alloc(sizeof(StandinCommand) * kStandinCommands);
    return engine_publish(e, self, ".transport", &s->transport) && engine_publish(e, self, ".output", &s->output);
}

constinit const KTableDesc k_standin_ktables[] = {
    {.name = ".queue", .components = "PlayerItem", .capacity = 4096, .id = &g_standin_queue, .cosmetic = true},
    {.name = ".chain", .components = "PlayerStage", .capacity = 8, .id = &g_standin_chain, .cosmetic = true},
    {.name = ".params", .components = "PlayerParam", .capacity = 192, .id = &g_standin_params, .cosmetic = true},
    {.name = ".presets", .components = "PlayerPreset", .capacity = 64, .id = &g_standin_presets, .cosmetic = true},
    {.name = ".devices", .components = "PlayerDevice", .capacity = 128, .id = &g_standin_devices, .cosmetic = true},
};
constinit const SystemDesc k_standin_systems[] = {
    {.name = ".take", .phase = Phase::PreSim, .fn = standin_take,
     .reads = "PlayerEnqueue, PlayerPlay, PlayerPause, PlayerStop, PlayerNext, PlayerPrevious, PlayerJump, PlayerSeek, PlayerSeekBy, PlayerCue, PlayerSetParam, PlayerApplyPreset, PlayerQueueEdit",
     .writes = ".queue, .chain, .params, .presets, .devices, .transport, .output", .main_thread = true, .runs = RunWhen::Always},
};

void standin_shutdown(Engine& e, ModuleInstance&) {
    if (StandinPlayer* s = engine_module_state_as<StandinPlayer>(e, "player")) { mem_free(s->commands); s->commands = nullptr; }
}

// ---- the library's ----

constinit u32 g_standin_roots = 0, g_standin_rows = 0;

void standin_library_take(TickContext& ctx) {
    StandinPlayer* p = engine_module_state_as<StandinPlayer>(*ctx.engine, "player");
    if (!p) return;
    for (const LibraryAddRoot& e : ctx.received<LibraryAddRoot>()) put(*p, "LibraryAddRoot", 0, 0, 0.0, e.path);
    for (const LibraryRemoveRoot& e : ctx.received<LibraryRemoveRoot>()) put(*p, "LibraryRemoveRoot", e.root);
    for (const LibraryQuery& e : ctx.received<LibraryQuery>()) { StandinCommand* c = put(*p, "LibraryQuery", e.id, e.first, (f64)(u8)e.view, e.text); c->c = e.count; c->d = e.parent | ((u64)(u8)e.sort << 56); }
    for (const LibraryClose& e : ctx.received<LibraryClose>()) put(*p, "LibraryClose", e.id);
    for (const LibraryPrioritize& e : ctx.received<LibraryPrioritize>()) put(*p, "LibraryPrioritize", e.query, e.dir, 0.0, e.path);
    for (const LibraryOverride& e : ctx.received<LibraryOverride>()) {
        char text[400];
        format(text, sizeof text, "%s=%s", e.field, e.value);
        put(*p, "LibraryOverride", e.track, e.clear, 0.0, text);
    }
    for (const LibraryPath& e : ctx.received<LibraryPath>()) put(*p, "LibraryPath", e.id, e.index, 0.0, e.path);
    for (const LibraryCollect& e : ctx.received<LibraryCollect>()) put(*p, "LibraryCollect", e.id, e.row, (f64)e.kind, e.text);
}

bool standin_library_startup(Engine& e, ModuleInstance& self) {
    StandinLibrary* s = arena_push<StandinLibrary>(self.arena);
    if (!s) return false;
    self.set_state(s);
    return engine_publish(e, self, ".scan", &s->scan);
}

constinit const KTableDesc k_standin_library_ktables[] = {
    {.name = ".roots", .components = "LibraryRoot", .capacity = 64, .id = &g_standin_roots, .cosmetic = true},
    {.name = ".rows", .components = "LibraryRow", .capacity = kLibraryMaxRows, .id = &g_standin_rows, .cosmetic = true},
};
constinit const SystemDesc k_standin_library_systems[] = {
    {.name = ".take", .phase = Phase::PreSim, .fn = standin_library_take,
     .reads = "LibraryAddRoot, LibraryRemoveRoot, LibraryQuery, LibraryClose, LibraryPrioritize, LibraryOverride, LibraryPath, LibraryCollect",
     .writes = ".roots, .rows, .scan", .after = "player.take", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

StandinLibrary* standin_library(Engine& e) { return engine_module_state_as<StandinLibrary>(e, "library"); }

LibraryRoot* standin_root(Engine& e, u32 id, const char* path, LibraryRootState state, u32 tracks) {
    KTable* t = engine_ktable(e, g_standin_roots);
    LibraryRoot r{};
    r.id = id; r.state = (u8)state; r.tracks = tracks;
    format(r.path, sizeof r.path, "%s", path);
    Handle h = t->spawn(r);
    return h.is_null() ? nullptr : &t->rows_of<LibraryRoot>()[h.index()];
}

LibraryRow* standin_row(Engine& e, u32 query, u32 index, LibraryRowKind kind, u64 id, const char* name, const char* title, const char* artist, const char* album) {
    KTable* t = engine_ktable(e, g_standin_rows);
    LibraryRow* r = (LibraryRow*)mem_alloc(sizeof(LibraryRow));
    *r = LibraryRow{};
    r->query = query; r->index = index; r->kind = (u8)kind; r->id = id; r->root = 1;
    format(r->name, sizeof r->name, "%s", name);
    format(r->title, sizeof r->title, "%s", title);
    format(r->artist, sizeof r->artist, "%s", artist);
    format(r->album, sizeof r->album, "%s", album);
    if (kind == LibraryRowKind::Track) format(r->path, sizeof r->path, "/music/%s", name);
    Handle h = t->spawn(*r);
    mem_free(r);
    return h.is_null() ? nullptr : &t->rows_of<LibraryRow>()[h.index()];
}

void standin_rows_clear(Engine& e, u32 query) {
    KTable* t = engine_ktable(e, g_standin_rows);
    Span<LibraryRow> rows = t->rows_of<LibraryRow>();
    for (u32 i = 0; i < rows.len; ++i) if (t->is_alive(i) && rows[i].query == query) t->destroy(t->handle_at(i));
}

KV_MODULE_DEFINE(standin_library_module) = {
    .name       = "library",
    .events     = k_library_events,
    .components = k_library_components,
    .ktables    = k_standin_library_ktables,
    .resources  = k_library_resources,
    .systems    = k_standin_library_systems,
    .startup    = standin_library_startup,
    .memory     = {.reserve = 64 * 1024},
};

StandinPlayer* standin_player(Engine& e) { return engine_module_state_as<StandinPlayer>(e, "player"); }

u32 standin_count(Engine& e, const char* type) {
    StandinPlayer* s = standin_player(e);
    u32 n = 0;
    for (u32 i = 0; s && i < s->ncommands; ++i) n += strcmp(s->commands[i].type, type) == 0;
    return n;
}

const StandinCommand* standin_last(Engine& e, const char* type) {
    StandinPlayer* s = standin_player(e);
    for (u32 i = s ? s->ncommands : 0; i > 0; --i) if (strcmp(s->commands[i - 1].type, type) == 0) return &s->commands[i - 1];
    return nullptr;
}

PlayerItem* standin_item(Engine& e, u32 pos, const char* path, PlayerItemState state, u64 id) {
    KTable* q = engine_ktable(e, g_standin_queue);
    PlayerItem r{};
    r.id = id ? id : pos;
    r.pos = pos;
    r.state = (u8)state;
    format(r.path, sizeof r.path, "%s", path);
    Handle h = q->spawn(r);
    return h.is_null() ? nullptr : &q->rows_of<PlayerItem>()[h.index()];
}

PlayerItem* standin_find(Engine& e, u64 id) {
    KTable* q = engine_ktable(e, g_standin_queue);
    Span<PlayerItem> rows = q->rows_of<PlayerItem>();
    for (u32 i = 0; i < rows.len; ++i) if (q->is_alive(i) && rows[i].id == id) return &rows[i];
    return nullptr;
}

void standin_remove(Engine& e, u64 id) {
    KTable* q = engine_ktable(e, g_standin_queue);
    Span<PlayerItem> rows = q->rows_of<PlayerItem>();
    for (u32 i = 0; i < rows.len; ++i) if (q->is_alive(i) && rows[i].id == id) { q->destroy(q->handle_at(i)); return; }
}

PlayerStage* standin_stage(Engine& e, u32 index, const char* name) {
    KTable* t = engine_ktable(e, g_standin_chain);
    PlayerStage r{};
    r.index = index;
    r.flat = 1;
    format(r.name, sizeof r.name, "%s", name);
    Handle h = t->spawn(r);
    return h.is_null() ? nullptr : &t->rows_of<PlayerStage>()[h.index()];
}

PlayerParam* standin_param(Engine& e, u32 stage, u32 index, const char* name, f64 lo, f64 hi, f64 step, const char* unit, f64 value) {
    KTable* t = engine_ktable(e, g_standin_params);
    PlayerParam r{};
    r.stage = stage; r.index = index;
    r.lo = lo; r.hi = hi; r.step = step;
    r.value = r.initial = value;
    format(r.name, sizeof r.name, "%s", name);
    format(r.unit, sizeof r.unit, "%s", unit);
    Handle h = t->spawn(r);
    return h.is_null() ? nullptr : &t->rows_of<PlayerParam>()[h.index()];
}

void standin_preset(Engine& e, u32 stage, u32 index, const char* name) {
    KTable* t = engine_ktable(e, g_standin_presets);
    PlayerPreset r{};
    r.stage = stage; r.index = index;
    format(r.name, sizeof r.name, "%s", name);
    t->spawn(r);
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
    .shutdown   = standin_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

} // namespace kv
