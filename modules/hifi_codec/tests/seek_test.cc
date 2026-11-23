// The file in pieces, and seeking: the FLAC decoder over a sparse span whose pieces arrive
// in every awkward order, and seeks checked against a straight decode of the same file.
//
// The span is reserved address space, and a piece is committed only when it is delivered,
// so a decoder that reads a piece it was not given faults: it cannot get away with
// reading zeros. A straight decode of a whole file is the oracle for everything here; the
// straight decode is itself checked against FLAC's own MD5 in flac_test.cc.
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_codec/flac.h"
#include "korvid/hifi_codec/sparse.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/random.h"
#include "korvid/core/test.h"
#include "data.h"
#include "pieces.h"
#include "md5.h"
#include <initializer_list>
#include <string.h>

using namespace kv;
using kvtest::File;
using kvtest::read_data;
using namespace kvtest;

namespace {

// The FLAC MD5 of some samples.
void md5_of(const i32* s, u64 frames, const AudioFormat& f, u8 out[16]) {
    kvtest::Md5 m;
    u32 nbytes = (f.bits + 7u) / 8u;
    u8 buf[4096];
    u32 fill = 0;
    for (usize i = 0, n = (usize)frames * f.channels; i < n; ++i) {
        for (u32 b = 0; b < nbytes; ++b) buf[fill++] = (u8)((u32)s[i] >> (8 * b));
        if (fill > sizeof buf - 4) { m.update(buf, fill); fill = 0; }
    }
    m.update(buf, fill);
    m.finish(out);
}

constexpr const char* kGoodNames[] = {
    "tone_16_1ch_44100.flac", "tone_16_2ch_44100.flac", "tone_16_2ch_48000.flac", "tone_24_2ch_48000.flac",
    "tone_24_1ch_96000.flac", "tone_24_2ch_96000.flac", "tone_16_2ch_192000.flac", "tone_24_2ch_192000.flac",
    "tone_24_2ch_192000_seektable.flac",
};

} // namespace

void test_sparse_span(TestCtx& c) {
    c.begin("sparse span");
    u8 bytes[10000] = {};
    u8 have[3] = {1, 0, 1};
    SparseSpan s{bytes, 10000, 12, have}; // pieces of 4096: [0, 4096) [4096, 8192) [8192, 10000)
    KV_CHECK(c, sparse_pieces(s) == 3);
    KV_CHECK(c, sparse_has(s, 0, 4096) && !sparse_has(s, 0, 4097) && sparse_has(s, 8192, 5000) && sparse_has(s, 20000, 5));
    KV_CHECK(c, sparse_has(s, 100, 0));
    ByteRange m = sparse_missing(s, 100, 9000);
    KV_CHECK(c, m.at == 4096 && m.len == 9100 - 4096);
    m = sparse_missing(s, 5000, 100000);
    KV_CHECK(c, m.at == 5000 && m.len == 5000); // clipped to the file
    KV_CHECK(c, sparse_missing(s, 8192, 100).len == 0);
    SparseSpan e{bytes, 0, 12, have};
    KV_CHECK(c, sparse_pieces(e) == 0 && sparse_has(e, 0, 10));
    SparseSpan w = sparse_whole(Span<const u8>(bytes, 10000));
    KV_CHECK(c, sparse_pieces(w) == 1 && sparse_has(w, 0, 10000) && w.bytes == bytes && w.size == 10000);
}

