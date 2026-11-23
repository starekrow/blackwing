// The FLAC decoder against files made by libFLAC (tests/data/make_data.py), and against
// the same files broken.
//
// The oracle for a good file is FLAC's own: its header holds the MD5 of the audio it was
// encoded from, and a decode is right when its samples, packed as the encoder saw them,
// hash to that. make_data.py checked each header's MD5 against the PCM it encoded, so the
// chain does not rest on a FLAC decoder agreeing with itself.
//
// A broken file has no oracle but this: it must never crash, never write outside what it
// was given, never return more than it was asked for, and say what went wrong in words.
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_codec/flac.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/random.h"
#include "korvid/core/test.h"
#include "data.h"
#include "md5.h"
#include <initializer_list>
#include <stdio.h>
#include <string.h>

using namespace kv;

namespace {

constexpr u32 kGuard = 0xC0FFEE11u;
constexpr u32 kGuardWords = 64;

using kvtest::File;
using kvtest::read_data;
using kvtest::drop;

// A decoder's memory and an output buffer, each with guard words after it.
struct Rig {
    u8*  memory = nullptr; u32 memory_len = 0;
    i32* out = nullptr;    u32 out_frames = 0, out_channels = 0;

    void make(u32 mem_bytes, u32 frames, u32 channels) {
        memory_len = mem_bytes;
        memory = (u8*)mem_alloc(mem_bytes + kGuardWords * 4);
        out_frames = frames; out_channels = channels;
        out = (i32*)mem_alloc(((usize)frames * channels + kGuardWords) * 4);
        for (u32 i = 0; i < kGuardWords; ++i) {
            memcpy(memory + mem_bytes + i * 4, &kGuard, 4);
            out[(usize)frames * channels + i] = (i32)kGuard;
        }
    }
    bool intact() const {
        for (u32 i = 0; i < kGuardWords; ++i) {
            u32 w; memcpy(&w, memory + memory_len + i * 4, 4);
            if (w != kGuard || (u32)out[(usize)out_frames * out_channels + i] != kGuard) return false;
        }
        return true;
    }
    void free() { mem_free(memory); mem_free(out); *this = Rig{}; }
};

// A whole decode, kept.
struct Decoded {
    bool         opened = false;
    const char*  open_reason = "";
    DecoderInfo  info;
    i32*         samples = nullptr; // interleaved
    u64          frames = 0;
    DecodeStatus status = DecodeStatus::Ok;
    char         reason[128] = "";
    bool         behaved = true;    // never more than asked, guards intact, the end is sticky
};
void drop(Decoded& d) { mem_free(d.samples); d = Decoded{}; }

// Decodes `bytes` with the FLAC decoder in pieces of `chunk` frames, keeping at most
// `keep` frames (and stopping there: a lying header may promise billions).
Decoded decode_all(Span<const u8> bytes, u32 chunk, u64 keep, u32 mem_bytes = 0) {
    const DecoderDesc& d = k_flac_decoder;
    Decoded r;
    Rig rig;
    rig.make(mem_bytes ? mem_bytes : d.memory_bytes, chunk, 8);
    DecoderOpen o = d.open(sparse_whole(bytes), Span<u8>(rig.memory, rig.memory_len));
    r.open_reason = o.reason;
    r.behaved = rig.intact();
    if (o.state) {
        r.opened = true;
        r.info = o.info;
        u32 ch = o.info.format.channels;
        r.samples = (i32*)mem_alloc((usize)(keep ? keep : 1) * ch * 4);
        for (;;) {
            u32 ask = chunk;
            if (keep - r.frames < ask) ask = (u32)(keep - r.frames);
            if (ask == 0) break;
            DecodeResult x = d.decode(o.state, rig.out, ask);
            if (x.frames > ask || !rig.intact()) { r.behaved = false; break; }
            memcpy(r.samples + r.frames * ch, rig.out, (usize)x.frames * ch * 4);
            r.frames += x.frames;
            r.status = x.status;
            if (x.status != DecodeStatus::Ok) {
                format(r.reason, sizeof r.reason, "%s", x.reason);
                DecodeResult again = d.decode(o.state, rig.out, ask);
                if (again.frames != 0 || again.status != x.status) r.behaved = false;
                break;
            }
            if (x.frames < ask) { r.behaved = false; break; } // short with Ok is against the contract
        }
        d.close(o.state);
    }
    rig.free();
    return r;
}

// The MD5 FLAC would have computed: each sample little-endian in the whole bytes its depth needs.
void pcm_md5(const Decoded& d, u8 out[16]) {
    kvtest::Md5 m;
    u32 nbytes = (d.info.format.bits + 7u) / 8u;
    usize n = (usize)d.frames * d.info.format.channels;
    u8 buf[4096];
    u32 fill = 0;
    for (usize i = 0; i < n; ++i) {
        u32 v = (u32)d.samples[i];
        for (u32 b = 0; b < nbytes; ++b) buf[fill++] = (u8)(v >> (8 * b));
        if (fill > sizeof buf - 4) { m.update(buf, fill); fill = 0; }
    }
    m.update(buf, fill);
    m.finish(out);
}

bool in_range(const Decoded& d) {
    i64 hi = ((i64)1 << (d.info.format.bits - 1)) - 1, lo = -hi - 1;
    usize n = (usize)d.frames * d.info.format.channels;
    for (usize i = 0; i < n; ++i)
        if (d.samples[i] < lo || d.samples[i] > hi) return false;
    return true;
}

struct Good { const char* name; u16 bits, channels; u32 rate; u64 frames; };
constexpr Good kGood[] = {
    {"tone_16_1ch_44100.flac", 16, 1, 44100, 22050},  {"tone_16_2ch_44100.flac", 16, 2, 44100, 22050},
    {"tone_16_2ch_48000.flac", 16, 2, 48000, 24000},  {"tone_24_2ch_48000.flac", 24, 2, 48000, 24000},
    {"tone_24_1ch_96000.flac", 24, 1, 96000, 24000},  {"tone_24_2ch_96000.flac", 24, 2, 96000, 24000},
    {"tone_16_2ch_192000.flac", 16, 2, 192000, 48000}, {"tone_24_2ch_192000.flac", 24, 2, 192000, 48000},
};

} // namespace

