// The face against a stand-in, with no window: Krome's view at a fixed size, its controls
// pressed by id, and neither the player nor the library in the engine. What a table says
// reaches the screen; a press reaches the stand-in as the player's command; still costs
// nothing; hidden, nothing is read or written; a panel that does not load says so where
// it would be, and the rest work.
#include "standin.h"
#include "korvid/asset/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/krome/krome.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/module.h"
#include "korvid/ui_dev/module.h"
#include "korvid/ui_dev/ui_dev.h"
#include <string.h>

namespace {

using namespace kv;

// A pass of a loop that draws: pump, the ticks owed, the Frame cycle.
void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}
// A pass that does not: the ticks, and no Frame cycle. What a hidden window gets.
void ticks(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); }
}

bool until_ready(Engine& e) {
    for (u32 i = 0; i < 600 && !ui_dev_ready(e) && !ui_dev_stats(e).failed; ++i) frames(e, 1);
    return ui_dev_ready(e);
}

bool shows(Engine& e, const char* key, const char* want) {
    const char* got = krome_model_text(e, ui_dev_screen(e), key);
    if (strcmp(got, want) == 0) return true;
    KV_ERROR("test", "%s shows \"%s\", wanted \"%s\"", key, got, want);
    return false;
}

// A text node's content as it is drawn.
const char* node_text(const KromeTree& t, u32 node) {
    u32 at = t.nodes[node].shown != KROME_NONE ? t.nodes[node].shown : t.nodes[node].text;
    return at == KROME_NONE ? "" : t.strings.data + at;
}

const ModuleDesc* const k_list[] = {&platform_module, &platform_hid_module, &cli_module, &asset_module, &krome_module, &standin_player_module, &ui_dev_module};

} // namespace

void test_face(kv::TestCtx& c) {
    using namespace kv;
    c.begin("face");
    {
        ui_dev_configure(UiDevConfig{});
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        KV_CHECK(c, until_ready(e));
        StandinPlayer& p = *standin_player(e);
        Name scn = ui_dev_screen(e);

        // A table reaches the screen.
        frames(e, 2);
        KV_CHECK(c, shows(e, "t_state", "stopped") && shows(e, "t_pos", "0:00"));
        p.transport.state = (u8)PlayerState::Playing;
        p.transport.item = 1;
        p.transport.rate = 44100;
        p.transport.frames = 44100 * 245;
        p.transport.position = 44100 * 61;
        frames(e, 2);
        KV_CHECK(c, shows(e, "t_state", "playing") && shows(e, "t_pos", "1:01") && shows(e, "t_len", "4:05"));

        // A press reaches the player, as its command, once.
        KV_CHECK(c, krome_press(e, scn, "pause"));
        frames(e, 3);
        KV_CHECK(c, standin_count(e, "PlayerPause") == 1 && standin_count(e, "PlayerPlay") == 0 && ui_dev_stats(e).commands == 1);
        KV_CHECK(c, krome_press(e, scn, "play"));
        frames(e, 3);
        KV_CHECK(c, standin_count(e, "PlayerPlay") == 1 && standin_count(e, "PlayerPause") == 1);

        // Still: ten seconds with nothing changing. No layout, no paint, no model write, and
        // the face asks for no tick and no frame.
        frames(e, 10);
        KromeStats k0 = krome_stats(e);
        UiDevStats u0 = ui_dev_stats(e);
        bool asked = false;
        for (u32 i = 0; i < 600; ++i) {
            e.frame_wanted = false;
            frames(e, 1);
            asked = asked || e.frame_wanted || e.ticks_wanted;
        }
        KromeStats k1 = krome_stats(e);
        UiDevStats u1 = ui_dev_stats(e);
        KV_CHECK(c, k1.layouts == k0.layouts && k1.paints == k0.paints && k1.submits == k0.submits);
        KV_CHECK(c, u1.writes == u0.writes && !asked);

        // Playing, the position moves every tick and is written when its readout changes:
        // ten seconds of it are ten writes, not six hundred.
        for (u32 i = 0; i < 600; ++i) { p.transport.position += 735; frames(e, 1); }
        UiDevStats u2 = ui_dev_stats(e);
        KromeStats k2 = krome_stats(e);
        KV_CHECK(c, u2.writes - u1.writes == 10 && shows(e, "t_pos", "1:11"));
        KV_CHECK(c, k2.paints - k1.paints <= 11);
        KV_INFO("test", "face: ten seconds of position: %llu writes, %llu layouts, %llu paints", (unsigned long long)(u2.writes - u1.writes),
                (unsigned long long)(k2.layouts - k1.layouts), (unsigned long long)(k2.paints - k1.paints));

        // Hidden: the tables move and the face does not look. Nothing written, no frame asked.
        e.events.emit(WindowVisibility{false, WindowHidden::Minimized});
        ticks(e, 2);
        UiDevStats h0 = ui_dev_stats(e);
        KromeStats kh0 = krome_stats(e);
        e.frame_wanted = false;
        p.transport.state = (u8)PlayerState::Paused;
        for (u32 i = 0; i < 600; ++i) { p.transport.position += 735; ticks(e, 1); }
        UiDevStats h1 = ui_dev_stats(e);
        KromeStats kh1 = krome_stats(e);
        KV_CHECK(c, h1.writes == h0.writes && h1.syncs == h0.syncs && h1.asleep - h0.asleep == 600 && !e.frame_wanted);
        KV_CHECK(c, kh1.layouts == kh0.layouts && kh1.paints == kh0.paints);
        KV_CHECK(c, shows(e, "t_state", "playing"));
        // Seen again: what it missed, at once.
        e.events.emit(WindowVisibility{true, WindowHidden::None});
        frames(e, 3);
        KV_CHECK(c, shows(e, "t_state", "paused") && shows(e, "t_pos", "1:21"));

        engine_shutdown(e);
    }

    c.begin("face broken");
    {
        // The Transport panel's file is not markup: its place says so, and the screen is up.
        UiDevConfig cfg;
        cfg.open = false;
        ui_dev_configure(cfg);
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        static const char k_junk[] = "this is not a kimb";
        krome_provide(e, "ui_dev/transport.kimb", k_junk, sizeof k_junk - 1);
        ui_dev_open(e);
        KV_CHECK(c, until_ready(e));
        UiDevStats u = ui_dev_stats(e);
        KV_CHECK(c, u.broken == 1 && !u.failed);
        const KromeTree* t = krome_tree(e, ui_dev_screen(e));
        bool said = false;
        for (u32 i = 0; t && i < t->nodes.len; ++i)
            if (t->nodes[i].type == KromeType::Text && strstr(node_text(*t, i), "ui_dev/transport.kimb")) said = true;
        KV_CHECK(c, said);
        // The stand-in's player is untouched by it, and ticks go on.
        frames(e, 30);
        KV_CHECK(c, standin_player(e)->ncommands == 0);
        engine_shutdown(e);
        ui_dev_configure(UiDevConfig{});
    }
}
