// The player in a real engine, driven only by events and read only through its table and
// resources, playing into the capture device with the test driving its clock.
//
// Each test starts an engine with platform, cli, the player and a small module that
// writes down the events the player emits. A step is: pump the platform (the I/O queue),
// run one tick. The capture device's frames are pulled between steps, so "when" is exact.
#include "korvid/blackwing_player/module.h"
#include "korvid/cli/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/hifi_platform/capture.h"
#include "korvid/platform/module.h"
#include "../../hifi_codec/tests/md5.h"
#include "korvid/platform/io.h"
#include "korvid/hifi_codec/flac.h"
#include <initializer_list>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

using namespace kv;

namespace {

// ---- the listener: the player's events, written down in order ---------------------------

struct Heard { char kind; u64 item; u64 frames; bool damaged; char reason[96]; u32 rate; u32 seq; };
constinit Heard g_log[256] = {};
constinit u32   g_nlog = 0;

// Kept in the order things happened (`seq`), not the order the handlers ran in.
void put(u32 seq, char kind, u64 item, u64 frames = 0, bool damaged = false, const char* reason = "", u32 rate = 0) {
    if (g_nlog == 256) return;
    u32 at = g_nlog++;
    while (at > 0 && g_log[at - 1].seq > seq) { g_log[at] = g_log[at - 1]; --at; }
    Heard& h = g_log[at];
    h.kind = kind; h.item = item; h.frames = frames; h.damaged = damaged; h.rate = rate; h.seq = seq;
    format(h.reason, sizeof h.reason, "%s", reason);
}
void on_started(TickContext&, Span<const PlayerTrackStarted> ev) { for (auto& e : ev) put(e.seq, 'S', e.item, e.frames, false, "", e.rate); }
void on_ended(TickContext&, Span<const PlayerTrackEnded> ev)     { for (auto& e : ev) put(e.seq, 'E', e.item, e.frames, e.damaged, e.reason); }
void on_failed(TickContext&, Span<const PlayerTrackFailed> ev)   { for (auto& e : ev) put(e.seq, 'F', e.item, 0, false, e.reason); }
void on_queue_ended(TickContext&, Span<const PlayerQueueEnded> ev) { for (auto& e : ev) put(e.seq, 'Q', 0); }
void on_underrun(TickContext&, Span<const HifiUnderrun> ev)      { for (auto& e : ev) put(e.seq, 'U', e.count); }
void on_seeked(TickContext&, Span<const PlayerSeeked> ev)        { for (auto& e : ev) put(e.seq, 'K', e.item, e.frame); }
constinit f64 g_param_value = 0.0;
constinit u32 g_nparam = 0; // PlayerParamChanged heard, ever: the log holds 256 of anything
void on_param(TickContext&, Span<const PlayerParamChanged> ev)   { for (auto& e : ev) { put(e.seq, 'P', e.param); g_param_value = e.value; ++g_nparam; } }

// The devices' events in a log of their own: other tests (and the machine) make and pull
// devices while these run, and the track log's tests say nothing of them.
struct DevHeard { u8 kind; char name[96]; };
constinit DevHeard g_dev[64] = {};
constinit u32 g_ndev = 0;
void on_device(TickContext&, Span<const PlayerDeviceChanged> ev) {
    for (auto& e : ev) { if (g_ndev == 64) return; g_dev[g_ndev].kind = e.kind; format(g_dev[g_ndev].name, sizeof g_dev[0].name, "%s", e.name); ++g_ndev; }
}
// How many of a kind about the capture device have been heard.
u32 dev_heard(PlayerDeviceChange kind) {
    u32 n = 0;
    for (u32 i = 0; i < g_ndev; ++i) n += g_dev[i].kind == (u8)kind && strcmp(g_dev[i].name, "capture") == 0;
    return n;
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".device",  KV_ON(PlayerDeviceChanged, on_device),  .phase = Phase::PostSim},
    {.name = ".started", KV_ON(PlayerTrackStarted, on_started), .phase = Phase::PostSim},
    {.name = ".ended",   KV_ON(PlayerTrackEnded, on_ended),     .phase = Phase::PostSim},
    {.name = ".failed",  KV_ON(PlayerTrackFailed, on_failed),   .phase = Phase::PostSim},
    {.name = ".queue",   KV_ON(PlayerQueueEnded, on_queue_ended), .phase = Phase::PostSim},
    {.name = ".under",   KV_ON(HifiUnderrun, on_underrun),      .phase = Phase::PostSim},
    {.name = ".seeked",  KV_ON(PlayerSeeked, on_seeked),        .phase = Phase::PostSim},
    {.name = ".param",   KV_ON(PlayerParamChanged, on_param),   .phase = Phase::PostSim},
};
KV_MODULE_DEFINE(k_listener) = {.name = "listener", .handlers = k_handlers};

// The log as a string of kinds and items: "S1 E1 S2 E2 Q0".
const char* log_text() {
    static char text[1024];
    usize at = 0;
    text[0] = 0;
    for (u32 i = 0; i < g_nlog && at + 16 < sizeof text; ++i)
        at += (usize)format(text + at, sizeof text - at, "%s%c%llu", i ? " " : "", g_log[i].kind, (unsigned long long)g_log[i].item);
    return text;
}
// The first event of a kind about an item; an empty one (kind 0) when there is none.
const Heard& ev(char kind, u64 item) {
    static const Heard none = {};
    for (u32 i = 0; i < g_nlog; ++i) if (g_log[i].kind == kind && g_log[i].item == item) return g_log[i];
    return none;
}
bool heard(char kind, u64 item) { return ev(kind, item).kind == kind; }

// ---- the rig ----------------------------------------------------------------------------

char g_data_dir[900];
const char* data(const char* name) {
    static char paths[8][1024];
    static u32 next = 0;
    char* p = paths[next++ % 8];
    format(p, 1024, "%s/%s", g_data_dir, name);
    return p;
}

struct Lines { char text[4096]; usize at = 0; };
void collect(void* user, const char* line) {
    Lines& l = *(Lines*)user;
    l.at += (usize)format(l.text + l.at, sizeof l.text - l.at, "%s\n", line);
    if (l.at >= sizeof l.text) l.at = sizeof l.text - 1;
}

struct Rig {
    Engine       e;
    HifiCapture* cap = nullptr;
    bool         up  = false;

    bool start(u32 delay = 0, u32 piece = 256u << 10, usize memory = 256u << 20, f64 lead = 5.0, bool wait_for_absent = false) {
        g_nlog = 0;
        g_nparam = 0;
        g_ndev = 0;
        HifiCaptureDesc cd;
        cd.delay_frames = delay;
        cd.keep_bytes = 16u << 20;
        cap = hifi_capture_create(cd);
        PlayerConfig pc;
        pc.device = "capture";
        pc.device_backend = cap;
        pc.piece_bytes = piece;
        pc.memory_bytes = memory;
        pc.lead_seconds = lead;
        pc.wait_for_absent = wait_for_absent;
        player_configure(pc);
        CliConfig cli; cli.no_stdin = true;
        cli_configure(cli);
        PlatformMounts mounts; mounts.none = true;
        platform_configure_mounts(mounts);
        static const ModuleDesc* const list[] = {&platform_module, &cli_module, &blackwing_player_module, &k_listener};
        up = engine_init(e, list);
        return up;
    }
    void stop() {
        if (up) engine_shutdown(e);
        up = false;
        hifi_capture_destroy(cap);
        cap = nullptr;
    }

