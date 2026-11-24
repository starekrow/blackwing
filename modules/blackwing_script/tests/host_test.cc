// The script host over the player and the library themselves, playing into a capture
// device on the wall clock: each test is a Lua script in this directory, run as
// `blackwing run` would run it, to the code it ends with.
//
// The walk is the conformance test: it reaches every name of the two APIs from Lua, and
// this file's listeners (one on every event type of both schemas, by id, with no handler
// written for any) say that each command it emitted arrived natively.
#include "korvid/blackwing_glue/module.h"
#include "korvid/blackwing_library/module.h"
#include "korvid/blackwing_library/schema.h"
#include "korvid/blackwing_player/module.h"
#include "korvid/blackwing_player/schema.h"
#include "korvid/blackwing_script/host.h"
#include "korvid/blackwing_script/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "../../blackwing_library/tests/gen.h"
#include <string.h>

namespace {

using namespace kv;

constexpr u32 kTypes = 64;
constinit u32 g_delivered[kTypes] = {};  // events delivered, by where their type is in the two schemas
void on_any(void* user, u32, const void*) { ++*(u32*)user; }

struct Run {
    Engine e;
    char dir[900] = "";      // this file's directory
    char tmp[1024] = "";
    char data[1100] = "";
    char music[1100] = "";
    char script[1100] = "";
    char tone[1100] = "";
    ScriptHostStatus status;

    // Starts an engine with the player on its own capture device, a library on a fresh
    // directory, the glue and the host on `name`; runs it until the script ends the run or
    // `seconds` pass. True when it ended the run.
    bool go(TestCtx& c, const char* name, const char* const* args, u32 nargs, f64 seconds, bool ends_run = true) {
        const char* here = __FILE__;
        const char* slash = strrchr(here, '/');
        format(dir, sizeof dir, "%.*s", slash ? (int)(slash - here) : 0, here);
        format(script, sizeof script, "%s/%s", dir, name);
        format(tone, sizeof tone, "%s/../../hifi_codec/tests/data/tone_16_2ch_8000_long.flac", dir);
        PlayerConfig pc;
        pc.device = "capture";
        player_configure(pc);
        LibraryConfig lc;
        lc.data_dir = data;
        library_configure(lc);
        CliConfig cli; cli.no_stdin = true;
        cli_configure(cli);
        PlatformMounts mounts; mounts.none = true;
        platform_configure_mounts(mounts);
        ScriptHostConfig sc;
        sc.file = script;
        sc.args = args;
        sc.nargs = nargs;
        sc.ends_run = ends_run;
        script_host_configure(sc);
        static const ModuleDesc* const list[] = {&platform_module, &cli_module, &blackwing_player_module, &blackwing_library_module, &blackwing_glue_module,
                                                 &blackwing_script_module};
        KV_CHECK(c, engine_init(e, list));
        memset(g_delivered, 0, sizeof g_delivered);
        u32 k = 0;
        for (const EventTypeDesc& d : k_player_events) engine_listen(e, engine_event_type_id(e, d.name), on_any, &g_delivered[k++]);
        for (const EventTypeDesc& d : k_library_events) engine_listen(e, engine_event_type_id(e, d.name), on_any, &g_delivered[k++]);
        f64 until = time_now() + seconds;
        while (!e.quit_requested && time_now() < until) { step(); }
        status = script_host_status(e);
        return e.quit_requested;
    }
    void step() { platform_pump(e); engine_frame(e, e.clock.tick_dt); time_sleep(0.002); }
    void stop() {
        engine_shutdown(e);
        script_host_configure(ScriptHostConfig{});
        player_configure(PlayerConfig{});
    }
    bool dirs() {
        if (!io_make_temp_dir("bw_script", tmp, sizeof tmp)) return false;
        format(data, sizeof data, "%s/data", tmp);
        format(music, sizeof music, "%s/music", tmp);
        return true;
    }
};

// How many of a schema's event type have been delivered, by its name.
u32 delivered(const char* name) {
    u32 k = 0;
    for (const EventTypeDesc& d : k_player_events) { if (strcmp(d.name, name) == 0) return g_delivered[k]; ++k; }
    for (const EventTypeDesc& d : k_library_events) { if (strcmp(d.name, name) == 0) return g_delivered[k]; ++k; }
    return 0;
}

} // namespace

