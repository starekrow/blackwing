// The equalizer in the path: a recut with it, unchanged, against a straight render. A
// filter's history is not carried from one render to the next: the recut runs in from
// before its start to settle it. The two then agree to within a bit: the dither is the
// same function of the frame, and what is left of the filter's past is far below it.
#include "korvid/hifi_eq/stages.h"
#include "korvid/hifi/hifi.h"
#include "korvid/hifi_codec/flac.h"
#include "korvid/hifi_platform/capture.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "../../hifi_codec/tests/data.h"
#include <string.h>

using namespace kv;

namespace {

constinit const StageDesc* const k_chain[] = {&k_gain_stage, &k_eq_stage};

void preset(Hifi* h, const char* name) {
    const EqPreset* p = eq_preset(name);
    for (u32 b = 0; b < kEqBands; ++b) {
        EqBand band = eq_preset_band(*p, b);
        hifi_set_param(h, 1, b * 4 + 0, (f64)band.kind); hifi_set_param(h, 1, b * 4 + 1, band.freq);
        hifi_set_param(h, 1, b * 4 + 2, band.gain_db);   hifi_set_param(h, 1, b * 4 + 3, band.q);
    }
}

// The file through the path with the preset, `recuts` recuts on the way; the capture.
i16* run(const kvtest::File& f, u64 frames, u32 recuts, u32* done) {
    HifiCaptureDesc cd; cd.keep_bytes = 16u << 20;
    HifiCapture* cap = hifi_capture_create(cd);
    HifiConfig hc; hc.device = "capture"; hc.device_backend = cap; hc.stages = k_chain;
    Hifi* h = hifi_create(hc);
    preset(h, "rock");
    hifi_add_source(h, {1, &k_flac_decoder, sparse_whole(f.bytes())});
    HifiStatus st;
    f64 until = time_now() + 2.0;
    do { time_sleep(0.001); hifi_status(h, &st); } while (st.written < frames && time_now() < until);
    u64 pulled = 0;
    u32 asked = 0;
    while (pulled < frames) {
        if (asked < recuts && pulled >= (asked + 1) * (frames / (recuts + 1))) {
            hifi_set_param(h, 1, 2, 5.0); // the value it has: a recut that changes nothing
            ++asked;
            u32 before = st.recuts;
            until = time_now() + 2.0;
            while (st.recuts == before && time_now() < until) { time_sleep(0.0005); pulled += hifi_capture_pull(cap, 64); HifiEvent ev[16]; hifi_poll(h, ev, 16); hifi_status(h, &st); }
        }
        pulled += hifi_capture_pull(cap, 256);
        HifiEvent ev[16]; hifi_poll(h, ev, 16);
        hifi_status(h, &st);
    }
    *done = st.recuts;
    HifiCaptureSegment seg[2];
    hifi_capture_segments(cap, seg, 2);
    i16* out = (i16*)mem_alloc((usize)frames * 4);
    memcpy(out, hifi_capture_data(cap) + seg[0].offset, (usize)frames * 4);
    hifi_destroy(h);
    hifi_capture_destroy(cap);
    return out;
}

} // namespace

void test_eq_recut(TestCtx& c) {
    c.begin("eq recut");
    kvtest::File f = kvtest::read_data("tone_16_2ch_48000.flac");
    const u64 n = 24000;
    u32 none = 0, some = 0;
    i16* straight = run(f, n, 0, &none);
    i16* recut = run(f, n, 6, &some);
    u32 differ = 0, worst = 0;
    for (u64 i = 0; i < n * 2; ++i) {
        i32 d = (i32)recut[i] - (i32)straight[i];
        u32 a = (u32)(d < 0 ? -d : d);
        differ += a != 0;
        if (a > worst) worst = a;
    }
    KV_CHECK(c, none == 0 && some >= 6);
    KV_CHECK(c, worst <= 1);
    KV_CHECK(c, differ < n / 100); // and the bit is rare
    KV_INFO("test", "eq recut: %u recuts with the rock preset unchanged; %u of %llu samples a bit off a straight render, none by more",
            some, differ, (unsigned long long)n * 2);
    mem_free(straight); mem_free(recut);
    kvtest::drop(f);
}