    void step(u32 n = 1) { for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, e.clock.tick_dt); } }
    const PlayerTransport& t() { return *player_transport(e); }
    const PlayerOutput&    o() { return *player_output(e); }
    PlayerItem item(u64 id) {
        KTable* q = engine_ktable(e, "player.queue");
        Span<PlayerItem> rows = q->rows_of<PlayerItem>();
        for (u32 i = 0; i < rows.len; ++i) if (q->is_alive(i) && rows[i].id == id) return rows[i];
        return PlayerItem{};
    }
    // Done: before the place.
    bool done(u64 id) { PlayerItem it = item(id); return it.id && it.pos < t().place_pos; }
    void add(const char* path) { PlayerEnqueue q{}; format(q.path, sizeof q.path, "%s", path); e.events.emit(q); }
    template <class E> void say(const E& ev) { e.events.emit(ev); }

    // Steps (sleeping a little between, for the I/O and the render thread) until the lead
    // holds `frames`, or two seconds pass.
    bool wait_lead(u64 frames) {
        f64 until = time_now() + 2.0;
        for (;;) { step(); if (o().lead_frames >= frames) return true; if (time_now() > until) return false; time_sleep(0.0005); }
    }
    bool wait_log(char kind, u64 item) {
        f64 until = time_now() + 2.0;
        for (;;) { step(); if (heard(kind, item)) return true; if (time_now() > until) return false; time_sleep(0.0005); }
    }
    u32 pull(u32 frames) { u32 n = hifi_capture_pull(cap, frames); step(); return n; }
    // Paces a device against a small lead: pulls what is surely there, keeping back more
    // than the callback's ramp (which would fade a lead about to run dry), and the rest
    // only once the lead has stopped growing (the stream's end).
    // "Stopped growing" is a fifth of a second unchanged, so a slow build (a sanitizer's)
    // is not taken for the end.
    void pace(u32* still) {
        time_sleep(0.0005);
        step();
        u32 lead = o().lead_frames;
        if (lead > 1200 || lead != last_lead) { if (lead > 1200) pull(lead - 1000 > 1024 ? 1024 : lead - 1000); *still = 0; last_lead = o().lead_frames; return; }
        if (++*still > 400) { pull(lead ? lead : 256); *still = 0; }
    }
    u32 last_lead = ~0u;
    // Pulls as a device would, not as fast as a loop can: a driven clock outruns any
    // decoder, and frames taken before the decoder has made the next ones are an underrun
    // no hardware would cause. Paced (pace), until an event, or ten seconds.
    bool play_until(char kind, u64 item) {
        f64 until = time_now() + 10.0;
        u32 still = 0;
        while (!heard(kind, item)) {
            if (time_now() > until) return false;
            pace(&still);
        }
        return true;
    }

    const u8* seg_data(u32 k, HifiDeviceFormat* f, u64* frames) {
        HifiCaptureSegment seg[8];
        u32 n = hifi_capture_segments(cap, seg, 8);
        if (k >= n) { *frames = 0; return nullptr; }
        *f = seg[k].format;
        *frames = seg[k].bytes / hifi_frame_bytes(seg[k].format);
        return hifi_capture_data(cap) + seg[k].offset;
    }
};

// A FLAC file's samples, 16-bit stereo, decoded straight: what a capture is compared with.
struct Pcm16 { i16* s = nullptr; u64 frames = 0; };
Pcm16 decode16(const char* path) {
    Pcm16 p;
    IoBytes b;
    if (!io_read_file(path, &b)) return p;
    const DecoderDesc& d = k_flac_decoder;
    u8* mem = (u8*)mem_alloc(d.memory_bytes);
    DecoderOpen o = d.open(sparse_whole(Span<const u8>(b.data, (u32)b.size)), Span<u8>(mem, d.memory_bytes));
    if (o.state) {
        i32 buf[4096 * 2];
        p.s = (i16*)mem_alloc((usize)(o.info.frames + 4096) * 4);
        for (;;) {
            DecodeResult r = d.decode(o.state, buf, 4096);
            for (u32 i = 0; i < r.frames * 2; ++i) p.s[p.frames * 2 + i] = (i16)buf[i];
            p.frames += r.frames;
            if (r.status != DecodeStatus::Ok) break;
        }
        d.close(o.state);
    }
    mem_free(mem);
    b.free();
    return p;
}
// The first captured frame at or after `from` where `pcm` from `at` runs for 64 frames.
u64 find_run16(const u8* d, u64 from, u64 to, const Pcm16& pcm, u64 at) {
    for (u64 k = from; k + 64 <= to; ++k) if (memcmp(d + k * 4, pcm.s + at * 2, 256) == 0) return k;
    return ~0ull;
}
u32 count_of(char kind, u64 item) { u32 n = 0; for (u32 i = 0; i < g_nlog; ++i) n += g_log[i].kind == kind && g_log[i].item == item; return n; }
const Heard& last_of(char kind, u64 item) {
    static const Heard none = {};
    for (u32 i = g_nlog; i-- > 0;) if (g_log[i].kind == kind && g_log[i].item == item) return g_log[i];
    return none;
}

// The first `frames` 16-bit frames of a capture hash to a FLAC file's own MD5.
bool md5_is(const u8* captured, u64 frames, u32 channels, const char* flac_path) {
    u8 want[16] = {}, got[16];
    FILE* f = fopen(flac_path, "rb");
    if (!f) return false;
    fseek(f, 26, SEEK_SET);
    usize n = fread(want, 1, 16, f);
    fclose(f);
    kvtest::Md5 m;
    m.update(captured, (usize)frames * channels * 2);
    m.finish(got);
    return n == 16 && memcmp(want, got, 16) == 0;
}
bool all_zero(const u8* p, usize n) { for (usize i = 0; i < n; ++i) if (p[i]) return false; return true; }

// Whether the log is exactly `want`; says what it was when it is not.
bool log_is(const char* want) {
    if (strcmp(log_text(), want) == 0) return true;
    KV_ERROR("test", "the log was \"%s\", not \"%s\"", log_text(), want);
    return false;
}
// Where an event is in the log; a large number when it is not there.
u32 log_at(char kind, u64 item) {
    for (u32 i = 0; i < g_nlog; ++i) if (g_log[i].kind == kind && g_log[i].item == item) return i;
    return 1000;
}

const u64 kN = 22050; // frames in tone_16_2ch_44100.flac

} // namespace