void test_flac_pieces(TestCtx& c) {
    c.begin("flac in pieces");
    u32 sh[4];
    u32 nsh = shifts(sh);
    u32 runs = 0, waits = 0;
    for (const char* name : kGoodNames) {
        File f = read_data(name);
        Pcm want = straight(f.bytes());
        KV_CHECK(c, want.frames > 0 && want.status == DecodeStatus::End);
        u8 md5[16];
        md5_of(want.s, want.frames, want.info.format, md5);
        KV_CHECK(c, memcmp(md5, f.data + 26, 16) == 0); // STREAMINFO is first, and its MD5 26 bytes in

        for (u32 k = 0; k < nsh; ++k) {
            for (u32 o = 0; o < (u32)Order::Count; ++o) {
                for (u32 chunk : {1009u, 8192u}) {
                    Fed fed;
                    fed.make(f, sh[k]);
                    Driver dr;
                    dr.make(fed, (Order)o, chunk);
                    bool opened = dr.open();
                    KV_CHECK(c, opened && dr.behaved);
                    if (opened && dr.info.frames == want.frames) {
                        Pcm got;
                        got.ch = dr.info.format.channels;
                        got.s = (i32*)mem_alloc((usize)(want.frames + chunk) * got.ch * 4);
                        DecodeResult r;
                        do {
                            r = dr.decode(chunk);
                            memcpy(got.s + got.frames * got.ch, dr.out, (usize)r.frames * got.ch * 4);
                            got.frames += r.frames;
                        } while (r.status == DecodeStatus::Ok && got.frames <= want.frames);
                        u8 m2[16];
                        md5_of(got.s, got.frames, dr.info.format, m2);
                        bool ok = r.status == DecodeStatus::End && got.frames == want.frames && memcmp(m2, md5, 16) == 0 && dr.behaved;
                        if (!ok) KV_ERROR("test", "%s in pieces of %u, %s, %u at a time: %llu frames, %s", name, 1u << sh[k],
                                          order_name((Order)o), chunk, (unsigned long long)got.frames, decode_status_name(r.status));
                        KV_CHECK(c, ok);
                        drop(got);
                    }
                    waits += dr.waits;
                    ++runs;
                    dr.free();
                    fed.free();
                }
            }
        }
        drop(want);
        kvtest::drop(f);
    }
    KV_INFO("test", "flac in pieces: %u decodes, %u waits", runs, waits);
}

void test_flac_seek(TestCtx& c) {
    c.begin("flac seek");
    u32 sh[4];
    u32 nsh = shifts(sh);
    u32 seeks = 0;
    for (const char* name : kGoodNames) {
        File f = read_data(name);
        Pcm want = straight(f.bytes());
        u64 n = want.frames;
        Rng rng = Rng::seeded(n, 5);
        // Targets: the edges of the file and of its frames (4608 a block), and anywhere.
        u64 targets[40];
        u32 nt = 0;
        for (u64 t : {(u64)0, (u64)1, (u64)4607, (u64)4608, (u64)4609, (u64)9216, n / 2, n - 4608, n - 1, n}) targets[nt++] = t;
        while (nt < 40) targets[nt++] = rng.next() % n;

        // Whole and in pieces; the targets in order, then shuffled, from one decoder each time.
        for (u32 k = 0; k <= nsh; ++k) {
            for (u32 pass = 0; pass < 2; ++pass) {
                Fed fed;
                fed.make(f, k < nsh ? sh[k] : 18);
                if (k == nsh) fed.give_all();
                Driver dr;
                dr.make(fed, pass ? Order::Scattered : Order::Asked, 1500);
                bool opened = dr.open();
                KV_CHECK(c, opened);
                if (!opened) { dr.free(); fed.free(); continue; }
                u64 order[40];
                memcpy(order, targets, sizeof order);
                if (pass) for (u32 i = 39; i > 0; --i) { u32 j = rng.next() % (i + 1); u64 t = order[i]; order[i] = order[j]; order[j] = t; }
                for (u64 t : order) {
                    SeekResult s = dr.seek(t);
                    KV_CHECK(c, s.status == SeekStatus::Ok);
                    u64 len = n - t < 3000 ? n - t : 3000;
                    DecodeStatus end = DecodeStatus::Ok;
                    bool same = s.status == SeekStatus::Ok && same_from(dr, want, t, len, &end);
                    if (t + len == n && same) { // at the end: it says so
                        DecodeResult r = dr.decode(16);
                        same = r.frames == 0 && r.status == DecodeStatus::End;
                    }
                    if (!same) KV_ERROR("test", "%s: a seek to %llu (pieces of %u, pass %u) is not the straight decode", name,
                                        (unsigned long long)t, 1u << (k < nsh ? sh[k] : 18), pass);
                    KV_CHECK(c, same);
                    ++seeks;
                }
                // Past the end: refused in words, and the decoder is where it was.
                dr.seek(1000);
                SeekResult past = dr.seek(n + 1);
                KV_CHECK(c, past.status == SeekStatus::Failed && past.reason[0] != 0);
                KV_CHECK(c, same_from(dr, want, 1000, 2000));
                KV_CHECK(c, dr.behaved);
                dr.free();
                fed.free();
            }
        }

        // Behind an ID3v2 tag: the same seeks land on the same frames.
        {
            u32 tag = 10 + 5000;
            File t;
            t.len = tag + f.len;
            t.data = (u8*)mem_alloc(t.len);
            memset(t.data, 0, tag);
            memcpy(t.data, "ID3\x04\x00\x00", 6);
            t.data[8] = (u8)(5000 >> 7); t.data[9] = (u8)(5000 & 0x7F);
            memcpy(t.data + tag, f.data, f.len);
            Fed fed;
            fed.make(t, sh[0]);
            Driver dr;
            dr.make(fed, Order::Asked, 1000);
            KV_CHECK(c, dr.open() && dr.info.frames == n);
            for (u32 i = 0; i < 12; ++i) {
                u64 at = targets[39 - i] < n - 1000 ? targets[39 - i] : n - 1000;
                KV_CHECK(c, dr.seek(at).status == SeekStatus::Ok && same_from(dr, want, at, 1000));
            }
            KV_CHECK(c, dr.behaved);
            dr.free();
            fed.free();
            kvtest::drop(t);
        }

        // A seek that waits and is then left: the decoder goes on from where it was.
        {
            Fed fed;
            fed.make(f, sh[0]);
            Driver dr;
            dr.make(fed, Order::Asked, 1000);
            dr.open();
            KV_CHECK(c, same_from(dr, want, 0, 1000));
            SeekResult s = dr.d->seek(dr.state, n - 10);
            KV_CHECK(c, s.status == SeekStatus::Wait);
            KV_CHECK(c, same_from(dr, want, 1000, 6000));
            KV_CHECK(c, dr.behaved);
            dr.free();
            fed.free();
        }
        drop(want);
        kvtest::drop(f);
    }
    KV_INFO("test", "flac seek: %u seeks", seeks);
}

