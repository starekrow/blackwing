// The script host, this slice's part of it: one Lua script the user runs, in process, over
// the player's and the library's API.
//
// The binding is the engine's (modules/script in its checkout): an events object that emits
// an event of a named type from a table of its fields and reads a batch as tables, a table
// object over a ktable's rows, a resource as a table of its fields, each made from the
// field tables the modules carry. What is here is the host around it: the Lua state, the
// surface as `kv` shows it (built from the two modules' schema.h), the spec's `ctx` and
// record (prelude.h, in Lua), and one unit, script.slice, that runs the script's handlers
// and systems after each tick's PostSim.
//
// The unit reads every event type, table and resource of the two APIs. That list is not
// typed here: it is put together from the schemas' own tables when the program is
// compiled (k_reads). What the script emits goes through the engine's writer for code
// outside the schedule, so it is delivered at the start of the next tick, as the spec has
// a slice's commands.
#include "korvid/blackwing_script/module.h"
#include "korvid/blackwing_script/host.h"
#include "prelude.h"
#include "korvid/blackwing_library/schema.h"
#include "korvid/blackwing_player/schema.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/lua/lua.h"
#include "korvid/platform/io.h"
#include "korvid/script/script.h"
#include <initializer_list>
#include <string.h>

namespace kv {

namespace {

constinit ScriptHostConfig g_script_config = {};
constinit int g_script_exit = 0;

// Every name the unit reads: the event types of both schemas, their tables and resources.
struct ReadsList {
    char text[4096];
    constexpr ReadsList() : text() {
        usize n = 0;
        auto add = [&](const char* s) {
            if (n) { text[n++] = ','; text[n++] = ' '; }
            for (; *s; ++s) text[n++] = *s;
        };
        for (const EventTypeDesc& d : k_player_events) add(d.name);
        for (const EventTypeDesc& d : k_library_events) add(d.name);
        for (const PlayerSurfaceTable& t : k_player_tables) add(t.name);
        for (const LibrarySurfaceTable& t : k_library_tables) add(t.name);
        text[n] = 0;
    }
};
constinit const ReadsList k_reads;

struct Host {
    Engine*    e = nullptr;
    lua_State* L = nullptr;
    int        ref_slice = LUA_NOREF, ref_shutdown = LUA_NOREF, ref_hear = LUA_NOREF;
    f64        next_due = -1.0; // seconds until a system of the script's is next due, as of the last slice
    ScriptHostStatus status;
};

Host* host_state(Engine& e) { return engine_module_state_as<Host>(e, "script"); }
Host* host_of(lua_State* L) { return (Host*)lua_touserdata(L, lua_upvalueindex(1)); }

// The script has failed: said once, with its file and line (the message's own), and stopped.
void fail(Host& h, const char* what, const char* message) {
    h.status.state = ScriptHostState::Failed;
    h.status.code = 1;
    g_script_exit = 1;
    format(h.status.error, sizeof h.status.error, "%s", message ? message : "");
    print_err("blackwing: %s: %s\n", what, message ? message : "(no message)");
    if (g_script_config.ends_run) engine_request_quit(*h.e);
}

// ---- what the prelude is given ---------------------------------------------------------------

int l_ktable(lua_State* L) {
    Host* h = host_of(L);
    const char* name = luaL_checkstring(L, 1);
    u32 id = engine_ktable_id(*h->e, name);
    KTable* t = id ? engine_ktable_now(*h->e, id) : nullptr; // the unit declared the read: its access is checked
    if (!t) return luaL_error(L, "there is no table '%s'", name);
    script_push_ktable(L, t);
    return 1;
}

int l_resource(lua_State* L) {
    Host* h = host_of(L);
    const char* name = luaL_checkstring(L, 1);
    if (!script_push_resource(L, *h->e, name)) { lua_pushnil(L); return 1; }
    return 1;
}

int l_quit(lua_State* L) {
    Host* h = host_of(L);
    int code = (int)luaL_optinteger(L, 1, 0);
    if (h->status.state == ScriptHostState::Running) {
        h->status.state = ScriptHostState::Done;
        h->status.code = code;
        g_script_exit = code;
        if (g_script_config.ends_run) engine_request_quit(*h->e);
    }
    return 0;
}

// The script's own words: info to stdout, the rest to stderr. It is the user's command.
int l_out(lua_State* L) {
    const char* level = luaL_checkstring(L, 1);
    const char* text = luaL_checkstring(L, 2);
    if (strcmp(level, "info") == 0) { print("%s\n", text); print_flush(); }
    else print_err("%s: %s\n", level, text);
    return 0;
}

// A field as a surface lists it: its name, and its type as a header would write it.
void push_fields(lua_State* L, Span<const FieldDesc> fields) {
    lua_newtable(L);
    int n = 0;
    for (const FieldDesc& f : fields) {
        if (strcmp(f.name, "unused") == 0 || strncmp(f.name, "pad", 3) == 0) continue; // padding is not shown
        lua_newtable(L);
        lua_pushstring(L, f.name);
        lua_setfield(L, -2, "name");
        char type[32];
        if (f.count > 1) format(type, sizeof type, "%s[%u]", field_type_name(f.type), f.count);
        else             format(type, sizeof type, "%s", field_type_name(f.type));
        lua_pushstring(L, type);
        lua_setfield(L, -2, "type");
        lua_rawseti(L, -2, ++n);
    }
}

// One surface, from a module's schema: its version, events (each a command or not, with its
// fields), tables and resources, enums and constants. The same shape for both modules.
template <class Enums, class Tables, class Constants>
void push_surface(lua_State* L, const char* version, Span<const EventTypeDesc> events, Span<const char* const> commands, const Enums& enums,
                  const Tables& tables, const Constants& constants) {
    lua_newtable(L);
    lua_pushstring(L, version);
    lua_setfield(L, -2, "version");
    lua_newtable(L);
    for (const EventTypeDesc& d : events) {
        lua_newtable(L);
        bool command = false;
        for (const char* c : commands) command = command || strcmp(c, d.name) == 0;
        lua_pushboolean(L, command);
        lua_setfield(L, -2, "command");
        push_fields(L, d.fields);
        lua_setfield(L, -2, "fields");
        lua_setfield(L, -2, d.name);
    }
    lua_setfield(L, -2, "events");
    lua_newtable(L);
    for (const auto& t : tables) {
        lua_newtable(L);
        lua_pushstring(L, t.component);
        lua_setfield(L, -2, "component");
        lua_pushboolean(L, t.resource);
        lua_setfield(L, -2, "resource");
        push_fields(L, t.fields);
        lua_setfield(L, -2, "fields");
        lua_setfield(L, -2, t.name);
    }
    lua_setfield(L, -2, "tables");
    lua_newtable(L);
    for (const auto& en : enums) {
        lua_newtable(L);
        int n = 0;
        for (const char* v : en.values) { lua_pushstring(L, v); lua_rawseti(L, -2, ++n); }
        lua_setfield(L, -2, en.name);
    }
    lua_setfield(L, -2, "enums");
    lua_newtable(L);
    for (const auto& k : constants) { lua_pushinteger(L, (lua_Integer)k.value); lua_setfield(L, -2, k.name); }
    lua_setfield(L, -2, "constants");
}

// The libraries the spec gives a script: Lua's base (less what reads files), string,
// table, math, utf8 and coroutine. No io, os, debug or package.
void open_libraries(lua_State* L) {
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},        {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
                                    {LUA_MATHLIBNAME, luaopen_math},  {LUA_UTF8LIBNAME, luaopen_utf8},  {LUA_COLIBNAME, luaopen_coroutine}};
    for (const luaL_Reg& l : libs) { luaL_requiref(L, l.name, l.func, 1); lua_pop(L, 1); }
    for (const char* gone : {"dofile", "loadfile", "collectgarbage"}) { lua_pushnil(L); lua_setglobal(L, gone); }
}

// The host's table for the prelude; everything a script sees is made from it.
void push_host(lua_State* L, Host& h) {
    lua_newtable(L);
    // kv: the engine's script libraries that name no path.
    lua_newtable(L);
    script_open_json(L);   lua_setfield(L, -2, "json");
    script_open_sha256(L); lua_setfield(L, -2, "sha256");
    script_open_random(L); lua_setfield(L, -2, "random");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "kv");
    lua_setfield(L, -2, "kv");
    // say: emits outside the schedule, delivered at the next boundary. hear: the unit's, pointed at each run's context.
    script_push_events(L, *h.e, nullptr);
    lua_setfield(L, -2, "say");
    script_push_events(L, *h.e, nullptr);
    lua_pushvalue(L, -1);
    h.ref_hear = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setfield(L, -2, "hear");
    struct Fn { const char* name; lua_CFunction fn; };
    for (const Fn& f : {Fn{"ktable", l_ktable}, Fn{"resource", l_resource}, Fn{"quit", l_quit}, Fn{"out", l_out}}) {
        lua_pushlightuserdata(L, &h);
        lua_pushcclosure(L, f.fn, 1);
        lua_setfield(L, -2, f.name);
    }
    lua_newtable(L);
    push_surface(L, k_player_surface_version, k_player_events, k_player_commands, k_player_enums, k_player_tables, k_player_constants);
    lua_setfield(L, -2, "player");
    push_surface(L, k_library_surface_version, k_library_events, k_library_commands, k_library_enums, k_library_tables, k_library_constants);
    lua_setfield(L, -2, "library");
    lua_setfield(L, -2, "surfaces");
    lua_newtable(L);
    for (u32 i = 0; i < g_script_config.nargs; ++i) { lua_pushstring(L, g_script_config.args[i]); lua_rawseti(L, -2, (lua_Integer)i + 1); }
    lua_setfield(L, -2, "args");
}

