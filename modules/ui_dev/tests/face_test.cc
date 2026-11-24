// The face against a stand-in, with no window: Krome's view at a fixed size, its controls
// pressed by id or by a pointer the test moves, and neither the player nor the library in
// the engine. What a table says reaches the screen; a control reaches the stand-in as the
// player's command; still costs nothing; hidden, nothing is read or written; a panel that
// does not load says so where it would be, and the rest work.
#include "standin.h"
#include "korvid/asset/module.h"
#include "korvid/blackwing_player/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/krome/krome.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/module.h"
#include "korvid/ui_dev/module.h"
#include "korvid/ui_dev/ui_dev.h"
#include <stdlib.h>
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
// Passes of a loop paced as blackwing's window is: a tick each, and a frame only when one
// was asked for. What it cost is counted.
struct Cost { u64 drawn = 0, tick_asks = 0; };
Cost paced(Engine& e, u32 n) {
    Cost c;
    for (u32 i = 0; i < n; ++i) {
        platform_pump(e);
        engine_frame(e, 1.0 / 60.0);
        if (e.ticks_wanted) ++c.tick_asks;
        if (e.frame_wanted) { e.frame_wanted = false; engine_frame_cycle(e, 1.0 / 60.0); ++c.drawn; }
    }
    return c;
}

bool until_ready(Engine& e) {
    for (u32 i = 0; i < 600 && !ui_dev_ready(e) && !ui_dev_stats(e).failed; ++i) frames(e, 1);
    frames(e, 6); // the chain's controls are written in the first tick it is ready, and the screen built over them
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
// Whether some text on the screen has `part` in it; how many do.
u32 on_screen(Engine& e, const char* part) {
    const KromeTree* t = krome_tree(e, ui_dev_screen(e));
    u32 n = 0;
    for (u32 i = 0; t && i < t->nodes.len; ++i)
        if (t->nodes[i].type == KromeType::Text && !(t->nodes[i].flags & KROME_DEAD) && strstr(node_text(*t, i), part)) ++n; // a rebuilt region leaves its old nodes behind, dead
    return n;
}

// The pointer at a place within a node (by its id), with a button down or not: a frame of it.
bool point(Engine& e, const char* id, f32 fx, f32 fy, bool down, f32 wheel = 0.0f) {
    rect2 r{};
    if (!krome_find_rect(e, ui_dev_screen(e), id, &r)) { KV_ERROR("test", "there is no node '%s'", id); return false; }
    input_pointer_override(e, r.x + r.w * fx, r.y + r.h * fy, down ? 1 : 0, wheel);
    frames(e, 1);
    return true;
}
// A press on an edit, which gives it the keyboard, and the text typed into it, as much a
// tick as a tick of typing carries.
bool type_into(Engine& e, const char* id, const char* text) {
    if (!point(e, id, 0.5f, 0.5f, false) || !point(e, id, 0.5f, 0.5f, true) || !point(e, id, 0.5f, 0.5f, false)) return false;
    usize n = strlen(text);
    for (usize at = 0; at < n;) {
        char chunk[48];
        usize len = n - at < sizeof chunk - 1 ? n - at : sizeof chunk - 1;
        memcpy(chunk, text + at, len);
        chunk[len] = 0;
        input_text_override(e, chunk);
        frames(e, 1);
        at += len;
    }
    frames(e, 2);
    return true;
}

// Where dev-slider puts its value for the pointer at `fx` of a node's width.
f64 slider_at(Engine& e, const char* id, f32 fx) {
    rect2 r{};
    krome_find_rect(e, ui_dev_screen(e), id, &r);
    f64 k = ((f64)(r.w * fx) - 7.0) / ((f64)r.w - 14.0);
    return k < 0.0 ? 0.0 : k > 1.0 ? 1.0 : k;
}
bool near(f64 a, f64 b, f64 by) { return a - b <= by && b - a <= by; }

// The same face over the player itself, playing into a capture device on the wall clock.
const ModuleDesc* const k_real[] = {&platform_module, &platform_hid_module, &cli_module, &asset_module, &krome_module, &blackwing_player_module, &ui_dev_module};

// Frames, a few milliseconds apart, until a model key shows `want` (or has it in it), or
// `seconds` pass.
bool until_shows(Engine& e, const char* key, const char* want, f64 seconds = 4.0, bool part = false) {
    f64 until = time_now() + seconds;
    for (;;) {
        frames(e, 1);
        const char* got = krome_model_text(e, ui_dev_screen(e), key);
        if (part ? strstr(got, want) != nullptr : strcmp(got, want) == 0) return true;
        if (time_now() > until) { KV_ERROR("test", "%s shows \"%s\", waited for \"%s\"", key, got, want); return false; }
        time_sleep(0.004);
    }
}

const ModuleDesc* const k_list[] = {&platform_module, &platform_hid_module, &cli_module, &asset_module, &krome_module, &standin_player_module, &ui_dev_module};

// A chain of two stages for the stand-in: a gain with a level and a switch, an equalizer's
// first band, and two presets.
void make_chain(Engine& e) {
    standin_stage(e, 0, "gain");
    standin_stage(e, 1, "eq");
    standin_param(e, 0, 0, "gain_db", -100.0, 0.0, 0.5, "dB", 0.0);
    standin_param(e, 0, 1, "mute", 0.0, 1.0, 1.0, "", 0.0);
    standin_param(e, 1, 0, "b0.kind", 0.0, 2.0, 1.0, "", 1.0);
    standin_param(e, 1, 1, "b0.freq", 10.0, 24000.0, 0.0, "Hz", 31.25);
    standin_param(e, 1, 2, "b0.gain", -24.0, 24.0, 0.5, "dB", 0.0);
    standin_preset(e, 1, 0, "flat");
    standin_preset(e, 1, 1, "bass");
}

} // namespace

