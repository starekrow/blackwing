// The audio path: the caller's side, the render thread and the device callback.
//
// What is shared, and how:
//
//   caller <-> render   `lock` guards the command list, the event list and the device
//                       pointer. Neither side holds it while it decodes, sleeps or waits
//                       for the other; the render thread waits on `wake` when idle.
//   render  -> callback the ring's bytes and `write` (released after the bytes are in),
//                       `end_frame` (the stream ends here for now: running dry there is
//                       not an underrun), `skip_to` (drop everything before this frame).
//   callback -> render  `read`, `silent_run`, `underruns`.
//   caller  -> callback `want_pause`.
//   caller <-> render   `held`, under `lock`: the device is held. The render thread holds
//                       it when there is nothing to play and the device has played out
//                       what it was given; play, or something to play, lets it go.
//   render  -> caller   `readers`: odd while the render thread is in a step, where every
//                       decoder call is made. A caller that clears a piece's byte in a
//                       source's map waits for it to move on before the piece goes.
//
// Stream frames are counted from zero and never reset: `write` and `read` are positions
// in one endless stream, the ring holds the frames between them, and a mark (a source
// starts here, ends here) is a stream frame. A change of device format keeps counting.
#include "korvid/hifi/hifi.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/time.h"
#include <math.h>
#include <new>
#include <string.h>

namespace kv {

namespace {

constexpr u64 kNever       = ~(u64)0;
constexpr u32 kChunkFrames = 4096; // what the render thread decodes at a time
constexpr u32 kMaxPending  = 256;  // commands, and events, in flight
constexpr u32 kMaxStarved  = 16;   // sources told their bytes will not come, remembered
constexpr f64 kWantNap     = 0.002; // how soon a decoder waiting for bytes is tried again

enum class CmdKind : u8 { Add, Flush, Starve, Quit };
struct Cmd { CmdKind kind = CmdKind::Add; HifiSource source; u32 epoch = 0; char reason[96] = ""; };

// What the render thread tells the caller. A mark carries the stream frame it becomes
// true at; the others are true at once.
struct Note {
    HifiEvent event;
    u64       frame = 0;
    u32       epoch = 0;
};

template <class T, u32 N> struct Fifo {
    T   items[N];
    u32 head = 0, count = 0;
    bool push(const T& t) { if (count == N) return false; items[(head + count++) % N] = t; return true; }
    bool pop(T* t) { if (!count) return false; *t = items[head]; head = (head + 1) % N; --count; return true; }
    T&   at(u32 i) { return items[(head + i) % N]; }
    void remove(u32 i) { for (u32 k = i; k + 1 < count; ++k) at(k) = at(k + 1); --count; }
};

// A source with its decoder open, or opening.
struct Playing {
    HifiSource  src;
    void*       state   = nullptr;
    u8*         memory  = nullptr;
    DecoderInfo info;
    bool        opening = false; // its decoder's open is waiting for bytes
    ByteRange   wanted;          // the last Want said for it, while it waits
};

struct Starved { u64 id = 0; char reason[96] = ""; };

} // namespace

struct Hifi {
    HifiConfig config;

    // ---- caller <-> render, under `lock` ----
    OsMutex lock;
    OsCond  wake;
    Fifo<Cmd, kMaxPending>  cmds;
    Fifo<Note, kMaxPending> notes;
    HifiDevice*      device = nullptr; // opened and closed by the render thread, under the lock
    HifiDeviceFormat format;
    bool             shared = false;
    u32              device_bits = 0;  // the widest source so far in this device: for `exact`

    // ---- the caller's alone ----
    Fifo<Note, kMaxPending> pending;   // marks not yet reached
    u32 epoch         = 0;             // bumped by a flush; older marks are dropped
    u64 heard         = 0;
    u32 seen_underruns = 0;
    u64 cur_source    = 0;
    u64 cur_frame     = 0;             // the stream frame the source being heard started at
    OsThread thread;

    // ---- render -> callback ----
    u8* ring         = nullptr;        // ring_frames * frame_bytes; replaced only while no device is open
    u32 ring_frames  = 0;
    u32 frame_bytes  = 0;
    u32 ramp_frames  = 1;
    u32 start_frames = 1;
    u64 write        = 0;              // atomic
    u64 end_frame    = kNever;         // atomic
    u64 skip_to      = 0;              // atomic

    // ---- callback -> anyone ----
    u64 read         = 0;              // atomic
    u64 silent_run   = 0;              // atomic: frames of silence the callback has written since its last real frame
    u32 underruns    = 0;              // atomic