// Runs the function on the stack (with `nargs` arguments above it) under a traceback;
// false, with the message on the stack, when it raised.
bool call(lua_State* L, int nargs, int nresults) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, script_traceback);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    return rc == LUA_OK;
}

bool load_script(Host& h) {
    lua_State* L = h.L;
    // The prelude, given the host's table: what it returns is kept.
    if (luaL_loadbuffer(L, k_script_prelude, strlen(k_script_prelude), "=blackwing_script") != LUA_OK) { fail(h, "the host's own Lua", lua_tostring(L, -1)); return false; }
    push_host(L, h);
    if (!call(L, 1, 1)) { fail(h, "the host's own Lua", lua_tostring(L, -1)); return false; }
    lua_getfield(L, -1, "slice");
    h.ref_slice = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "shutdown");
    h.ref_shutdown = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "load");
    lua_remove(L, -2); // load

    // The user's script: text, run once; what it returns is its record.
    IoBytes file;
    const char* path = g_script_config.file;
    if (!io_read_now(path, &file)) {
        char why[600];
        format(why, sizeof why, "%s cannot be read", path);
        fail(h, "the script", why);
        return false;
    }
    char chunk[1100];
    format(chunk, sizeof chunk, "@%s", path);
    int rc = luaL_loadbufferx(L, (const char*)file.data, file.size, chunk, "t");
    file.free();
    if (rc != LUA_OK) { fail(h, "the script does not parse", lua_tostring(L, -1)); return false; }
    if (!call(L, 0, 1)) { fail(h, "the script raised as it loaded", lua_tostring(L, -1)); return false; }
    if (!call(L, 1, 0)) { fail(h, "the script's record", lua_tostring(L, -1)); return false; }
    return true;
}