void test_md5(TestCtx& c) {
    c.begin("md5");
    // RFC 1321's test suite.
    struct { const char* text; const char* hex; } cases[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"},
    };
    for (auto& k : cases) {
        kvtest::Md5 m;
        m.update(k.text, strlen(k.text));
        u8 d[16]; m.finish(d);
        char hex[33];
        for (u32 i = 0; i < 16; ++i) format(hex + i * 2, 3, "%02x", d[i]);
        KV_CHECK(c, strcmp(hex, k.hex) == 0);
    }
}

void test_flac_good(TestCtx& c) {
    c.begin("flac good");
    for (const Good& g : kGood) {
        File f = read_data(g.name);
        KV_CHECK(c, f.len > 42);
        if (f.len <= 42) continue;
        Span<const u8> bytes(f.data, f.len);
        const DecoderDesc* d = decoder_for(bytes);
        KV_CHECK(c, d == &k_flac_decoder);
        KV_CHECK(c, decoder_for(Span<const u8>(f.data, 64)) == &k_flac_decoder); // the head is enough to say yes

        // Three sizes of piece: one frame at a time, an odd size, more than the file.
        for (u32 chunk : {1u, 1009u, 65536u}) {
            Decoded r = decode_all(bytes, chunk, g.frames + 1000);
            KV_CHECK(c, r.opened && r.behaved);
            KV_CHECK(c, r.info.format.bits == g.bits && r.info.format.channels == g.channels && r.info.format.rate == g.rate);
            KV_CHECK(c, r.info.frames == g.frames && r.info.delay == 0 && r.info.padding == 0);
            KV_CHECK(c, r.frames == g.frames);
            KV_CHECK(c, r.status == DecodeStatus::End);
            KV_CHECK(c, in_range(r));
            u8 md5[16];
            pcm_md5(r, md5);
            KV_CHECK(c, memcmp(md5, f.data + 26, 16) == 0); // STREAMINFO's MD5: the file's samples exactly
            drop(r);
        }
        drop(f);
    }
}