void test_script_host(kv::TestCtx& c) {
    using namespace kv;

    // The record: handlers and systems with their `reads` and `emits`, ctx, the rules a unit
    // is held to. The script ends the run with a code of its own.
    c.begin("script record");
    {
        Run r;
        KV_CHECK(c, r.dirs());
        char tone[1100];
        const char* here = __FILE__;
        const char* slash = strrchr(here, '/');
        format(tone, sizeof tone, "%.*s/../../hifi_codec/tests/data/tone_16_2ch_8000_long.flac", slash ? (int)(slash - here) : 0, here);
        const char* args[] = {tone};
        bool ended = r.go(c, "record.lua", args, 1, 15.0);
        if (r.status.state == ScriptHostState::Failed) KV_ERROR("test", "record.lua: %s", r.status.error);
        KV_CHECK(c, ended && r.status.state == ScriptHostState::Done && r.status.code == 7 && script_host_exit_code() == 7);
        KV_CHECK(c, r.status.slices > 10 && r.status.emitted == 3); // enqueue, play, and the handler's seek: the refused ones emitted nothing
        KV_CHECK(c, delivered("PlayerEnqueue") == 1 && delivered("PlayerPlay") == 1 && delivered("PlayerSeekBy") == 1 && delivered("PlayerStop") == 0);
        r.stop();
        io_remove_tree(r.tmp);
    }

    // A script that raises says so, with its file and line, and is stopped. The player is not.
    c.begin("script raises");
    {
        Run r;
        KV_CHECK(c, r.dirs());
        bool ended = r.go(c, "raises.lua", nullptr, 0, 5.0, false);
        KV_CHECK(c, !ended && r.status.state == ScriptHostState::Failed && r.status.code == 1 && script_host_exit_code() == 1);
        KV_CHECK(c, strstr(r.status.error, "raises.lua:11:") && strstr(r.status.error, "nothing") && r.status.slices == 2);
        if (!strstr(r.status.error, "raises.lua:11:")) KV_ERROR("test", "it said: %s", r.status.error);
        // The engine runs on, and the player does what it is told.
        u64 slices = r.status.slices;
        PlayerEnqueue q{};
        format(q.path, sizeof q.path, "%s", r.tone);
        r.e.events.emit(q);
        r.e.events.emit(PlayerPlay{});
        f64 until = time_now() + 4.0;
        while (time_now() < until && player_transport(r.e)->state != (u8)PlayerState::Playing) r.step();
        KV_CHECK(c, player_transport(r.e)->state == (u8)PlayerState::Playing && script_host_status(r.e).slices == slices);
        r.stop();
        io_remove_tree(r.tmp);
    }
    // One that does not parse, and one that returns no record: each said, neither run.
    c.begin("script does not load");
    {
        Run r;
        KV_CHECK(c, r.dirs());
        bool ended = r.go(c, "no such file.lua", nullptr, 0, 1.0);
        KV_CHECK(c, ended && r.status.state == ScriptHostState::Failed && strstr(r.status.error, "cannot be read") && r.status.slices == 0);
        r.stop();
        io_remove_tree(r.tmp);
    }

    // The conformance walk: every name of the two APIs, from Lua.
    c.begin("script walk");
    {
        Run r;
        KV_CHECK(c, r.dirs());
        io_make_dirs(r.music);
        kvtest::make_library(r.music, 24, 12);
        char tone[1100], missing[1100], n_events[16], n_tables[16], n_commands[16];
        const char* here = __FILE__;
        const char* slash = strrchr(here, '/');
        format(tone, sizeof tone, "%.*s/../../hifi_codec/tests/data/tone_16_2ch_8000_long.flac", slash ? (int)(slash - here) : 0, here);
        format(missing, sizeof missing, "%s/not there.flac", r.tmp);
        // What the schemas have, natively: the walk's surfaces must list as many.
        format(n_events, sizeof n_events, "%u", (u32)(KV_COUNTOF(k_player_events) + KV_COUNTOF(k_library_events)));
        format(n_tables, sizeof n_tables, "%u", (u32)(KV_COUNTOF(k_player_tables) + KV_COUNTOF(k_library_tables)));
        format(n_commands, sizeof n_commands, "%u", (u32)(KV_COUNTOF(k_player_commands) + KV_COUNTOF(k_library_commands)));
        const char* args[] = {tone, r.music, missing, n_events, n_tables, n_commands};
        bool ended = r.go(c, "walk.lua", args, 6, 60.0);
        if (r.status.state == ScriptHostState::Failed) KV_ERROR("test", "walk.lua: %s", r.status.error);
        KV_CHECK(c, ended && r.status.state == ScriptHostState::Done && r.status.code == 0);
        // Each command the script emitted by name arrived as the native event of that name.
        u32 arrived = 0;
        for (const char* name : k_player_commands) { if (delivered(name)) ++arrived; else KV_ERROR("test", "%s was not delivered", name); }
        for (const char* name : k_library_commands) { if (delivered(name)) ++arrived; else KV_ERROR("test", "%s was not delivered", name); }
        KV_CHECK(c, arrived == KV_COUNTOF(k_player_commands) + KV_COUNTOF(k_library_commands));
        // Every name in the schemas is a name the commands lists or a notification: none is in neither.
        for (const char* name : k_player_commands) { bool is = false; for (const EventTypeDesc& d : k_player_events) is = is || strcmp(d.name, name) == 0; KV_CHECK(c, is); }
        for (const char* name : k_library_commands) { bool is = false; for (const EventTypeDesc& d : k_library_events) is = is || strcmp(d.name, name) == 0; KV_CHECK(c, is); }
        r.stop();
        io_remove_tree(r.tmp);
    }

    // The scripted pass: a root added, a track found, played, a seek, a band, the transport
    // read, and an exit code.
    c.begin("script pass");
    {
        Run r;
        KV_CHECK(c, r.dirs());
        io_make_dirs(r.music);
        kvtest::make_library(r.music, 24, 12);
        char tone[1100];
        const char* here = __FILE__;
        const char* slash = strrchr(here, '/');
        format(tone, sizeof tone, "%.*s/../../hifi_codec/tests/data/tone_16_2ch_8000_long.flac", slash ? (int)(slash - here) : 0, here);
        const char* args[] = {r.music, tone};
        bool ended = r.go(c, "pass.lua", args, 2, 60.0);
        if (r.status.state == ScriptHostState::Failed) KV_ERROR("test", "pass.lua: %s", r.status.error);
        KV_CHECK(c, ended && r.status.state == ScriptHostState::Done && r.status.code == 0 && player_output(r.e)->underruns == 0);
        r.stop();
        io_remove_tree(r.tmp);
    }
}
