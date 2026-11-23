// The MP3 decoder against files made by LAME (tests/data/make_mp3.py), against what
// libmpg123 made of each, and against itself.
//
// There is no MD5 in an MP3. The oracle for a good file is a reference decoder: each file's
// <name>.ref is libmpg123's 16-bit decode with its gapless trimming, and this decoder's 24
// bits, trimmed by its own delay and padding and rounded to 16, must be within a bit of it.
// Then the decoder against itself: every piece size and order of delivery gives the same
// samples as a straight decode, and a decode from a seek equals the same stretch of a
// straight one. Broken files keep only the contract's promises.
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_codec/flac.h"
#include "korvid/hifi_codec/mp3.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/random.h"
#include "korvid/core/test.h"
#include "data.h"
#include "pieces.h"
#include <initializer_list>
#include <string.h>

using namespace kv;
using kvtest::File;
using kvtest::read_data;
using namespace kvtest;

namespace {

const DecoderDesc& kMp3 = k_mp3_decoder;

struct Good { const char* name; u32 rate; u16 channels; bool gapless; };
constexpr Good kGood[] = {
    {"cbr128_44100_2ch", 44100, 2, true}, {"vbr_44100_2ch", 44100, 2, true},  {"vbr_48000_2ch", 48000, 2, true},
    {"cbr64_22050_1ch", 22050, 1, true},  {"vbr_24000_2ch", 24000, 2, true},  {"cbr32_8000_1ch", 8000, 1, true},
    {"cbr320_32000_2ch", 32000, 2, true}, {"notag_44100_2ch", 44100, 2, false}, {"free_44100_2ch", 44100, 2, false},
    {"crc_44100_2ch", 44100, 2, true},
};

File mp3(const char* name) {
    char path[128];
    format(path, sizeof path, "mp3/%s", name);
    return read_data(path);
}

Pcm decode_mp3(Span<const u8> bytes) { return straight(bytes, kMp3, 1u << 20); } // a length that lies short is not where it stops

// A sample at 24 bits, rounded to 16.
i32 to16(i32 v) {
    i32 r = (v + 128) >> 8;
    return r > 32767 ? 32767 : r < -32768 ? -32768 : r;
}

// The file with `tag` bytes of ID3v2 in front: a title, then zeros standing for art.
File behind_tag(const File& f, u32 tag) {
    File t;
    t.len = tag + f.len;
    t.data = (u8*)mem_alloc(t.len);
    memset(t.data, 0, tag);
    memcpy(t.data, "ID3\x03\x00\x00", 6);
    u32 size = tag - 10;
    t.data[6] = (u8)((size >> 21) & 0x7F); t.data[7] = (u8)((size >> 14) & 0x7F);
    t.data[8] = (u8)((size >> 7) & 0x7F);  t.data[9] = (u8)(size & 0x7F);
    memcpy(t.data + 10, "TIT2\x00\x00\x00\x05\x00\x00\x00Tone", 15);
    memcpy(t.data + tag, f.data, f.len);
    return t;
}

bool same_pcm(const Pcm& a, const Pcm& b) {
    return a.frames == b.frames && a.ch == b.ch && memcmp(a.s, b.s, (usize)a.frames * a.ch * 4) == 0;
}

} // namespace

void test_mp3_reference(TestCtx& c) {
    c.begin("mp3 reference");
    i32 worst = 0;
    u64 off = 0, total = 0;
    for (const Good& g : kGood) {
        char name[64], ref[64];
        format(name, sizeof name, "%s.mp3", g.name);
        format(ref, sizeof ref, "%s.mp3.ref", g.name);
        File f = mp3(name), r = mp3(ref);
        KV_CHECK(c, decoder_for(Span<const u8>(f.data, f.len < 64 ? f.len : 64)) == &kMp3);
        Pcm p = decode_mp3(f.bytes());
        const DecoderInfo& in = p.info;
        KV_CHECK(c, in.format.rate == g.rate && in.format.channels == g.channels && in.format.bits == 24);
        KV_CHECK(c, p.status == DecodeStatus::End);
        // A LAME tag says the length and the gapless trim; without one the length is a guess.
        KV_CHECK(c, in.estimated == !g.gapless && (g.gapless ? in.frames == p.frames && in.delay > 0 && in.padding > 0 : in.delay == 0));
        u64 from = in.delay, n = p.frames - in.delay - in.padding;
        u64 rn = r.len / 2 / g.channels;
        if (n != rn) KV_ERROR("test", "%s: %llu frames trimmed, the reference has %llu", g.name, (unsigned long long)n, (unsigned long long)rn);
        KV_CHECK(c, n == rn);
        i32 bad = 0;
        u64 m = n < rn ? n : rn;
        for (u64 i = 0; i < m * g.channels; ++i) {
            i16 want;
            memcpy(&want, r.data + i * 2, 2);
            i32 d = to16(p.s[from * g.channels + i]) - want;
            if (d < 0) d = -d;
            if (d > bad) bad = d;
            off += d != 0;
        }
        total += m * g.channels;
        if (bad > 1) KV_ERROR("test", "%s: %d from the reference at worst", g.name, bad);
        KV_CHECK(c, bad <= 1);
        if (bad > worst) worst = bad;
        drop(p);
        kvtest::drop(f); kvtest::drop(r);
    }
    KV_INFO("test", "mp3 reference: %zu files, at worst %d from libmpg123 at 16 bits, %llu of %llu samples differ",
            sizeof kGood / sizeof kGood[0], worst, (unsigned long long)off, (unsigned long long)total);
}

