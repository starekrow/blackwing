// The MP3 decoder: dr_mp3's push decoder, one MPEG frame at a time, behind the decoder
// contract. dr_mp3 is handed a window of the file's bytes and decodes the frame at its
// front (after any junk it skips), so nothing pulls: before each frame the window must be
// in the span, or the call returns Wait having touched nothing.
//
// The walk. Which bytes make up which frame is decided by dr_mp3's own search from where
// the last frame ended, and that depends only on the bytes, where it starts, and two
// things it remembers (the last header and a free-format frame's size). So a place in the
// walk is those three things and a count of the frames before it; the decoder keeps such
// a place (a mark) every few frames, as it goes, and a seek starts from one. From a mark
// it walks forward without decoding (dr_mp3 still keeps its bit reservoir) to two frames
// before the target, decodes those two to fill the filter bank's history, and lands on
// the target's frame. What comes out is the straight decode's, sample for sample.
//
// A frame counts by its header, whether or not its audio could be decoded: a frame whose
// bit reservoir is missing (the first after junk) is silence, not a gap, so the frames
// keep their numbers. Junk (bytes no frame claims) is stepped over and counted.
#include "korvid/hifi_codec/mp3.h"
#include "korvid/core/format.h"
#include <math.h>
#include <new>
#include <string.h>