// ---- the unit ----------------------------------------------------------------------------------

void script_slice(TickContext& ctx) {
    Host& h = ctx.state_as<Host>();
    if (!h.L || h.status.state != ScriptHostState::Running) return;
    lua_State* L = h.L;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref_hear);
    int hear = lua_gettop(L);
    script_events_set_context(L, hear, &ctx);
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref_slice);
    lua_pushnumber(L, time_now());
    bool ok = call(L, 1, 2);
    script_events_set_context(L, hear, nullptr);
    if (ok) {
        h.next_due = lua_tonumber(L, -2);
        h.status.emitted = (u64)lua_tointeger(L, -1);
        ++h.status.slices;
    } else {
        fail(h, "the script raised", lua_tostring(L, -1));
    }
    lua_settop(L, top);
    // A system waiting on the clock for less than a resting loop's heartbeat needs its tick.
    if (h.status.state == ScriptHostState::Running && h.next_due >= 0.0 && h.next_due < 0.3) engine_want_ticks(*ctx.engine);
}

bool script_startup(Engine& e, ModuleInstance& self) {
    Host* h = arena_push<Host>(self.arena);
    if (!h) return false;
    self.set_state(h);
    h->e = &e;
    g_script_exit = 0;
    if (!g_script_config.file || !g_script_config.file[0]) return true;
    h->L = kv_lua_newstate(mem_general());
    if (!h->L) { KV_ERROR("script", "no memory for the script's Lua state"); return false; }
    h->status.state = ScriptHostState::Running;
    open_libraries(h->L);
    load_script(*h); // a script that will not load has failed; the engine has not
    lua_settop(h->L, 0);
    engine_want_ticks(e);
    return true;
}

void script_shutdown(Engine&, ModuleInstance& self) {
    Host* h = self.state_as<Host>();
    if (!h || !h->L) return;
    if (h->status.state == ScriptHostState::Running || h->status.state == ScriptHostState::Done) {
        lua_rawgeti(h->L, LUA_REGISTRYINDEX, h->ref_shutdown);
        if (!call(h->L, 0, 0)) print_err("blackwing: the script's shutdown raised: %s\n", lua_tostring(h->L, -1));
    }
    lua_close(h->L);
    h->L = nullptr;
}

constinit const SystemDesc k_systems[] = {
    {.name = ".slice", .phase = Phase::PostSim, .fn = script_slice, .reads = k_reads.text, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

void script_host_configure(const ScriptHostConfig& config) { g_script_config = config; }

ScriptHostStatus script_host_status(Engine& e) {
    Host* h = host_state(e);
    return h ? h->status : ScriptHostStatus{};
}

int script_host_exit_code() { return g_script_exit; }

KV_MODULE_DEFINE(blackwing_script_module) = {
    .name     = "script",
    .systems  = k_systems,
    .startup  = script_startup,
    .shutdown = script_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