void test_mp3_tags(TestCtx& c) {
    c.begin("mp3 tags");
    File plain = mp3("cbr128_44100_2ch.mp3");
    Pcm want = decode_mp3(plain.bytes());

    // A front tag, an APEv2 and an ID3v1 tag behind: the same samples, nothing more.
    File tagged = mp3("tagged_44100_2ch.mp3");
    KV_CHECK(c, decoder_for(Span<const u8>(tagged.data, (u32)decoder_head_bytes(tagged.bytes(), tagged.len))) == &kMp3);
    Pcm got = decode_mp3(tagged.bytes());
    KV_CHECK(c, same_pcm(got, want) && got.status == DecodeStatus::End && got.info.delay == want.info.delay);
    drop(got);

    // Four megabytes of art in front, in pieces: stepped over by its size, not one piece
    // inside it asked for.
    File big = behind_tag(plain, 4u << 20);
    for (u32 shift : {12u, 16u}) {
        Fed fed; fed.make(big, shift);
        Driver dr; dr.make(fed, Order::Asked, 4096, &kMp3);
        KV_CHECK(c, dr.open());
        Pcm p; p.ch = 2;
        p.s = (i32*)mem_alloc((usize)(want.frames + 4096) * 8);
        DecodeResult r;
        do { r = dr.decode(4096); memcpy(p.s + p.frames * 2, dr.out, (usize)r.frames * 8); p.frames += r.frames; } while (r.status == DecodeStatus::Ok);
        KV_CHECK(c, same_pcm(p, want) && r.status == DecodeStatus::End);
        u64 inside = 0;
        for (u64 i = 1; ((i + 1) << shift) <= (4u << 20); ++i) inside += fed.has(i);
        KV_CHECK(c, inside == 0);
        KV_CHECK(c, dr.behaved);
        drop(p); dr.free(); fed.free();
    }
    kvtest::drop(big);

    // A VBRI header where the Info frame was: its length, and no gapless trim.
    File vbr = mp3("vbr_44100_2ch.mp3"), vbri = mp3("vbri_44100_2ch.mp3");
    Pcm a = decode_mp3(vbr.bytes()), b = decode_mp3(vbri.bytes());
    KV_CHECK(c, same_pcm(a, b) && !b.info.estimated && b.info.frames == a.info.frames && b.info.delay == 0 && b.status == DecodeStatus::End);
    drop(a); drop(b);
    kvtest::drop(vbr); kvtest::drop(vbri);

    drop(want);
    kvtest::drop(tagged);
    kvtest::drop(plain);
}