#if KV_COMPILER_MSVC
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#define DR_MP3_NO_STDIO
#define DR_MP3_FLOAT_OUTPUT
#define DR_MP3_ONLY_MP3 // Layer I and II frames are junk here: an .mp3 is Layer III
// Its search takes a sync as a frame when the next ten frames in the window agree with
// it. Junk fewer than ten frames past a good frame then costs every frame before it;
// four agreeing frames are as sure on any file that is not built to fool it.
#define DRMP3_MAX_FRAME_SYNC_MATCHES 4
#define DRMP3_ASSERT(expression) ((void)0)
#define DRMP3_MALLOC(sz)     ((void)(sz), (void*)0)
#define DRMP3_REALLOC(p, sz) ((void)(p), (void)(sz), (void*)0)
#define DRMP3_FREE(p)        ((void)(p))
#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3.h"
#if KV_COMPILER_MSVC
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace kv {

namespace {

constexpr u32 kWindow   = 32768;     // bytes handed to dr_mp3 per frame: its search looks ten frames of the largest kind ahead
constexpr u32 kSpfMax   = 1152;      // frames of PCM in an MPEG frame, at most
constexpr u64 kSeekAsk  = 1u << 20;  // the most a seek asks for in one wait
constexpr u64 kSearch   = 1u << 20;  // how far past the front tags open() looks for the first frame
constexpr u32 kStride   = 8;         // frames between marks, to begin with
constexpr u32 kMinMarks = 16;        // fewer than this and the decoder was given too little memory
constexpr u32 kGapless  = 529;       // the decoder's own delay, which LAME's figures leave out
constexpr u32 kAlign    = 16;

// A place in the walk: where the search for frame `frame` starts, and what dr_mp3
// remembers there.
struct Mark {
    u64 pos   = 0;
    u64 frame = 0;
    u8  header[4] = {};
    i32 free_bytes = 0;
};

struct Mp3State {
    drmp3dec dec;               // the decoder at (pos, count)
    drmp3dec scan;              // a seek's, until it lands
    SparseSpan file;
    u64 first = 0, end = 0;     // the audio: past the front tags and any Info frame, to the back tags
    u64 pos = 0;                // where the search for the next frame starts
    u64 count = 0;              // MPEG frames walked to there
    u32 spf = 0;                // PCM frames per MPEG frame
    u32 rate = 0;
    u16 channels = 0;
    u32 lead = 0;               // frames a seek walks from a mark before decoding: the bit reservoir's depth
    f64 frame_bytes = 0;        // an MPEG frame's average size, for what a seek asks for
    u8  toc[100] = {};          // the Xing table, when there is one
    bool has_toc = false;
    u64 toc_bytes = 0;

    float raw[kSpfMax * 2];     // one frame, as dr_mp3 gives it
    i32   pcm[kSpfMax * 2];     // the frame being returned, at 24 bits
    u32   pcm_n = 0, pcm_at = 0;
    u64   produced = 0;         // the frame of the file decode() returns next
    u64   junk = 0;             // bytes stepped over since the last frame
    u64   skipped = 0;          // junk with frames after it: the file was damaged in the middle

    DecoderInfo  info;
    DecodeStatus status = DecodeStatus::Ok;
    char reason[96] = "";
    char seek_reason[96] = "";

    Mark* marks = nullptr;
    u32   nmarks = 0, cap = 0, stride = kStride;
};

u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
u32 le32(const u8* p) { return ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0]; }

// The length of the ID3v2 tag at `h` (which holds 10 bytes), footer and all: 0 when there
// is none, ~0 when it says "ID3" and is not a tag.
u64 id3_length(const u8* h) {
    if (memcmp(h, "ID3", 3) != 0) return 0;
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return ~0ull;
    u32 size = ((u32)h[6] << 21) | ((u32)h[7] << 14) | ((u32)h[8] << 7) | (u32)h[9];
    return 10 + size + ((h[5] & 0x10) ? 10u : 0u);
}

// Whether `h` is a Layer III frame header: sync, a version, a bitrate and a rate.
bool layer3_header(const u8* h) {
    return h[0] == 0xFF && (h[1] & 0xE0) == 0xE0 && ((h[1] >> 3) & 3) != 1 && ((h[1] >> 1) & 3) == 1 &&
           (h[2] >> 4) != 15 && ((h[2] >> 2) & 3) != 3;
}

bool mp3_probe(Span<const u8> head) {
    u64 off = 0;
    for (u32 tags = 0; tags < 4 && head.len - off >= 10; ++tags) {
        u64 t = id3_length(head.data + off);
        if (t == ~0ull) return false;
        if (t == 0) break;
        if (t > head.len - off) return false;
        off += t;
    }
    for (u32 zeros = 0; off < head.len && head.data[off] == 0 && zeros < 64; ++zeros) ++off; // padding past a tag's size
    return head.len - off >= 4 && layer3_header(head.data + off);
}

enum class Step : u8 { Frame, Junk, End, Wait };

// One call of dr_mp3 from *pos: a frame found (into `raw` when it is not null, its count
// of samples in *got: 0 when its audio could not be decoded), junk stepped over, the end,
// or a wait for the window, with `ask` bytes asked for when that is more. Nothing changes
// on a wait.
Step step(Mp3State& s, drmp3dec* d, u64* pos, float* raw, u32* got, u32* chans, u64* adv, ByteRange* need, u64 ask = 0) {
    if (*pos >= s.end) return Step::End;
    u64 win = s.end - *pos < kWindow ? s.end - *pos : kWindow;
    ByteRange miss = sparse_missing(s.file, *pos, win);
    if (miss.len) {
        u64 want = ask > win ? ask : win;
        if (want > s.end - *pos) want = s.end - *pos;
        *need = sparse_missing(s.file, *pos, want);
        return Step::Wait;
    }
    drmp3dec_frame_info fi;
    memset(&fi, 0, sizeof fi);
    int n = drmp3dec_decode_frame(d, s.file.bytes + *pos, (int)win, raw, &fi);
    *adv = fi.frame_bytes > 0 ? (u64)fi.frame_bytes : win;
    *pos += *adv;
    if (fi.layer != 3) return Step::Junk;
    if (s.rate && ((u32)fi.sample_rate != s.rate || drmp3_hdr_frame_samples(d->header) != s.spf)) return Step::Junk; // another stream's frame
    *got = n > 0 ? (u32)n : 0;
    *chans = fi.channels == 1 ? 1u : 2u;
    return Step::Frame;
}

// The marks: one every `stride` frames of the walk, from frame 0. When they are all used,
// every other one goes and the stride doubles.
void note_mark(Mp3State& s, const drmp3dec& d, u64 pos, u64 count) {
    Mark& last = s.marks[s.nmarks - 1];
    if (count != last.frame + s.stride) return;
    if (s.nmarks == s.cap) {
        u32 k = 0;
        for (u32 i = 0; i < s.nmarks; i += 2) s.marks[k++] = s.marks[i];
        s.nmarks = k;
        s.stride *= 2;
        if (count != s.marks[s.nmarks - 1].frame + s.stride) return;
    }
    Mark& m = s.marks[s.nmarks++];
    m.pos = pos;
    m.frame = count;
    memcpy(m.header, d.header, 4);
    m.free_bytes = d.free_format_bytes;
}

// A frame into `s.pcm` at the stream's channels, 24 bits: `got` samples of `chans`
// channels from `raw`, or silence.
void take_frame(Mp3State& s, const float* raw, u32 got, u32 chans) {
    const u32 ch = s.channels;
    if (got == 0) {
        memset(s.pcm, 0, (usize)s.spf * ch * sizeof(i32));
    } else {
        for (u32 i = 0; i < got; ++i) {
            for (u32 c = 0; c < ch; ++c) {
                f32 v;
                if (chans == ch)      v = raw[i * chans + c];
                else if (chans == 1)  v = raw[i];                                   // a mono frame in a stereo stream
                else                  v = 0.5f * (raw[i * 2] + raw[i * 2 + 1]);     // and the other way round
                v *= 8388608.0f;
                i32 q;
                if (v != v)                   q = 0;
                else if (v >= 8388607.0f)     q = 8388607;
                else if (v <= -8388608.0f)    q = -8388608;
                else                          q = (i32)lrintf(v);
                s.pcm[(usize)i * ch + c] = q;
            }
        }
        if (got < s.spf) memset(s.pcm + (usize)got * ch, 0, (usize)(s.spf - got) * ch * sizeof(i32));
    }
    s.pcm_n = s.spf;
    s.pcm_at = 0;
}

void finish(Mp3State& s) {
    if (!s.info.estimated && s.info.frames && s.produced < s.info.frames) {
        s.status = DecodeStatus::Damaged;
        format(s.reason, sizeof s.reason, "the file is cut short or damaged: %llu of %llu frames could be read",
               (unsigned long long)s.produced, (unsigned long long)s.info.frames);
    } else if (s.skipped) {
        s.status = DecodeStatus::Damaged;
        format(s.reason, sizeof s.reason, "%llu bytes in the middle of the file are not MPEG audio, and were skipped",
               (unsigned long long)s.skipped);
    } else {
        s.status = DecodeStatus::End;
    }
}

// The Xing, Info or VBRI header in the frame at `f` (all of it there, `len` bytes): true
// when the frame is one, with what it says put in `s`.
bool read_header_frame(Mp3State& s, const u8* f, u64 len) {
    const bool mpeg1 = DRMP3_HDR_TEST_MPEG1(f) != 0, mono = DRMP3_HDR_IS_MONO(f);
    // After the side information; LAME leaves out the CRC's two bytes when it has one.
    u64 at = 4 + (mpeg1 ? (mono ? 17u : 32u) : (mono ? 9u : 17u));
    auto xing = [&](u64 o) { return o + 8 <= len && (memcmp(f + o, "Xing", 4) == 0 || memcmp(f + o, "Info", 4) == 0); };
    if (!xing(at) && DRMP3_HDR_IS_CRC(f) && xing(at + 2)) at += 2;
    if (xing(at)) {
        u32 flags = be32(f + at + 4);
        u64 p = at + 8;
        u32 nframes = 0;
        if (flags & 1) { if (p + 4 > len) return true; nframes = be32(f + p); p += 4; }
        if (flags & 2) { if (p + 4 > len) return true; s.toc_bytes = be32(f + p); p += 4; }
        if (flags & 4) { if (p + 100 > len) return true; memcpy(s.toc, f + p, 100); s.has_toc = true; p += 100; }
        if (flags & 8) { if (p + 4 > len) return true; p += 4; }
        if (nframes) s.info.frames = (u64)nframes * s.spf;
        // The LAME tag: an encoder's name, and 12 bits each of delay and padding 21 bytes in.
        bool named = p + 36 <= len;
        for (u32 i = 0; named && i < 4; ++i) named = f[p + i] >= 0x20 && f[p + i] < 0x7F;
        if (named && s.info.frames) {
            u32 delay = ((u32)f[p + 21] << 4) | (f[p + 22] >> 4);
            u32 padding = ((u32)(f[p + 22] & 0xF) << 8) | f[p + 23];
            u64 d = (u64)delay + kGapless, q = padding > kGapless ? padding - kGapless : 0;
            if (d + q < s.info.frames) { s.info.delay = (u32)d; s.info.padding = (u32)q; }
        }
        return true;
    }
    if (36 + 26 <= len && memcmp(f + 36, "VBRI", 4) == 0) {
        u32 nframes = be32(f + 36 + 14);
        if (nframes) s.info.frames = (u64)nframes * s.spf;
        return true;
    }
    return false;
}

DecoderOpen mp3_open(const SparseSpan& file, Span<u8> memory) {
    DecoderOpen out;
    if (file.size == 0) { out.reason = "the file is empty"; return out; }

    // The front: any ID3v2 tags, stepped over by their sizes, never read.
    u64 start = 0;
    for (u32 tags = 0; tags < 4 && file.size - start >= 10; ++tags) {
        ByteRange miss = sparse_missing(file, start, 10);
        if (miss.len) { out.need = miss; return out; }
        u64 t = id3_length(file.bytes + start);
        if (t == 0 || t == ~0ull) break;
        if (t > file.size - start) { out.reason = "not an MP3 file: the tag at its front says it is longer than the file"; return out; }
        start += t;
    }

    // The back: an ID3v1 tag, and an APEv2 tag before it, which are not frames.
    u64 tail = file.size - start < 160 ? file.size - start : 160;
    if (tail) {
        ByteRange miss = sparse_missing(file, file.size - tail, tail);
        if (miss.len) { out.need = miss; return out; }
    }
    u64 end = file.size;
    if (end - start >= 128 && memcmp(file.bytes + end - 128, "TAG", 3) == 0) end -= 128;
    if (end - start >= 32 && memcmp(file.bytes + end - 32, "APETAGEX", 8) == 0) {
        const u8* foot = file.bytes + end - 32;
        u64 n = (u64)le32(foot + 12) + ((le32(foot + 20) & 0x80000000u) ? 32u : 0u);
        if (n >= 32 && n <= end - start) end -= n;
    }

    usize skew = (usize)(-(intptr_t)memory.data) & (kAlign - 1);
    usize head = (sizeof(Mp3State) + kAlign - 1) & ~(usize)(kAlign - 1);
    if (!memory.data || memory.len < skew + head + kMinMarks * sizeof(Mark)) { out.reason = "the decoder was given too little memory"; return out; }
    Mp3State* s = new (memory.data + skew) Mp3State();
    s->file = file;
    s->end  = end;
    s->marks = (Mark*)(memory.data + skew + head);
    s->cap = (u32)((memory.len - skew - head) / sizeof(Mark)) & ~1u;

    // The first frame, by dr_mp3's own search.
    memset(&s->scan, 0, sizeof s->scan);
    u64 pos = start, fstart = 0, flen = 0;
    u32 got = 0, chans = 0;
    for (;;) {
        u64 adv = 0;
        ByteRange need;
        Step st = step(*s, &s->scan, &pos, nullptr, &got, &chans, &adv, &need);
        if (st == Step::Wait) { out.need = need; return out; }
        if (st == Step::End || pos - start > kSearch) { out.reason = "not an MP3 file: no MPEG audio frame was found in it"; return out; }
        if (st == Step::Frame) {
            const u8* h = s->scan.header;
            flen = (u64)drmp3_hdr_frame_bytes(h, s->scan.free_format_bytes) + (u64)drmp3_hdr_padding(h);
            fstart = pos - flen;
            break;
        }
    }
    const u8* h = file.bytes + fstart;
    s->rate     = drmp3_hdr_sample_rate_hz(h);
    s->spf      = drmp3_hdr_frame_samples(h);
    s->channels = DRMP3_HDR_IS_MONO(h) ? 1 : 2;
    s->info.format = AudioFormat{s->rate, s->channels, 24};
    u32 kbps = drmp3_hdr_bitrate_kbps(h);
    s->frame_bytes = kbps ? (f64)s->spf * kbps * 125.0 / s->rate : (f64)flen;

    // An Info frame is not audio: the walk begins after it.
    s->first = read_header_frame(*s, h, flen) ? fstart + flen : start;
    if (s->first > s->end) s->first = s->end;
    if (!s->info.frames && s->frame_bytes > 0) {
        s->info.frames = (u64)((f64)(s->end - s->first) / s->frame_bytes) * s->spf;
        s->info.estimated = true;
    } else if (s->info.frames) {
        s->frame_bytes = (f64)(s->end - s->first) / (f64)(s->info.frames / s->spf);
    }
    if (s->frame_bytes < 1) s->frame_bytes = 1;

    // How many frames back a seek begins, for the reservoir (511 bytes) to be full: the
    // smallest frame this stream can have, less its header and side information.
    u32 min_kbps = s->spf == 1152 ? 32u : 8u;
    u64 smallest = kbps ? (u64)s->spf * min_kbps * 125 / s->rate : flen;
    u64 side = 4 + 2 + (s->spf == 1152 ? 32 : 17);
    u64 main = smallest > side + 8 ? smallest - side : 8;
    s->lead = (u32)(511 / main + 2);
    if (s->lead > 128) s->lead = 128;

    memset(&s->dec, 0, sizeof s->dec);
    s->pos = s->first;
    s->marks[0] = Mark{s->first, 0, {0, 0, 0, 0}, 0};
    s->nmarks = 1;
    out.state = s;
    out.info  = s->info;
    return out;
}

DecodeResult mp3_decode(void* state, i32* out, u32 frames) {
    Mp3State& s = *(Mp3State*)state;
    DecodeResult r;
    r.status = s.status;
    r.reason = s.reason;
    if (s.status != DecodeStatus::Ok || frames == 0) return r;
    r.status = DecodeStatus::Ok;
    const u32 ch = s.channels;
    u32 done = 0;
    bool ended = false;
    while (done < frames) {
        if (s.pcm_at < s.pcm_n) {
            u32 k = s.pcm_n - s.pcm_at;
            if (k > frames - done) k = frames - done;
            memcpy(out + (usize)done * ch, s.pcm + (usize)s.pcm_at * ch, (usize)k * ch * sizeof(i32));
            s.pcm_at += k;
            done += k;
            s.produced += k;
            continue;
        }
        note_mark(s, s.dec, s.pos, s.count);
        u32 got = 0, chans = 0;
        u64 adv = 0;
        ByteRange need;
        Step st = step(s, &s.dec, &s.pos, s.raw, &got, &chans, &adv, &need);
        if (st == Step::Wait) { r.status = DecodeStatus::Wait; r.need = need; break; }
        if (st == Step::End) { ended = true; break; }
        if (st == Step::Junk) { s.junk += adv; continue; }
        if (s.count && s.junk) s.skipped += s.junk;
        s.junk = 0;
        take_frame(s, s.raw, got, chans);
        ++s.count;
    }
    r.frames = done;
    if (ended) {
        finish(s);
        r.status = s.status;
        r.reason = s.reason;
    }
    return r;
}

// Where frame `t` probably is, for asking for enough of the file at once.
u64 guess_pos(const Mp3State& s, u64 t) {
    u64 total = s.info.frames / (s.spf ? s.spf : 1);
    if (s.has_toc && total) {
        f64 pct = (f64)t * 100.0 / (f64)total;
        if (pct > 99.0) pct = 99.0;
        u32 i = (u32)pct;
        f64 a = s.toc[i], b = i < 99 ? s.toc[i + 1] : 256.0;
        f64 bytes = s.toc_bytes ? (f64)s.toc_bytes : (f64)(s.end - s.first);
        return s.first + (u64)((a + (b - a) * (pct - i)) / 256.0 * bytes);
    }
    return s.first + (u64)((f64)t * s.frame_bytes);
}

SeekResult mp3_seek(void* state, u64 frame) {
    Mp3State& s = *(Mp3State*)state;
    SeekResult r;
    r.reason = s.seek_reason;
    if (!s.info.estimated && s.info.frames && frame > s.info.frames) {
        format(s.seek_reason, sizeof s.seek_reason, "frame %llu is past the end: the file has %llu",
               (unsigned long long)frame, (unsigned long long)s.info.frames);
        return r;
    }
    const u64 t = frame / s.spf;
    const u32 within = (u32)(frame % s.spf);
    const u64 from = t >= 2 ? t - 2 : 0; // decoded from here, for the filter bank's history

    // Where to walk from: where the decoder is, if that is not past it, or the last mark
    // that leaves the reservoir time to fill.
    u32 lo = 0, hi = s.nmarks;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (s.marks[mid].frame + s.lead <= from) lo = mid; else hi = mid;
    }
    const Mark& m = s.marks[lo];
    u64 pos, cnt;
    if (s.count <= from && s.count >= m.frame) {
        memcpy(&s.scan, &s.dec, sizeof s.scan);
        pos = s.pos; cnt = s.count;
    } else {
        memset(&s.scan, 0, sizeof s.scan);
        memcpy(s.scan.header, m.header, 4);
        s.scan.free_format_bytes = m.free_bytes;
        pos = m.pos; cnt = m.frame;
    }
    // Walked without decoding to `from`, then decoded into `raw` (scratch between calls:
    // decode() converts each frame out of it at once), the last into `pcm` once it has come.
    const u64 target = guess_pos(s, t);
    for (;;) {
        note_mark(s, s.scan, pos, cnt);
        u64 ask = target + kWindow > pos ? target + kWindow - pos : 0;
        if (ask > kSeekAsk) ask = kSeekAsk;
        u32 got = 0, chans = 0;
        u64 adv = 0;
        ByteRange need;
        Step st = step(s, &s.scan, &pos, cnt >= from ? s.raw : nullptr, &got, &chans, &adv, &need, ask);
        if (st == Step::Wait) { r.status = SeekStatus::Wait; r.need = need; return r; }
        if (st == Step::End) {
            if (cnt == t && within == 0) { // the end itself: the next decode says so
                memcpy(&s.dec, &s.scan, sizeof s.dec);
                s.pos = pos; s.count = cnt;
                s.pcm_n = s.pcm_at = 0;
                s.produced = frame;
                s.junk = 0;
                finish(s);
                r.status = SeekStatus::Ok;
                return r;
            }
            format(s.seek_reason, sizeof s.seek_reason, "frame %llu is past the end: the file has %llu",
                   (unsigned long long)frame, (unsigned long long)(cnt * s.spf));
            return r;
        }
        if (st == Step::Junk) continue;
        if (cnt == t) {
            memcpy(&s.dec, &s.scan, sizeof s.dec);
            s.pos = pos;
            s.count = cnt + 1;
            take_frame(s, s.raw, got, chans);
            s.pcm_at = within;
            s.produced = frame;
            s.junk = 0;
            s.status = DecodeStatus::Ok;
            s.reason[0] = 0;
            r.status = SeekStatus::Ok;
            return r;
        }
        ++cnt;
    }
}

void mp3_close(void*) {}

} // namespace

constinit const DecoderDesc k_mp3_decoder = {
    .name         = "mp3",
    .memory_bytes = 1u << 20, // two of dr_mp3's decoders (46 KB), a frame twice over, and about 40000 marks
    .probe        = mp3_probe,
    .open         = mp3_open,
    .decode       = mp3_decode,
    .seek         = mp3_seek,
    .close        = mp3_close,
};

} // namespace kv