void test_player(TestCtx& c) {
    {
        const char* here = __FILE__;
        const char* slash = strrchr(here, '/');
        format(g_data_dir, sizeof g_data_dir, "%.*s/../../hifi_codec/tests/data", slash ? (int)(slash - here) : 0, here);
    }
    LogLevel was = log_level();
    log_set_level(LogLevel::Warn);
    const char* A = "tone_16_2ch_44100.flac";

    // Two files, by events alone: exact at the device, joined, every event in order, the
    // table and the resources telling the same story.
    c.begin("player plays");
    {
        Rig r;
        KV_CHECK(c, r.start());
        KV_CHECK(c, r.t().state == (u8)PlayerState::Stopped && r.t().queued == 0);
        r.add(data(A)); r.add(data(A));
        r.step();
        KV_CHECK(c, r.t().queued == 2 && r.t().left == 2 && r.t().state == (u8)PlayerState::Stopped);
        KV_CHECK(c, r.item(1).state == (u8)PlayerItemState::Idle && strstr(r.item(2).path, A) != nullptr);
        r.step(5);
        KV_CHECK(c, r.item(1).state == (u8)PlayerItemState::Idle && !r.o().open); // queued is not playing: nothing is read
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(kN * 2));
        KV_CHECK(c, r.t().state == (u8)PlayerState::Starting && r.t().item == 0 && g_nlog == 0); // in the lead; not heard
        KV_CHECK(c, r.o().open && r.o().exact && !r.o().shared && r.o().rate == 44100 && r.o().channels == 2);
        r.pull(1000);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Playing && r.t().item == 1 && r.t().position == 1000);
        KV_CHECK(c, r.t().frames == kN && r.t().rate == 44100);
        KV_CHECK(c, r.item(1).state == (u8)PlayerItemState::Playing && r.item(1).rate == 44100 && r.item(1).bits == 16 && r.item(1).frames == kN);
        u64 pulled = 1000;
        const u32 sizes[] = {512, 1, 4096, 333, 2048};
        for (u32 i = 0; pulled < kN * 2 + 3000; ++i) pulled += r.pull(sizes[i % 5]);
        KV_CHECK(c, r.wait_log('Q', 0));
        KV_CHECK(c, log_is("S1 E1 S2 E2 Q0"));
        KV_CHECK(c, r.t().state == (u8)PlayerState::Stopped && r.t().item == 0 && r.t().left == 0);
        KV_CHECK(c, r.done(1) && r.done(2) && !r.item(2).damaged);
        KV_CHECK(c, r.o().underruns == 0);
        HifiDeviceFormat f; u64 frames;
        const u8* d = r.seg_data(0, &f, &frames);
        KV_CHECK(c, d && f.sample == HifiSampleFormat::S16 && frames == pulled && hifi_capture_opens(r.cap) == 1);
        KV_CHECK(c, d && md5_is(d, kN, 2, data(A)));                         // the first file, exact
        KV_CHECK(c, d && md5_is(d + kN * 4, kN, 2, data(A)));                // the second, straight after it
        KV_CHECK(c, d && all_zero(d + kN * 8, (usize)(frames - kN * 2) * 4)); // and silence
        r.stop();
    }

    // TrackStarted is emitted when the listener hears it: with a device 1000 frames from
    // the ear, in the tick after the 1001st frame is taken, and not one sooner.
    c.begin("player clock");
    {
        Rig r;
        KV_CHECK(c, r.start(1000));
        r.add(data(A));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(kN));
        r.pull(1000);
        r.step(3);
        KV_CHECK(c, g_nlog == 0 && r.t().state == (u8)PlayerState::Starting && r.t().position == 0);
        r.pull(1);
        KV_CHECK(c, log_is("S1") && r.t().state == (u8)PlayerState::Playing && r.t().position == 1);
        KV_CHECK(c, heard('S', 1) && ev('S', 1).frames == kN && ev('S', 1).rate == 44100);
        r.pull((u32)kN - 1001);                       // taken: all of it; heard: all but the last 1000
        KV_CHECK(c, !heard('E', 1) && r.t().position == kN - 1000);
        r.pull(999);
        KV_CHECK(c, !heard('E', 1));
        r.pull(1);
        KV_CHECK(c, heard('E', 1) && ev('E', 1).frames == kN && !ev('E', 1).damaged);
        KV_CHECK(c, r.wait_log('Q', 0));
        KV_CHECK(c, log_is("S1 E1 Q0"));
        r.stop();
    }

    // Pause takes effect in the device's next callback and holds; play goes on from there.
    c.begin("player pause");
    {
        Rig r;
        KV_CHECK(c, r.start());
        r.add(data(A));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(kN));
        r.pull(4000);
        r.say(PlayerPause{});
        r.step();                                     // the tick that takes the command
        KV_CHECK(c, r.t().state == (u8)PlayerState::Paused);
        r.pull(512);                                  // one period: the ramp is inside it
        u64 held = r.t().position;
        KV_CHECK(c, held > 4000 && held <= 4000 + 221);
        r.pull(512); r.pull(2048);
        KV_CHECK(c, r.t().position == held && r.t().state == (u8)PlayerState::Paused && r.t().item == 1);
        // Paused, with everything it asked for in: the player wants no tick (a loop sleeps).
        r.step(4);
        KV_CHECK(c, !r.e.ticks_wanted);
        HifiDeviceFormat f; u64 frames;
        const u8* d = r.seg_data(0, &f, &frames);
        KV_CHECK(c, d && all_zero(d + held * 4, (usize)(frames - held) * 4)); // silence from the end of the ramp
        r.say(PlayerPlay{});
        r.step();
        KV_CHECK(c, r.t().state == (u8)PlayerState::Playing && r.e.ticks_wanted); // playing: every tick, at the clock's rate
        KV_CHECK(c, r.play_until('Q', 0));
        KV_CHECK(c, log_is("S1 E1 Q0") && r.o().underruns == 0);
        // Stopped at the queue's end: nothing is wanted again.
        r.step(4);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Stopped && !r.e.ticks_wanted);
        r.stop();
    }

    // A main thread held for two seconds (a compositor stalling a resize, a dragged title
    // bar) is not heard: the device, on its own clock, plays from the lead while no tick
    // runs, and the player catches up after.
    c.begin("player stalled");
    {
        g_nlog = 0;
        PlayerConfig pc;
        pc.device = "capture"; // none given: the player's own, on the wall clock
        player_configure(pc);
        CliConfig cli; cli.no_stdin = true;
        cli_configure(cli);
        PlatformMounts mounts; mounts.none = true;
        platform_configure_mounts(mounts);
        static const ModuleDesc* const list[] = {&platform_module, &cli_module, &blackwing_player_module, &k_listener};
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        PlayerEnqueue q{};
        format(q.path, sizeof q.path, "%s", data("tone_16_2ch_8000_long.flac"));
        e.events.emit(q);
        e.events.emit(PlayerPlay{});
        auto run = [&](f64 seconds) {
            f64 until = time_now() + seconds;
            while (time_now() < until && !heard('Q', 0)) { platform_pump(e); engine_frame(e, e.clock.tick_dt); time_sleep(0.002); }
        };
        run(0.7);
        KV_CHECK(c, heard('S', 1) && player_transport(e)->state == (u8)PlayerState::Playing);
        u64 before = player_transport(e)->position;
        time_sleep(2.0); // no pump, no tick
        run(0.05);
        u64 after = player_transport(e)->position;
        KV_CHECK(c, after - before > 8000 * 19 / 10 && after - before < 8000 * 23 / 10); // it played through: two seconds of an 8 kHz file
        run(3.0);
        KV_CHECK(c, log_is("S1 E1 Q0") && !ev('E', 1).damaged && player_output(e)->underruns == 0);
        engine_shutdown(e);
    }

    // What cannot be played fails alone, in words, and the next plays. A file cut short
    // plays what it has.
    c.begin("player failures");
    {
        Rig r;
        KV_CHECK(c, r.start());
        r.add(data("broken/not_flac.flac"));
        r.add("/no/such/file.flac");
        r.add(data(A));
        r.add(data("broken/truncated.flac"));
        r.add(data("broken/magic_only.flac"));
        r.add(data(A));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_log('F', 2) && r.wait_log('F', 1));
        KV_CHECK(c, strstr(ev('F', 1).reason, "not a kind of audio file") && strstr(ev('F', 2).reason, "no such file"));
        KV_CHECK(c, r.item(1).state == (u8)PlayerItemState::Failed && r.item(1).reason[0] && r.item(2).state == (u8)PlayerItemState::Failed);
        KV_CHECK(c, r.play_until('Q', 0));
        // A failure is known as soon as it is found, which is ahead of what is being heard,
        // so the F's are early; what is heard is in order.
        KV_CHECK(c, g_nlog == 10 && heard('F', 1) && heard('F', 2) && heard('F', 5));
        KV_CHECK(c, log_at('S', 3) < log_at('E', 3) && log_at('E', 3) < log_at('S', 4) && log_at('S', 4) < log_at('E', 4));
        KV_CHECK(c, log_at('E', 4) < log_at('S', 6) && log_at('S', 6) < log_at('E', 6) && log_at('E', 6) < log_at('Q', 0));
        if (g_nlog != 10) KV_ERROR("test", "the log was: %s", log_text());
        const Heard& cut = ev('E', 4);
        KV_CHECK(c, cut.damaged && cut.frames > 0 && cut.frames < kN && strstr(cut.reason, "cut short"));
        KV_CHECK(c, r.done(4) && r.item(4).damaged && r.item(4).reason[0]);
        KV_CHECK(c, heard('F', 5) && ev('F', 5).reason[0] && r.item(5).state == (u8)PlayerItemState::Failed);
        KV_CHECK(c, heard('E', 6) && !ev('E', 6).damaged && r.t().left == 0 && r.t().state == (u8)PlayerState::Stopped);
        KV_CHECK(c, r.o().underruns == 0 && !heard('U', 1));
        r.stop();
    }

    // Next ends the item playing without an Ended and starts the one after it from its
    // first frame. Stop holds nothing: the item starts over on the next play.
    c.begin("player next and stop");
    {
        Rig r;
        KV_CHECK(c, r.start());
        r.add(data(A)); r.add(data(A)); r.add(data(A));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(kN * 2));
        r.pull(5000);
        KV_CHECK(c, r.t().item == 1);
        r.say(PlayerNext{});
        r.step();
        KV_CHECK(c, r.done(1) && r.t().item == 0);
        KV_CHECK(c, r.wait_lead(kN * 2));             // items 2 and 3, the jump's new lead
        r.pull(2000);
        KV_CHECK(c, log_is("S1 S2") && r.t().item == 2);
        HifiDeviceFormat f; u64 frames;
        const u8* d = r.seg_data(0, &f, &frames);
        // A next is a jump: item 1 fades out at once and item 2 fades in from its first
        // frame (10 ms each, 441 frames), with less than a callback of silence between.
        // After its fade in, item 2 is exact.
        const u64 fade = 441;
        u64 after = 5000;
        while (after < 5000 + 2 * fade + 1024 && memcmp(d + after * 4, d + fade * 4, 256) != 0) ++after;
        KV_CHECK(c, after <= 5000 + 2 * fade + 1024 && memcmp(d + after * 4, d + fade * 4, (usize)(frames - after) * 4) == 0);

        u64 at = r.t().position;
        r.say(PlayerStop{});
        r.step();
        KV_CHECK(c, r.t().state == (u8)PlayerState::Stopped && r.t().item == 0);
        // The stop reaches the device when the render thread has passed it on, a moment
        // later: pull until the lead has been dropped, as a device would go on pulling.
        for (u32 i = 0; i < 200 && (i == 0 || r.o().lead_frames > 0); ++i) { time_sleep(0.001); r.pull(500); }
        KV_CHECK(c, r.o().lead_frames == 0);
        r.step(3);
        KV_CHECK(c, r.item(2).state == (u8)PlayerItemState::Ready && r.t().left == 2 && at > 0); // not done: it will start over
        KV_CHECK(c, !heard('Q', 0));                  // stopped is not finished
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(kN * 2));
        r.pull(100);
        KV_CHECK(c, r.t().item == 2);
        KV_CHECK(c, r.t().position == 100);
        if (r.t().position != 100) KV_ERROR("test", "position %llu, item %llu", (unsigned long long)r.t().position, (unsigned long long)r.t().item);
        KV_CHECK(c, log_is("S1 S2 S2"));
        KV_CHECK(c, r.play_until('Q', 0));
        KV_CHECK(c, log_is("S1 S2 S2 E2 S3 E3 Q0") && r.o().underruns == 0);
        r.stop();
    }

    // Seek, seek-by, previous and jump: each a jump in hifi. A seek lands on its frame,
    // says PlayerSeeked (not a new start) when the frame is heard, and the position
    // counts from it; seek-bys in one tick add up; previous restarts a row past three
    // seconds and goes back a row within them; a jump plays the row asked for.
    c.begin("player seek");
    {
        const char* L = "tone_16_2ch_8000_long.flac"; // 32000 frames at 8 kHz: 4 seconds
        Pcm16 pcm = decode16(data(L));
        KV_CHECK(c, pcm.frames == 32000);
        const u64 fade = 80;                           // 10 ms at 8 kHz
        Rig r;
        KV_CHECK(c, r.start());
        r.add(data(L)); r.add(data(L)); r.add(data(L));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(8000));
        u32 still = 0;
        while (r.t().position < 2000 && still < 1000) r.pace(&still);
        KV_CHECK(c, log_is("S1") && r.t().item == 1);
        u64 before = 0;
        { HifiDeviceFormat f; r.seg_data(0, &f, &before); }
        r.say(PlayerSeek{0, 20000});
        r.step();
        KV_CHECK(c, r.t().item == 0 && r.t().seek_item == 1 && r.t().seek_frame == 20000 && r.t().place == 1); // starting: the old audio on its way out
        f64 until = time_now() + 2.0;
        while (!heard('K', 1) && time_now() < until) r.pace(&still);
        KV_CHECK(c, log_is("S1 K1") && ev('K', 1).frames == 20000 && r.t().seek_item == 0);
        HifiDeviceFormat f; u64 frames = 0;
        const u8* d = r.seg_data(0, &f, &frames);
        u64 after = d ? find_run16(d, before, frames, pcm, 20000 + fade) : ~0ull;
        KV_CHECK(c, after != ~0ull && memcmp(d + after * 4, pcm.s + (20000 + fade) * 2, (usize)(frames - after) * 4) == 0);
        KV_CHECK(c, after != ~0ull && r.t().item == 1 && r.t().position == 20000 + fade + (frames - after)); // from the target, every frame heard since
        // At once: the old audio as it was until the jump reached the device, then its
        // fade, then nothing but silence until the new audio's fade in.
        u64 cut = before;
        while (d && cut < frames && memcmp(d + cut * 4, pcm.s + cut * 2, 4) == 0) ++cut;
        u64 quiet_from = cut + fade, quiet_to = after - fade; // the new audio's fade in begins at quiet_to
        KV_CHECK(c, after != ~0ull && quiet_from <= quiet_to + 1);
        KV_CHECK(c, after != ~0ull && (quiet_from >= quiet_to || all_zero(d + quiet_from * 4, (usize)(quiet_to - quiet_from) * 4)));

        // Two seek-bys in one tick: a quarter of a second each, from the same place.
        u64 p0 = r.t().position;
        r.say(PlayerSeekBy{0.25}); r.say(PlayerSeekBy{0.25});
        r.step();
        KV_CHECK(c, r.t().seek_frame == p0 + 4000);
        until = time_now() + 2.0;
        while (count_of('K', 1) < 2 && time_now() < until) r.pace(&still);
        KV_CHECK(c, count_of('K', 1) == 2 && last_of('K', 1).frames == p0 + 4000);

        // Past three seconds, previous is this row from its start; and within them,
        // with no row before it, the same again.
        r.say(PlayerSeek{0, 28000});
        until = time_now() + 2.0;
        while (count_of('K', 1) < 3 && time_now() < until) r.pace(&still);
        r.say(PlayerPrevious{});
        until = time_now() + 2.0;
        while (count_of('K', 1) < 4 && time_now() < until) r.pace(&still);
        KV_CHECK(c, count_of('K', 1) == 4 && last_of('K', 1).frames == 0 && count_of('S', 1) == 1);
        // The next row, and back within its first three seconds: the row before, started again.
        r.say(PlayerNext{});
        until = time_now() + 2.0;
        while (!heard('S', 2) && time_now() < until) r.pace(&still);
        r.say(PlayerPrevious{});
        until = time_now() + 2.0;
        while (count_of('S', 1) < 2 && time_now() < until) r.pace(&still);
        KV_CHECK(c, heard('S', 2) && count_of('S', 1) == 2 && r.t().item == 1 && !r.done(2));
        // A jump to the third row: the first two are done, and it plays to the end.
        r.say(PlayerJump{3});
        until = time_now() + 5.0;
        while (!heard('Q', 0) && time_now() < until) r.pace(&still);
        KV_CHECK(c, heard('S', 3) && heard('E', 3) && !heard('E', 1) && !heard('E', 2) && heard('Q', 0));
        KV_CHECK(c, r.done(1) && r.done(2));
        KV_CHECK(c, r.o().underruns == 0);
        r.stop();
        mem_free(pcm.s);
    }

    // Volume and mute: a change is heard by a recut, and said when it is; the chain's row
    // shows the value heard and whether it is flat; exact is off while the gain is not 0
    // dB, and on again at 0; mute is silence.
    c.begin("player volume");
    {
        Rig r;
        KV_CHECK(c, r.start());
        r.add(data("tone_16_2ch_8000_long.flac"));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(8000));
        u32 still = 0;
        while (r.t().position < 2000 && still < 1000) r.pace(&still);
        KV_CHECK(c, r.o().exact);
        auto row = [&]() { KTable* q = engine_ktable(r.e, "player.chain"); return q ? q->rows_of<PlayerStage>()[0] : PlayerStage{}; };
        KV_CHECK(c, strcmp(row().name, "gain") == 0 && row().flat && row().params[0] == 0.0 && strcmp(row().param_names[1], "mute") == 0);
        r.say(PlayerSetParam{0, 0, -6.0});
        f64 until = time_now() + 2.0;
        while (!heard('P', 0) && time_now() < until) r.pace(&still);
        KV_CHECK(c, heard('P', 0) && g_param_value == -6.0 && row().params[0] == -6.0 && !row().flat && !r.o().exact);
        r.say(PlayerSetParam{0, 0, 3.0});                // above 0 dB: kept at 0
        until = time_now() + 2.0;
        while (count_of('P', 0) < 2 && time_now() < until) r.pace(&still);
        for (u32 i = 0; i < 20; ++i) r.pace(&still);   // past the join's fade
        KV_CHECK(c, count_of('P', 0) == 2 && g_param_value == 0.0 && row().flat && r.o().exact && !r.o().joining);
        r.say(PlayerSetParam{0, 1, 1.0});
        until = time_now() + 2.0;
        while (!heard('P', 1) && time_now() < until) r.pace(&still);
        r.pull(300); r.pull(500);                       // past the join's fade (80 frames), then what is checked
        HifiDeviceFormat f; u64 frames = 0;
        const u8* d = r.seg_data(0, &f, &frames);
        KV_CHECK(c, heard('P', 1) && !row().flat && d && all_zero(d + (frames - 500) * 4, 500 * 4)); // muted: silence
        KV_CHECK(c, r.o().underruns == 0);
        r.stop();
    }

    // A place taken up without playing (PlayerCue): the transport is at a row and a frame,
    // paused, with nothing heard and no device opened; play goes on from there. This is what
    // a session comes back to.
    c.begin("player cue");
    {
        const char* L = "tone_16_2ch_8000_long.flac"; // 32000 frames at 8 kHz
        Pcm16 pcm = decode16(data(L));
        const u64 fade = 80;
        Rig r;
        KV_CHECK(c, r.start());
        r.add(data(A)); r.add(data(L)); r.add(data(A));
        r.step(3);
        r.say(PlayerCue{2, 12000, 32000, 8000});
        r.step(3);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Paused && r.t().item == 2 && r.t().place == 2 && r.t().place_pos == 2 && r.t().position == 12000);
        KV_CHECK(c, r.t().rate == 8000 && r.t().frames == 32000 && r.t().seek_item == 0 && r.t().left == 2); // what the cue said of the row; the row before the place is done
        KV_CHECK(c, g_nlog == 0 && hifi_capture_opens(r.cap) == 0 && r.o().lead_frames == 0);               // nothing heard, nothing said, no device
        r.step(4);
        KV_CHECK(c, !r.e.ticks_wanted);                                                                      // and it rests there
        // A seek while cued moves the place and stays cued; so does a seek by seconds, counted in what the cue said.
        r.say(PlayerSeekBy{1.0});
        r.step(2);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Paused && r.t().position == 20000 && r.t().seek_item == 0);
        r.say(PlayerSeek{0, 16000});
        r.step(2);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Paused && r.t().item == 2 && r.t().position == 16000 && hifi_capture_opens(r.cap) == 0);
        // Play: from that row, at that frame. It starts (it was never heard, so it is no seek), and the first frames heard are the file's from there.
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(4000));
        u32 still = 0;
        f64 until = time_now() + 2.0;
        while (!heard('S', 2) && time_now() < until) r.pace(&still);
        KV_CHECK(c, log_is("S2") && r.t().state == (u8)PlayerState::Playing && r.t().item == 2 && r.t().rate == 8000 && r.t().frames == 32000);
        HifiDeviceFormat f; u64 frames = 0;
        const u8* d = r.seg_data(0, &f, &frames);
        u64 at = d ? find_run16(d, 0, frames, pcm, 16000 + fade) : ~0ull;
        KV_CHECK(c, at != ~0ull && at <= 2 * fade && memcmp(d + at * 4, pcm.s + (16000 + fade) * 2, (usize)(frames - at) * 4) == 0);
        KV_CHECK(c, r.t().position >= 16000 && r.t().position < 16000 + frames + 1);
        // Cued again while it plays: it stops there, as a stop would, and holds the new place. A stop ends a cue.
        r.say(PlayerCue{3, 100, 0, 0});
        r.step(3);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Paused && r.t().item == 3 && r.t().position == 100 && r.t().rate == 0 && r.t().place_pos == 3);
        r.say(PlayerStop{});
        r.step(2);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Stopped && r.t().item == 0 && r.t().position == 0 && r.t().place == 3);
        // A row that is not there: nothing changes.
        r.say(PlayerCue{99, 0, 0, 0});
        r.step(2);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Stopped && r.t().place == 3);
        // Cued, then played to the end from the cue: the last row, from its frame.
        r.say(PlayerCue{3, 1000, 0, 0});
        r.step(2);
        r.say(PlayerPlay{});
        KV_CHECK(c, r.play_until('Q', 0));
        KV_CHECK(c, heard('S', 3) && heard('E', 3) && r.o().underruns == 0);
        mem_free(pcm.s);
        r.stop();
    }

    // Parameters describe themselves (player.params), presets are data (player.presets),
    // and applying one is a command: every parameter of the stage, in one recut.
    c.begin("player params and presets");
    {
        Rig r;
        KV_CHECK(c, r.start());
        KTable* pt = engine_ktable(r.e, "player.params");
        KTable* st = engine_ktable(r.e, "player.presets");
        KV_CHECK(c, pt && st);
        auto param = [&](u32 stage, u32 index) {
            Span<PlayerParam> rows = pt->rows_of<PlayerParam>();
            for (u32 i = 0; i < rows.len; ++i) if (pt->is_alive(i) && rows[i].stage == stage && rows[i].index == index) return rows[i];
            return PlayerParam{};
        };
        u32 nparams = 0, npresets = 0, bass = ~0u;
        for (u32 i = 0; i < pt->rows_of<PlayerParam>().len; ++i) nparams += pt->is_alive(i);
        Span<PlayerPreset> presets = st->rows_of<PlayerPreset>();
        for (u32 i = 0; i < presets.len; ++i) {
            if (!st->is_alive(i)) continue;
            ++npresets;
            if (strcmp(presets[i].name, "bass") == 0 && presets[i].stage == 1) bass = presets[i].index;
        }
        KV_CHECK(c, nparams == 42 && npresets == 7 && bass == 1);
        PlayerParam gain = param(0, 0), mute = param(0, 1), b3gain = param(1, 14), b0freq = param(1, 1);
        KV_CHECK(c, strcmp(gain.name, "gain_db") == 0 && strcmp(gain.unit, "dB") == 0 && gain.lo == -100.0 && gain.hi == 0.0 && gain.step == 0.5 && gain.value == 0.0);
        KV_CHECK(c, strcmp(mute.name, "mute") == 0 && mute.lo == 0.0 && mute.hi == 1.0 && mute.step == 1.0 && mute.unit[0] == 0);
        KV_CHECK(c, strcmp(b3gain.name, "b3.gain") == 0 && b3gain.lo == -24.0 && b3gain.hi == 24.0 && strcmp(b0freq.unit, "Hz") == 0 && b0freq.initial == 31.25);

        r.add(data("tone_16_2ch_8000_long.flac"));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(8000));
        u32 still = 0;
        while (r.t().position < 2000 && still < 1000) r.pace(&still);
        r.say(PlayerApplyPreset{1, bass});
        f64 until = time_now() + 2.0;
        while (g_nparam < 40 && time_now() < until) r.pace(&still);
        KTable* chain = engine_ktable(r.e, "player.chain");
        KV_CHECK(c, g_nparam == 40);                                         // each of the forty, said as heard
        KV_CHECK(c, param(1, 2).value == 6.0 && param(1, 6).value == 4.0 && param(1, 10).value == 2.0 && param(1, 14).value == 0.0);
        KV_CHECK(c, chain && !chain->rows_of<PlayerStage>()[1].flat && chain->rows_of<PlayerStage>()[1].params[2] == 6.0 && !r.o().exact);
        // A preset that is not there changes nothing.
        r.say(PlayerApplyPreset{1, 99});
        r.say(PlayerApplyPreset{0, 0});
        for (u32 i = 0; i < 10; ++i) r.pace(&still);
        KV_CHECK(c, g_nparam == 40 && param(1, 2).value == 6.0);
        // Flat again: exact again.
        r.say(PlayerApplyPreset{1, 0});
        until = time_now() + 2.0;
        while (g_nparam < 80 && time_now() < until) r.pace(&still);
        for (u32 i = 0; i < 20; ++i) r.pace(&still);
        KV_CHECK(c, g_nparam == 80 && param(1, 2).value == 0.0 && chain->rows_of<PlayerStage>()[1].flat && r.o().exact);
        KV_CHECK(c, r.o().underruns == 0);
        r.stop();
    }

    // The queue as a list. An edit to the row after the one playing, when its audio is
    // already in the lead, is heard as the edit (a recut at the boundary); before that it
    // costs nothing. Rows have ids that survive a move, and a removed row leaves the
    // table. Removing the row playing plays what takes its place; clear stops.
    c.begin("player queue");
    {
        const char* T = "broken/truncated.flac"; // the same format as A, and shorter: told apart by length
        for (u32 kase = 0; kase < 3; ++kase) {
            Rig r;
            KV_CHECK(c, r.start(0, 256u << 10, 256u << 20, kase == 2 ? 0.1 : 5.0));
            r.add(data(A)); r.add(data(T)); r.add(data(A));
            r.say(PlayerPlay{});
            KV_CHECK(c, r.wait_lead(kase == 2 ? 1000 : kN + 8000)); // in the lead: all of 1 and the start of 2 (not with a short lead)
            u32 still = 0;
            while (r.t().position < 2000 && still < 1000) r.pace(&still);
            KV_CHECK(c, r.t().item == 1);
            if (kase == 1) r.say(PlayerQueueEdit{(u8)PlayerEditOp::Move, 3, 2});  // 1, 3, 2
            else           r.say(PlayerQueueEdit{(u8)PlayerEditOp::Remove, 2, 0}); // 1, 3
            r.step();
            KV_CHECK(c, r.t().queued == (kase == 1 ? 3u : 2u) && r.item(3).pos == 2 && (kase == 1 ? r.item(2).pos == 3 : r.item(2).id == 0));
            f64 until = time_now() + 5.0;
            while (!heard('Q', 0) && time_now() < until) r.pace(&still);
            KV_CHECK(c, log_is(kase == 1 ? "S1 E1 S3 E3 S2 E2 Q0" : "S1 E1 S3 E3 Q0"));
            HifiDeviceFormat f; u64 frames = 0;
            const u8* d = r.seg_data(0, &f, &frames);
            u64 z = 0;
            while (d && z < frames && all_zero(d + z * 4, 4)) ++z;
            KV_CHECK(c, d && md5_is(d + z * 4, kN, 2, data(A)) && md5_is(d + (z + kN) * 4, kN, 2, data(A))); // 1, then 3, exact
            if (kase != 1) KV_CHECK(c, d && all_zero(d + (z + 2 * kN) * 4, (usize)(frames - z - 2 * kN) * 4)); // and not 2
            KV_CHECK(c, r.o().underruns == 0 && r.o().recuts == (kase == 2 ? 0u : 1u)); // in the lead: a recut; not yet: none
            r.stop();
        }
        // Insert before a row; remove the row playing; clear.
        Rig r;
        KV_CHECK(c, r.start());
        r.add(data(A)); r.add(data(A));
        PlayerEnqueue q{}; format(q.path, sizeof q.path, "%s", data(T)); q.before = 2;
        r.say(q);                                       // 1, 3, 2
        r.step();
        KV_CHECK(c, r.item(3).pos == 2 && r.item(2).pos == 3);
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(kN));
        u32 still = 0;
        while (r.t().position < 2000 && still < 1000) r.pace(&still);
        r.say(PlayerQueueEdit{(u8)PlayerEditOp::Remove, 1, 0}); // the row playing: what takes its place plays
        f64 until = time_now() + 2.0;
        while (!heard('S', 3) && time_now() < until) r.pace(&still);
        KV_CHECK(c, heard('S', 3) && !heard('E', 1) && r.item(1).id == 0 && r.t().place == 3 && r.item(3).pos == 1);
        r.say(PlayerQueueEdit{(u8)PlayerEditOp::Clear, 0, 0});
        r.step(3);
        KV_CHECK(c, r.t().queued == 0 && r.t().state == (u8)PlayerState::Stopped && r.item(2).id == 0 && r.item(3).id == 0);
        r.add(data(A));                                 // and the list goes on from empty
        r.say(PlayerPlay{});
        until = time_now() + 3.0;
        while (!heard('Q', 0) && time_now() < until) r.pace(&still);
        KV_CHECK(c, heard('S', 4) && heard('E', 4) && heard('Q', 0));
        r.stop();
    }

    // The command line says the same things: its commands emit the same events.
    c.begin("player commands");
    {
        Rig r;
        KV_CHECK(c, r.start());
        Lines out;
        char line[1200];
        format(line, sizeof line, "player add \"%s\" \"%s\"", data(A), data("broken/truncated.flac"));
        KV_CHECK(c, cli_run(r.e, line, collect, &out) && strstr(out.text, "queued 2"));
        KV_CHECK(c, cli_run(r.e, "player play", collect, &out));
        KV_CHECK(c, r.wait_lead(kN));
        r.pull(3000);
        KV_CHECK(c, cli_run(r.e, "player pause", collect, &out));
        r.step();
        out = Lines{};
        KV_CHECK(c, cli_run(r.e, "player status", collect, &out));
        KV_CHECK(c, strstr(out.text, "paused; item 1") && strstr(out.text, "2 queued, 2 left"));
        KV_CHECK(c, strstr(out.text, "44100 Hz, 2 ch, s16, unshared, exact") && strstr(out.text, "playing") && strstr(out.text, A));
        KV_CHECK(c, cli_run(r.e, "player next", collect, &out) && cli_run(r.e, "player play", collect, &out));
        r.step(2);
        KV_CHECK(c, r.play_until('Q', 0));
        KV_CHECK(c, log_is("S1 S2 E2 Q0") && ev('E', 2).damaged);
        out = Lines{};
        KV_CHECK(c, cli_run(r.e, "player status", collect, &out) && strstr(out.text, "stopped") && strstr(out.text, "cut short"));
        out = Lines{};
        KV_CHECK(c, !cli_run(r.e, "player add", collect, &out));       // no file: refused
        KV_CHECK(c, !cli_run(r.e, "player pause now", collect, &out)); // an argument it does not take
        r.stop();
    }

    // Files larger than the memory cap, in 4 KB pieces: each starts before it is all read,
    // plays exact, and the next follows it in the same stream. Never more than the cap
    // is held.
    c.begin("player in pieces");
    {
        Rig r;
        const usize cap = 40u << 10; // the file is 56 KB
        KV_CHECK(c, r.start(0, 4096, cap, 0.05));
        r.add(data(A)); r.add(data(A));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(2000));
        u64 most = 0;
        bool early = false, seen_s1 = false;
        f64 until = time_now() + 5.0;
        u32 still = 0;
        while (!heard('Q', 0) && time_now() < until) {
            r.pace(&still);
            if (r.o().bytes_held > most) most = r.o().bytes_held;
            if (!seen_s1 && heard('S', 1)) { seen_s1 = true; early = r.item(1).bytes_read < r.item(1).size && r.item(1).size == 56367; }
        }
        KV_CHECK(c, log_is("S1 E1 S2 E2 Q0"));
        KV_CHECK(c, early && most <= cap && most > 0);
        KV_CHECK(c, r.o().underruns == 0 && !ev('E', 1).damaged && !ev('E', 2).damaged);
        HifiDeviceFormat f; u64 frames = 0;
        const u8* d = r.seg_data(0, &f, &frames);
        u64 z = 0; // the pacing pulls silence before the device starts: the music from its first frame
        while (d && z < frames && all_zero(d + z * 4, 4)) ++z;
        KV_CHECK(c, d && frames >= z + kN * 2 && md5_is(d + z * 4, kN, 2, data(A)) && md5_is(d + (z + kN) * 4, kN, 2, data(A)));
        KV_CHECK(c, r.o().bytes_held == 0 && r.item(1).bytes_read == 0); // both done: nothing held
        KV_INFO("test", "player in pieces: at most %llu bytes held, under a cap of %llu", (unsigned long long)most, (unsigned long long)cap);
        r.stop();
    }

    // And seeks in a file larger than the cap: back to near its start (pieces given back,
    // read again), forward near its end, and on to its end, never past the cap, the audio
    // from each target the file's own.
    c.begin("player seek under the cap");
    {
        const char* L = "tone_16_2ch_8000_long.flac"; // 74 KB, four seconds
        Pcm16 pcm = decode16(data(L));
        // 48 KB: twelve pieces, two thirds of the file. A seek needs what its search looks at
        // and a frame's window held at once (dr_flac starts the search again after a wait):
        // at ten pieces it never lands.
        const usize cap = 48u << 10;
        Rig r;
        KV_CHECK(c, r.start(0, 4096, cap, 0.05));
        r.add(data(L));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(300));
        u32 still = 0;
        u64 most = 0;
        f64 until = time_now() + 5.0;
        while (r.t().position < 20000 && time_now() < until) { r.pace(&still); if (r.o().bytes_held > most) most = r.o().bytes_held; }
        u64 targets[2] = {1000, 28000};
        bool exact = true;
        for (u32 k = 0; k < 2; ++k) {
            HifiDeviceFormat f; u64 before = 0;
            r.seg_data(0, &f, &before);
            r.say(PlayerSeek{0, targets[k]});
            until = time_now() + 3.0;
            while (count_of('K', 1) < k + 1 && time_now() < until) { r.pace(&still); if (r.o().bytes_held > most) most = r.o().bytes_held; }
            for (u32 i = 0; i < 30; ++i) { r.pace(&still); if (r.o().bytes_held > most) most = r.o().bytes_held; }
            u64 frames = 0;
            const u8* d = r.seg_data(0, &f, &frames);
            u64 at = d ? find_run16(d, before, frames, pcm, targets[k] + 80) : ~0ull; // after the 10 ms fade in
            exact = exact && at != ~0ull;
        }
        until = time_now() + 5.0;
        while (!heard('Q', 0) && time_now() < until) { r.pace(&still); if (r.o().bytes_held > most) most = r.o().bytes_held; }
        KV_CHECK(c, count_of('K', 1) == 2 && heard('E', 1) && heard('Q', 0) && exact);
        KV_CHECK(c, most <= cap && r.o().underruns == 0);
        KV_INFO("test", "player seek under the cap: at most %llu bytes held of a 74 KB file under a %llu-byte cap", (unsigned long long)most, (unsigned long long)cap);
        r.stop();
        mem_free(pcm.s);
    }

    // A file cut short on the disk while it is being read: what was read plays, and the
    // item ends damaged, saying so.
    c.begin("player cut while read");
    {
        char dir[512], path[600];
        KV_CHECK(c, io_make_temp_dir("blackwing-cut", dir, sizeof dir));
        format(path, sizeof path, "%s/cut.flac", dir);
        IoBytes b;
        KV_CHECK(c, io_read_file(data(A), &b) && io_write_file(path, b.data, b.size));
        b.free();
        Rig r;
        KV_CHECK(c, r.start(0, 4096, 32u << 10, 0.05));
        r.add(path);
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(1000));
        KV_CHECK(c, r.item(1).bytes_read <= (32u << 10));
        KV_CHECK(c, truncate(path, 36u << 10) == 0);
        f64 until = time_now() + 5.0;
        u32 still = 0;
        while (!heard('Q', 0) && time_now() < until) r.pace(&still);
        const Heard& e = ev('E', 1);
        KV_CHECK(c, log_is("S1 E1 Q0"));
        KV_CHECK(c, e.damaged && strstr(e.reason, "cut short") && e.frames > 4000 && e.frames < kN);
        KV_CHECK(c, r.item(1).damaged && strstr(r.item(1).reason, "cut short"));
        r.stop();
        io_remove_tree(dir);
    }

    // The device pulled while playing: paused at what was heard (its delay's frames were
    // not), the row gone, nothing opened anywhere; play pressed with it still gone waits
    // (the config says to), and it plugged back plays from where it was heard. Then the
    // device chosen while playing: the music moves to it from what was heard.
    c.begin("player device lost");
    {
        Rig r;
        KV_CHECK(c, r.start(1000, 256u << 10, 256u << 20, 5.0, true));
        auto row = [&](const char* name) {
            KTable* d = engine_ktable(r.e, "player.devices");
            Span<PlayerDevice> rows = d->rows_of<PlayerDevice>();
            for (u32 i = 0; i < rows.len; ++i) if (d->is_alive(i) && strcmp(rows[i].name, name) == 0) return rows[i];
            return PlayerDevice{};
        };
        r.step();
        KV_CHECK(c, row("capture").present && strcmp(r.o().chosen, "capture") == 0 && !r.o().following);
        r.add(data(A));
        r.say(PlayerPlay{});
        KV_CHECK(c, r.wait_lead(kN));
        r.pull(2000); r.pull(2000);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Playing && r.t().position == 3000 && row("capture").open);
        hifi_capture_unplug(r.cap);
        f64 until = time_now() + 2.0;
        while (!r.o().lost && time_now() < until) { r.step(); time_sleep(0.001); }
        KV_CHECK(c, r.o().lost && r.o().device[0] == 0 && r.o().none_why[0]);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Paused && r.t().item == 1 && r.t().position == 3000);
        KV_CHECK(c, dev_heard(PlayerDeviceChange::Lost) == 1);
        KV_CHECK(c, !row("capture").present && dev_heard(PlayerDeviceChange::Gone) == 1);
        r.step(20);
        KV_CHECK(c, hifi_capture_opens(r.cap) == 1);
        // Play with it gone: waits.
        r.say(PlayerPlay{});
        r.step(20);
        KV_CHECK(c, hifi_capture_opens(r.cap) == 1 && r.t().state == (u8)PlayerState::Starting && strstr(r.o().none_why, "not there"));
        // Back: it plays from where it was heard.
        hifi_capture_plug(r.cap);
        until = time_now() + 2.0;
        while (hifi_capture_opens(r.cap) < 2 && time_now() < until) { r.step(); time_sleep(0.001); }
        KV_CHECK(c, hifi_capture_opens(r.cap) == 2 && !r.o().lost);
        KV_CHECK(c, dev_heard(PlayerDeviceChange::Back) == 1 && dev_heard(PlayerDeviceChange::Moved) == 1);
        KV_CHECK(c, r.wait_lead(12000));
        r.pull(2000); r.pull(2000);
        KV_CHECK(c, r.wait_log('K', 1) && ev('K', 1).frames == 3000);
        KV_CHECK(c, r.t().state == (u8)PlayerState::Playing && r.t().position > 3000);
        // Chosen while playing: moved, from what was heard.
        u64 at = r.t().position;
        u32 moved = dev_heard(PlayerDeviceChange::Moved);
        PlayerSelectDevice sd{};
        format(sd.name, sizeof sd.name, "capture");
        r.say(sd);
        until = time_now() + 2.0;
        while (hifi_capture_opens(r.cap) < 3 && time_now() < until) { r.step(); time_sleep(0.001); }
        KV_CHECK(c, hifi_capture_opens(r.cap) == 3);
        KV_CHECK(c, dev_heard(PlayerDeviceChange::Moved) == moved + 1);
        KV_CHECK(c, r.wait_lead(12000));
        g_nlog = 0;
        r.pull(2000); r.pull(2000);
        KV_CHECK(c, r.wait_log('K', 1) && ev('K', 1).frames >= at && ev('K', 1).frames < at + 200);
        r.stop();
    }

    // Shut down in the middle of everything: reading, decoding, playing. Nothing hangs
    // and nothing leaks (the leak report at exit says).
    c.begin("player shutdown");
    {
        Rig r;
        KV_CHECK(c, r.start());
        r.add(data(A)); r.add(data("tone_24_2ch_96000.flac")); r.add(data(A));
        r.say(PlayerPlay{});
        r.step(2);
        r.stop();
        Rig q;
        KV_CHECK(c, q.start());
        q.add(data(A)); q.add(data(A));
        q.say(PlayerPlay{});
        KV_CHECK(c, q.wait_lead(kN));
        q.pull(700);
        q.stop();
    }
    log_set_level(was);
}