// Seeks into damage, past a file's real end, and in a file that does not say its length.
void test_flac_seek_broken(TestCtx& c) {
    c.begin("flac seek broken");
    File good = read_data("tone_16_2ch_44100.flac");
    Pcm want = straight(good.bytes());

    // Cut off inside a frame: a seek before the cut is exact; one after it fails or finds
    // nothing, and either way the decoder can still seek back.
    {
        File f = read_data("broken/truncated.flac");
        Pcm cut = straight(f.bytes());
        Fed fed; fed.make(f, 12);
        Driver dr; dr.make(fed, Order::Asked, 1024);
        KV_CHECK(c, dr.open() && dr.info.frames == want.frames);
        KV_CHECK(c, dr.seek(cut.frames / 2).status == SeekStatus::Ok && same_from(dr, want, cut.frames / 2, 2000));
        SeekResult s = dr.seek(want.frames - 100);
        if (s.status == SeekStatus::Ok) {
            DecodeResult r = dr.decode(1024);
            KV_CHECK(c, r.frames == 0 && r.status == DecodeStatus::Damaged && r.reason[0] != 0);
        } else {
            KV_CHECK(c, s.status == SeekStatus::Failed && s.reason[0] != 0);
        }
        KV_CHECK(c, dr.seek(100).status == SeekStatus::Ok && same_from(dr, want, 100, 4000));
        KV_CHECK(c, dr.behaved);
        dr.free(); fed.free(); drop(cut); kvtest::drop(f);
    }
    // A header that promises twice the frames: a seek into the half that is not there
    // fails or finds nothing; a seek into the half that is, is exact.
    {
        File f = read_data("broken/lying_length.flac");
        Fed fed; fed.make(f, 12);
        Driver dr; dr.make(fed, Order::Stingy, 1024);
        KV_CHECK(c, dr.open() && dr.info.frames == want.frames * 2);
        SeekResult s = dr.seek(want.frames + 5000);
        if (s.status == SeekStatus::Ok) {
            DecodeResult r = dr.decode(1024);
            KV_CHECK(c, r.frames == 0 && r.status == DecodeStatus::Damaged);
        } else {
            KV_CHECK(c, s.reason[0] != 0);
        }
        KV_CHECK(c, dr.seek(want.frames - 3000).status == SeekStatus::Ok && same_from(dr, want, want.frames - 3000, 3000));
        KV_CHECK(c, dr.behaved);
        dr.free(); fed.free(); kvtest::drop(f);
    }
    // One frame spoiled: a seek before it is exact up to it, where the decode ends Damaged
    // as a straight one does; a seek into or past it fails, or lands, and whatever then
    // plays is the file's own samples from some frame on, never noise.
    {
        File f = read_data("broken/bad_frame.flac");
        Fed fed; fed.make(f, 12);
        Driver dr; dr.make(fed, Order::Backward, 4608);
        KV_CHECK(c, dr.open());
        u32 exact = 0, into = 0;
        for (u64 t = 0; t < want.frames; t += 1500) {
            SeekResult s = dr.seek(t);
            if (s.status != SeekStatus::Ok) { KV_CHECK(c, s.reason[0] != 0); ++into; continue; }
            u64 len = want.frames - t < 1000 ? want.frames - t : 1000;
            DecodeResult r = dr.decode((u32)len);
            bool at_t = memcmp(dr.out, want.s + t * 2, (usize)r.frames * 8) == 0;
            if (at_t && r.frames == len) { ++exact; continue; }
            if (at_t && r.frames && r.status == DecodeStatus::Damaged) { ++exact; ++into; continue; } // up to the damage, as a straight decode stops
            ++into;
            // Not the samples at t: then the file's own from a later frame boundary, or none.
            bool found = r.frames == 0;
            for (u64 at = 0; !found && at + r.frames <= want.frames; at += 4608)
                found = memcmp(dr.out, want.s + at * 2, (usize)r.frames * 8) == 0;
            KV_CHECK(c, found);
        }
        KV_CHECK(c, exact >= 10 && into >= 1); // most of the file is good, and the damage was met
        KV_CHECK(c, dr.seek(0).status == SeekStatus::Ok && same_from(dr, want, 0, 4608));
        KV_CHECK(c, dr.behaved);
        KV_INFO("test", "flac seek into damage: %u exact, %u into the spoiled frame", exact, into);
        dr.free(); fed.free(); kvtest::drop(f);
    }
    // A file that does not say its length: no binary search, so seeks are by brute force,
    // and past the end is found by reaching it.
    {
        File f = read_data("tone_16_2ch_44100.flac");
        u64 packed = 0;
        for (u32 i = 0; i < 8; ++i) packed = (packed << 8) | f.data[18 + i];
        packed &= ~(((u64)1 << 36) - 1);
        for (u32 i = 0; i < 8; ++i) f.data[18 + i] = (u8)(packed >> (56 - 8 * i));
        Fed fed; fed.make(f, 12);
        Driver dr; dr.make(fed, Order::Scattered, 1024);
        KV_CHECK(c, dr.open() && dr.info.frames == 0);
        for (u64 t : {(u64)17000, (u64)3, (u64)9300, want.frames - 1}) {
            SeekResult s = dr.seek(t);
            KV_CHECK(c, s.status == SeekStatus::Ok);
            KV_CHECK(c, same_from(dr, want, t, want.frames - t < 500 ? want.frames - t : 500));
        }
        dr.seek(2000);
        SeekResult past = dr.seek(want.frames + 10000);
        KV_CHECK(c, past.status == SeekStatus::Failed && past.reason[0] != 0);
        KV_CHECK(c, same_from(dr, want, 2000, 1000));
        KV_CHECK(c, dr.behaved);
        dr.free(); fed.free(); kvtest::drop(f);
    }
    // Spoiled files, in pieces, seeked at random: only the contract's promises.
    {
        File base = read_data("tone_24_2ch_48000.flac");
        File work; work.data = (u8*)mem_alloc(base.len); work.len = base.len;
        Rng rng = Rng::seeded(20261123, 9);
        u32 misbehaved = 0;
        for (u32 round = 0; round < 200; ++round) {
            memcpy(work.data, base.data, base.len);
            work.len = round % 3 == 2 ? rng.next() % (base.len + 1) : base.len;
            if (round % 3 != 2) for (u32 k = 0, flips = 1 + rng.next() % 8; k < flips; ++k) work.data[rng.next() % base.len] ^= (u8)(1u << (rng.next() % 8));
            Fed fed; fed.make(work, 12);
            Driver dr; dr.make(fed, (Order)(round % (u32)Order::Count), 2048);
            if (dr.open()) {
                for (u32 k = 0; k < 6 && dr.behaved; ++k) {
                    dr.seek(rng.next() % 30000);
                    dr.decode(2048);
                }
            }
            if (!dr.behaved) ++misbehaved;
            dr.free(); fed.free();
        }
        KV_CHECK(c, misbehaved == 0);
        mem_free(work.data);
        kvtest::drop(base);
    }
    drop(want);
    kvtest::drop(good);
}
