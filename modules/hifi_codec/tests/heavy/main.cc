// hifi_codec's heavy test: real FLAC files, as many as it is pointed at.
//
//   kormake test hifi_codec --heavy -- ~/Music/SomeAlbum another/dir one.flac
//
// Every .flac under each argument is decoded whole and its samples' MD5 compared with the
// one in its header. A file whose header has no MD5 (all zeros) is decoded and counted
// but proves nothing. With no arguments there is nothing to do and it passes: the files
// are somebody's music and are not in the repository. It also prints how fast each
// decoded, as a multiple of real time.
//
// Then each file is seeked to sixteen places at random, in random order, and what is
// decoded from each is compared with the same stretch of the straight decode.
#include "korvid/hifi_codec/decoder.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/random.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "../md5.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

using namespace kv;

namespace {

struct Totals { u32 files = 0, exact = 0, unproven = 0, failed = 0; f64 audio = 0.0, spent = 0.0; f64 slowest = 1e30;
                u32 seeks = 0, seeks_wrong = 0; f64 seek_spent = 0.0, seek_slowest = 0.0; };

constexpr u32 kTargets = 16;   // seeks per file
constexpr u32 kStretch = 1024; // frames compared after each

bool ends_with_flac(const char* p) {
    usize n = strlen(p);
    if (n < 5) return false;
    const char* e = p + n - 5;
    return e[0] == '.' && (e[1] | 32) == 'f' && (e[2] | 32) == 'l' && (e[3] | 32) == 'a' && (e[4] | 32) == 'c';
}

void one_file(TestCtx& c, const char* path, Totals& t, u8* memory, i32* out) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { KV_ERROR("test", "cannot open %s", path); ++t.failed; c.check(false, "open the file", path, 0); return; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    u8* data = (u8*)mem_alloc((usize)size + 1);
    usize got = fread(data, 1, (usize)size, fp);
    fclose(fp);
    Span<const u8> bytes(data, (u32)got);
    const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    ++t.files;

    const DecoderDesc* d = decoder_for(bytes);
    DecoderOpen o;
    if (d) o = d->open(sparse_whole(bytes), Span<u8>(memory, d->memory_bytes));
    if (!d || !o.state) {
        KV_ERROR("test", "%s: %s", name, d ? o.reason : "no decoder takes it");
        ++t.failed; c.check(false, "a decoder opens it", path, 0);
        mem_free(data);
        return;
    }
    // Where STREAMINFO's MD5 is: 26 bytes past "fLaC", which may follow an ID3v2 tag.
    u32 at = 0;
    if (got >= 10 && memcmp(data, "ID3", 3) == 0)
        at = 10 + (((u32)data[6] << 21) | ((u32)data[7] << 14) | ((u32)data[8] << 7) | data[9]) + ((data[5] & 0x10) ? 10u : 0u);
    const u8* want = data + at + 26;
    bool has_md5 = false;
    for (u32 i = 0; i < 16; ++i) has_md5 = has_md5 || want[i] != 0;

    const AudioFormat fmt = o.info.format;
    u32 nbytes = (fmt.bits + 7u) / 8u;
    kvtest::Md5 m;
    u64 frames = 0;
    // The seek targets, and the straight decode's frames at each, kept as they go by.
    Rng rng = Rng::seeded((u64)got, 11);
    u64 targets[kTargets] = {};
    u32 ntargets = o.info.frames > kStretch ? kTargets : 0;
    for (u32 i = 0; i < ntargets; ++i) targets[i] = (((u64)rng.next() << 32) | rng.next()) % (o.info.frames - kStretch);
    i32* kept = (i32*)mem_alloc((usize)kTargets * kStretch * fmt.channels * 4);
    DecodeResult r;
    static u8 packed[4096 * 8 * 4];
    f64 t0 = time_now();
    do {
        r = d->decode(o.state, out, 4096);
        usize n = (usize)r.frames * fmt.channels, fill = 0;
        for (usize i = 0; i < n; ++i) {
            u32 v = (u32)out[i];
            for (u32 b = 0; b < nbytes; ++b) packed[fill++] = (u8)(v >> (8 * b));
        }
        m.update(packed, fill);
        for (u32 i = 0; i < ntargets; ++i) {
            u64 from = targets[i] > frames ? targets[i] : frames, to = targets[i] + kStretch < frames + r.frames ? targets[i] + kStretch : frames + r.frames;
            if (from < to) memcpy(kept + ((usize)i * kStretch + (from - targets[i])) * fmt.channels, out + (from - frames) * fmt.channels, (usize)(to - from) * fmt.channels * 4);
        }
        frames += r.frames;
    } while (r.status == DecodeStatus::Ok);
    f64 spent = time_now() - t0;
    u8 md5[16];
    m.finish(md5);
    f64 seconds = fmt.rate ? (f64)frames / fmt.rate : 0.0;
    f64 speed = spent > 0.0 ? seconds / spent : 0.0;
    t.audio += seconds; t.spent += spent;
    if (speed < t.slowest) t.slowest = speed;

    bool whole = r.status == DecodeStatus::End && (o.info.frames == 0 || frames == o.info.frames);
    bool exact = whole && has_md5 && memcmp(md5, want, 16) == 0;
    const char* verdict = exact ? "exact" : (whole && !has_md5) ? "no MD5 in the file" : "WRONG";
    KV_INFO("test", "%-6s %u/%u/%u %8.1fs %6.0fx  %s%s%s", verdict, fmt.bits, fmt.channels, fmt.rate, seconds, speed, name,
            r.status == DecodeStatus::Damaged ? ": " : "", r.status == DecodeStatus::Damaged ? r.reason : "");
    if (exact) ++t.exact; else if (whole && !has_md5) ++t.unproven; else ++t.failed;
    c.check(exact || (whole && !has_md5), "the decode is the file's samples", path, 0);

    if (whole) {
        u32 order[kTargets];
        for (u32 i = 0; i < kTargets; ++i) order[i] = i;
        for (u32 i = kTargets - 1; i > 0; --i) { u32 j = rng.next() % (i + 1); u32 x = order[i]; order[i] = order[j]; order[j] = x; }
        u32 wrong = 0;
        for (u32 k = 0; k < ntargets; ++k) {
            u32 i = order[k];
            f64 s0 = time_now();
            SeekResult s = d->seek(o.state, targets[i]);
            f64 took = time_now() - s0;
            u32 n = 0;
            while (s.status == SeekStatus::Ok && n < kStretch) {
                DecodeResult x = d->decode(o.state, out + (usize)n * fmt.channels, kStretch - n);
                n += x.frames;
                if (x.status != DecodeStatus::Ok) break;
            }
            bool same = n == kStretch && memcmp(out, kept + (usize)i * kStretch * fmt.channels, (usize)kStretch * fmt.channels * 4) == 0;
            if (!same) { ++wrong; KV_ERROR("test", "%s: a seek to %llu is not the straight decode", name, (unsigned long long)targets[i]); }
            t.seek_spent += took;
            if (took > t.seek_slowest) t.seek_slowest = took;
            ++t.seeks;
        }
        t.seeks_wrong += wrong;
        c.check(wrong == 0, "a seek lands on the frame", path, 0);
    }
    mem_free(kept);
    d->close(o.state);
    mem_free(data);
}

void walk(TestCtx& c, const char* path, Totals& t, u8* memory, i32* out) {
    struct stat st;
    if (stat(path, &st) != 0) { KV_ERROR("test", "no such path: %s", path); c.check(false, "the path exists", path, 0); return; }
    if (S_ISREG(st.st_mode)) { if (ends_with_flac(path)) one_file(c, path, t, memory, out); return; }
    if (!S_ISDIR(st.st_mode)) return;
    DIR* dir = opendir(path);
    if (!dir) return;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        char child[4096];
        format(child, sizeof child, "%s/%s", path, e->d_name);
        walk(c, child, t, memory, out);
    }
    closedir(dir);
}

} // namespace

