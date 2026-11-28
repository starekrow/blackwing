// The face against a stand-in, with no window: Krome's view at a fixed size, its controls
// pressed by id or by a pointer the test moves, and neither the player nor the library in
// the engine. What a table says reaches the screen; a control reaches the stand-in as the
// player's command; still costs nothing; hidden, nothing is read or written; a panel that
// does not load says so where it would be, and the rest work.
#include "standin.h"
#include "../../blackwing_library/tests/gen.h"
#include "korvid/asset/module.h"
#include "korvid/blackwing_glue/module.h"
#include "korvid/blackwing_library/module.h"
#include "korvid/blackwing_player/module.h"
#include "korvid/blackwing_session/module.h"
#include "korvid/blackwing_media/module.h"
#include "korvid/blackwing_session/session.h"
#include "korvid/blackwing_library/schema.h"
#include "korvid/blackwing_player/schema.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/krome/krome.h"
#include "korvid/platform/io.h"
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

constexpr u32 kWindow = 0xD0E00001, kTitles = 0xD0E00002, kCollect = 0xD0E00011, kCollectPlay = 0xD0E00012; // the face's ids at the library

// How many of the player's commands have arrived (the library's stand-in writes its own
// into the same list).
u32 player_commands(Engine& e) {
    StandinPlayer* s = standin_player(e);
    u32 n = 0;
    for (u32 i = 0; s && i < s->ncommands; ++i) n += strncmp(s->commands[i].type, "Player", 6) == 0;
    return n;
}

// The same face over the player itself, playing into a capture device on the wall clock.
const ModuleDesc* const k_real[] = {&platform_module, &platform_hid_module, &cli_module, &asset_module, &krome_module, &blackwing_player_module,
                                    &blackwing_library_module, &blackwing_glue_module, &blackwing_session_module, &blackwing_media_module, &ui_dev_module};

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

// The library's answer to the face's window, as the stand-in's test gives it: rows
// [first, first + count) of `total`, each a track called "song <row>".
void answer_tracks(Engine& e, u32 first, u32 count, u32 total) {
    standin_rows_clear(e, kWindow);
    char name[64], title[64];
    for (u32 i = 0; i < count && first + i < total; ++i) {
        format(name, sizeof name, "%05u.flac", first + i);
        format(title, sizeof title, "song %u", first + i);
        standin_row(e, kWindow, first + i, LibraryRowKind::Track, 1000 + first + i, name, title, "An Artist", "An Album");
    }
    e.events.emit(LibraryResult{kWindow, total, first, count, 9, 0.002});
}
// The window as the face last asked for it; null when it has not.
const StandinCommand* asked_query(Engine& e) { return standin_last(e, "LibraryQuery"); }
LibraryView asked_view(const StandinCommand* q) { return (LibraryView)(u8)q->value; }
u64 asked_parent(const StandinCommand* q) { return q->d & 0x00FFFFFFFFFFFFFFull; }
LibrarySort asked_sort(const StandinCommand* q) { return (LibrarySort)(u8)(q->d >> 56); }