    // ---- caller -> callback, render ----
    u32 want_pause   = 0;              // atomic
    u32 flushes_done = 0;              // atomic: the last flush the render thread has carried out
    u64 readers      = 0;              // atomic: odd while the render thread is in a step
    bool held        = false;          // under `lock`: the device is held (hifi_device_hold)

    // ---- the callback's alone ----
    bool running = false;              // taking from the lead
    bool fresh   = true;               // the next start is a clean one: full gain from its first frame
    f32  gain    = 1.0f;

    // ---- the render thread's alone ----
    Fifo<HifiSource, kMaxPending> queue;
    bool         have_cur = false, have_nxt = false;
    Playing      cur;                  // being decoded into the lead
    Playing      nxt;                  // opened early, so the stream knows whether it goes on past cur's last frame
    u64          cur_frames = 0;       // decoded so far
    bool         cur_done   = false;   // the decoder has given its last
    DecodeStatus cur_status = DecodeStatus::End;
    char         cur_reason[96] = "";
    bool         cur_marked = false;   // its Started mark is out
    bool         draining   = false;   // waiting for the lead to play out before the device changes
    u32          playout_waits = 0;    // looks, since there was work, for the device to have played out
    u32          render_epoch = 0;
    Starved      starved[kMaxStarved]; // sources whose bytes will not come
    u32          nstarved = 0;
    i32*         scratch = nullptr;    // kChunkFrames * 8 decoded samples
    u32          scratch_frames = 0, scratch_at = 0;
};

namespace {

// ---- the callback ---------------------------------------------------------------------

// In double: an f32 cannot hold a 32-bit sample, and the product must not round twice.
template <class T> inline T scaled(T v, f32 g) { return (T)llrint((f64)v * (f64)g); }
template <> inline f32 scaled<f32>(f32 v, f32 g) { return v * g; }

// One frame from the ring to `out`, times `g`. At full gain it is a copy: the ramp's
// first and last frames are exact too.
inline void copy_scaled(const Hifi& h, const u8* src, u8* out, f32 g) {
    u32 channels = h.format.channels;
    if (g == 1.0f) { memcpy(out, src, h.frame_bytes); return; }
    if (h.format.sample == HifiSampleFormat::S16) {
        const i16* s = (const i16*)src; i16* o = (i16*)out;
        for (u32 c = 0; c < channels; ++c) o[c] = scaled<i16>(s[c], g);
    } else if (h.format.sample == HifiSampleFormat::F32) {
        const f32* s = (const f32*)src; f32* o = (f32*)out;
        for (u32 c = 0; c < channels; ++c) o[c] = scaled<f32>(s[c], g);
    } else {
        const i32* s = (const i32*)src; i32* o = (i32*)out;
        for (u32 c = 0; c < channels; ++c) o[c] = scaled<i32>(s[c], g);
    }
}

// The device's callback, on its realtime thread. Takes from the lead; writes silence when
// there is nothing to take or it is holding. Its only arithmetic is the ramp.
void hifi_callback(void* user, void* out_v, u32 frames) {
    Hifi& h = *(Hifi*)user;
    u8*   out = (u8*)out_v;
    const u32 fb = h.frame_bytes;
    const u64 w    = atomic_load(&h.write, MO_ACQUIRE);
    const u64 endf = atomic_load(&h.end_frame, MO_ACQUIRE);
    const u64 skip = atomic_load(&h.skip_to, MO_ACQUIRE);
    const bool pause = atomic_load(&h.want_pause, MO_RELAXED) != 0;
    u64 r = h.read; // only this thread writes it
    u32 produced = 0;

    // Up to three passes: playing; then, if that ramped down for a flush, starting what
    // was put in the lead behind it, in this same callback.
    for (u32 pass = 0; pass < 3 && produced < frames; ++pass) {
        if (!h.running) {
            if (skip > r) { r = skip < w ? skip : w; h.fresh = true; } // nothing sounding: step over what was flushed
            u64 ready = w - r;
            if (pause || ready == 0 || (ready < h.start_frames && w != endf)) break;
            h.running = true;
            h.gain    = h.fresh ? 1.0f : 0.0f;
            h.fresh   = false;
        }
        const u64  avail  = w - r;
        const bool down   = pause || skip > r;     // told to stop: ramp to silence
        const bool more   = (w != endf);           // the stream goes on past what is here
        const u32  want   = frames - produced;
        const u32  take   = want < avail ? want : (u32)avail;
        const u32  ramp   = h.ramp_frames;
        const f32  step   = 1.0f / (f32)ramp;
        const bool nearly_dry = more && (avail - take) < ramp;
        u8* dst = out + (usize)produced * fb;
        u32 took = 0;
        if (h.gain == 1.0f && !down && !nearly_dry) {
            // The whole of normal playback: the ring's bytes, untouched.
            u32 at    = (u32)(r % h.ring_frames);
            u32 first = h.ring_frames - at < take ? h.ring_frames - at : take;
            memcpy(dst, h.ring + (usize)at * fb, (usize)first * fb);
            memcpy(dst + (usize)first * fb, h.ring, (usize)(take - first) * fb);
            took = take;
        } else {
            for (; took < take; ++took) {
                if (down) {
                    h.gain -= step;
                    if (h.gain <= 0.0f) { h.gain = 0.0f; break; }
                } else {
                    // Running dry with more expected: down to silence by the last frame there is.
                    f32 limit = 1.0f;
                    if (more) { u64 left = avail - took - 1; if (left < ramp) limit = (f32)left * step; }
                    f32 g = h.gain + step;
                    h.gain = g < limit ? g : limit;
                }
                u32 at = (u32)((r + took) % h.ring_frames);
                copy_scaled(h, h.ring + (usize)at * fb, dst + (usize)took * fb, h.gain);
            }
        }
        r += took;
        produced += took;
        if (down && h.gain <= 0.0f) {
            h.running = false;                      // and round again: a flush may have new audio behind it
            if (skip > r) { r = skip < w ? skip : w; h.fresh = true; }
            continue;
        }
        if (r == w) {
            h.running = false;
            if (more) atomic_fetch_add(&h.underruns, 1u, MO_RELAXED); // ran dry with the stream unfinished
            else      h.fresh = true;                                   // the stream's end: the next start is clean
        }
        break;
    }
    if (produced < frames) memset(out + (usize)produced * fb, 0, (usize)(frames - produced) * fb);
    u64 quiet = frames - produced;
    if (produced == 0) quiet += atomic_load(&h.silent_run, MO_RELAXED);
    atomic_store(&h.silent_run, quiet, MO_RELEASE);
    atomic_store(&h.read, r, MO_RELEASE);
}

// ---- the render thread ----------------------------------------------------------------

void note(Hifi& h, HifiEventKind kind, u64 source, u64 frame, const char* reason = "") {
    Note n;
    n.event.kind   = kind;
    n.event.source = source;
    n.frame = frame;
    n.epoch = h.render_epoch;
    format(n.event.reason, sizeof n.event.reason, "%s", reason);
    if (kind == HifiEventKind::Started) n.event.info = h.cur.info;
    if (kind == HifiEventKind::Ended)   { n.event.info = h.cur.info; n.event.status = h.cur_status; n.event.frames = h.cur_frames; }
    os_mutex_lock(&h.lock);
    if (!h.notes.push(n)) KV_ERROR("hifi", "the event list is full: an event for source %llu is lost", (unsigned long long)source);
    os_mutex_unlock(&h.lock);
}

// Forgets that a source was starved, once it is gone.
void forget_starved(Hifi& h, u64 id) {
    for (u32 i = 0; i < h.nstarved; ++i)
        if (h.starved[i].id == id) { h.starved[i] = h.starved[--h.nstarved]; return; }
}
const char* starved_reason(Hifi& h, u64 id) {
    for (u32 i = 0; i < h.nstarved; ++i) if (h.starved[i].id == id) return h.starved[i].reason;
    return nullptr;
}

// Says that a source waits for `need`, once for each range it waits for.
void want(Hifi& h, Playing& p, ByteRange need) {
    if (p.wanted.at == need.at && p.wanted.len == need.len) return;
    p.wanted = need;
    Note n;
    n.event.kind   = HifiEventKind::Want;
    n.event.source = p.src.id;
    n.event.need   = need;
    n.epoch = h.render_epoch;
    os_mutex_lock(&h.lock);
    if (!h.notes.push(n)) KV_ERROR("hifi", "the event list is full: a want for source %llu is lost", (unsigned long long)p.src.id);
    os_mutex_unlock(&h.lock);
}

// Lets go of a source: its decoder closed, its memory freed, Released sent.
void release(Hifi& h, Playing& p) {
    if (p.state) p.src.decoder->close(p.state);
    mem_free(p.memory);
    note(h, HifiEventKind::Released, p.src.id, 0);
    forget_starved(h, p.src.id);
    p = Playing{};
}
void release_cur(Hifi& h) {
    if (!h.have_cur) return;
    release(h, h.cur);
    h.have_cur = false;
    h.scratch_frames = h.scratch_at = 0;
}
void release_nxt(Hifi& h) {
    if (!h.have_nxt) return;
    release(h, h.nxt);
    h.have_nxt = false;
}

// Opens the next source that will open, as h.nxt; the ones that will not, fail here. One
// whose open waits for bytes stays h.nxt, opening, and is tried again on the next call.
void prepare_next(Hifi& h) {
    for (;;) {
        if (!h.have_nxt) {
            HifiSource s;
            if (!h.queue.pop(&s)) return;
            h.nxt = Playing{};
            h.nxt.src = s;
            h.nxt.opening = true;
            h.have_nxt = true;
            if (s.decoder) h.nxt.memory = (u8*)mem_alloc(s.decoder->memory_bytes);
        }
        Playing& p = h.nxt;
        if (!p.opening) return;
        const char* why = "";
        if (!p.src.decoder) why = "no decoder takes this kind of file";
        else {
            DecoderOpen o = p.src.decoder->open(p.src.file, Span<u8>(p.memory, p.src.decoder->memory_bytes));
            if (o.need.len) {
                const char* dead = starved_reason(h, p.src.id);
                if (!dead) { want(h, p, o.need); return; } // still opening
                why = dead;
            } else if (o.state) {
                p.state = o.state; p.info = o.info;
            } else {
                why = o.reason;
            }
            const AudioFormat& f = o.info.format;
            if (o.state && (f.channels == 0 || f.channels > 8 || f.rate == 0 || f.bits == 0 || f.bits > 32))
                why = "the file describes a format that cannot be played";
        }
        if (why[0]) { note(h, HifiEventKind::Failed, p.src.id, 0, why); release(h, p); h.have_nxt = false; continue; }
        p.opening = false;
        p.wanted = ByteRange{};
        return;
    }
}

void close_device(Hifi& h) {
    os_mutex_lock(&h.lock);
    HifiDevice* d = h.device;
    h.device = nullptr;
    h.held   = false;
    os_mutex_unlock(&h.lock);
    if (d) hifi_device_close(d); // no callback runs after this
    h.running = false;           // the callback's, and it is gone
    h.fresh   = true;
    h.gain    = 1.0f;
}

HifiSampleFormat narrowest_for(u32 bits) {
    return bits <= 16 ? HifiSampleFormat::S16 : bits <= 24 ? HifiSampleFormat::S24 : HifiSampleFormat::S32;
}

// Whether the open device can go straight on with a source of this format.
bool device_fits(const Hifi& h, const AudioFormat& f) {
    return h.device && h.format.rate == f.rate && h.format.channels == f.channels && f.bits <= hifi_sample_bits(h.format.sample);
}

// Opens the device for the current source's format, with a ring to match. False, with
// the reason, when it cannot.
bool open_device(Hifi& h, const char** reason) {
    const AudioFormat& f = h.cur.info.format;
    HifiDeviceDesc d;
    d.name     = h.config.device;
    d.backend  = h.config.device_backend;
    d.want     = HifiDeviceFormat{f.rate, f.channels, narrowest_for(f.bits)};
    d.callback = hifi_callback;
    d.user     = &h;
    HifiDeviceOpened o = hifi_device_open(d);
    if (!o.device) { *reason = o.reason; return false; }
    if (o.format.rate != f.rate || o.format.channels != f.channels) {
        hifi_device_close(o.device);
        *reason = "the device will not play at this rate and channel count, and there is no resampler yet";
        return false;
    }
    if (f.bits > hifi_sample_bits(o.format.sample) && o.format.sample != HifiSampleFormat::F32) {
        hifi_device_close(o.device);
        *reason = "the device's samples are narrower than the file's, and there is no dither yet";
        return false;
    }
    // No callback is running: the ring and what describes it can change.
    mem_free(h.ring);
    h.frame_bytes  = hifi_frame_bytes(o.format);
    u64 want = (u64)(h.config.lead_seconds * (f64)o.format.rate);
    h.ring_frames  = (u32)(want < 2 * kChunkFrames ? 2 * kChunkFrames : want);
    h.ring         = (u8*)mem_alloc((usize)h.ring_frames * h.frame_bytes);
    u32 ramp  = (u32)(h.config.ramp_seconds * (f64)o.format.rate);
    u32 start = (u32)(h.config.start_seconds * (f64)o.format.rate);
    h.ramp_frames  = ramp ? ramp : 1;
    h.start_frames = start ? (start < h.ring_frames ? start : h.ring_frames) : 1;
    // The stream goes on from where it was; what was never taken is gone with the old ring.
    u64 w = atomic_load(&h.write, MO_RELAXED);
    atomic_store(&h.read, w, MO_RELEASE);
    atomic_store(&h.silent_run, (u64)0, MO_RELEASE);
    os_mutex_lock(&h.lock);
    h.device = o.device;
    h.held   = false;
    h.format = o.format;
    h.shared = o.shared;
    h.device_bits = f.bits;
    os_mutex_unlock(&h.lock);
    hifi_device_start(o.device);
    return true;
}

// Scratch samples (i32 at the source's depth) into the ring at stream frame `at`.
void convert_in(Hifi& h, const i32* src, u64 at, u32 frames) {
    const u32 channels = h.format.channels, bits = h.cur.info.format.bits;
    for (u32 i = 0; i < frames; ++i) {
        u8* dst = h.ring + (usize)((at + i) % h.ring_frames) * h.frame_bytes;
        const i32* s = src + (usize)i * channels;
        switch (h.format.sample) {
        case HifiSampleFormat::S16: { i16* o = (i16*)dst; for (u32 c = 0; c < channels; ++c) o[c] = (i16)((u32)s[c] << (16 - bits)); } break;
        case HifiSampleFormat::S24: { i32* o = (i32*)dst; for (u32 c = 0; c < channels; ++c) o[c] = (i32)((u32)s[c] << (24 - bits)); } break;
        case HifiSampleFormat::S32: { i32* o = (i32*)dst; for (u32 c = 0; c < channels; ++c) o[c] = (i32)((u32)s[c] << (32 - bits)); } break;
        case HifiSampleFormat::F32: { f32* o = (f32*)dst; f32 k = 1.0f / (f32)((u64)1 << (bits - 1)); for (u32 c = 0; c < channels; ++c) o[c] = (f32)s[c] * k; } break;
        case HifiSampleFormat::Count: break;
        }
    }
}

// The current source cannot be played. If nothing follows it, the stream ends where it
// is: the callback was told more was coming, and now it is not.
void fail_cur(Hifi& h, const char* why) {
    note(h, HifiEventKind::Failed, h.cur.src.id, 0, why);
    release_cur(h);
    if (h.queue.count == 0 && !h.have_nxt) atomic_store(&h.end_frame, atomic_load(&h.write, MO_RELAXED), MO_RELEASE);
}

// Takes the commands waiting. Returns false on Quit.
bool take_commands(Hifi& h) {
    Cmd c;
    for (;;) {
        os_mutex_lock(&h.lock);
        bool any = h.cmds.pop(&c);
        os_mutex_unlock(&h.lock);
        if (!any) return true;
        if (c.kind == CmdKind::Quit) return false;
        if (c.kind == CmdKind::Add) {
            if (!h.queue.push(c.source)) {
                Note n; n.event.kind = HifiEventKind::Failed; n.event.source = c.source.id; n.epoch = h.render_epoch;
                format(n.event.reason, sizeof n.event.reason, "too many sources are queued");
                Note rel; rel.event.kind = HifiEventKind::Released; rel.event.source = c.source.id; rel.epoch = h.render_epoch;
                os_mutex_lock(&h.lock); h.notes.push(n); h.notes.push(rel); os_mutex_unlock(&h.lock);
            }
        } else if (c.kind == CmdKind::Starve) {
            if (!starved_reason(h, c.source.id) && h.nstarved < kMaxStarved) {
                h.starved[h.nstarved].id = c.source.id;
                format(h.starved[h.nstarved].reason, sizeof h.starved[0].reason, "%s", c.reason);
                ++h.nstarved;
            }
        } else { // Flush: everything queued and everything in the lead goes
            release_cur(h);
            release_nxt(h);
            HifiSource s;
            while (h.queue.pop(&s)) { note(h, HifiEventKind::Released, s.id, 0); forget_starved(h, s.id); }
            h.draining = false;
            h.render_epoch = c.epoch;
            u64 w = atomic_load(&h.write, MO_RELAXED);
            atomic_store(&h.end_frame, w, MO_RELEASE);   // nothing more is expected...
            atomic_store(&h.skip_to, w, MO_RELEASE);     // ...and what is there is to be stepped over
            atomic_store(&h.flushes_done, c.epoch, MO_RELEASE);
        }
    }
}

bool has_commands(Hifi& h) {
    os_mutex_lock(&h.lock);
    bool any = h.cmds.count != 0;
    os_mutex_unlock(&h.lock);
    return any;
}

// Lets a held device go on, unless it is held for a pause.
void release_hold(Hifi& h) {
    os_mutex_lock(&h.lock);
    if (h.held && !atomic_load(&h.want_pause, MO_RELAXED)) {
        hifi_device_hold(h.device, false);
        h.held = false;
    }
    os_mutex_unlock(&h.lock);
}

// With nothing to do: holds the device once it has played out what it was given (all of
// it, or up to the pause), followed by its delay's worth of silence. True when it is held,
// or there is no device: the render thread may then wait for a command.
bool try_hold(Hifi& h) {
    os_mutex_lock(&h.lock);
    bool done = !h.device || h.held;
    if (!done) {
        u32 delay = hifi_device_delay(h.device);
        bool paused = atomic_load(&h.want_pause, MO_RELAXED) != 0;
        bool empty  = atomic_load(&h.read, MO_ACQUIRE) == atomic_load(&h.write, MO_RELAXED);
        if ((paused || empty) && atomic_load(&h.silent_run, MO_ACQUIRE) >= (delay ? delay : 1)) {
            hifi_device_hold(h.device, true);
            h.held = true;
            done = true;
        }
    }
    os_mutex_unlock(&h.lock);
    return done;
}

// One step of work. Returns how long to sleep before the next: 0 to go straight on, a
// negative number to wait for a command.
f64 render_step(Hifi& h) {
    if (!h.have_cur) {
        prepare_next(h);
        if (!h.have_nxt || h.nxt.opening) {
            // Nothing to play. If sources failed just now, the stream ends where it is.
            atomic_store(&h.end_frame, atomic_load(&h.write, MO_RELAXED), MO_RELEASE);
            if (!h.have_nxt) return -1.0;
            try_hold(h); // its bytes are on their way: the device need not play silence meanwhile
            return kWantNap;
        }
        h.cur = h.nxt; h.nxt = Playing{};
        h.have_cur = true; h.have_nxt = false;
        h.cur_frames = 0; h.cur_done = false; h.cur_marked = false;
        h.cur_status = DecodeStatus::End; h.cur_reason[0] = 0;
        h.draining = h.device && !device_fits(h, h.cur.info.format);
    }

    if (h.draining) {
        // The device must change. Wait for what is in the lead to be taken and for the
        // device to have played it out (its delay's worth of silence since), then close it.
        u64 w = atomic_load(&h.write, MO_RELAXED);
        atomic_store(&h.end_frame, w, MO_RELEASE);
        bool empty = atomic_load(&h.read, MO_ACQUIRE) == w;
        if (!empty || atomic_load(&h.silent_run, MO_ACQUIRE) < hifi_device_delay(h.device))
            return atomic_load(&h.want_pause, MO_RELAXED) ? -1.0 : 0.002;
        close_device(h);
        h.draining = false;
    }
    if (!h.device) {
        const char* why = "";
        if (!open_device(h, &why)) { fail_cur(h, why); return 0.0; }
    }
    release_hold(h); // there is something to play

    if (h.scratch_at == h.scratch_frames && !h.cur_done) {
        DecodeResult r = h.cur.src.decoder->decode(h.cur.state, h.scratch, kChunkFrames);
        h.scratch_frames = r.frames; h.scratch_at = 0;
        h.cur_frames += r.frames;
        if (r.status == DecodeStatus::Wait) {
            if (const char* dead = starved_reason(h, h.cur.src.id)) {
                r.status = DecodeStatus::Damaged; // the bytes will not come: it ends here
                r.reason = dead;
            } else {
                want(h, h.cur, r.need);
                if (r.frames == 0) return kWantNap; // nothing to put in the lead until they come
            }
        } else {
            h.cur.wanted = ByteRange{};
        }
        if (r.status != DecodeStatus::Ok && r.status != DecodeStatus::Wait) {
            h.cur_done = true; h.cur_status = r.status;
            format(h.cur_reason, sizeof h.cur_reason, "%s", r.reason);
            prepare_next(h); // opened now, a lead's length before it is needed, its wants said in time
        }
    }

    u64 w = atomic_load(&h.write, MO_RELAXED);
    u32 left = h.scratch_frames - h.scratch_at;
    if (left) {
        u64 used = w - atomic_load(&h.read, MO_ACQUIRE);
        u32 room = h.ring_frames - (u32)used;
        if (room == 0) return atomic_load(&h.want_pause, MO_RELAXED) ? -1.0 : 0.005; // the lead is full
        u32 n = left < room ? left : room;
        convert_in(h, h.scratch + (usize)h.scratch_at * h.format.channels, w, n);
        if (!h.cur_marked) { note(h, HifiEventKind::Started, h.cur.src.id, w); h.cur_marked = true; }
        // If these are the stream's last frames for now, say so before they can be seen:
        // the callback must not take the end of the music for a lead running dry.
        // The stream goes on only if another source is ready that this device can take:
        // so the next is opened now, before the current one's last frames go out.
        bool last = h.cur_done && n == left;
        if (last) {
            if (!take_commands(h)) return -2.0;
            if (!h.have_cur) return 0.0; // a flush came in with those commands
            prepare_next(h);
            // The next source is still waiting for its first bytes: hold these frames
            // back while the lead lasts, so the stream can go on into it without a gap.
            if (h.have_nxt && h.nxt.opening && w - atomic_load(&h.read, MO_ACQUIRE) > h.start_frames && !atomic_load(&h.want_pause, MO_RELAXED))
                return kWantNap;
        }
        bool goes_on = !last || (h.have_nxt && !h.nxt.opening && device_fits(h, h.nxt.info.format));
        atomic_store(&h.end_frame, goes_on ? kNever : w + n, MO_RELEASE);
        atomic_store(&h.write, w + n, MO_RELEASE);
        if (h.device_bits < h.cur.info.format.bits) { os_mutex_lock(&h.lock); h.device_bits = h.cur.info.format.bits; os_mutex_unlock(&h.lock); }
        h.scratch_at += n;
        w += n;
    }
    if (h.cur_done && h.scratch_at == h.scratch_frames) {
        if (!h.cur_marked) { note(h, HifiEventKind::Started, h.cur.src.id, w); h.cur_marked = true; } // a source with no frames at all
        note(h, HifiEventKind::Ended, h.cur.src.id, w, h.cur_reason);
        release_cur(h);
    }
    return 0.0;
}

void render_thread(void* arg) {
    Hifi& h = *(Hifi*)arg;
    for (;;) {
        if (!take_commands(h)) break;
        // Every decoder call is inside this. The fence after the count orders this
        // thread's loads of the sources' maps after it (hifi_read_fence's other half).
        atomic_add_fetch(&h.readers, (u64)1, MO_SEQ_CST);
        atomic_fence(MO_SEQ_CST);
        f64 wait = render_step(h);
        atomic_add_fetch(&h.readers, (u64)1, MO_SEQ_CST);
        if (wait < -1.5) break;
        if (wait >= 0.0) h.playout_waits = 0;
        if (wait > 0.0) { time_sleep(wait); continue; }
        if (wait < 0.0) {
            // Nothing to do until the caller says something, once the device is held.
            if (!try_hold(h)) {
                // Still playing out: look again shortly, sooner if a command comes. Korvid
                // has no timed wait on a condition, so this is short sleeps for as long
                // as the play-out lasts (at most the lead). Past six seconds the device
                // has stopped calling back on its own (a daemon gone), and the naps grow.
                f64 nap = ++h.playout_waits < 300 ? 0.002 : 0.05;
                for (u32 i = 0; i < 10 && !has_commands(h); ++i) time_sleep(nap);
                continue;
            }
            os_mutex_lock(&h.lock);
            // Play lets a held device go and wakes this thread: it may have more to play
            // out before it can be held again.
            while (h.cmds.count == 0 && (!h.have_cur || atomic_load(&h.want_pause, MO_RELAXED)) && (h.held || !h.device))
                os_cond_wait(&h.wake, &h.lock);
            os_mutex_unlock(&h.lock);
        }
    }
    release_cur(h);
    release_nxt(h);
    close_device(h);
}

void send(Hifi& h, const Cmd& c) {
    os_mutex_lock(&h.lock);
    if (!h.cmds.push(c)) KV_ERROR("hifi", "the command list is full: a command is lost");
    os_cond_signal(&h.wake);
    os_mutex_unlock(&h.lock);
}

} // namespace

Hifi* hifi_create(const HifiConfig& config) {
    Hifi* h = new (mem_alloc(sizeof(Hifi))) Hifi();
    h->config = config;
    if (!h->config.device) h->config.device = "default";
    os_mutex_init(&h->lock);
    os_cond_init(&h->wake);
    h->scratch = (i32*)mem_alloc((usize)kChunkFrames * 8 * sizeof(i32));
    if (!os_thread_start(&h->thread, render_thread, h)) {
        os_cond_destroy(&h->wake); os_mutex_destroy(&h->lock);
        mem_free(h->scratch); mem_free(h);
        return nullptr;
    }
    return h;
}

void hifi_destroy(Hifi* h) {
    if (!h) return;
    Cmd c; c.kind = CmdKind::Quit;
    send(*h, c);
    os_thread_join(&h->thread);
    os_cond_destroy(&h->wake);
    os_mutex_destroy(&h->lock);
    mem_free(h->ring);
    mem_free(h->scratch);
    mem_free(h);
}

void hifi_add_source(Hifi* h, const HifiSource& source) {
    Cmd c; c.kind = CmdKind::Add; c.source = source;
    send(*h, c);
}

void hifi_pause(Hifi* h) { atomic_store(&h->want_pause, 1u, MO_RELEASE); }

void hifi_play(Hifi* h) {
    os_mutex_lock(&h->lock);
    atomic_store(&h->want_pause, 0u, MO_RELEASE);
    if (h->held) { hifi_device_hold(h->device, false); h->held = false; } // at once, not when the render thread wakes
    os_cond_signal(&h->wake); // the render thread may be waiting on a full lead
    os_mutex_unlock(&h->lock);
}

void hifi_flush(Hifi* h) {
    ++h->epoch;
    h->pending.count = 0; // marks not yet reached never will be
    h->cur_source = 0;
    Cmd c; c.kind = CmdKind::Flush; c.epoch = h->epoch;
    send(*h, c);
}

void hifi_starve(Hifi* h, u64 id, const char* reason) {
    Cmd c; c.kind = CmdKind::Starve; c.source.id = id;
    format(c.reason, sizeof c.reason, "%s", reason);
    send(*h, c);
}

u64 hifi_read_fence(Hifi* h) {
    atomic_fence(MO_SEQ_CST); // the caller's clearing of a byte comes before this load
    u64 v = atomic_load(&h->readers, MO_SEQ_CST);
    return v + (v & 1);       // in a step: passed when it ends. Between steps: passed now
}

bool hifi_read_passed(Hifi* h, u64 fence) { return atomic_load(&h->readers, MO_SEQ_CST) >= fence; }

u32 hifi_poll(Hifi* h, HifiEvent* out, u32 cap) {
    u32 n = 0;
    // The render thread's notes: the timeless ones out at once, the marks kept until reached.
    os_mutex_lock(&h->lock);
    u32 delay = h->device ? hifi_device_delay(h->device) : 0;
    while (h->notes.count) {
        Note& x = h->notes.at(0);
        bool mark = x.event.kind == HifiEventKind::Started || x.event.kind == HifiEventKind::Ended;
        if (mark) {
            if (x.epoch == h->epoch && !h->pending.push(x)) break; // full: leave it for the next poll
        } else {
            if (n == cap) break;
            out[n++] = x.event;
        }
        h->notes.remove(0);
    }
    os_mutex_unlock(&h->lock);

    // What has been heard: the frames taken, less those still on their way to the ear.
    // Silence written since the last real frame has pushed real frames out ahead of it.
    u64 taken = atomic_load(&h->read, MO_ACQUIRE);
    u64 quiet = atomic_load(&h->silent_run, MO_ACQUIRE);
    u64 in_flight = quiet >= delay ? 0 : delay - quiet;
    u64 now = taken > in_flight ? taken - in_flight : 0;
    if (now > h->heard) h->heard = now;

    while (h->pending.count && n < cap) {
        Note& x = h->pending.at(0);
        bool reached = x.event.kind == HifiEventKind::Started ? h->heard > x.frame : h->heard >= x.frame;
        if (!reached) break;
        if (x.event.kind == HifiEventKind::Started) { h->cur_source = x.event.source; h->cur_frame = x.frame; }
        else if (h->cur_source == x.event.source) h->cur_source = 0;
        out[n++] = x.event;
        h->pending.remove(0);
    }

    u32 under = atomic_load(&h->underruns, MO_RELAXED);
    if (under != h->seen_underruns && n < cap) {
        HifiEvent e; e.kind = HifiEventKind::Underrun; e.frames = under - h->seen_underruns;
        h->seen_underruns = under;
        out[n++] = e;
    }
    return n;
}

void hifi_status(Hifi* h, HifiStatus* out) {
    HifiStatus s;
    os_mutex_lock(&h->lock);
    s.device_open = h->device != nullptr;
    s.format = h->format;
    s.shared = h->shared;
    s.exact  = s.device_open && !s.shared && h->device_bits <= hifi_sample_bits(h->format.sample);
    s.held   = h->held;
    os_mutex_unlock(&h->lock);
    s.flushing = atomic_load(&h->flushes_done, MO_ACQUIRE) != h->epoch;
    s.paused  = atomic_load(&h->want_pause, MO_RELAXED) != 0;
    s.taken   = atomic_load(&h->read, MO_ACQUIRE);
    s.written = atomic_load(&h->write, MO_ACQUIRE);
    s.lead_frames = s.written > s.taken ? (u32)(s.written - s.taken) : 0;
    s.underruns = atomic_load(&h->underruns, MO_RELAXED);
    s.heard    = h->heard;
    s.source   = h->cur_source;
    s.position = h->cur_source && h->heard > h->cur_frame ? h->heard - h->cur_frame : 0;
    *out = s;
}

} // namespace kv