void test_mp3_pieces(TestCtx& c) {
    c.begin("mp3 in pieces");
    u32 sh[4];
    u32 nsh = shifts(sh);
    u32 runs = 0, waits = 0;
    const char* names[] = {"cbr128_44100_2ch.mp3", "vbr_48000_2ch.mp3", "cbr64_22050_1ch.mp3", "cbr32_8000_1ch.mp3",
                           "free_44100_2ch.mp3", "notag_44100_2ch.mp3", "tagged_44100_2ch.mp3", "long_44100_2ch.mp3"};
    for (const char* name : names) {
        File f = mp3(name);
        Pcm want = decode_mp3(f.bytes());
        KV_CHECK(c, want.frames > 0 && want.status == DecodeStatus::End);
        for (u32 k = 0; k < nsh; ++k) {
            for (u32 o = 0; o < (u32)Order::Count; ++o) {
                if (o == (u32)Order::Scattered && f.len > (512u << 10) && k == 0) continue; // a long file in pages at random: slow, and nothing new
                for (u32 chunk : {577u, 4096u}) {
                    Fed fed; fed.make(f, sh[k]);
                    Driver dr; dr.make(fed, (Order)o, chunk, &kMp3);
                    bool opened = dr.open();
                    KV_CHECK(c, opened && dr.behaved && dr.info.frames == want.info.frames);
                    if (opened) {
                        Pcm got; got.ch = dr.info.format.channels;
                        got.s = (i32*)mem_alloc((usize)(want.frames + chunk) * got.ch * 4);
                        DecodeResult r;
                        do {
                            r = dr.decode(chunk);
                            if (got.frames + r.frames > want.frames) break;
                            memcpy(got.s + got.frames * got.ch, dr.out, (usize)r.frames * got.ch * 4);
                            got.frames += r.frames;
                        } while (r.status == DecodeStatus::Ok);
                        bool ok = r.status == DecodeStatus::End && same_pcm(got, want) && dr.behaved;
                        if (!ok) KV_ERROR("test", "%s in pieces of %u, %s, %u at a time: %llu frames, %s", name, 1u << sh[k],
                                          order_name((Order)o), chunk, (unsigned long long)got.frames, decode_status_name(r.status));
                        KV_CHECK(c, ok);
                        drop(got);
                    }
                    waits += dr.waits;
                    ++runs;
                    dr.free(); fed.free();
                }
            }
        }
        drop(want);
        kvtest::drop(f);
    }
    KV_INFO("test", "mp3 in pieces: %u decodes, %u waits", runs, waits);
}

void test_mp3_seek(TestCtx& c) {
    c.begin("mp3 seek");
    u32 sh[4];
    u32 nsh = shifts(sh);
    u32 seeks = 0;
    for (const char* name : {"long_44100_2ch.mp3", "vbr_24000_2ch.mp3", "cbr32_8000_1ch.mp3", "notag_44100_2ch.mp3", "tagged_44100_2ch.mp3"}) {
        File f = mp3(name);
        Pcm want = decode_mp3(f.bytes());
        const u64 n = want.frames;
        const u64 spf = want.info.format.rate >= 32000 ? 1152 : 576;
        Rng rng = Rng::seeded(n, 11);
        u64 targets[40];
        u32 nt = 0;
        for (u64 t : {(u64)0, (u64)1, spf - 1, spf, spf + 1, 2 * spf, 3 * spf + 7, n / 2, n - spf, n - 1, n}) targets[nt++] = t;
        while (nt < 40) targets[nt++] = rng.next() % n;
        for (u32 k = 0; k <= nsh; ++k) {
            for (u32 pass = 0; pass < 2; ++pass) {
                Fed fed; fed.make(f, k < nsh ? sh[k] : 18);
                if (k == nsh) fed.give_all();
                Driver dr; dr.make(fed, pass ? Order::Scattered : Order::Asked, 1500, &kMp3);
                if (!dr.open()) { KV_CHECK(c, false); dr.free(); fed.free(); continue; }
                u64 order[40];
                memcpy(order, targets, sizeof order);
                if (pass) for (u32 i = 39; i > 0; --i) { u32 j = rng.next() % (i + 1); u64 t = order[i]; order[i] = order[j]; order[j] = t; }
                for (u64 t : order) {
                    SeekResult s = dr.seek(t);
                    u64 len = n - t < 3000 ? n - t : 3000;
                    bool same = s.status == SeekStatus::Ok && same_from(dr, want, t, len);
                    if (t + len == n && same) {
                        DecodeResult r = dr.decode(16);
                        same = r.frames == 0 && r.status == DecodeStatus::End;
                    }
                    if (!same) KV_ERROR("test", "%s: a seek to %llu (pieces of %u, pass %u) is not the straight decode (%s)", name,
                                        (unsigned long long)t, 1u << (k < nsh ? sh[k] : 18), pass, s.reason);
                    KV_CHECK(c, same);
                    ++seeks;
                }
                dr.seek(1000);
                SeekResult past = dr.seek(n + (want.info.estimated ? 5000 : 1));
                KV_CHECK(c, past.status == SeekStatus::Failed && past.reason[0] != 0);
                KV_CHECK(c, same_from(dr, want, 1000, 2000));
                KV_CHECK(c, dr.behaved);
                dr.free(); fed.free();
            }
        }
        // A seek that waits and is then left: the decoder goes on from where it was. (In
        // the short files open() has had all the audio, and there is nothing to wait for.)
        if (!strcmp(name, "long_44100_2ch.mp3")) {
            Fed fed; fed.make(f, sh[0]);
            Driver dr; dr.make(fed, Order::Asked, 1000, &kMp3);
            dr.open();
            KV_CHECK(c, same_from(dr, want, 0, 1000));
            SeekResult s = dr.d->seek(dr.state, n - 10);
            KV_CHECK(c, s.status == SeekStatus::Wait && s.need.len > 0);
            KV_CHECK(c, same_from(dr, want, 1000, n - 1000 < 6000 ? n - 1000 : 6000));
            KV_CHECK(c, dr.behaved);
            dr.free(); fed.free();
        }
        drop(want);
        kvtest::drop(f);
    }

    // Memory for only a few marks: they thin out as the walk goes, and seeks stay exact.
    {
        File f = mp3("long_44100_2ch.mp3");
        Pcm want = decode_mp3(f.bytes());
        u32 mem = 1024;
        for (;; mem += 64) {
            u8* m = (u8*)mem_alloc(mem);
            DecoderOpen o = kMp3.open(sparse_whole(f.bytes()), Span<u8>(m, mem));
            mem_free(m);
            if (o.state) break;
        }
        Fed fed; fed.make(f, 14);
        Driver dr; dr.make(fed, Order::Asked, 2000, &kMp3, mem);
        KV_CHECK(c, dr.open());
        KV_CHECK(c, same_from(dr, want, 0, want.frames - 10)); // the walk, through every mark
        Rng rng = Rng::seeded(77, 3);
        for (u32 i = 0; i < 30; ++i) {
            u64 t = rng.next() % want.frames;
            u64 len = want.frames - t < 2000 ? want.frames - t : 2000;
            KV_CHECK(c, dr.seek(t).status == SeekStatus::Ok && same_from(dr, want, t, len));
            ++seeks;
        }
        KV_CHECK(c, dr.behaved);
        dr.free(); fed.free(); drop(want); kvtest::drop(f);
    }
    KV_INFO("test", "mp3 seek: %u seeks", seeks);
}

