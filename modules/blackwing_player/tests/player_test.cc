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
#include <initializer_list>
#include <unistd.h>
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

constinit const HandlerDesc k_handlers[] = {
    {.name = ".started", KV_ON(PlayerTrackStarted, on_started), .phase = Phase::PostSim},
    {.name = ".ended",   KV_ON(PlayerTrackEnded, on_ended),     .phase = Phase::PostSim},
    {.name = ".failed",  KV_ON(PlayerTrackFailed, on_failed),   .phase = Phase::PostSim},
    {.name = ".queue",   KV_ON(PlayerQueueEnded, on_queue_ended), .phase = Phase::PostSim},
    {.name = ".under",   KV_ON(HifiUnderrun, on_underrun),      .phase = Phase::PostSim},
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

    bool start(u32 delay = 0, u32 piece = 256u << 10, usize memory = 256u << 20, f64 lead = 5.0) {
        g_nlog = 0;
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
    void add(const char* path) { PlayerEnqueue q{}; format(q.path, sizeof q.path, "%s", path); e.events.emit(q); }
    template <class E> void say(const E& ev) { e.events.emit(ev); }

    // Steps (sleeping a little between, for the I/O and the render thread) until the lead
    // holds `frames`, or two seconds pass.
    bool wait_lead(u64 frames) {
        f64 until = time_now() + 2.0;
        for (;;) { step(); if (o().lead_frames >= frames) return true; if (time_now() > until) return false; time_sleep(0.0005); }
    }
    // Steps until the lead has stopped changing: the reads have landed and the render
    // thread has decoded all it is going to. For after a command whose effect on the lead
    // cannot be named as a number (a next: some dropped, some put back).
    void settle() {
        u32 last = ~0u, same = 0;
        f64 until = time_now() + 2.0;
        while (same < 3 && time_now() < until) {
            time_sleep(0.004);
            step();
            u32 lead = o().lead_frames;
            same = lead == last ? same + 1 : 0;
            last = lead;
        }
    }
    bool wait_log(char kind, u64 item) {
        f64 until = time_now() + 2.0;
        for (;;) { step(); if (heard(kind, item)) return true; if (time_now() > until) return false; time_sleep(0.0005); }
    }
    u32 pull(u32 frames) { u32 n = hifi_capture_pull(cap, frames); step(); return n; }
    // Paces a device against a small lead: pulls what is surely there, keeping back more
    // than the callback's ramp (which would fade a lead about to run dry), and the rest
    // only once the lead has stopped growing (the stream's end).
    void pace(u32* still) {
        time_sleep(0.0005);
        step();
        u32 lead = o().lead_frames;
        if (lead > 1200) { pull(lead - 1000 > 1024 ? 1024 : lead - 1000); *still = 0; }
        else if (++*still > 40) { pull(lead ? lead : 256); *still = 0; }
    }
    // Pulls as a device would, not as fast as a loop can: a driven clock outruns any
    // decoder, and frames taken before the decoder has made the next ones are an underrun
    // no hardware would cause. A short sleep, then at most what is in the lead (or a
    // little silence while the next file is on its way). Until an event, or two seconds.
    bool play_until(char kind, u64 item) {
        f64 until = time_now() + 2.0;
        while (!heard(kind, item)) {
            if (time_now() > until) return false;
            time_sleep(0.001);
            step();
            u32 lead = o().lead_frames;
            pull(lead > 1024 ? 1024 : lead > 0 ? lead : 256);
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
        KV_CHECK(c, r.item(1).state == (u8)PlayerItemState::Waiting && strstr(r.item(2).path, A) != nullptr);
        r.step(5);
        KV_CHECK(c, r.item(1).state == (u8)PlayerItemState::Waiting && !r.o().open); // queued is not playing: nothing is read
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
        KV_CHECK(c, r.item(1).state == (u8)PlayerItemState::Done && r.item(2).state == (u8)PlayerItemState::Done && !r.item(2).damaged);
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
        HifiDeviceFormat f; u64 frames;
        const u8* d = r.seg_data(0, &f, &frames);
        KV_CHECK(c, d && all_zero(d + held * 4, (usize)(frames - held) * 4)); // silence from the end of the ramp
        r.say(PlayerPlay{});
        r.step();
        KV_CHECK(c, r.t().state == (u8)PlayerState::Playing);
        KV_CHECK(c, r.play_until('Q', 0));
        KV_CHECK(c, log_is("S1 E1 Q0") && r.o().underruns == 0);
        r.stop();
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
        KV_CHECK(c, r.item(4).state == (u8)PlayerItemState::Done && r.item(4).damaged && r.item(4).reason[0]);
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
        KV_CHECK(c, r.item(1).state == (u8)PlayerItemState::Done && r.t().item == 0);
        r.settle();                                   // items 2 and 3, put back behind the flush
        KV_CHECK(c, r.o().lead_frames >= kN * 2);
        r.pull(2000);
        KV_CHECK(c, log_is("S1 S2") && r.t().item == 2);
        HifiDeviceFormat f; u64 frames;
        const u8* d = r.seg_data(0, &f, &frames);
        // After the ramp out of item 1 (at most 220 frames), item 2 from its first frame, exact.
        u64 start = 5000;
        while (start < 5000 + 222 && !md5_is(d + start * 4, 64, 2, data(A)) && memcmp(d + start * 4, d, 256) != 0) ++start;
        KV_CHECK(c, start <= 5000 + 220 && memcmp(d + start * 4, d, (usize)(frames - start) * 4) == 0);

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

    // A file cut short on the disk while it is being read: what was read plays, and the
    // item ends damaged, saying so.
    c.begin("player cut while read");
    {
        char dir[512], path[600];
        KV_CHECK(c, io_make_temp_dir("blackwing-cut", dir, sizeof dir));
        format(path, sizeof path, "%s/cut.flac", dir);
        IoBytes b;
        KV_CHECK(c, io_read_now(data(A), &b) && io_write_now(path, b.data, b.size));
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
