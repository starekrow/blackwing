// The audio path, played into the capture device with its clock driven by the test: the
// test decides when each frame is taken, so what was captured, and when each event
// arrived, can be checked to the frame.
//
// The files are hifi_codec's (modules/hifi_codec/tests/data). "Exact" is FLAC's own MD5
// again, now at the device's side of the path, and a direct comparison with what the
// decoder gives.
#include "korvid/hifi/hifi.h"
#include "korvid/hifi_codec/flac.h"
#include "korvid/hifi_platform/capture.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "../../hifi_codec/tests/md5.h"
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace kv;

namespace {

struct File { u8* data = nullptr; u32 len = 0; Span<const u8> bytes() const { return Span<const u8>(data, len); } };

File read_data(const char* name) {
    char path[1024];
    const char* here = __FILE__;
    const char* slash = strrchr(here, '/');
    format(path, sizeof path, "%.*s/../../hifi_codec/tests/data/%s", slash ? (int)(slash - here) : 0, here, name);
    File f;
    FILE* fp = fopen(path, "rb");
    if (!fp) { KV_ERROR("test", "cannot open %s", path); return f; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f.data = (u8*)mem_alloc((usize)n + 1);
    f.len  = (u32)fread(f.data, 1, (usize)n, fp);
    fclose(fp);
    return f;
}
void drop(File& f) { mem_free(f.data); f = File{}; }

// A file decoded straight through the decoder: what the path must reproduce.
struct Pcm { i32* s = nullptr; u64 frames = 0; AudioFormat fmt; };
Pcm decode(const File& f) {
    Pcm p;
    const DecoderDesc& d = k_flac_decoder;
    u8* mem = (u8*)mem_alloc(d.memory_bytes);
    DecoderOpen o = d.open(sparse_whole(f.bytes()), Span<u8>(mem, d.memory_bytes));
    if (o.state) {
        p.fmt = o.info.format;
        u64 cap = o.info.frames + 16;
        p.s = (i32*)mem_alloc((usize)cap * p.fmt.channels * 4);
        for (;;) {
            DecodeResult r = d.decode(o.state, p.s + p.frames * p.fmt.channels, (u32)(cap - p.frames));
            p.frames += r.frames;
            if (r.status != DecodeStatus::Ok || p.frames == cap) break;
        }
        d.close(o.state);
    }
    mem_free(mem);
    return p;
}
void drop(Pcm& p) { mem_free(p.s); p = Pcm{}; }

// The test's end of a path: a capture device, a Hifi on it, and the events so far.
struct Rig {
    HifiCapture* cap = nullptr;
    Hifi*        h   = nullptr;
    HifiEvent    events[256];
    u32          nevents = 0;

    void make(u32 delay = 0, f64 lead = 5.0, bool wall = false) {
        HifiCaptureDesc cd;
        cd.delay_frames = delay;
        cd.wall_clock   = wall;
        cd.keep_bytes   = 16u << 20;
        cap = hifi_capture_create(cd);
        HifiConfig hc;
        hc.device = "capture";
        hc.device_backend = cap;
        hc.lead_seconds = lead;
        h = hifi_create(hc);
    }
    void free() { hifi_destroy(h); hifi_capture_destroy(cap); *this = Rig{}; }

    void poll() { nevents += hifi_poll(h, events + nevents, 256 - nevents); }
    HifiStatus status() { HifiStatus s; hifi_status(h, &s); return s; }
    u32 pull(u32 frames) { u32 n = hifi_capture_pull(cap, frames); poll(); return n; }

    // Waits (up to two seconds) for the render thread to have written `frames` stream frames.
    bool wait_written(u64 frames) {
        f64 until = time_now() + 2.0;
        while (status().written < frames) { if (time_now() > until) return false; time_sleep(0.0005); }
        return true;
    }
    // Waits for an event of a kind about a source; polls as it goes.
    // Waits for the render thread to have carried out the last flush.
    bool wait_flushed() {
        f64 until = time_now() + 2.0;
        while (status().flushing) { if (time_now() > until) return false; time_sleep(0.0005); }
        return true;
    }
    // Waits for the device to be held (or let go), as the render thread decides.
    bool wait_held(bool held) {
        f64 until = time_now() + 2.0;
        while (status().held != held) { if (time_now() > until) return false; time_sleep(0.0005); }
        return true;
    }
    bool wait_event(HifiEventKind kind, u64 source) {
        f64 until = time_now() + 2.0;
        for (;;) {
            poll();
            if (has(kind, source)) return true;
            if (time_now() > until) return false;
            time_sleep(0.0005);
        }
    }
    bool has(HifiEventKind kind, u64 source) const { return find(kind, source) != nullptr; }
    const HifiEvent* find(HifiEventKind kind, u64 source) const {
        for (u32 i = 0; i < nevents; ++i)
            if (events[i].kind == kind && events[i].source == source) return &events[i];
        return nullptr;
    }
    u32 count(HifiEventKind kind) const { u32 n = 0; for (u32 i = 0; i < nevents; ++i) n += events[i].kind == kind; return n; }
    u32 index(HifiEventKind kind, u64 source) const {
        for (u32 i = 0; i < nevents; ++i) if (events[i].kind == kind && events[i].source == source) return i;
        return ~0u;
    }
};

// One captured sample as the integer it stands for.
i32 sample_at(const u8* data, const HifiDeviceFormat& f, u64 index) {
    if (f.sample == HifiSampleFormat::S16) { i16 v; memcpy(&v, data + index * 2, 2); return v; }
    i32 v; memcpy(&v, data + index * 4, 4); return v;
}

// Whether `frames` captured frames at `seg_frame` are exactly `pcm` from `pcm_frame`
// (the source's bits, shifted up into the device's word).
bool same(const u8* seg, const HifiDeviceFormat& f, u64 seg_frame, const Pcm& pcm, u64 pcm_frame, u64 frames) {
    u32 shift = hifi_sample_bits(f.sample) - pcm.fmt.bits;
    for (u64 i = 0; i < frames * f.channels; ++i)
        if (sample_at(seg, f, seg_frame * f.channels + i) != (i32)((u32)pcm.s[pcm_frame * f.channels + i] << shift)) return false;
    return true;
}

bool silent(const u8* seg, const HifiDeviceFormat& f, u64 from_frame, u64 frames) {
    for (u64 i = 0; i < frames * f.channels; ++i)
        if (sample_at(seg, f, from_frame * f.channels + i) != 0) return false;
    return true;
}

// The MD5 FLAC would compute over the first `frames` captured frames of a segment.
void capture_md5(const u8* seg, const HifiDeviceFormat& f, u64 frames, u8 out[16]) {
    kvtest::Md5 m;
    if (f.sample == HifiSampleFormat::S16) m.update(seg, (usize)frames * f.channels * 2);
    else for (u64 i = 0; i < frames * f.channels; ++i) m.update(seg + i * 4, 3); // S24: the low three bytes
    m.finish(out);
}

struct Good { const char* name; u64 frames; };
constexpr Good kGood[] = {
    {"tone_16_1ch_44100.flac", 22050},  {"tone_16_2ch_44100.flac", 22050}, {"tone_16_2ch_48000.flac", 24000},
    {"tone_24_2ch_48000.flac", 24000},  {"tone_24_1ch_96000.flac", 24000}, {"tone_24_2ch_96000.flac", 24000},
    {"tone_16_2ch_192000.flac", 48000}, {"tone_24_2ch_192000.flac", 48000},
};

} // namespace

// Every good file, through the whole path, pulled in uneven pieces: what reaches the
// device hashes to the file's own MD5, and nothing but silence follows it.
void test_hifi_exact(TestCtx& c) {
    c.begin("hifi exact");
    for (const Good& g : kGood) {
        File f = read_data(g.name);
        Rig r; r.make();
        hifi_add_source(r.h, {1, &k_flac_decoder, sparse_whole(f.bytes())});
        KV_CHECK(c, r.wait_written(g.frames));
        const u32 sizes[] = {1, 7, 480, 512, 1024, 333, 4096, 2, 9000};
        u64 pulled = 0;
        for (u32 i = 0; pulled < g.frames + 3000; ++i) pulled += r.pull(sizes[i % 9]);
        HifiCaptureSegment seg[4];
        KV_CHECK(c, hifi_capture_segments(r.cap, seg, 4) == 1);
        const u8* data = hifi_capture_data(r.cap) + seg[0].offset;
        u8 md5[16];
        capture_md5(data, seg[0].format, g.frames, md5);
        KV_CHECK(c, memcmp(md5, f.data + 26, 16) == 0);
        KV_CHECK(c, silent(data, seg[0].format, g.frames, pulled - g.frames));
        KV_CHECK(c, r.wait_event(HifiEventKind::Released, 1));
        HifiStatus s = r.status();
        KV_CHECK(c, s.exact && !s.shared && s.device_open);
        KV_CHECK(c, s.taken == g.frames && s.heard == g.frames && s.underruns == 0 && s.source == 0);
        KV_CHECK(c, r.has(HifiEventKind::Started, 1) && r.has(HifiEventKind::Ended, 1) && r.has(HifiEventKind::Released, 1));
        KV_CHECK(c, r.count(HifiEventKind::Underrun) == 0 && r.count(HifiEventKind::Failed) == 0);
        const HifiEvent* e = r.find(HifiEventKind::Ended, 1);
        KV_CHECK(c, e && e->status == DecodeStatus::End && e->frames == g.frames);
        r.free();
        drop(f);
    }
}

// Two files of one format, one after the other: one device, one stream, the second's
// first frame straight after the first's last.
void test_hifi_join(TestCtx& c) {
    c.begin("hifi join");
    File fa = read_data("tone_16_2ch_44100.flac"), fb = read_data("tone_16_2ch_44100.flac");
    File fc = read_data("tone_16_1ch_44100.flac"); // not joined to: only to differ from A in content below
    Pcm a = decode(fa);
    // B is A's bytes with a different id: the join is checked by position, and by content against A twice.
    Rig r; r.make();
    hifi_add_source(r.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    hifi_add_source(r.h, {2, &k_flac_decoder, sparse_whole(fb.bytes())});
    KV_CHECK(c, r.wait_written(a.frames * 2));
    u64 pulled = 0;
    while (pulled < a.frames * 2 + 1000) pulled += r.pull(777);
    HifiCaptureSegment seg[4];
    KV_CHECK(c, hifi_capture_segments(r.cap, seg, 4) == 1 && hifi_capture_opens(r.cap) == 1);
    const u8* data = hifi_capture_data(r.cap);
    KV_CHECK(c, same(data, seg[0].format, 0, a, 0, a.frames));
    KV_CHECK(c, same(data, seg[0].format, a.frames, a, 0, a.frames)); // no frame between them, none lost
    KV_CHECK(c, silent(data, seg[0].format, a.frames * 2, pulled - a.frames * 2));
    KV_CHECK(c, r.index(HifiEventKind::Started, 1) < r.index(HifiEventKind::Ended, 1));
    KV_CHECK(c, r.index(HifiEventKind::Ended, 1) < r.index(HifiEventKind::Started, 2));
    KV_CHECK(c, r.index(HifiEventKind::Started, 2) < r.index(HifiEventKind::Ended, 2));
    KV_CHECK(c, r.status().underruns == 0 && r.status().taken == a.frames * 2);
    r.free();

    // The second added only after the first is wholly decoded and partly played: still one stream.
    Rig q; q.make();
    hifi_add_source(q.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    KV_CHECK(c, q.wait_written(a.frames));
    q.pull(10000);
    hifi_add_source(q.h, {2, &k_flac_decoder, sparse_whole(fb.bytes())});
    KV_CHECK(c, q.wait_written(a.frames * 2));
    pulled = 10000;
    while (pulled < a.frames * 2 + 500) pulled += q.pull(1024);
    KV_CHECK(c, hifi_capture_segments(q.cap, seg, 4) == 1);
    data = hifi_capture_data(q.cap);
    KV_CHECK(c, same(data, seg[0].format, 0, a, 0, a.frames) && same(data, seg[0].format, a.frames, a, 0, a.frames));
    KV_CHECK(c, q.status().underruns == 0);
    q.free();
    drop(a); drop(fa); drop(fb); drop(fc);
}

// Started and Ended arrive when the listener hears them: with a device that says it is
// 1000 frames from the ear, 1000 frames after the callback took them, to the frame.
void test_hifi_clock(TestCtx& c) {
    c.begin("hifi clock");
    File fa = read_data("tone_16_2ch_44100.flac"), fb = read_data("tone_16_2ch_44100.flac");
    const u64 n = 22050, delay = 1000;
    Rig r; r.make((u32)delay);
    hifi_add_source(r.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    hifi_add_source(r.h, {2, &k_flac_decoder, sparse_whole(fb.bytes())});
    KV_CHECK(c, r.wait_written(n * 2));
    r.poll();
    KV_CHECK(c, !r.has(HifiEventKind::Started, 1)); // decoded, queued, in the lead: not heard
    r.pull((u32)delay);
    KV_CHECK(c, !r.has(HifiEventKind::Started, 1) && r.status().heard == 0); // taken, still on its way
    r.pull(1);
    KV_CHECK(c, r.has(HifiEventKind::Started, 1) && r.status().heard == 1);
    KV_CHECK(c, r.status().source == 1 && r.status().position == 1);
    r.pull(5000 - 1);
    KV_CHECK(c, r.status().position == 5000 && r.status().taken == 5000 + delay);

    // The join: B's first frame is stream frame n, heard when n + delay + 1 have been taken.
    r.pull((u32)(n - 5000 - 1));                     // taken: n + delay - 1; heard: n - 1
    KV_CHECK(c, !r.has(HifiEventKind::Ended, 1) && r.status().position == n - 1);
    r.pull(1);                                       // heard: n. A's last frame has been heard
    KV_CHECK(c, r.has(HifiEventKind::Ended, 1) && !r.has(HifiEventKind::Started, 2));
    r.pull(1);                                       // heard: n + 1. B's first has
    KV_CHECK(c, r.has(HifiEventKind::Started, 2) && r.status().source == 2 && r.status().position == 1);

    // The end: the callback runs out of frames and writes silence, which pushes the last
    // real frames out to the ear. Ended(B) comes when `delay` frames of silence have gone.
    r.pull((u32)(n - delay - 1));                    // taken: 2n. Everything real is taken
    KV_CHECK(c, r.status().taken == 2 * n && !r.has(HifiEventKind::Ended, 2) && r.status().heard == 2 * n - delay);
    r.pull((u32)delay - 1);
    KV_CHECK(c, !r.has(HifiEventKind::Ended, 2));
    r.pull(1);
    KV_CHECK(c, r.has(HifiEventKind::Ended, 2) && r.status().heard == 2 * n && r.status().source == 0);
    r.free();
    drop(fa); drop(fb);
}

// A change of rate and depth between sources: the lead drains, the device is opened
// again, and each side of the change is exact.
void test_hifi_format_change(TestCtx& c) {
    c.begin("hifi format change");
    File fa = read_data("tone_16_2ch_44100.flac"), fb = read_data("tone_24_2ch_96000.flac"), fc = read_data("tone_16_2ch_44100.flac");
    Pcm a = decode(fa), b = decode(fb);
    Rig r; r.make(256);
    hifi_add_source(r.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    hifi_add_source(r.h, {2, &k_flac_decoder, sparse_whole(fb.bytes())});
    hifi_add_source(r.h, {3, &k_flac_decoder, sparse_whole(fc.bytes())});
    // A driven clock can take frames far faster than any decoder makes them, which no
    // device does. So each stretch is pulled only once it is in the lead: wait for the
    // source to be written, pull until it has ended, then pull silence (the device
    // playing out its delay) until the next device is open.
    const u64 written_by[3] = {a.frames, a.frames + b.frames, a.frames * 2 + b.frames};
    for (u32 k = 0; k < 3; ++k) {
        f64 until = time_now() + 3.0;
        while (hifi_capture_opens(r.cap) < k + 1 && time_now() < until) { r.pull(256); time_sleep(0.0005); }
        KV_CHECK(c, r.wait_written(written_by[k]));
        for (u32 i = 0; i < 400 && !r.has(HifiEventKind::Ended, k + 1); ++i) r.pull(1000);
    }
    KV_CHECK(c, r.has(HifiEventKind::Ended, 1) && r.has(HifiEventKind::Ended, 2) && r.has(HifiEventKind::Ended, 3));
    HifiCaptureSegment seg[8];
    u32 nseg = hifi_capture_segments(r.cap, seg, 8);
    KV_CHECK(c, nseg == 3 && hifi_capture_opens(r.cap) == 3);
    const u8* data = hifi_capture_data(r.cap);
    if (nseg == 3) {
        KV_CHECK(c, seg[0].format.rate == 44100 && seg[0].format.sample == HifiSampleFormat::S16);
        KV_CHECK(c, seg[1].format.rate == 96000 && seg[1].format.sample == HifiSampleFormat::S24);
        KV_CHECK(c, seg[2].format.rate == 44100 && seg[2].format.sample == HifiSampleFormat::S16);
        // Each device was started before its source was in the lead, so silence may lead.
        // Find where the music starts and check all of it from there.
        for (u32 k = 0; k < 3; ++k) {
            const Pcm& p = k == 1 ? b : a;
            const u8* s = data + seg[k].offset;
            u64 frames = seg[k].bytes / hifi_frame_bytes(seg[k].format), start = 0;
            while (start < frames && silent(s, seg[k].format, start, 1)) ++start;
            KV_CHECK(c, start + p.frames <= frames);
            KV_CHECK(c, start + p.frames <= frames && same(s, seg[k].format, start, p, 0, p.frames));
            KV_CHECK(c, start + p.frames <= frames && silent(s, seg[k].format, start + p.frames, frames - start - p.frames));
        }
    }
    KV_CHECK(c, r.status().underruns == 0);
    KV_CHECK(c, r.index(HifiEventKind::Ended, 1) < r.index(HifiEventKind::Started, 2) && r.index(HifiEventKind::Ended, 2) < r.index(HifiEventKind::Started, 3));
    r.free();
    drop(a); drop(b); drop(fa); drop(fb); drop(fc);
}

// The largest step between one captured sample and the next in a channel, over a span:
// a click is a step far larger than the music's own.
i32 largest_step(const u8* seg, const HifiDeviceFormat& f, u64 from, u64 frames) {
    i32 worst = 0;
    for (u64 i = from + 1; i < from + frames; ++i)
        for (u32 ch = 0; ch < f.channels; ++ch) {
            i32 d = sample_at(seg, f, i * f.channels + ch) - sample_at(seg, f, (i - 1) * f.channels + ch);
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
    return worst;
}

// Pause holds the playhead after a ramp down; play lets it go with a ramp up. No frame
// of the source is lost, and neither edge is a step.
void test_hifi_pause(TestCtx& c) {
    c.begin("hifi pause");
    File fa = read_data("tone_16_2ch_44100.flac");
    Pcm a = decode(fa);
    const u32 ramp = 220; // 5 ms at 44.1 kHz
    Rig r; r.make();
    hifi_add_source(r.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    KV_CHECK(c, r.wait_written(a.frames));
    r.pull(4000);
    hifi_pause(r.h);
    r.pull(1000);                                   // the ramp, then held
    u64 held = r.status().taken;
    KV_CHECK(c, held > 4000 && held <= 4000 + ramp && r.status().paused);
    r.pull(3000);
    KV_CHECK(c, r.status().taken == held);          // nothing taken while paused
    // Played out to the pause: the device is held, so a paused player costs nothing.
    KV_CHECK(c, r.wait_held(true) && hifi_capture_held(r.cap) && hifi_capture_holds(r.cap) == 1);
    hifi_play(r.h);
    KV_CHECK(c, !r.status().held && !hifi_capture_held(r.cap)); // let go at once, not when the render thread wakes
    u64 pulled = 8000;
    while (pulled < a.frames + 12000) pulled += r.pull(512);
    KV_CHECK(c, r.status().taken == a.frames && r.has(HifiEventKind::Ended, 1) && r.status().underruns == 0);
    // Nothing left to play, and all of it heard: held again.
    KV_CHECK(c, r.wait_held(true) && hifi_capture_holds(r.cap) == 2);
    // Something to play lets it go.
    File fb = read_data("tone_16_2ch_44100.flac");
    hifi_add_source(r.h, {2, &k_flac_decoder, sparse_whole(fb.bytes())});
    KV_CHECK(c, r.wait_held(false) && !hifi_capture_held(r.cap));
    KV_CHECK(c, r.wait_written(a.frames + 1000));

    HifiCaptureSegment seg[2];
    hifi_capture_segments(r.cap, seg, 2);
    const u8* data = hifi_capture_data(r.cap);
    const HifiDeviceFormat& f = seg[0].format;
    KV_CHECK(c, same(data, f, 0, a, 0, 4000));                              // exact up to the pause
    KV_CHECK(c, silent(data, f, held, 8000 - held));                        // silence while held
    KV_CHECK(c, same(data, f, 8000 + ramp, a, held + ramp, a.frames - held - ramp)); // exact again after the ramp up
    i32 music = largest_step(data, f, 0, 4000);
    KV_CHECK(c, largest_step(data, f, 3990, 600) <= music);                 // into the pause: no step bigger than the music's
    KV_CHECK(c, largest_step(data, f, 7900, 600) <= music);                 // and out of it
    // The frames in the ramps are the source's, scaled down: never louder than the source.
    bool quieter = true;
    for (u64 i = 0; i < (held - 4000) * 2; ++i) {
        i32 got = sample_at(data, f, 4000 * 2 + i), want = a.s[4000 * 2 + i];
        if ((got < 0 ? -got : got) > (want < 0 ? -want : want)) quieter = false;
    }
    KV_CHECK(c, quieter);
    r.free();
    drop(a); drop(fa); drop(fb);
}

// Flush drops what is playing (with the ramp) and what is queued; what is added next
// starts clean, exact from its first frame.
void test_hifi_flush(TestCtx& c) {
    c.begin("hifi flush");
    File fa = read_data("tone_16_2ch_44100.flac"), fb = read_data("tone_16_2ch_44100.flac"), fq = read_data("tone_16_2ch_44100.flac");
    Pcm a = decode(fa);
    const u32 ramp = 220;
    Rig r; r.make();
    hifi_add_source(r.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    hifi_add_source(r.h, {2, &k_flac_decoder, sparse_whole(fq.bytes())});   // queued behind it: never heard
    KV_CHECK(c, r.wait_written(a.frames * 2));
    r.pull(6000);
    KV_CHECK(c, r.status().source == 1);
    hifi_flush(r.h);
    hifi_add_source(r.h, {3, &k_flac_decoder, sparse_whole(fb.bytes())});
    KV_CHECK(c, r.wait_flushed() && r.wait_event(HifiEventKind::Released, 2) && r.has(HifiEventKind::Released, 1));
    KV_CHECK(c, r.wait_written(a.frames * 3));
    KV_CHECK(c, r.status().source == 0);
    u64 pulled = 6000;
    while (pulled < a.frames + 12000) pulled += r.pull(1024);

    HifiCaptureSegment seg[2];
    KV_CHECK(c, hifi_capture_segments(r.cap, seg, 2) == 1);
    const u8* data = hifi_capture_data(r.cap);
    const HifiDeviceFormat& f = seg[0].format;
    KV_CHECK(c, same(data, f, 0, a, 0, 6000));
    // After the ramp (at most `ramp` frames of A fading), source 3 from its first frame, exact.
    u64 start = 6000;
    while (start < 6000 + ramp + 2 && !same(data, f, start, a, 0, 64)) ++start;
    KV_CHECK(c, start <= 6000 + ramp);
    KV_CHECK(c, same(data, f, start, a, 0, a.frames));
    KV_CHECK(c, largest_step(data, f, 5990, start - 5990) <= largest_step(data, f, 0, 4000));
    KV_CHECK(c, r.has(HifiEventKind::Started, 3) && r.has(HifiEventKind::Ended, 3));
    KV_CHECK(c, !r.has(HifiEventKind::Ended, 1) && !r.has(HifiEventKind::Started, 2) && !r.has(HifiEventKind::Ended, 2));
    KV_CHECK(c, r.wait_event(HifiEventKind::Released, 3));
    KV_CHECK(c, r.status().underruns == 0 && r.count(HifiEventKind::Released) == 3);
    r.free();

    // A flush with nothing after it: silence, and no underrun counted for the cut.
    Rig q; q.make();
    hifi_add_source(q.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    KV_CHECK(c, q.wait_written(a.frames));
    q.pull(3000);
    hifi_flush(q.h);
    KV_CHECK(c, q.wait_flushed() && q.wait_event(HifiEventKind::Released, 1));
    q.pull(5000);
    hifi_capture_segments(q.cap, seg, 2);
    KV_CHECK(c, silent(hifi_capture_data(q.cap), seg[0].format, 3000 + ramp, 5000 - ramp));
    KV_CHECK(c, q.status().underruns == 0 && !q.has(HifiEventKind::Ended, 1));
    q.free();

    // A flush, a wait with the device pulling silence, a second flush, and only then
    // something new: it starts clean and is announced, as after one flush.
    Rig w; w.make();
    hifi_add_source(w.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    hifi_add_source(w.h, {2, &k_flac_decoder, sparse_whole(fq.bytes())});
    KV_CHECK(c, w.wait_written(a.frames * 2));
    w.pull(5000);
    hifi_flush(w.h);
    hifi_add_source(w.h, {3, &k_flac_decoder, sparse_whole(fb.bytes())});
    KV_CHECK(c, w.wait_flushed() && w.wait_written(a.frames * 3));
    w.pull(2000);
    KV_CHECK(c, w.has(HifiEventKind::Started, 3) && w.status().source == 3);
    hifi_flush(w.h);
    KV_CHECK(c, w.wait_flushed());
    w.pull(1000);
    w.pull(1000);
    KV_CHECK(c, w.status().source == 0 && w.status().lead_frames == 0);
    hifi_add_source(w.h, {4, &k_flac_decoder, sparse_whole(fb.bytes())});
    KV_CHECK(c, w.wait_written(a.frames * 4));
    u64 before = w.status().taken;
    w.pull(100);
    KV_CHECK(c, w.status().taken == before + 100);
    KV_CHECK(c, w.has(HifiEventKind::Started, 4) && w.status().source == 4 && w.status().position == 100);
    if (!w.has(HifiEventKind::Started, 4)) KV_ERROR("test", "taken %llu heard %llu written %llu before %llu", (unsigned long long)w.status().taken, (unsigned long long)w.status().heard, (unsigned long long)w.status().written, (unsigned long long)before);
    w.free();
    drop(a); drop(fa); drop(fb); drop(fq);
}

// A decoder that can be made to stall: FLAC's, with a gate in front of decode.
u32 g_stall = 0; // atomic
DecodeResult stalling_decode(void* state, i32* out, u32 frames) {
    while (atomic_load(&g_stall, MO_ACQUIRE)) time_sleep(0.0005);
    return k_flac_decoder.decode(state, out, frames);
}

// A source that stops arriving. While there is lead, nothing is heard of it. When the
// lead runs out there is one underrun: a ramp to silence, not a cut. When the source
// comes back, playback does too, and no frame of it is lost.
void test_hifi_underrun(TestCtx& c) {
    c.begin("hifi underrun");
    DecoderDesc slow = k_flac_decoder;
    slow.decode = stalling_decode;
    File fa = read_data("tone_16_2ch_192000.flac");  // 48000 frames
    Pcm a = decode(fa);
    const u32 ramp = 960;                            // 5 ms at 192 kHz
    Rig r; r.make(0, 0.1);                           // a lead of 0.1 s: 19200 frames
    hifi_add_source(r.h, {1, &slow, sparse_whole(fa.bytes())});
    KV_CHECK(c, r.wait_written(19200));              // the lead is full
    atomic_store(&g_stall, 1u, MO_RELEASE);
    time_sleep(0.02);                                // the render thread is in the stalled decode by now, or holding a chunk it can still write
    r.pull(8000);                                    // well inside the lead
    KV_CHECK(c, r.status().underruns == 0 && r.count(HifiEventKind::Underrun) == 0);
    time_sleep(0.02);
    u64 before = r.status().written;                 // all there will be until the stall ends
    u64 pulled = 8000;
    while (pulled < before + 6000) pulled += r.pull(1000);
    KV_CHECK(c, r.status().written == before && r.status().taken == before);
    KV_CHECK(c, r.status().underruns == 1 && r.count(HifiEventKind::Underrun) == 1);
    KV_CHECK(c, !r.has(HifiEventKind::Ended, 1));

    HifiCaptureSegment seg[2];
    hifi_capture_segments(r.cap, seg, 2);
    const u8* data = hifi_capture_data(r.cap);
    const HifiDeviceFormat& f = seg[0].format;
    KV_CHECK(c, same(data, f, 0, a, 0, before - ramp));               // exact until the ramp
    KV_CHECK(c, silent(data, f, before, pulled - before));            // then silence
    i32 music = largest_step(data, f, 0, 8000);
    KV_CHECK(c, largest_step(data, f, before - ramp - 10, ramp + 100) <= music); // and no step between them
    KV_CHECK(c, sample_at(data, f, (before - 1) * 2) == 0 && sample_at(data, f, (before - 1) * 2 + 1) == 0);

    atomic_store(&g_stall, 0u, MO_RELEASE);
    KV_CHECK(c, r.wait_written(before + 19200 < a.frames ? before + 19200 : a.frames)); // the lead is full again
    u64 resumed = pulled;
    // Paced, as above: a frame is pulled only when the lead is far enough ahead of it.
    while (!r.has(HifiEventKind::Ended, 1) && pulled < 200000) {
        u64 need = r.status().taken + 1000 + ramp + 1;
        KV_CHECK(c, r.wait_written(need < a.frames ? need : a.frames));
        pulled += r.pull(1000);
    }
    KV_CHECK(c, r.has(HifiEventKind::Ended, 1) && r.status().taken == a.frames && r.status().underruns == 1);
    // It comes back with a ramp up from where it stopped: after the ramp, exact to the end.
    hifi_capture_segments(r.cap, seg, 2);
    KV_CHECK(c, same(data, f, resumed + ramp, a, before + ramp, a.frames - before - ramp));
    KV_CHECK(c, largest_step(data, f, resumed - 10, ramp + 100) <= music);
    r.free();
    drop(a); drop(fa);
}

// Sources that cannot be played, between ones that can: each fails by itself, in words,
// and the next plays.
void test_hifi_failures(TestCtx& c) {
    c.begin("hifi failures");
    File good = read_data("tone_16_2ch_44100.flac"), cut = read_data("broken/truncated.flac");
    File junk = read_data("broken/not_flac.flac"), magic = read_data("broken/magic_only.flac"), good2 = read_data("tone_16_2ch_44100.flac");
    Pcm a = decode(good), t = decode(cut);
    KV_CHECK(c, t.frames > 0 && t.frames < a.frames);
    Rig r; r.make();
    hifi_add_source(r.h, {1, decoder_for(junk.bytes()), sparse_whole(junk.bytes())});   // nobody's file: a null decoder
    hifi_add_source(r.h, {2, &k_flac_decoder, sparse_whole(magic.bytes())});            // claimed, then refused
    hifi_add_source(r.h, {3, &k_flac_decoder, sparse_whole(cut.bytes())});              // plays what it has
    hifi_add_source(r.h, {4, &k_flac_decoder, sparse_whole(good.bytes())});
    hifi_add_source(r.h, {5, &k_flac_decoder, sparse_whole(junk.bytes())});             // fails at the very end
    KV_CHECK(c, r.wait_event(HifiEventKind::Released, 5));
    KV_CHECK(c, r.wait_written(t.frames + a.frames));
    u64 pulled = 0;
    while (pulled < t.frames + a.frames + 4000) pulled += r.pull(1024);
    for (u64 id : {1, 2, 5}) {
        const HifiEvent* e = r.find(HifiEventKind::Failed, id);
        KV_CHECK(c, e && e->reason[0] != 0);
        KV_CHECK(c, !r.has(HifiEventKind::Started, id) && !r.has(HifiEventKind::Ended, id) && r.has(HifiEventKind::Released, id));
    }
    const HifiEvent* e3 = r.find(HifiEventKind::Ended, 3);
    KV_CHECK(c, e3 && e3->status == DecodeStatus::Damaged && e3->reason[0] != 0 && e3->frames == t.frames);
    const HifiEvent* e4 = r.find(HifiEventKind::Ended, 4);
    KV_CHECK(c, e4 && e4->status == DecodeStatus::End);
    KV_CHECK(c, r.wait_event(HifiEventKind::Released, 4) && r.count(HifiEventKind::Released) == 5);
    HifiCaptureSegment seg[2];
    KV_CHECK(c, hifi_capture_segments(r.cap, seg, 2) == 1);
    const u8* data = hifi_capture_data(r.cap);
    KV_CHECK(c, same(data, seg[0].format, 0, a, 0, t.frames));             // the cut file: what it had, exactly
    KV_CHECK(c, same(data, seg[0].format, t.frames, a, 0, a.frames));      // then the good one, straight after
    KV_CHECK(c, silent(data, seg[0].format, t.frames + a.frames, pulled - t.frames - a.frames));
    KV_CHECK(c, r.status().underruns == 0);                                 // a failure at the end is not a lead running dry

    // A device that does not exist: the source fails, in words.
    HifiConfig hc; hc.device = "no such device";
    Hifi* h = hifi_create(hc);
    hifi_add_source(h, {9, &k_flac_decoder, sparse_whole(good2.bytes())});
    HifiEvent ev[8]; u32 n = 0;
    f64 until = time_now() + 2.0;
    while (n < 2 && time_now() < until) { n += hifi_poll(h, ev + n, 8 - n); time_sleep(0.0005); }
    KV_CHECK(c, n == 2 && ev[0].kind == HifiEventKind::Failed && ev[0].reason[0] != 0 && ev[1].kind == HifiEventKind::Released);
    hifi_destroy(h);

    // Destroyed in the middle of things: nothing hangs, nothing leaks (the leak report says).
    Rig q; q.make();
    hifi_add_source(q.h, {1, &k_flac_decoder, sparse_whole(good.bytes())});
    hifi_add_source(q.h, {2, &k_flac_decoder, sparse_whole(good2.bytes())});
    q.pull(100);
    q.free();
    r.free();
    drop(a); drop(t); drop(good); drop(cut); drop(junk); drop(magic); drop(good2);
}

// The capture device on its own clock, as hardware would pull: a file plays in its own
// time and arrives exact.
void test_hifi_wall_clock(TestCtx& c) {
    c.begin("hifi wall clock");
    File fa = read_data("tone_24_2ch_96000.flac");   // a quarter of a second
    Pcm a = decode(fa);
    Rig r; r.make(480, 5.0, true);
    f64 t0 = time_now();
    hifi_add_source(r.h, {1, &k_flac_decoder, sparse_whole(fa.bytes())});
    KV_CHECK(c, r.wait_event(HifiEventKind::Ended, 1));
    f64 took = time_now() - t0;
    KV_CHECK(c, took > 0.24 && took < 1.5);          // real time, not as fast as it will go
    HifiStatus s = r.status();
    KV_CHECK(c, s.underruns == 0 && s.taken == a.frames && s.exact);
    hifi_destroy(r.h); r.h = nullptr;                // stops the device's thread before the capture is read
    HifiCaptureSegment seg[2];
    KV_CHECK(c, hifi_capture_segments(r.cap, seg, 2) == 1);
    const u8* data = hifi_capture_data(r.cap);
    u64 frames = seg[0].bytes / hifi_frame_bytes(seg[0].format), start = 0;
    while (start < frames && silent(data, seg[0].format, start, 1)) ++start;
    KV_CHECK(c, start + a.frames <= frames && same(data, seg[0].format, start, a, 0, a.frames));
    u8 md5[16];
    if (start + a.frames <= frames) { capture_md5(data + start * hifi_frame_bytes(seg[0].format), seg[0].format, a.frames, md5); KV_CHECK(c, memcmp(md5, fa.data + 26, 16) == 0); }
    hifi_capture_destroy(r.cap);
    drop(a); drop(fa);
}

namespace {

// A file whose pieces the test hands over when hifi asks for them: a span over a copy of
// its bytes, with a map the test fills.
struct Fed {
    const File* src = nullptr;
    u8*  bytes = nullptr;
    u8*  have = nullptr;
    u32  shift = 12;
    u64  pieces = 0;
    u32  given = 0;
    SparseSpan span() const { return SparseSpan{bytes, src->len, shift, have}; }
    void make(const File& f, u32 sh) {
        src = &f; shift = sh;
        pieces = ((u64)f.len + (1u << sh) - 1) >> sh;
        bytes = (u8*)mem_alloc(f.len);
        memset(bytes, 0xA5, f.len); // not the file: a piece read before it is given decodes wrong
        have = (u8*)mem_alloc(pieces);
        memset(have, 0, pieces);
    }
    void give(ByteRange r, u64 limit = ~0ull) {
        if (!r.len) return;
        for (u64 i = r.at >> shift; i <= (r.at + r.len - 1) >> shift && i < pieces; ++i) {
            if (have[i] || (i << shift) >= limit) continue;
            u64 from = i << shift, len = src->len - from < (1u << shift) ? src->len - from : (1u << shift);
            memcpy(bytes + from, src->data + from, len);
            atomic_store(&have[i], (u8)1, MO_RELEASE);
            ++given;
        }
    }
    void free() { mem_free(bytes); mem_free(have); }
};

// Hands over what each new Want of `id` asks for, below `limit`; returns the wants seen.
u32 serve(Rig& r, u32* seen, Fed& f, u64 id, u64 limit = ~0ull) {
    r.poll();
    u32 n = 0;
    for (; *seen < r.nevents; ++*seen)
        if (r.events[*seen].kind == HifiEventKind::Want && r.events[*seen].source == id) { f.give(r.events[*seen].need, limit); ++n; }
    return n;
}

} // namespace

// Sources whose bytes arrive only when hifi asks for them. Two of one format, in 4 KB
// pieces: each waits, says what it wants, and plays exact; the second follows the first
// in the same stream, its first bytes asked for while the first was still in the lead.
void test_hifi_pieces(TestCtx& c) {
    c.begin("hifi pieces");
    File fa = read_data("tone_24_2ch_96000.flac"), fb = read_data("tone_24_2ch_96000.flac");
    Pcm a = decode(fa);
    Fed da, db;
    da.make(fa, 12); db.make(fb, 12);
    Rig r; r.make(0, 0.1);
    hifi_add_source(r.h, {1, &k_flac_decoder, da.span()});
    hifi_add_source(r.h, {2, &k_flac_decoder, db.span()});
    u32 seen = 0, wants = 0;
    u64 pulled = 0;
    f64 until = time_now() + 5.0;
    while (!r.has(HifiEventKind::Ended, 2) && time_now() < until) {
        for (u32 k = 0; k < 50; ++k) { // the lead is 0.1 s: serve, then pull only what is surely there
            u32 seen1 = seen;
            wants += serve(r, &seen, da, 1);
            seen = seen1;
            wants += serve(r, &seen, db, 2);
            time_sleep(0.0002);
        }
        HifiStatus s = r.status();
        u64 ready = s.written > s.taken ? s.written - s.taken : 0;
        if (ready > 2000) pulled += r.pull((u32)(ready - 1000));
        else if (r.has(HifiEventKind::Released, 2)) pulled += r.pull(1000);
    }
    pulled += r.pull(2000);
    KV_CHECK(c, r.has(HifiEventKind::Ended, 1) && r.has(HifiEventKind::Ended, 2));
    KV_CHECK(c, wants >= 4 && da.given == da.pieces && db.given == db.pieces);
    HifiCaptureSegment seg[4];
    KV_CHECK(c, hifi_capture_segments(r.cap, seg, 4) == 1);
    const u8* data = hifi_capture_data(r.cap) + seg[0].offset;
    KV_CHECK(c, same(data, seg[0].format, 0, a, 0, a.frames));
    KV_CHECK(c, same(data, seg[0].format, a.frames, a, 0, a.frames)); // the second straight after: gapless
    const HifiEvent* e1 = r.find(HifiEventKind::Ended, 1);
    const HifiEvent* e2 = r.find(HifiEventKind::Ended, 2);
    KV_CHECK(c, e1 && e2 && e1->status == DecodeStatus::End && e2->status == DecodeStatus::End);
    KV_CHECK(c, r.count(HifiEventKind::Underrun) == 0 && r.status().underruns == 0);
    r.free();
    da.free(); db.free();
    drop(a); drop(fa); drop(fb);
}

// Bytes that will not come. A source that plays until it needs a piece that is never
// given, and is starved there: it ends Damaged with the reason, and what was heard
// before is exact. A source starved before it could open fails.
void test_hifi_starved(TestCtx& c) {
    c.begin("hifi starved");
    File fa = read_data("tone_16_2ch_44100.flac"), fb = read_data("tone_16_2ch_44100.flac");
    Pcm a = decode(fa);
    Fed da, db;
    da.make(fa, 12); db.make(fb, 12);
    Rig r; r.make(0, 5.0);
    hifi_add_source(r.h, {1, &k_flac_decoder, da.span()});
    u32 seen = 0;
    const u64 limit = fa.len / 2;           // nothing past half the file comes
    bool starved = false;
    f64 until = time_now() + 3.0;
    while (!starved && time_now() < until) {
        serve(r, &seen, da, 1, limit);
        for (u32 i = 0; i < r.nevents; ++i)
            if (r.events[i].kind == HifiEventKind::Want && r.events[i].need.at >= limit && !starved) {
                hifi_starve(r.h, 1, "the disk went away");
                starved = true;
            }
        time_sleep(0.0005);
    }
    KV_CHECK(c, starved);
    time_sleep(0.05); // the render thread looks again within a few milliseconds, and ends the stream there
    u64 pulled = 0;
    while (!r.has(HifiEventKind::Ended, 1) && pulled < 100000) pulled += r.pull(1000);
    const HifiEvent* e = r.find(HifiEventKind::Ended, 1);
    KV_CHECK(c, e && e->status == DecodeStatus::Damaged && strstr(e->reason, "the disk went away"));
    KV_CHECK(c, e && e->frames >= 4608 && e->frames < a.frames); // its first frame at least, and not the whole
    HifiCaptureSegment seg[2];
    hifi_capture_segments(r.cap, seg, 2);
    KV_CHECK(c, e && same(hifi_capture_data(r.cap) + seg[0].offset, seg[0].format, 0, a, 0, e->frames));

    // Starved before it opened: Failed, with the reason, and Released.
    hifi_starve(r.h, 2, "the file could not be read: Input/output error");
    hifi_add_source(r.h, {2, &k_flac_decoder, db.span()});
    KV_CHECK(c, r.wait_event(HifiEventKind::Failed, 2) && r.wait_event(HifiEventKind::Released, 2));
    KV_CHECK(c, strstr(r.find(HifiEventKind::Failed, 2)->reason, "Input/output"));
    r.free();
    da.free(); db.free();
    drop(a); drop(fa); drop(fb);
}