void test_flac_contract(TestCtx& c) {
    c.begin("flac contract");
    File f = read_data("tone_16_2ch_44100.flac");
    Span<const u8> bytes(f.data, f.len);
    const DecoderDesc& d = k_flac_decoder;
    KV_CHECK(c, strcmp(d.name, "flac") == 0 && decoders().len == 2 && decoders()[0] == &d); // FLAC first, then MP3

    // Two decoders of one file, a call each in turn at different sizes: neither disturbs
    // the other, since each one's state is in its own memory.
    Rig a, b;
    a.make(d.memory_bytes, 700, 2);
    b.make(d.memory_bytes, 300, 2);
    DecoderOpen oa = d.open(sparse_whole(bytes), Span<u8>(a.memory, a.memory_len));
    DecoderOpen ob = d.open(sparse_whole(bytes), Span<u8>(b.memory, b.memory_len));
    KV_CHECK(c, oa.state && ob.state && oa.state != ob.state);
    KV_CHECK(c, (u8*)oa.state >= a.memory && (u8*)oa.state < a.memory + a.memory_len); // the state is in the memory given
    Decoded whole = decode_all(bytes, 4096, 30000);
    u64 na = 0, nb = 0;
    bool same = true;
    while (oa.state && ob.state) {
        DecodeResult ra = d.decode(oa.state, a.out, 700);
        DecodeResult rb = d.decode(ob.state, b.out, 300);
        if (na + ra.frames <= whole.frames) same = same && memcmp(a.out, whole.samples + na * 2, (usize)ra.frames * 8) == 0; else same = false;
        if (nb + rb.frames <= whole.frames) same = same && memcmp(b.out, whole.samples + nb * 2, (usize)rb.frames * 8) == 0; else same = false;
        na += ra.frames; nb += rb.frames;
        if (ra.status != DecodeStatus::Ok && rb.status != DecodeStatus::Ok) break;
    }
    KV_CHECK(c, same && na == whole.frames && nb == whole.frames);
    KV_CHECK(c, a.intact() && b.intact());
    if (oa.state) d.close(oa.state);
    if (ob.state) d.close(ob.state);
    a.free(); b.free();

    // Memory that is misaligned, and memory that is too small: the first works, the second
    // fails in words and writes nowhere it should not.
    {
        Rig r; r.make(d.memory_bytes + 16, 512, 2);
        DecoderOpen o = d.open(sparse_whole(bytes), Span<u8>(r.memory + 3, d.memory_bytes));
        KV_CHECK(c, o.state != nullptr);
        if (o.state) { DecodeResult x = d.decode(o.state, r.out, 512); KV_CHECK(c, x.frames == 512 && memcmp(r.out, whole.samples, 512 * 8) == 0); d.close(o.state); }
        KV_CHECK(c, r.intact());
        r.free();
    }
    for (u32 small : {0u, 16u, 1024u, 20000u}) {
        Decoded r = decode_all(bytes, 512, 1000, small ? small : 1);
        KV_CHECK(c, !r.opened && r.behaved && r.open_reason[0] != 0);
        drop(r);
    }
    {
        DecoderOpen o = d.open(sparse_whole(bytes), Span<u8>());
        KV_CHECK(c, !o.state && o.reason[0] != 0);
    }

    // A FLAC stream behind an ID3v2 tag is still a FLAC file.
    {
        u32 tag = 10 + 300;
        u8* t = (u8*)mem_alloc(tag + f.len);
        memset(t, 0, tag);
        memcpy(t, "ID3\x04\x00\x00", 6);
        t[8] = (u8)(300 >> 7); t[9] = (u8)(300 & 0x7F);
        memcpy(t + tag, f.data, f.len);
        Span<const u8> tagged(t, tag + f.len);
        KV_CHECK(c, decoder_for(tagged) == &d);
        Decoded r = decode_all(tagged, 4096, 30000);
        KV_CHECK(c, r.opened && r.frames == whole.frames && r.status == DecodeStatus::End);
        KV_CHECK(c, r.opened && r.frames == whole.frames && memcmp(r.samples, whole.samples, (usize)whole.frames * 8) == 0);
        drop(r);
        mem_free(t);
    }

    KV_CHECK(c, strcmp(decode_status_name(DecodeStatus::Damaged), "damaged") == 0);
    drop(whole);
    drop(f);
}