int main(int argc, char** argv) {
    TestCtx c;
    c.begin("flac files");
    Totals t;
    u8*  memory = (u8*)mem_alloc(8u << 20);
    i32* out    = (i32*)mem_alloc(4096 * 8 * 4);
    for (int i = 1; i < argc; ++i) walk(c, argv[i], t, memory, out);
    mem_free(memory);
    mem_free(out);
    if (argc < 2) KV_INFO("test", "no files named: pass directories or .flac files after --");
    else KV_INFO("test", "%u files: %u exact, %u without an MD5, %u failed; %.0f s of audio in %.1f s (%.0fx real time, slowest %.0fx)",
                 t.files, t.exact, t.unproven, t.failed, t.audio, t.spent, t.spent > 0 ? t.audio / t.spent : 0.0, t.files ? t.slowest : 0.0);
    if (t.seeks) KV_INFO("test", "%u seeks, %u wrong; %.0f us each on average, the slowest %.0f us", t.seeks, t.seeks_wrong,
                         1e6 * t.seek_spent / t.seeks, 1e6 * t.seek_slowest);
    c.check(argc < 2 || t.files > 0, "some .flac files were found", __FILE__, __LINE__);
    int rc = test_report(c, "hifi_codec_heavy");
    mem_shutdown();
    return rc;
}