const ModuleDesc* const k_list[] = {&platform_module, &platform_hid_module, &cli_module, &asset_module, &krome_module, &standin_player_module,
                                    &standin_library_module, &blackwing_session_module, &blackwing_media_module, &ui_dev_module}; // media off: it says so

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
            KV_CHECK(c, player_commands(e) == 1 && standin_count(e, b[1]) == 1);
        }
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "back") && krome_press(e, scn, "on"));
        frames(e, 3);
        KV_CHECK(c, player_commands(e) == 2 && p.commands[0].value == -10.0 && p.commands[1].value == 10.0 && standin_count(e, "PlayerSeekBy") == 2);

        // The keyboard. The transport's shortcuts, from anywhere on the screen: Space is play
        // or pause, whichever it is not; Ctrl with the arrows seeks and skips.
        auto key = [&](Key k, bool ctrl = false) {
            if (ctrl) { input_key_override(e, Key::LCtrl, true); frames(e, 1); }
            input_key_override(e, k, true);
            frames(e, 1);
            input_key_override(e, k, false);
            frames(e, 1);
            if (ctrl) { input_key_override(e, Key::LCtrl, false); frames(e, 1); }
            frames(e, 3);
        };
        standin_clear(e);
        key(Key::Space);
        KV_CHECK(c, player_commands(e) == 1 && standin_count(e, "PlayerPause") == 1); // it is playing: Space pauses
        p.transport.state = (u8)PlayerState::Paused;
        frames(e, 2);
        key(Key::Space);
        KV_CHECK(c, player_commands(e) == 2 && standin_count(e, "PlayerPlay") == 1);
        p.transport.state = (u8)PlayerState::Playing;
        frames(e, 2);
        standin_clear(e);
        key(Key::Right, true); key(Key::Left, true); key(Key::Down, true); key(Key::Up, true); key(Key::S, true);
        KV_CHECK(c, standin_count(e, "PlayerSeekBy") == 2 && standin_count(e, "PlayerNext") == 1 && standin_count(e, "PlayerPrevious") == 1 && standin_count(e, "PlayerStop") == 1 && player_commands(e) == 5);
        KV_CHECK(c, krome_errors(e)->total == 0); // no shortcut, style or binding of the face's was refused
        // Every control is reached by Tab, in order, and worked with Space: the first is the first button.
        standin_clear(e);
        input_key_override(e, Key::Tab, true); frames(e, 1); input_key_override(e, Key::Tab, false); frames(e, 2);
        key(Key::Space);
        KV_INFO("test", "face: Tab then Space sent %u commands: %u previous, %u pause, %u play", player_commands(e), standin_count(e, "PlayerPrevious"), standin_count(e, "PlayerPause"), standin_count(e, "PlayerPlay"));
        KV_CHECK(c, standin_count(e, "PlayerPrevious") == 1 && player_commands(e) == 1); // the button with the focus, and not the shortcut as well
        input_key_override(e, Key::Escape, true); frames(e, 1); input_key_override(e, Key::Escape, false); frames(e, 2);
        input_pointer_override(e, 5, 795, 1); frames(e, 1); input_pointer_override(e, 5, 795, 0); frames(e, 2); // a press on nothing: the focus is let go

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
        KV_CHECK(c, cmd && cmd->a == row && player_commands(e) == 1);
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
        KV_CHECK(c, cmd && cmd->value == (f64)PlayerEditOp::Clear && player_commands(e) == 1);

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
        KV_CHECK(c, player_commands(e) == 0);

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
        KV_CHECK(c, set && set->a == 0 && set->b == 1 && set->value == 1.0 && player_commands(e) == 1);
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

    c.begin("face library");
    {
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        make_chain(e);
        StandinPlayer& p = *standin_player(e);
        standin_root(e, 1, "/music", LibraryRootState::Idle, 1200);
        LibraryRoot* gone = standin_root(e, 2, "/media/usb/more", LibraryRootState::Offline, 40);
        format(gone->reason, sizeof gone->reason, "its directory is not there");
        KV_CHECK(c, until_ready(e));
        Name scn = ui_dev_screen(e);
        // The roots, each with its state, and why one is offline, where the root shows.
        KV_CHECK(c, on_screen(e, "/media/usb/more") == 1 && on_screen(e, "offline") == 1 && on_screen(e, "its directory is not there") == 1 && on_screen(e, "1200 tracks") == 1);
        // The face asked for its window: the roots' folders, from the top.
        const StandinCommand* q = asked_query(e);
        KV_CHECK(c, q && q->a == kWindow && asked_view(q) == LibraryView::Folders && asked_parent(q) == 0 && q->b == 0 && q->c > 0 && shows(e, "l_crumb", "Folders"));
        standin_row(e, kWindow, 0, LibraryRowKind::Directory, 11, "/music");
        e.events.emit(LibraryResult{kWindow, 1, 0, 1, 5, 0.0021});
        frames(e, 4);
        KV_CHECK(c, shows(e, "l_total", "1") && on_screen(e, "/music") == 2 && shows(e, "l_status", "1 rows, answered in 2.1 ms; generation 5")); // the root, and its folder in the list

        // A folder opened: its window asked for, and the folder to be listed before the rest.
        // What the table holds meanwhile is the list above, and is not shown as this one.
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "lo_0"));
        frames(e, 4);
        q = asked_query(e);
        const StandinCommand* pr = standin_last(e, "LibraryPrioritize");
        KV_CHECK(c, q && asked_view(q) == LibraryView::Folders && asked_parent(q) == 11 && pr && pr->b == 11 && shows(e, "l_crumb", "Folders / /music") && shows(e, "l_total", "0"));
        standin_rows_clear(e, kWindow);
        standin_row(e, kWindow, 0, LibraryRowKind::Directory, 21, "Albums");
        LibraryRow* t1 = standin_row(e, kWindow, 1, LibraryRowKind::Track, 101, "01 one.flac", "One", "Somebody", "First");
        t1->rate = 44100; t1->frames = 44100 * 185; format(t1->date, sizeof t1->date, "1999");
        LibraryRow* t2 = standin_row(e, kWindow, 2, LibraryRowKind::Track, 102, "02 two.flac");
        t2->missing = 1;
        LibraryRow* t3 = standin_row(e, kWindow, 3, LibraryRowKind::Track, 103, "03 three.flac", "Three");
        t3->offline = 1;
        LibraryRow* t4 = standin_row(e, kWindow, 4, LibraryRowKind::Track, 104, "04 four.flac");
        format(t4->reason, sizeof t4->reason, "not a FLAC file");
        e.events.emit(LibraryResult{kWindow, 5, 0, 5, 6, 0.001});
        frames(e, 4);
        KV_CHECK(c, shows(e, "l_total", "5") && on_screen(e, "One") == 1 && on_screen(e, "3:05, 1999") == 1 && on_screen(e, "Somebody") == 1);
        KV_CHECK(c, on_screen(e, "02 two.flac") == 1 && on_screen(e, "missing") == 1 && on_screen(e, "its root is offline") == 1 && on_screen(e, "not a FLAC file") == 1);
        // At rest for a moment: what is in view is what is scanned next.
        standin_clear(e);
        f64 until = time_now() + 0.5;
        while (time_now() < until) { frames(e, 1); time_sleep(0.01); }
        pr = standin_last(e, "LibraryPrioritize");
        KV_CHECK(c, pr && pr->a == kWindow && pr->b == 0 && standin_count(e, "LibraryPrioritize") == 1 && standin_count(e, "LibraryQuery") == 0);

        // A row queued is one command, whatever kind it is; the folder itself too.
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "lq_1") && krome_press(e, scn, "lq_0"));
        frames(e, 3);
        KV_CHECK(c, standin_count(e, "LibraryCollect") == 2 && player_commands(e) == 0);
        const StandinCommand* col = standin_last(e, "LibraryCollect");
        KV_CHECK(c, col && col->a == kCollect && col->b == 21 && col->value == (f64)LibraryRowKind::Directory);
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "l_queue_all"));
        frames(e, 3);
        col = standin_last(e, "LibraryCollect");
        KV_CHECK(c, col && col->a == kCollect && col->b == 11 && col->value == (f64)LibraryRowKind::Directory);
        // Played: the queue emptied, the row collected, and play once the last path is in.
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "lp_1"));
        frames(e, 3);
        col = standin_last(e, "LibraryCollect");
        const StandinCommand* edit = standin_last(e, "PlayerQueueEdit");
        KV_CHECK(c, col && col->a == kCollectPlay && col->b == 101 && col->value == (f64)LibraryRowKind::Track && edit && edit->value == (f64)PlayerEditOp::Clear);
        KV_CHECK(c, standin_count(e, "PlayerPlay") == 0);
        LibraryCollected got{};
        got.id = kCollectPlay; got.index = 0; got.total = 2;
        format(got.path, sizeof got.path, "/music/01 one.flac");
        e.events.emit(got);
        frames(e, 6);
        KV_CHECK(c, standin_count(e, "PlayerPlay") == 0); // not all in yet
        got.index = 1;
        e.events.emit(got);
        frames(e, 8);
        KV_CHECK(c, standin_count(e, "PlayerPlay") == 1);

        // A field edited: the track's title, for the user; and given back.
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "lo_1"));
        frames(e, 4);
        KV_CHECK(c, shows(e, "l_edit_open", "true") && shows(e, "l_edit_name", "One") && shows(e, "l_edit_field", "title") && shows(e, "l_edit_value", "One"));
        krome_set(e, scn, "l_edit_value", "One, Corrected");
        frames(e, 2);
        KV_CHECK(c, krome_press(e, scn, "l_edit_save"));
        frames(e, 4);
        const StandinCommand* ov = standin_last(e, "LibraryOverride");
        KV_CHECK(c, ov && ov->a == 101 && ov->b == 0 && strcmp(ov->text, "title=One, Corrected") == 0 && shows(e, "l_edit_open", "false"));
        KV_CHECK(c, krome_press(e, scn, "lo_1"));
        frames(e, 4);
        KV_CHECK(c, krome_press(e, scn, "l_edit_clear"));
        frames(e, 4);
        ov = standin_last(e, "LibraryOverride");
        KV_CHECK(c, ov && ov->a == 101 && ov->b == 1 && standin_count(e, "LibraryOverride") == 2);

        // Up, and the other views: artists, an artist's albums, an album's tracks; sorted; searched.
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "l_up"));
        frames(e, 4);
        q = asked_query(e);
        KV_CHECK(c, q && asked_view(q) == LibraryView::Folders && asked_parent(q) == 0 && shows(e, "l_crumb", "Folders"));
        KV_CHECK(c, krome_press(e, scn, "lv_artists"));
        frames(e, 4);
        q = asked_query(e);
        KV_CHECK(c, q && asked_view(q) == LibraryView::Artists && shows(e, "l_view", "artists") && shows(e, "l_crumb", "Artists"));
        standin_rows_clear(e, kWindow);
        LibraryRow* ar = standin_row(e, kWindow, 0, LibraryRowKind::Artist, 501, "Somebody");
        ar->albums = 2; ar->count = 20;
        e.events.emit(LibraryResult{kWindow, 1, 0, 1, 6, 0.001});
        frames(e, 4);
        KV_CHECK(c, on_screen(e, "2 albums, 20 tracks") == 1 && krome_press(e, scn, "lo_0"));
        frames(e, 4);
        q = asked_query(e);
        KV_CHECK(c, q && asked_view(q) == LibraryView::Albums && asked_parent(q) == 501 && shows(e, "l_crumb", "Artists / Somebody"));
        standin_rows_clear(e, kWindow);
        LibraryRow* al = standin_row(e, kWindow, 0, LibraryRowKind::Album, 601, "First");
        al->count = 10; format(al->date, sizeof al->date, "1999");
        e.events.emit(LibraryResult{kWindow, 1, 0, 1, 6, 0.001});
        frames(e, 4);
        KV_CHECK(c, on_screen(e, "10 tracks, 1999") == 1 && krome_press(e, scn, "lo_0"));
        frames(e, 4);
        q = asked_query(e);
        KV_CHECK(c, q && asked_view(q) == LibraryView::AlbumTracks && asked_parent(q) == 601 && shows(e, "l_crumb", "Artists / Somebody / First"));
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "l_play_all"));
        frames(e, 3);
        col = standin_last(e, "LibraryCollect");
        KV_CHECK(c, col && col->a == kCollectPlay && col->b == 601 && col->value == (f64)LibraryRowKind::Album && standin_count(e, "PlayerQueueEdit") == 1);
        KV_CHECK(c, krome_press(e, scn, "lv_genres"));
        frames(e, 4);
        KV_CHECK(c, asked_view(asked_query(e)) == LibraryView::Genres);
        KV_CHECK(c, krome_press(e, scn, "lv_tracks") );
        frames(e, 4);
        KV_CHECK(c, asked_view(asked_query(e)) == LibraryView::Tracks && asked_sort(asked_query(e)) == LibrarySort::Default);
        KV_CHECK(c, krome_press(e, scn, "ls_title"));
        frames(e, 4);
        KV_CHECK(c, asked_view(asked_query(e)) == LibraryView::Tracks && asked_sort(asked_query(e)) == LibrarySort::Title && shows(e, "l_sort", "title"));
        // Every track is no one row's: nothing to collect, and the queue is left alone.
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "l_play_all"));
        frames(e, 3);
        KV_CHECK(c, standin_count(e, "LibraryCollect") == 0 && player_commands(e) == 0);
        // Words typed into the search: a search for them, each time they change.
        KV_CHECK(c, type_into(e, "l_search", "blue mo"));
        frames(e, 4);
        q = asked_query(e);
        KV_CHECK(c, q && asked_view(q) == LibraryView::Search && strcmp(q->text, "blue mo") == 0 && shows(e, "l_crumb", "search: blue mo") && shows(e, "l_view", "search"));
        standin_clear(e);
        KV_CHECK(c, krome_press(e, scn, "l_queue_all"));
        frames(e, 3);
        col = standin_last(e, "LibraryCollect");
        KV_CHECK(c, col && col->a == kCollect && strcmp(col->text, "blue mo") == 0);
        KV_CHECK(c, krome_press(e, scn, "l_up"));
        frames(e, 4);
        KV_CHECK(c, asked_view(asked_query(e)) == LibraryView::Tracks && shows(e, "l_search", ""));
        input_pointer_override(e, 5, 795, 0);
        frames(e, 2);

        // A root added by its path, typed; one removed.
        standin_clear(e);
        KV_CHECK(c, type_into(e, "l_root_path", "/srv/music"));
        input_key_override(e, Key::Enter, true);
        frames(e, 1);
        input_key_override(e, Key::Enter, false);
        frames(e, 3);
        const StandinCommand* add = standin_last(e, "LibraryAddRoot");
        KV_CHECK(c, add && strcmp(add->text, "/srv/music") == 0 && shows(e, "l_root_path", ""));
        KV_CHECK(c, krome_press(e, scn, "lr_2"));
        frames(e, 3);
        const StandinCommand* rem = standin_last(e, "LibraryRemoveRoot");
        KV_CHECK(c, rem && rem->a == 2);
        input_pointer_override(e, 5, 795, 0);
        frames(e, 2);

        // The queue's rows are asked of the library by their paths, and shown by its titles.
        standin_item(e, 1, "/music/Album/01 First.flac", PlayerItemState::Playing);
        standin_item(e, 2, "/music/Album/02 Second.flac");
        standin_item(e, 3, "/elsewhere/03 Third.flac");
        p.transport.queued = p.transport.left = 3;
        p.transport.item = p.transport.place = 1;
        p.transport.place_pos = 1;
        standin_clear(e);
        frames(e, 10);
        KV_CHECK(c, standin_count(e, "LibraryPath") == 3);
        const StandinCommand* lp = standin_last(e, "LibraryPath");
        KV_CHECK(c, lp && lp->a == kTitles && lp->b == 2 && strcmp(lp->text, "/elsewhere/03 Third.flac") == 0);
        KV_CHECK(c, on_screen(e, "01 First.flac") >= 1 && shows(e, "t_title", "01 First.flac")); // not answered yet: the file's name
        LibraryRow* ta = standin_row(e, kTitles, 0, LibraryRowKind::Track, 901, "01 First.flac", "First Song", "The Band");
        format(ta->path, sizeof ta->path, "/music/Album/01 First.flac");
        LibraryRow* tb = standin_row(e, kTitles, 1, LibraryRowKind::Track, 902, "02 Second.flac", "Second Song");
        format(tb->path, sizeof tb->path, "/music/Album/02 Second.flac");
        LibraryRow* tc = standin_row(e, kTitles, 2, LibraryRowKind::Track, 0, "03 Third.flac");
        format(tc->path, sizeof tc->path, "/elsewhere/03 Third.flac");
        frames(e, 3);
        KV_CHECK(c, shows(e, "t_title", "The Band - First Song") && on_screen(e, "The Band - First Song") == 2 && on_screen(e, "Second Song") == 1 && on_screen(e, "03 Third.flac") == 1);
        // The queue shifts under the answers: a row is not shown under another file's title.
        standin_remove(e, 1);
        standin_find(e, 2)->pos = 1;
        standin_find(e, 3)->pos = 2;
        p.transport.queued = p.transport.left = 2;
        p.transport.item = p.transport.place = 2;
        standin_clear(e);
        frames(e, 2);
        KV_CHECK(c, on_screen(e, "First Song") == 0 && on_screen(e, "02 Second.flac") >= 1);
        frames(e, 10);
        KV_CHECK(c, standin_count(e, "LibraryPath") == 3); // rows 0 and 1 asked again by their new paths, row 2 taken out
        lp = standin_last(e, "LibraryPath");
        KV_CHECK(c, lp != nullptr);

        // Hidden: its windows onto the library are closed. Seen again: asked for again.
        standin_clear(e);
        e.events.emit(WindowVisibility{false, WindowHidden::Minimized});
        ticks(e, 3);
        KV_CHECK(c, standin_count(e, "LibraryClose") == 2 && standin_count(e, "LibraryQuery") == 0);
        ticks(e, 60);
        KV_CHECK(c, standin_count(e, "LibraryQuery") == 0 && standin_count(e, "LibraryPath") == 0);
        e.events.emit(WindowVisibility{true, WindowHidden::None});
        frames(e, 10);
        KV_CHECK(c, standin_count(e, "LibraryQuery") == 1 && standin_count(e, "LibraryPath") == 2);
        engine_shutdown(e);
    }

    c.begin("face inspectors");
    {
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        make_chain(e);
        StandinPlayer& p = *standin_player(e);
        StandinLibrary& lib = *standin_library(e);
        KV_CHECK(c, until_ready(e));
        Name scn = ui_dev_screen(e);

        // Scan: what the library is doing, from its resource.
        lib.scan.active = 1; lib.scan.walking = 2; lib.scan.files_found = 31250; lib.scan.names_per_second = 4100.0; lib.scan.dirs_listed = 812;
        lib.scan.tags_read = 9000; lib.scan.tags_per_second = 650.0; lib.scan.bytes_read = 40u << 20; lib.scan.unread = 22250; lib.scan.slot = 1;
        lib.scan.tracks = 31250; lib.scan.generation = 44; lib.scan.cancelled = 3;
        format(lib.scan.dir, sizeof lib.scan.dir, "Artist/An Album");
        KV_CHECK(c, krome_press(e, scn, "tab_scan"));
        frames(e, 4);
        KV_CHECK(c, shows(e, "s_state", "scanning") && shows(e, "s_walking", "root 2: Artist/An Album") && shows(e, "s_found", "31250 files, 4100 a second; 812 directories listed"));
        KV_CHECK(c, shows(e, "s_read", "9000 tracks, 650 a second; 40.0 MB read") && shows(e, "s_unread", "22250 tracks") && shows(e, "s_queries", "3 stopped by a newer one"));
        KV_CHECK(c, shows(e, "s_slot", "something looked at is being scanned first") && shows(e, "s_index", "31250 tracks, generation 44") && on_screen(e, "31250 files") == 1);
        // A rebuilt index: said here, and where the library's list says what it has.
        lib.scan.rebuilt = 1; lib.scan.active = 0; lib.scan.walking = 0;
        frames(e, 3);
        KV_CHECK(c, shows(e, "s_state", "idle") && strstr(krome_model_text(e, scn, "s_index"), "made again at start-up") && strstr(krome_model_text(e, scn, "l_status"), "the index was made again"));

#if KV_DEV
        // Events: each command and notification as it passes, with its fields, newest last,
        // and no handler written for any of them. Nothing is kept while the tab is not up.
        e.events.emit(PlayerStop{});
        frames(e, 3);
        KV_CHECK(c, krome_press(e, scn, "tab_events"));
        frames(e, 4);
        KV_CHECK(c, shows(e, "e_summary", "0 seen, 0 kept") || strstr(krome_model_text(e, scn, "e_summary"), "seen")); // the face's own asks may already have passed
        u32 before_seen = (u32)atof(krome_model_text(e, scn, "e_summary"));
        e.events.emit(PlayerPlay{});
        e.events.emit(LibraryChanged{77});
        PlayerTrackFailed failed{};
        failed.item = 3; failed.seq = 9;
        format(failed.reason, sizeof failed.reason, "not a FLAC file");
        e.events.emit(failed);
        frames(e, 4);
        KV_CHECK(c, on_screen(e, "PlayerPlay{") == 1 && on_screen(e, "LibraryChanged{generation=77}") == 1 && on_screen(e, "PlayerTrackFailed{item=3, seq=9, reason=\"not a FLAC file\"}") == 1);
        KV_CHECK(c, (u32)atof(krome_model_text(e, scn, "e_summary")) == before_seen + 3 && on_screen(e, "PlayerStop") == 0 && on_screen(e, "KromeSet") == 0);
        // Filtered: the lines that have the words.
        KV_CHECK(c, type_into(e, "e_filter", "Failed"));
        frames(e, 3);
        KV_CHECK(c, on_screen(e, "PlayerTrackFailed{") == 1 && on_screen(e, "PlayerPlay{") == 0 && on_screen(e, "LibraryChanged") == 0);
        input_pointer_override(e, 5, 795, 0);
        frames(e, 2);
        // Held: what passes is not kept, and what is there stays to be read.
        KV_CHECK(c, krome_press(e, scn, "e_hold"));
        frames(e, 3);
        e.events.emit(PlayerNext{});
        e.events.emit(failed);
        frames(e, 4);
        KV_CHECK(c, on_screen(e, "PlayerTrackFailed{") == 1 && strstr(krome_model_text(e, scn, "e_summary"), "held") && (u32)atof(krome_model_text(e, scn, "e_summary")) == before_seen + 3);
        KV_CHECK(c, krome_press(e, scn, "e_hold"));
        frames(e, 3);
        e.events.emit(failed);
        frames(e, 4);
        KV_CHECK(c, on_screen(e, "PlayerTrackFailed{") == 2);
        KV_CHECK(c, krome_press(e, scn, "e_clear"));
        frames(e, 3);
        KV_CHECK(c, on_screen(e, "PlayerTrackFailed{") == 0 && strstr(krome_model_text(e, scn, "e_summary"), " 0 kept"));
        // Still, with the Events tab up and nothing passing: no frame.
        paced(e, 30);
        Cost quiet = paced(e, 300);
        KV_CHECK(c, quiet.drawn == 0 && quiet.tick_asks == 0);
#endif

        // Tables: a resource, then a table, each by its own description. Every field the
        // schema has is a line, by its name: one added to the schema would be one more.
        p.transport.position = 123456; p.transport.rate = 48000; p.transport.state = (u8)PlayerState::Paused;
        PlayerItem* it = standin_item(e, 1, "/music/a/01 one.flac", PlayerItemState::Failed);
        format(it->reason, sizeof it->reason, "its header says 9 channels");
        standin_item(e, 2, "/music/a/02 two.flac");
        p.transport.queued = 2;
        // Output: the devices there are, and a choice of one, or of the default followed.
        standin_device(e, "alsa_output.usb-dac", "USB DAC", true, true, true);
        standin_device(e, "alsa_output.hdmi", "HDMI", false, true, false);
        standin_device(e, "bluez_output.headset", "Headset", false, false, false);
        format(p.output.chosen, sizeof p.output.chosen, "default");
        p.output.following = 1;
        frames(e, 4);
        KV_CHECK(c, shows(e, "o_ndev", "3") && on_screen(e, "USB DAC") == 1 && on_screen(e, "Headset") == 1 && on_screen(e, "following the default") == 1);
        KV_CHECK(c, on_screen(e, "playing default ") == 1 && on_screen(e, "gone") == 1);
        KV_CHECK(c, krome_press(e, scn, "od_1"));
        frames(e, 3);
        const StandinCommand* chose = standin_last(e, "PlayerSelectDevice");
        KV_CHECK(c, chose && strcmp(chose->text, "alsa_output.hdmi") == 0);
        KV_CHECK(c, krome_press(e, scn, "o_follow"));
        frames(e, 3);
        chose = standin_last(e, "PlayerSelectDevice");
        KV_CHECK(c, chose && strcmp(chose->text, "default") == 0 && standin_count(e, "PlayerSelectDevice") == 2);
        KV_CHECK(c, krome_press(e, scn, "oa_0"));
        frames(e, 3);
        chose = standin_last(e, "PlayerSelectDevice");
        KV_CHECK(c, chose && strcmp(chose->text, "alsa_output.usb-dac") == 0 && chose->a == 1); // alone
        // Media: with the module off, the panel says why.
        KV_CHECK(c, krome_press(e, scn, "tab_media"));
        frames(e, 4);
        KV_CHECK(c, shows(e, "m_summary", "no surface") && shows(e, "m_why", "not asked for in this run"));
        KV_CHECK(c, krome_press(e, scn, "tab_tables"));
        frames(e, 5);
        KV_CHECK(c, on_screen(e, "player.transport") >= 1 && on_screen(e, "library.rows") == 1 && strstr(krome_model_text(e, scn, "x_summary"), "player.transport: 11 fields"));
        u32 named = 0;
        for (const FieldDesc& fd : k_player_transport_fields) named += on_screen(e, fd.name) >= 1;
        KV_CHECK(c, named == KV_COUNTOF(k_player_transport_fields) && on_screen(e, "123456") == 1 && on_screen(e, "48000") == 1);
        p.transport.position = 123999;
        frames(e, 3);
        KV_CHECK(c, on_screen(e, "123999") == 1 && on_screen(e, "123456") == 0);
        KV_CHECK(c, krome_press(e, scn, "xn_2")); // player.queue
        frames(e, 5);
        KV_CHECK(c, strstr(krome_model_text(e, scn, "x_summary"), "player.queue: 2 rows of PlayerItem, 12 fields each") && shows(e, "x_total", "26"));
        named = 0;
        for (const FieldDesc& fd : k_player_item_fields) named += on_screen(e, fd.name) >= 1;
        KV_CHECK(c, named == KV_COUNTOF(k_player_item_fields) && on_screen(e, "row 1 of 2") == 1 && on_screen(e, "row 2 of 2") == 1);
        KV_CHECK(c, on_screen(e, "\"/music/a/01 one.flac\"") == 1 && on_screen(e, "\"its header says 9 channels\"") == 1);
        KV_CHECK(c, krome_press(e, scn, "xn_7")); // library.scan (after player.devices)
        frames(e, 5);
        named = 0;
        for (const FieldDesc& fd : k_library_scan_fields) named += on_screen(e, fd.name) >= 1;
        KV_CHECK(c, named == KV_COUNTOF(k_library_scan_fields) && on_screen(e, "\"Artist/An Album\"") == 1);
        KV_CHECK(c, krome_press(e, scn, "xn_4")); // player.params: five rows, the stand-in's
        frames(e, 5);
        KV_CHECK(c, strstr(krome_model_text(e, scn, "x_summary"), "player.params: 5 rows of PlayerParam") && on_screen(e, "\"gain_db\"") == 1 && on_screen(e, "\"dB\"") >= 1);

        // Every failure has its place. A track that ends damaged, in its row, with how; a
        // path too long to take, where it was typed.
        KV_CHECK(c, on_screen(e, "its header says 9 channels") >= 1); // the queue's row too
        PlayerItem* two = standin_find(e, 2);
        two->damaged = 1; two->pos = 1; it->pos = 2;
        format(two->reason, sizeof two->reason, "cut short at 0:41");
        p.transport.place_pos = 2;
        frames(e, 3);
        KV_CHECK(c, on_screen(e, "damaged") == 1 && on_screen(e, "cut short at 0:41") >= 1);
        char longer[4200];
        memset(longer, 'a', sizeof longer);
        longer[0] = '/';
        longer[4150] = 0;
        standin_clear(e);
        KV_CHECK(c, type_into(e, "q_path", longer));
        input_key_override(e, Key::Enter, true);
        frames(e, 1);
        input_key_override(e, Key::Enter, false);
        frames(e, 3);
        KV_CHECK(c, standin_count(e, "PlayerEnqueue") == 0 && strstr(krome_model_text(e, scn, "q_note"), "a path typed here can be 510 bytes"));
        // One that fits is added, and the note goes.
        input_pointer_override(e, 5, 795, 0);
        krome_set(e, scn, "q_path", "/music/short.flac");
        frames(e, 2);
        KV_CHECK(c, krome_press(e, scn, "q_add"));
        frames(e, 3);
        KV_CHECK(c, standin_count(e, "PlayerEnqueue") == 1 && shows(e, "q_note", ""));
        engine_shutdown(e);
    }

    c.begin("face session");
    {
        // The face's part of the session: what it asks to have kept, and what it does with
        // what was kept. The session's own module is in the engine, off: it keeps no file.
        static SessionKeep asked[32];
        static u32 nasked;
        nasked = 0;
        auto last = [&](const char* key) -> const SessionKeep* {
            for (u32 i = nasked; i > 0; --i) if (strcmp(asked[i - 1].owner, "ui_dev") == 0 && strcmp(asked[i - 1].key, key) == 0) return &asked[i - 1];
            return nullptr;
        };
        auto text_of = [&](const char* key) -> const char* { const SessionKeep* k = last(key); return k ? k->text : "(never asked)"; };
        auto number_of = [&](const char* key) -> f64 { const SessionKeep* k = last(key); return k ? k->number : -1.0; };
        {
            Engine e;
            KV_CHECK(c, engine_init(e, k_list));
            engine_listen(e, engine_event_type_id(e, "SessionKeep"), [](void*, u32, const void* ev) { if (nasked < 32) asked[nasked++] = *(const SessionKeep*)ev; }, nullptr);
            krome_viewport(e, 1280, 800, 1.0f);
            make_chain(e);
            KV_CHECK(c, until_ready(e));
            Name scn = ui_dev_screen(e);
            frames(e, 4);
            KV_CHECK(c, strcmp(text_of("tab"), "chain") == 0 && number_of("lib_sort") == 0.0 && strcmp(text_of("lib_where"), "0 0 Folders\n") == 0);
            u32 before = nasked;
            frames(e, 60);
            KV_CHECK(c, nasked == before); // asked once: not again while nothing changes
            KV_CHECK(c, krome_press(e, scn, "tab_events") && krome_press(e, scn, "ls_artist") && krome_press(e, scn, "lv_artists"));
            frames(e, 6);
            standin_row(e, kWindow, 0, LibraryRowKind::Artist, 501, "Somebody");
            e.events.emit(LibraryResult{kWindow, 1, 0, 1, 6, 0.001});
            frames(e, 4);
            KV_CHECK(c, krome_press(e, scn, "lo_0"));
            frames(e, 6);
            KV_CHECK(c, strcmp(text_of("tab"), "events") == 0 && number_of("lib_sort") == (f64)(u8)LibrarySort::Artist);
            KV_CHECK(c, strcmp(text_of("lib_where"), "3 0 Artists\n4 501 Somebody\n") == 0 && last("w") == nullptr); // no window here: no size to keep
            engine_shutdown(e);
        }
        {
            // The next start: the session says what it kept, before the screen is built.
            Engine e;
            KV_CHECK(c, engine_init(e, k_list));
            krome_viewport(e, 1280, 800, 1.0f);
            make_chain(e);
            auto kept = [&](const char* key, f64 number, const char* text) {
                SessionKept k{};
                format(k.owner, sizeof k.owner, "ui_dev"); format(k.key, sizeof k.key, "%s", key); k.number = number; format(k.text, sizeof k.text, "%s", text);
                e.events.emit(k);
            };
            kept("tab", 0.0, "tables");
            kept("lib_sort", (f64)(u8)LibrarySort::Artist, "");
            kept("lib_where", 0.0, "3 0 Artists\n4 501 Somebody\n");
            SessionKept other{};
            format(other.owner, sizeof other.owner, "another face"); format(other.key, sizeof other.key, "tab"); format(other.text, sizeof other.text, "scan");
            e.events.emit(other); // not this face's: left alone
            KV_CHECK(c, until_ready(e));
            Name scn = ui_dev_screen(e);
            frames(e, 6);
            // The tab it was on, the list where it was, sorted as it was.
            KV_CHECK(c, shows(e, "r_tab", "tables") && on_screen(e, "player.transport") >= 1 && shows(e, "l_crumb", "Artists / Somebody") && shows(e, "l_sort", "artist"));
            const StandinCommand* q = asked_query(e);
            KV_CHECK(c, q && asked_view(q) == LibraryView::Albums && asked_parent(q) == 501 && asked_sort(q) == LibrarySort::Artist);
            KV_CHECK(c, krome_press(e, scn, "l_up"));
            frames(e, 4);
            KV_CHECK(c, asked_view(asked_query(e)) == LibraryView::Artists && shows(e, "l_crumb", "Artists"));
            engine_shutdown(e);
        }
        {
            // A search that was up comes back as a search, with its words in the edit.
            Engine e;
            KV_CHECK(c, engine_init(e, k_list));
            krome_viewport(e, 1280, 800, 1.0f);
            make_chain(e);
            SessionKept k{};
            format(k.owner, sizeof k.owner, "ui_dev"); format(k.key, sizeof k.key, "lib_text"); format(k.text, sizeof k.text, "blue morning");
            e.events.emit(k);
            KV_CHECK(c, until_ready(e));
            frames(e, 6);
            const StandinCommand* q = asked_query(e);
            KV_CHECK(c, q && asked_view(q) == LibraryView::Search && strcmp(q->text, "blue morning") == 0 && shows(e, "l_search", "blue morning") && shows(e, "l_view", "search"));
            engine_shutdown(e);
        }
    }

    c.begin("face fifty thousand");
    {
        // A library of fifty thousand tracks that answers two frames after it is asked. The
        // scrollbar is dragged from end to end: every frame within a refresh, rows not yet
        // answered drawn as placeholders, and the rows in view filled soon after it stops.
        Engine e;
        KV_CHECK(c, engine_init(e, k_list));
        krome_viewport(e, 1280, 800, 1.0f);
        make_chain(e);
        KV_CHECK(c, until_ready(e));
        Name scn = ui_dev_screen(e);
        KV_CHECK(c, krome_press(e, scn, "lv_tracks"));
        frames(e, 4);
        const u32 total = 50000;
        // The stand-in library: what was asked two frames ago is answered now.
        StandinCommand pending[3] = {};
        bool have[3] = {};
        u32 answered = 0, seen = 0;
        auto serve = [&]() {
            StandinPlayer& sp = *standin_player(e);
            if (have[0]) { answer_tracks(e, (u32)pending[0].b, (u32)pending[0].c, total); ++answered; }
            pending[0] = pending[1]; have[0] = have[1];
            have[1] = false;
            for (; seen < sp.ncommands; ++seen)
                if (strcmp(sp.commands[seen].type, "LibraryQuery") == 0) { pending[1] = sp.commands[seen]; have[1] = true; have[0] = false; } // the newest replaces the one before
            if (sp.ncommands > 900) { standin_clear(e); seen = 0; }
        };
        for (u32 i = 0; i < 12; ++i) { serve(); frames(e, 1); }
        KV_CHECK(c, shows(e, "l_total", "50000") && on_screen(e, "song 0") >= 1 && on_screen(e, "...") == 0);
        // A notch of the wheel is already there: the window was asked for wider than the view.
        u32 before_wheel = answered;
        input_pointer_override(e, 300, 400, 0, -1.0f);
        for (u32 i = 0; i < 6; ++i) { serve(); frames(e, 1); }
        const KromeTree* tree = krome_tree(e, scn);
        u32 list_node = tree ? krome_find(*tree, name_intern("l_list")) : KROME_NONE;
        KV_CHECK(c, list_node != KROME_NONE && tree->nodes[list_node].scroll.y > 0.0f); // it moved
        KV_CHECK(c, answered == before_wheel && on_screen(e, "...") == 0);                // and asked for nothing
        input_pointer_override(e, 300, 400, 0, 1.0f);
        for (u32 i = 0; i < 6; ++i) { serve(); frames(e, 1); }

        rect2 list{};
        KV_CHECK(c, krome_find_rect(e, scn, "l_list", &list));
        f32 bar_x = list.x + list.w - 3.0f;
        f64 worst = 0.0;
        u32 with_placeholders = 0, dragged = 0;
        auto frame = [&](f32 y, bool down) {
            serve();
            input_pointer_override(e, bar_x, y, down ? 1 : 0);
            f64 t0 = time_now();
            frames(e, 1);
            f64 took = time_now() - t0;
            if (took > worst) worst = took;
        };
        frame(list.y + 6.0f, false);
        frame(list.y + 6.0f, true);
        const u32 steps = 180; // three seconds, top to bottom
        for (u32 i = 1; i <= steps; ++i) {
            frame(list.y + 6.0f + (list.h - 12.0f) * (f32)i / (f32)steps, true);
            ++dragged;
            if (on_screen(e, "...")) ++with_placeholders;
        }
        // At rest, still held: how long until what is in view is filled.
        u32 to_filled = 0;
        while (to_filled < 60 && on_screen(e, "...")) { frame(list.y + list.h - 6.0f, true); ++to_filled; }
        frame(list.y + list.h - 6.0f, false);
        input_pointer_override(e, 5, 795, 0);
        for (u32 i = 0; i < 4; ++i) { serve(); frames(e, 1); }
        u32 first = (u32)atof(krome_model_text(e, scn, "l_first"));
        KV_INFO("test", "face: fifty thousand rows dragged end to end in %u frames: the longest %.2f ms, %u with placeholders, filled %u frames after it stopped; %u windows answered, the last from row %u",
                dragged, worst * 1e3, with_placeholders, to_filled, answered, first);
        KV_CHECK(c, on_screen(e, "song 49999") == 1 && on_screen(e, "...") == 0 && first > 49000);
        KV_CHECK(c, with_placeholders > 0 && to_filled <= 6);  // a hundred milliseconds at 60 Hz
#if !KV_DEBUG && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
        KV_CHECK(c, worst < 1.0 / 60.0); // one refresh of a 60 Hz display; not a debug or a sanitizer build's to meet
#endif
        // A track that changes under the list at rest changes in place: the list does not move.
        LibraryRow* changed = nullptr;
        KTable* lr = engine_ktable(e, "library.rows");
        for (u32 i = 0; i < lr->rows_of<LibraryRow>().len; ++i)
            if (lr->is_alive(i) && lr->rows_of<LibraryRow>()[i].query == kWindow && lr->rows_of<LibraryRow>()[i].index == 49990) changed = &lr->rows_of<LibraryRow>()[i];
        KV_CHECK(c, changed != nullptr);
        if (changed) format(changed->title, sizeof changed->title, "tags just read");
        frames(e, 3);
        KV_CHECK(c, on_screen(e, "tags just read") == 1 && on_screen(e, "song 49999") == 1 && (u32)atof(krome_model_text(e, scn, "l_first")) == first);
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
        char tmp[1024], data[1100];
        KV_CHECK(c, io_make_temp_dir("bw_face", tmp, sizeof tmp));
        format(data, sizeof data, "%s/data", tmp);
        LibraryConfig lc;
        lc.data_dir = data;
        library_configure(lc);
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
        // Flat and unmuted again, by the same controls.
        KV_CHECK(c, krome_press(e, scn, "cp_1_0") && krome_press(e, scn, "ct_0_1"));
        KV_CHECK(c, until_shows(e, "c_v_0_1", "off") && until_shows(e, "c_flat_1", "flat"));

        // ---- and the library ----
        // A root, by its path typed in: walked, read, and shown with its counts.
        char music[1100];
        format(music, sizeof music, "%s/music", tmp);
        kvtest::make_library(music, 60, 12);
        KV_CHECK(c, type_into(e, "l_root_path", music));
        input_key_override(e, Key::Enter, true);
        frames(e, 1);
        input_key_override(e, Key::Enter, false);
        f64 scan_until = time_now() + 20.0;
        while (time_now() < scan_until && !(on_screen(e, "60 tracks, 0 unread") == 1 && on_screen(e, "idle") == 1)) { frames(e, 1); time_sleep(0.004); }
        KV_CHECK(c, on_screen(e, "60 tracks, 0 unread") == 1 && on_screen(e, "idle") == 1 && on_screen(e, music) >= 1);
        input_pointer_override(e, 5, 795, 0);
        frames(e, 2);
        // Browsed: its folder, the artists, an artist's albums, an album's tracks, the genres.
        auto listed = [&](const char* text, f64 seconds = 4.0) {
            f64 end = time_now() + seconds;
            for (;;) { frames(e, 1); if (on_screen(e, text)) return true; if (time_now() > end) { KV_ERROR("test", "\"%s\" did not come on screen", text); return false; } time_sleep(0.004); }
        };
        KV_CHECK(c, krome_press(e, scn, "lo_0") && listed("Compilations"));           // the root's folder, opened
        KV_CHECK(c, krome_press(e, scn, "lv_artists") && listed("4 albums, 48 tracks"));
        KV_CHECK(c, krome_press(e, scn, "lo_0") && until_shows(e, "l_total", "4") && listed("12 tracks, 19"));
        KV_CHECK(c, krome_press(e, scn, "lo_0") && until_shows(e, "l_total", "12") && listed("Track Zq"));
        KV_CHECK(c, krome_press(e, scn, "lv_genres") && listed("Electronic") && listed("60 tracks"));
        KV_CHECK(c, krome_press(e, scn, "lv_tracks") && until_shows(e, "l_total", "60"));

        // Searched, and the one found played: the queue is that track, by the library's title.
        KV_CHECK(c, type_into(e, "l_search", "zq37") && until_shows(e, "l_total", "1") && listed("Track Zq37"));
        input_pointer_override(e, 5, 795, 0);
        KV_CHECK(c, krome_press(e, scn, "lp_0"));
        KV_CHECK(c, until_shows(e, "q_summary", "1 rows", 4.0, true) && until_shows(e, "t_title", "Track Zq37", 4.0, true));
        KV_CHECK(c, until_shows(e, "t_state", "stopped", 4.0)); // a tenth of a second of it
        // Its title corrected, by hand: shown in the list, and in the queue.
        KV_CHECK(c, krome_press(e, scn, "lo_0") && until_shows(e, "l_edit_open", "true"));
        krome_set(e, scn, "l_edit_value", "A Better Title");
        frames(e, 2);
        KV_CHECK(c, krome_press(e, scn, "l_edit_save") && listed("A Better Title"));
        f64 both_until = time_now() + 4.0;
        while (time_now() < both_until && on_screen(e, "A Better Title") < 2) { frames(e, 1); time_sleep(0.004); }
        KV_CHECK(c, on_screen(e, "A Better Title") == 2); // the library's row, and the queue's
        // An album queued from its row: twelve more, in its own order.
        KV_CHECK(c, krome_press(e, scn, "l_up") && krome_press(e, scn, "lv_albums") && until_shows(e, "l_total", "5"));
        KV_CHECK(c, krome_press(e, scn, "lq_1") && until_shows(e, "q_summary", "13 rows", 4.0, true));
        // The queue's titles are the library's: the list above shows albums, so every track's title on screen is a queue row's.
        f64 titles_until = time_now() + 4.0;
        while (time_now() < titles_until && on_screen(e, "Track Zq") < 5) { frames(e, 1); time_sleep(0.004); }
        KV_CHECK(c, on_screen(e, "Track Zq") >= 5);
        // The queue emptied, and the root removed.
        KV_CHECK(c, krome_press(e, scn, "q_clear") && until_shows(e, "q_summary", "0 rows, 0 left"));
        KV_CHECK(c, krome_press(e, scn, "lr_1"));
        f64 gone_until = time_now() + 4.0;
        while (time_now() < gone_until && on_screen(e, music)) { frames(e, 1); time_sleep(0.004); }
        KV_CHECK(c, on_screen(e, music) == 0 && shows(e, "o_underruns", "0"));
        engine_shutdown(e);
        io_remove_tree(tmp);
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
        KV_CHECK(c, player_commands(e) == 0);
        standin_player(e)->transport.state = (u8)PlayerState::Paused;
        frames(e, 3);
        KV_CHECK(c, shows(e, "t_state", "paused") && krome_press(e, ui_dev_screen(e), "play"));
        frames(e, 3);
        KV_CHECK(c, standin_count(e, "PlayerPlay") == 1);
        engine_shutdown(e);
        ui_dev_configure(UiDevConfig{});
    }
}