void test_face(kv::TestCtx& c) {
    using namespace kv;
    c.begin("face transport");
    {
        ui_dev_configure(UiDevConfig{});
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        make_chain(e);
        KV_CHECK(c, until_ready(e));
        StandinPlayer& p = *standin_player(e);
        Name scn = ui_dev_screen(e);
        KV_CHECK(c, ui_dev_stats(e).broken == 0);

        // A table reaches the screen.
        KV_CHECK(c, shows(e, "t_state", "stopped") && shows(e, "t_pos", "0:00") && shows(e, "t_row", "nothing queued"));
        standin_item(e, 1, "/music/Artist/Album/01 First.flac", PlayerItemState::Playing);
        standin_item(e, 2, "/music/Artist/Album/02 Second.flac");
        p.transport.state = (u8)PlayerState::Playing;
        p.transport.item = p.transport.place = 1;
        p.transport.place_pos = 1;
        p.transport.queued = p.transport.left = 2;
        p.transport.rate = 44100;
        p.transport.frames = 44100 * 245;
        p.transport.position = 44100 * 61;
        frames(e, 3);
        KV_CHECK(c, shows(e, "t_state", "playing") && shows(e, "t_pos", "1:01") && shows(e, "t_len", "4:05"));
        KV_CHECK(c, shows(e, "t_row", "1 of 2") && shows(e, "t_title", "01 First.flac") && shows(e, "t_seeking", ""));

        // Every transport command, each from its own control, once.
        const char* const buttons[][2] = {{"pause", "PlayerPause"}, {"play", "PlayerPlay"}, {"stop", "PlayerStop"}, {"next", "PlayerNext"}, {"previous", "PlayerPrevious"}};
        for (const auto& b : buttons) {
            standin_clear(e);
            KV_CHECK(c, krome_press(e, scn, b[0]));
            frames(e, 3);
            KV_CHECK(c, p.ncommands == 1 && standin_count(e, b[1]) == 1);
        }
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "back") && krome_press(e, scn, "on"));
        frames(e, 3);
        KV_CHECK(c, p.ncommands == 2 && p.commands[0].value == -10.0 && p.commands[1].value == 10.0 && standin_count(e, "PlayerSeekBy") == 2);

        // The bar: a press seeks to where it was pressed, a drag seeks as it goes, and while
        // it is held the bar is the pointer's, whatever the position does.
        standin_clear(e);
        KV_CHECK(c, point(e, "seek", 0.25f, 0.5f, false) && point(e, "seek", 0.25f, 0.5f, true));
        frames(e, 2);
        f64 k = slider_at(e, "seek", 0.25f);
        const StandinCommand* seek = standin_last(e, "PlayerSeek");
        KV_CHECK(c, seek && seek->a == 0 && near((f64)seek->b, k * 44100.0 * 245.0, 2.0) && standin_count(e, "PlayerSeek") == 1);
        p.transport.position += 44100 * 5; // the old audio goes on meanwhile
        KV_CHECK(c, point(e, "seek", 0.75f, 0.5f, true));
        frames(e, 2);
        f64 k2 = slider_at(e, "seek", 0.75f);
        seek = standin_last(e, "PlayerSeek");
        KV_CHECK(c, seek && near((f64)seek->b, k2 * 44100.0 * 245.0, 2.0) && standin_count(e, "PlayerSeek") == 2);
        char held[32];
        format(held, sizeof held, "%s", krome_model_text(e, scn, "t_bar"));
        KV_CHECK(c, near(atof(held), k2, 0.001));
        // Let go, with the seek on its way: the bar stays where it was sent, and says so.
        p.transport.seek_item = 1;
        p.transport.seek_frame = seek ? seek->b : 0;
        KV_CHECK(c, point(e, "seek", 0.75f, 0.5f, false));
        frames(e, 3);
        char going[48];
        format(going, sizeof going, "seeking to %llu:%02llu", (unsigned long long)(p.transport.seek_frame / 44100 / 60), (unsigned long long)(p.transport.seek_frame / 44100 % 60));
        KV_CHECK(c, near(atof(krome_model_text(e, scn, "t_bar")), k2, 0.01) && shows(e, "t_seeking", going));
        // Heard: the bar is the position's again.
        p.transport.seek_item = 0;
        p.transport.position = 44100 * 200;
        frames(e, 3);
        KV_CHECK(c, near(atof(krome_model_text(e, scn, "t_bar")), 200.0 / 245.0, 0.01) && shows(e, "t_seeking", "") && shows(e, "t_pos", "3:20"));
        input_pointer_override(e, 5, 795, 0);
        frames(e, 2);

        // Still: ten seconds with nothing changing, under a loop that draws only when asked.
        // No frame, no layout, no paint, no model write, and no tick asked for.
        paced(e, 30);
        KromeStats k0 = krome_stats(e);
        UiDevStats u0 = ui_dev_stats(e);
        Cost still = paced(e, 600);
        KromeStats k1 = krome_stats(e);
        UiDevStats u1 = ui_dev_stats(e);
        KV_CHECK(c, still.drawn == 0 && still.tick_asks == 0);
        KV_CHECK(c, k1.layouts == k0.layouts && k1.paints == k0.paints && k1.submits == k0.submits && u1.writes == u0.writes);

        // Playing, the position moves every tick and is written when it can be seen to: the
        // readout once a second, the bar once a pixel.
        p.transport.position = 44100 * 10;
        paced(e, 4);
        k1 = krome_stats(e);
        u1 = ui_dev_stats(e);
        rect2 bar{};
        krome_find_rect(e, scn, "seek", &bar);
        Cost moving{};
        for (u32 i = 0; i < 600; ++i) { p.transport.position += 735; Cost one = paced(e, 1); moving.drawn += one.drawn; }
        UiDevStats u2 = ui_dev_stats(e);
        KromeStats k2s = krome_stats(e);
        u64 pixels = (u64)((f64)bar.w * 10.0 / 245.0) + 1;
        KV_CHECK(c, u2.writes - u1.writes >= 10 && u2.writes - u1.writes <= 10 + pixels + 1 && shows(e, "t_pos", "0:20"));
        KV_CHECK(c, k2s.paints - k1.paints <= 10 + pixels + 1 && moving.drawn <= 2 * (10 + pixels + 1));
        KV_INFO("test", "face: ten seconds of position, a bar %.0f px wide: %llu writes, %llu layouts, %llu paints, %llu frames", (f64)bar.w,
                (unsigned long long)(u2.writes - u1.writes), (unsigned long long)(k2s.layouts - k1.layouts), (unsigned long long)(k2s.paints - k1.paints),
                (unsigned long long)moving.drawn);

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
        KV_CHECK(c, shows(e, "t_state", "paused") && shows(e, "t_pos", "0:30"));

        engine_shutdown(e);
    }

    c.begin("face queue");
    {
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        make_chain(e);
        StandinPlayer& p = *standin_player(e);
        // A thousand rows, in the table in an order that is not the list's.
        char path[128];
        for (u32 i = 1000; i >= 1; --i) {
            format(path, sizeof path, "/music/album/track %u.flac", i);
            PlayerItem* it = standin_item(e, i, path, i == 3 ? PlayerItemState::Playing : i == 5 ? PlayerItemState::Failed : PlayerItemState::Idle);
            if (i == 5) format(it->reason, sizeof it->reason, "not a FLAC file");
            if (i == 3) { it->rate = 96000; it->channels = 2; it->bits = 24; it->size = 30u << 20; it->bytes_read = 12u << 20; }
        }
        p.transport.queued = 1000;
        p.transport.left = 998;
        p.transport.place = p.transport.item = 3;
        p.transport.place_pos = 3;
        KV_CHECK(c, until_ready(e));
        Name scn = ui_dev_screen(e);
        frames(e, 6);
        // The list is as long as the queue, and holds the rows in view: each with its state
        // and its reason where it shows.
        KV_CHECK(c, shows(e, "q_summary", "1000 rows, 998 left") && shows(e, "q_total", "1000"));
        KV_CHECK(c, on_screen(e, "track 1.flac") == 1 && on_screen(e, "track 3.flac") >= 1 && on_screen(e, "track 900.flac") == 0);
        KV_CHECK(c, on_screen(e, "not a FLAC file") == 1 && on_screen(e, "failed") == 1 && on_screen(e, "96000 Hz, 2 ch, 24 bit") == 1 && on_screen(e, "12.0/30.0 MB") == 1);
        KV_CHECK(c, on_screen(e, "done") >= 2 && on_screen(e, "playing") >= 1);
        UiDevStats u = ui_dev_stats(e);

        // Scrolled: the rows for where it came to rest, a tick or two after it says where.
        KV_CHECK(c, point(e, "q_list", 0.5f, 0.5f, false, -1.0f));
        for (u32 i = 0; i < 40; ++i) point(e, "q_list", 0.5f, 0.5f, false, -6.0f);
        frames(e, 6);
        KV_CHECK(c, on_screen(e, "track 1.flac") == 0 && on_screen(e, "...") == 0);
        KV_CHECK(c, ui_dev_stats(e).writes > u.writes);
        input_pointer_override(e, 5, 795, 0);
        frames(e, 2);
        // A row changing under the list changes in place.
        u32 shown_first = (u32)atof(krome_model_text(e, scn, "q_first"));
        PlayerItem* seen = standin_find(e, shown_first + 20);
        KV_CHECK(c, seen != nullptr);
        if (seen) { seen->state = (u8)PlayerItemState::Reading; }
        frames(e, 3);
        KV_CHECK(c, on_screen(e, "reading") == 1);

        // The edits, each from a row's own buttons.
        char id[32];
        u32 row = shown_first + 20;
        standin_clear(e);
        format(id, sizeof id, "qj_%u", row);
        KV_CHECK(c, krome_press(e, scn, id));
        frames(e, 3);
        const StandinCommand* cmd = standin_last(e, "PlayerJump");
        KV_CHECK(c, cmd && cmd->a == row && p.ncommands == 1);
        format(id, sizeof id, "qx_%u", row);
        KV_CHECK(c, krome_press(e, scn, id));
        frames(e, 3);
        cmd = standin_last(e, "PlayerQueueEdit");
        KV_CHECK(c, cmd && cmd->a == row && cmd->value == (f64)PlayerEditOp::Remove);
        format(id, sizeof id, "qu_%u", row);
        KV_CHECK(c, krome_press(e, scn, id));
        frames(e, 3);
        cmd = standin_last(e, "PlayerQueueEdit");
        KV_CHECK(c, cmd && cmd->a == row && cmd->b == row - 1 && cmd->value == (f64)PlayerEditOp::Move); // before the row above
        format(id, sizeof id, "qd_%u", row);
        KV_CHECK(c, krome_press(e, scn, id));
        frames(e, 3);
        cmd = standin_last(e, "PlayerQueueEdit");
        KV_CHECK(c, cmd && cmd->a == row && cmd->b == row + 2 && cmd->value == (f64)PlayerEditOp::Move); // before the row two below
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "q_clear"));
        frames(e, 3);
        cmd = standin_last(e, "PlayerQueueEdit");
        KV_CHECK(c, cmd && cmd->value == (f64)PlayerEditOp::Clear && p.ncommands == 1);

        // A path typed in is added, and the edit emptied.
        standin_clear(e);
        krome_set(e, scn, "q_path", "/music/new/one.flac");
        frames(e, 2);
        KV_CHECK(c, krome_press(e, scn, "q_add"));
        frames(e, 3);
        cmd = standin_last(e, "PlayerEnqueue");
        KV_CHECK(c, cmd && strcmp(cmd->text, "/music/new/one.flac") == 0 && cmd->a == 0 && shows(e, "q_path", ""));
        // Nothing typed: nothing added.
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "q_add"));
        frames(e, 3);
        KV_CHECK(c, p.ncommands == 0);

        // The queue emptied under it: the list is empty too.
        for (u32 i = 1; i <= 1000; ++i) standin_remove(e, i);
        p.transport.queued = p.transport.left = 0;
        p.transport.place = p.transport.item = 0;
        frames(e, 6);
        KV_CHECK(c, shows(e, "q_summary", "0 rows, 0 left") && on_screen(e, "track ") == 0);

        // Still, with every panel up.
        paced(e, 30);
        KromeStats k0 = krome_stats(e);
        UiDevStats u0 = ui_dev_stats(e);
        Cost still = paced(e, 600);
        KV_CHECK(c, still.drawn == 0 && still.tick_asks == 0 && krome_stats(e).layouts == k0.layouts && krome_stats(e).paints == k0.paints && ui_dev_stats(e).writes == u0.writes);
        engine_shutdown(e);
    }

    c.begin("face output and chain");
    {
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        make_chain(e);
        StandinPlayer& p = *standin_player(e);
        KV_CHECK(c, until_ready(e));
        Name scn = ui_dev_screen(e);

        // The device, and exact or why not, from the tables alone.
        KV_CHECK(c, shows(e, "o_exact", "no device open") && shows(e, "o_why", "") && shows(e, "o_device", "none"));
        PlayerItem* it = standin_item(e, 1, "/m/a.flac", PlayerItemState::Playing);
        it->bits = 24; it->rate = 96000; it->channels = 2;
        p.transport.item = p.transport.place = 1;
        p.transport.queued = 1;
        p.output.open = 1; p.output.exact = 1; p.output.rate = 96000; p.output.channels = 2; p.output.bits = 24;
        p.output.lead_frames = 96000 * 5; p.output.bytes_held = 3u << 20;
        frames(e, 3);
        KV_CHECK(c, shows(e, "o_exact", "exact") && shows(e, "o_why", "") && shows(e, "o_device", "96000 Hz, 2 ch, 24-bit, unshared") && shows(e, "o_lead", "5.0 s"));
        KV_CHECK(c, shows(e, "o_held", "3.0 MB") && shows(e, "o_underruns", "0") && shows(e, "o_recut", "none yet"));
        p.output.exact = 0; p.output.shared = 1;
        frames(e, 3);
        KV_CHECK(c, shows(e, "o_exact", "not exact") && shows(e, "o_why", "the device is shared: it mixes, or converts on the way"));
        p.output.shared = 0; p.output.joining = 1;
        KTable* chain = engine_ktable(e, "player.chain");
        chain->rows_of<PlayerStage>()[1].flat = 0;
        frames(e, 3);
        KV_CHECK(c, shows(e, "o_why", "the eq stage is not flat; a join is under way") && shows(e, "c_summary", "2 stages, 1 not flat") && shows(e, "c_flat_1", "not flat"));
        p.output.joining = 0; p.output.bits = 16;
        chain->rows_of<PlayerStage>()[1].flat = 1;
        p.output.underruns = 2; p.output.recuts = 3; p.output.recut_seconds = 0.012; p.output.recut_render_seconds = 0.004; p.output.jump_seconds = 0.03;
        frames(e, 3);
        KV_CHECK(c, shows(e, "o_why", "the device's word is narrower: 24 bits in the file, 16 in the device's") && shows(e, "o_underruns", "2"));
        KV_CHECK(c, shows(e, "o_recut", "12.0 ms to the ear, 4.0 ms to render; 3 so far") && shows(e, "o_jump", "30.0 ms") && shows(e, "c_summary", "2 stages, all flat"));

        // The chain's controls came from the parameters' own descriptions: a slider each,
        // a switch for the one that is 0 or 1, a button a preset, and the values heard.
        rect2 r{};
        KV_CHECK(c, krome_find_rect(e, scn, "cs_0_0", &r) && krome_find_rect(e, scn, "cs_1_2", &r) && krome_find_rect(e, scn, "ct_0_1", &r) && krome_find_rect(e, scn, "cp_1_1", &r));
        KV_CHECK(c, !krome_find_rect(e, scn, "cs_0_1", &r)); // the switch has no slider
        KV_CHECK(c, shows(e, "c_v_0_0", "0.0 dB") && shows(e, "c_v_0_1", "off") && shows(e, "c_v_1_0", "1") && shows(e, "c_v_1_1", "31.2 Hz") && shows(e, "c_v_1_2", "0.0 dB"));
        KV_CHECK(c, near(atof(krome_model_text(e, scn, "c_k_0_0")), 1.0, 0.001) && near(atof(krome_model_text(e, scn, "c_k_1_2")), 0.5, 0.001));
        KV_CHECK(c, on_screen(e, "gain_db") == 1 && on_screen(e, "b0.freq") == 1 && on_screen(e, "bass") == 1);

        // A slider dragged: a command each time it moves, in its range and on its step; the
        // knob stays the pointer's, and the readout goes on saying what is heard.
        standin_clear(e);
        KV_CHECK(c, point(e, "cs_0_0", 0.5f, 0.5f, false) && point(e, "cs_0_0", 0.5f, 0.5f, true));
        frames(e, 2);
        f64 k = slider_at(e, "cs_0_0", 0.5f);
        f64 want = -100.0 + (f64)(i64)(k * 100.0 / 0.5 + 0.5) * 0.5;
        const StandinCommand* set = standin_last(e, "PlayerSetParam");
        KV_CHECK(c, set && set->a == 0 && set->b == 0 && set->value == want && standin_count(e, "PlayerSetParam") == 1);
        KV_CHECK(c, shows(e, "c_v_0_0", "0.0 dB")); // not heard yet
        KTable* params = engine_ktable(e, "player.params");
        PlayerParam* gain = nullptr;
        for (u32 i = 0; i < params->rows_of<PlayerParam>().len; ++i)
            if (params->is_alive(i) && params->rows_of<PlayerParam>()[i].stage == 0 && params->rows_of<PlayerParam>()[i].index == 0) gain = &params->rows_of<PlayerParam>()[i];
        KV_CHECK(c, gain != nullptr);
        if (gain) gain->value = -80.0; // heard, late and elsewhere
        frames(e, 3);
        KV_CHECK(c, shows(e, "c_v_0_0", "-80.0 dB") && near(atof(krome_model_text(e, scn, "c_k_0_0")), k, 0.001)); // the number is heard, the knob is held
        KV_CHECK(c, point(e, "cs_0_0", 0.25f, 0.5f, true));
        frames(e, 2);
        KV_CHECK(c, standin_count(e, "PlayerSetParam") == 2);
        KV_CHECK(c, point(e, "cs_0_0", 0.25f, 0.5f, false));
        frames(e, 3);
        KV_CHECK(c, near(atof(krome_model_text(e, scn, "c_k_0_0")), 0.2, 0.001)); // let go: the knob is where the value heard is
        input_pointer_override(e, 5, 795, 0);
        frames(e, 2);

        // The switch, and a preset.
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "ct_0_1"));
        frames(e, 3);
        set = standin_last(e, "PlayerSetParam");
        KV_CHECK(c, set && set->a == 0 && set->b == 1 && set->value == 1.0 && p.ncommands == 1);
        KV_CHECK(c, krome_press(e, scn, "cp_1_1"));
        frames(e, 3);
        set = standin_last(e, "PlayerApplyPreset");
        KV_CHECK(c, set && set->a == 1 && set->b == 1);

        // A parameter the face has never seen, added while it runs: it appears, with a slider.
        standin_param(e, 1, 3, "b0.q", 0.1, 10.0, 0.01, "", 1.41);
        frames(e, 8);
        KV_CHECK(c, krome_find_rect(e, scn, "cs_1_3", &r) && shows(e, "c_v_1_3", "1.41") && on_screen(e, "b0.q") == 1);
        KV_CHECK(c, shows(e, "c_v_0_0", "-80.0 dB") && shows(e, "o_underruns", "2")); // and the rest is as it was

        engine_shutdown(e);
    }

    c.begin("face on the player");
    {
        // No stand-in: the player, a capture device of its own on the wall clock, and the
        // face's controls. The test emits no command and reads only what the screen shows.
        char path[1024];
        const char* here = __FILE__;
        const char* slash = strrchr(here, '/');
        format(path, sizeof path, "%.*s/../../hifi_codec/tests/data/tone_16_2ch_8000_long.flac", slash ? (int)(slash - here) : 0, here);
        PlayerConfig pc;
        pc.device = "capture";
        player_configure(pc);
        Engine e;
        KV_CHECK(c, engine_init(e, k_real));
        krome_viewport(e, 1280, 800, 1.0f);
        KV_CHECK(c, until_ready(e));
        Name scn = ui_dev_screen(e);
        rect2 r{};
        // The chain's controls are the player's own parameters: forty-two, and seven presets.
        KV_CHECK(c, krome_find_rect(e, scn, "cs_0_0", &r) && krome_find_rect(e, scn, "ct_0_1", &r) && krome_find_rect(e, scn, "cs_1_38", &r) && krome_find_rect(e, scn, "cp_1_6", &r));
        KV_CHECK(c, shows(e, "c_v_0_0", "0.0 dB") && shows(e, "c_summary", "2 stages, all flat") && shows(e, "t_state", "stopped"));

        // A path typed into the edit, a key at a time as a keyboard would, and Enter: added.
        KV_CHECK(c, type_into(e, "q_path", path) && shows(e, "q_path", path));
        input_key_override(e, Key::Enter, true);
        frames(e, 1);
        input_key_override(e, Key::Enter, false);
        frames(e, 2);
        KV_CHECK(c, until_shows(e, "q_summary", "1 rows, 1 left") && shows(e, "q_path", ""));
        KV_CHECK(c, krome_press(e, scn, "play"));
        KV_CHECK(c, until_shows(e, "t_state", "playing") && shows(e, "t_title", "tone_16_2ch_8000_long.flac") && shows(e, "t_len", "0:04") && shows(e, "t_row", "1 of 1"));
        KV_CHECK(c, until_shows(e, "o_exact", "exact") && shows(e, "o_device", "8000 Hz, 2 ch, 16-bit, unshared") && shows(e, "o_why", ""));
        KV_CHECK(c, on_screen(e, "8000 Hz, 2 ch, 16 bit") == 1);

        // The volume, dragged: the readout follows what is heard, and exact says why not.
        KV_CHECK(c, point(e, "cs_0_0", 0.5f, 0.5f, false) && point(e, "cs_0_0", 0.5f, 0.5f, true));
        f64 k = slider_at(e, "cs_0_0", 0.5f);
        char heard[32];
        format(heard, sizeof heard, "%.1f dB", -100.0 + (f64)(i64)(k * 200.0 + 0.5) * 0.5);
        KV_CHECK(c, until_shows(e, "c_v_0_0", heard) && until_shows(e, "o_exact", "not exact") && until_shows(e, "o_why", "the gain stage is not flat", 2.0, true));
        KV_CHECK(c, point(e, "cs_0_0", 0.5f, 0.5f, false));
        input_pointer_override(e, 5, 795, 0);
        frames(e, 2);
        // A preset: the equalizer's bands as heard, and its stage said not flat.
        KV_CHECK(c, krome_press(e, scn, "cp_1_1"));
        KV_CHECK(c, until_shows(e, "c_v_1_2", "6.0 dB") && until_shows(e, "c_flat_1", "not flat") && until_shows(e, "o_why", "the eq stage is not flat", 2.0, true));
        // Mute, by its switch.
        KV_CHECK(c, krome_press(e, scn, "ct_0_1"));
        KV_CHECK(c, until_shows(e, "c_v_0_1", "on"));

        // A press on the bar seeks there; pause holds; play goes on to the end.
        KV_CHECK(c, point(e, "seek", 0.6f, 0.5f, false) && point(e, "seek", 0.6f, 0.5f, true) && point(e, "seek", 0.6f, 0.5f, false));
        input_pointer_override(e, 5, 795, 0);
        KV_CHECK(c, until_shows(e, "t_pos", "0:02") && until_shows(e, "t_seeking", ""));
        KV_CHECK(c, krome_press(e, scn, "pause"));
        KV_CHECK(c, until_shows(e, "t_state", "paused"));
        KV_CHECK(c, krome_press(e, scn, "play"));
        KV_CHECK(c, until_shows(e, "t_state", "stopped", 6.0) && until_shows(e, "q_summary", "1 rows, 0 left") && shows(e, "o_underruns", "0"));
        KV_CHECK(c, on_screen(e, "done") == 1);
        // Removed by its row's button: the queue is empty.
        KV_CHECK(c, krome_press(e, scn, "qx_1"));
        KV_CHECK(c, until_shows(e, "q_summary", "0 rows, 0 left"));
        engine_shutdown(e);
        player_configure(PlayerConfig{});
    }

    c.begin("face broken");
    {
        // The Queue panel's file is not markup: its place says so, and the others work.
        UiDevConfig cfg;
        cfg.open = false;
        ui_dev_configure(cfg);
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        make_chain(e);
        static const char k_junk[] = "this is not a kimb";
        krome_provide(e, "ui_dev/queue.kimb", k_junk, sizeof k_junk - 1);
        ui_dev_open(e);
        KV_CHECK(c, until_ready(e));
        UiDevStats u = ui_dev_stats(e);
        KV_CHECK(c, u.broken == 1 && !u.failed);
        KV_CHECK(c, on_screen(e, "ui_dev/queue.kimb") == 1 && on_screen(e, "queue: this panel did not load") == 1);
        // The transport beside it works, and the stand-in's player is untouched by any of it.
        KV_CHECK(c, standin_player(e)->ncommands == 0);
        standin_player(e)->transport.state = (u8)PlayerState::Paused;
        frames(e, 3);
        KV_CHECK(c, shows(e, "t_state", "paused") && krome_press(e, ui_dev_screen(e), "play"));
        frames(e, 3);
        KV_CHECK(c, standin_count(e, "PlayerPlay") == 1);
        engine_shutdown(e);
        ui_dev_configure(UiDevConfig{});
    }
}