void test_mp3_broken(TestCtx& c) {
    c.begin("mp3 broken");
    File plain = mp3("cbr128_44100_2ch.mp3");
    Pcm good = decode_mp3(plain.bytes());
    auto prefix = [&](const Pcm& p) { // how many of its frames are the good file's
        u64 n = p.frames < good.frames ? p.frames : good.frames, i = 0;
        while (i < n && memcmp(p.s + i * p.ch, good.s + i * good.ch, (usize)p.ch * 4) == 0) ++i;
        return i;
    };
    struct Case { const char* name; bool claimed; bool opens; DecodeStatus end; };
    const Case cases[] = {
        {"lying_long.mp3", true, true, DecodeStatus::Damaged},  {"lying_short.mp3", true, true, DecodeStatus::End},
        {"lying_gapless.mp3", true, true, DecodeStatus::End},   {"junk_between.mp3", true, true, DecodeStatus::Damaged},
        {"truncated.mp3", true, true, DecodeStatus::Damaged},   {"cut_in_frame.mp3", true, true, DecodeStatus::Damaged},
        {"one_frame.mp3", true, true, DecodeStatus::End},       {"info_only.mp3", true, true, DecodeStatus::Damaged},
        {"empty.mp3", false, false, DecodeStatus::End},         {"id3_only.mp3", false, false, DecodeStatus::End},
        {"id3_size_lies.mp3", false, false, DecodeStatus::End}, {"not_mp3.mp3", false, false, DecodeStatus::End},
        {"text.mp3", false, false, DecodeStatus::End},          {"zeros_then_frames.mp3", false, true, DecodeStatus::End},
    };
    for (const Case& k : cases) {
        char path[64];
        format(path, sizeof path, "broken/%s", k.name);
        File f = mp3(path);
        bool mine = kMp3.probe(Span<const u8>(f.data, (u32)decoder_head_bytes(f.bytes(), f.len)));
        if (mine != k.claimed) KV_ERROR("test", "%s: probe says %d", k.name, mine);
        KV_CHECK(c, mine == k.claimed);
        u8* mem = (u8*)mem_alloc(kMp3.memory_bytes);
        DecoderOpen o = kMp3.open(sparse_whole(f.bytes()), Span<u8>(mem, kMp3.memory_bytes));
        KV_CHECK(c, (o.state != nullptr) == k.opens && (o.state || o.reason[0]));
        if (!o.state) KV_INFO("test", "mp3 broken: %s: %s", k.name, o.reason);
        mem_free(mem);
        if (o.state) {
            Pcm p = decode_mp3(f.bytes());
            u64 same = prefix(p);
            if (p.status != k.end) KV_ERROR("test", "%s: ends %s", k.name, decode_status_name(p.status));
            KV_CHECK(c, p.status == k.end);
            KV_INFO("test", "mp3 broken: %s: %llu frames (%llu as the good file's), %s", k.name, (unsigned long long)p.frames,
                    (unsigned long long)same, decode_status_name(p.status));
            if (!strcmp(k.name, "lying_long.mp3") || !strcmp(k.name, "lying_short.mp3"))
                KV_CHECK(c, same_pcm(p, good));
            if (!strcmp(k.name, "truncated.mp3") || !strcmp(k.name, "cut_in_frame.mp3") || !strcmp(k.name, "junk_between.mp3"))
                KV_CHECK(c, same >= 1152 && same < good.frames);
            if (!strcmp(k.name, "one_frame.mp3")) KV_CHECK(c, p.frames == 1152 && p.info.estimated);
            if (!strcmp(k.name, "info_only.mp3")) KV_CHECK(c, p.frames == 0);
            if (!strcmp(k.name, "lying_gapless.mp3")) KV_CHECK(c, p.info.delay == 4095 + 529 && p.info.padding == 4095 - 529);
            if (!strcmp(k.name, "zeros_then_frames.mp3")) {
                File nt = mp3("notag_44100_2ch.mp3");
                Pcm q = decode_mp3(nt.bytes());
                KV_CHECK(c, same_pcm(p, q));
                drop(q); kvtest::drop(nt);
            }
            drop(p);
        }
        kvtest::drop(f);
    }
    {
        File f = mp3("broken/not_mp3.mp3");
        KV_CHECK(c, decoder_for(Span<const u8>(f.data, 64)) == &k_flac_decoder);
        kvtest::drop(f);
    }
    drop(good);
    kvtest::drop(plain);
}