void test_flac_broken(TestCtx& c) {
    c.begin("flac broken");
    File good = read_data("tone_16_2ch_44100.flac");
    Decoded whole = decode_all(Span<const u8>(good.data, good.len), 4096, 30000);
    KV_CHECK(c, whole.frames == 22050);

    // Not FLAC at all: nobody claims it, and opening it anyway fails in words.
    for (const char* name : {"broken/empty.flac", "broken/not_flac.flac"}) {
        File f = read_data(name);
        Span<const u8> bytes(f.data, f.len);
        KV_CHECK(c, decoder_for(bytes) == nullptr);
        Decoded r = decode_all(bytes, 512, 1000);
        KV_CHECK(c, !r.opened && r.behaved && r.open_reason[0] != 0);
        drop(r); drop(f);
    }
    // The magic and nothing else: claimed, and then refused.
    {
        File f = read_data("broken/magic_only.flac");
        Span<const u8> bytes(f.data, f.len);
        KV_CHECK(c, decoder_for(bytes) == &k_flac_decoder);
        Decoded r = decode_all(bytes, 512, 1000);
        KV_CHECK(c, !r.opened && r.behaved && r.open_reason[0] != 0);
        drop(r); drop(f);
    }
    // A header and no audio: refused, or opened and found empty and short.
    {
        File f = read_data("broken/header_only.flac");
        Decoded r = decode_all(Span<const u8>(f.data, f.len), 512, 30000);
        KV_CHECK(c, r.behaved);
        KV_CHECK(c, !r.opened ? r.open_reason[0] != 0 : (r.frames == 0 && r.status == DecodeStatus::Damaged && r.reason[0] != 0));
        drop(r); drop(f);
    }
    // Cut off inside a frame: what came before the cut plays, exactly, and the end is Damaged.
    {
        File f = read_data("broken/truncated.flac");
        Decoded r = decode_all(Span<const u8>(f.data, f.len), 1009, 30000);
        KV_CHECK(c, r.opened && r.behaved);
        KV_CHECK(c, r.frames > 4096 && r.frames < whole.frames);
        KV_CHECK(c, r.status == DecodeStatus::Damaged && r.reason[0] != 0);
        KV_CHECK(c, r.frames <= whole.frames && memcmp(r.samples, whole.samples, (usize)r.frames * 8) == 0);
        drop(r); drop(f);
    }
    // A header that promises twice the frames: all the real ones play, and the end is Damaged.
    {
        File f = read_data("broken/lying_length.flac");
        Decoded r = decode_all(Span<const u8>(f.data, f.len), 4096, 60000);
        KV_CHECK(c, r.opened && r.behaved && r.info.frames == 44100);
        KV_CHECK(c, r.frames == whole.frames && memcmp(r.samples, whole.samples, (usize)whole.frames * 8) == 0);
        KV_CHECK(c, r.status == DecodeStatus::Damaged && r.reason[0] != 0);
        drop(r); drop(f);
    }
    // One frame spoiled in the middle: it is dropped, not played as noise. What comes
    // before it is exact, nothing is invented, and the end says frames were lost.
    {
        File f = read_data("broken/bad_frame.flac");
        Decoded r = decode_all(Span<const u8>(f.data, f.len), 4096, 30000);
        KV_CHECK(c, r.opened && r.behaved);
        KV_CHECK(c, r.frames >= 4096 && r.frames < whole.frames);
        KV_CHECK(c, r.frames >= 4096 && memcmp(r.samples, whole.samples, 4096 * 8) == 0);
        KV_CHECK(c, r.status == DecodeStatus::Damaged && r.reason[0] != 0);
        KV_CHECK(c, in_range(r));
        drop(r); drop(f);
    }
    // A header that claims 8 channels of 32 bits over frames of 2 channels of 16: whatever
    // it makes of that, it stays inside what it was given.
    {
        File f = read_data("broken/lying_format.flac");
        Decoded r = decode_all(Span<const u8>(f.data, f.len), 4096, 30000);
        KV_CHECK(c, r.behaved);
        KV_CHECK(c, !r.opened ? r.open_reason[0] != 0 : r.frames <= 30000);
        drop(r); drop(f);
    }
    drop(whole);
    drop(good);
}

// A good file with a few bytes changed, or cut at any length, many times over with a
// seeded generator: nothing to compare against, only the contract's promises.
void test_flac_spoiled(TestCtx& c) {
    c.begin("flac spoiled");
    File good = read_data("tone_24_2ch_48000.flac");
    u8* work = (u8*)mem_alloc(good.len);
    Rng rng = Rng::seeded(20261122, 7);
    u32 opened = 0, damaged = 0, misbehaved = 0;
    for (u32 round = 0; round < 600; ++round) {
        memcpy(work, good.data, good.len);
        u32 len = good.len;
        if (round % 3 == 2) {
            len = rng.next() % (good.len + 1);                       // cut anywhere
        } else {
            u32 flips = 1 + rng.next() % 8;
            u32 span = (round % 3 == 0) ? 256 : good.len;             // in the headers, or anywhere
            for (u32 k = 0; k < flips; ++k) work[rng.next() % span] ^= (u8)(1u << (rng.next() % 8));
        }
        Decoded r = decode_all(Span<const u8>(work, len), 2048, 60000);
        if (!r.behaved) ++misbehaved;
        if (r.opened) ++opened;
        if (r.opened && r.status == DecodeStatus::Damaged) ++damaged;
        if (!r.opened && r.open_reason[0] == 0) ++misbehaved;         // a refusal says why
        if (r.opened && r.status == DecodeStatus::Damaged && r.reason[0] == 0) ++misbehaved;
        drop(r);
    }
    KV_CHECK(c, misbehaved == 0);
    KV_CHECK(c, opened > 300);  // most of them still open: the test reaches the frame decoder
    KV_CHECK(c, damaged > 100); // and most of those are caught
    KV_INFO("test", "flac spoiled: 600 files, %u opened, %u ended damaged", opened, damaged);
    mem_free(work);
    drop(good);
}