// Good files with a few bits changed, or cut anywhere, many times over, whole and in
// pieces with seeks: nothing to compare against, only the contract's promises.
void test_mp3_spoiled(TestCtx& c) {
    c.begin("mp3 spoiled");
    u32 opened = 0, damaged = 0, misbehaved = 0, runs = 0;
    for (const char* name : {"vbr_44100_2ch.mp3", "cbr64_22050_1ch.mp3", "free_44100_2ch.mp3"}) {
        File base = mp3(name);
        File work; work.data = (u8*)mem_alloc(base.len); work.len = base.len;
        Rng rng = Rng::seeded(20261123, 13);
        for (u32 round = 0; round < 200; ++round, ++runs) {
            memcpy(work.data, base.data, base.len);
            work.len = round % 3 == 2 ? rng.next() % (base.len + 1) : base.len;
            if (round % 3 != 2) {
                u32 flips = 1 + rng.next() % 12;
                u32 span = round % 3 == 0 ? 600 : base.len; // in the Info frame and the first frames, or anywhere
                for (u32 k = 0; k < flips; ++k) work.data[rng.next() % span] ^= (u8)(1u << (rng.next() % 8));
            }
            Fed fed; fed.make(work, 12);
            Driver dr; dr.make(fed, (Order)(round % (u32)Order::Count), 2048, &kMp3);
            if (dr.open()) {
                ++opened;
                for (u32 k = 0; k < 5 && dr.behaved; ++k) {
                    DecodeResult r = dr.decode(2048);
                    if (r.status == DecodeStatus::Damaged) { ++damaged; if (!r.reason[0]) dr.behaved = false; }
                    SeekResult s = dr.seek(rng.next() % 40000);
                    if (s.status == SeekStatus::Failed && !s.reason[0]) dr.behaved = false;
                }
                // A straight decode to the end: it ends, and says how.
                DecodeResult r;
                u32 calls = 0;
                do r = dr.decode(2048); while (r.status == DecodeStatus::Ok && ++calls < 10000);
                if (r.status == DecodeStatus::Ok) dr.behaved = false;
            } else if (!dr.reason[0]) {
                dr.behaved = false;
            }
            if (!dr.behaved) ++misbehaved;
            dr.free(); fed.free();
        }
        mem_free(work.data);
        kvtest::drop(base);
    }
    KV_CHECK(c, misbehaved == 0);
    KV_CHECK(c, opened > runs / 2);
    KV_INFO("test", "mp3 spoiled: %u files, %u opened, %u decodes ended damaged", runs, opened, damaged);
}
