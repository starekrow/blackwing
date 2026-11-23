// The audio path: the caller's side, the render thread and the device callback.
//
// A render is everything from decoder to ring: the sources being played in order, their
// decoders, and a ring the render thread fills. There are two slots, each a ring and its
// counters. One is heard: the callback takes from it. A jump (a seek, a skip, a flush)
// starts a new render in the other slot, and the callback joins it: it fades the heard
// ring out at once, then switches to the new one when it has a short lead, and fades
// that in. The new render's frames count on in the same stream from wherever the
// callback switched, so a mark the new render makes is placed (by the caller, in
// hifi_poll) once that frame is known.
//
// Every frame decoded is also kept, before any stage, in a source ring keyed by programme
// frame. A slot's ring is its chain run over that, so a change of parameters (an aligned
// recut) is a new render in the other slot of the same music in the same stream frames,
// from the next frame the callback takes, and the callback crossfades into it.
//
// What is shared, and how:
//
//   caller <-> render   `lock` guards the command list, the event list and the device
//                       pointer. Neither side holds it while it decodes, sleeps or waits
//                       for the other; the render thread waits on `wake` when idle.
//   render  -> callback each slot's bytes and `write` (released after the bytes are in),
//                       its `end` (the stream ends here for now: running dry there is not
//                       an underrun); `join_pending` (a render waits to be joined: its
//                       epoch), with `join_slot` and `join_fade` ahead of it. The render
//                       thread takes a request back with a compare-exchange, and the
//                       callback takes it up with one, so exactly one of them wins.
//   callback -> render  each slot's `read`; `heard_slot`, `joined` (the epoch it joined
//                       last) and the joined slot's `base` (its frame 0 in the stream);
//                       `read` (stream frames taken), `silent_run`, `underruns`.
//   caller  -> callback `want_pause`.
//   caller <-> render   `held`, under `lock`: the device is held. The render thread holds
//                       it when there is nothing to play and the device has played out
//                       what it was given; play, or something to play, lets it go.
//   render  -> caller   `readers`: odd while the render thread is in a step, where every
//                       decoder call is made. A caller that clears a piece's byte in a
//                       source's map waits for it to move on before the piece goes.
//
// Stream frames are the frames the device has taken, counted from zero and never reset.
// A slot counts its own frames from zero; the stream frame of one is the slot's `base`
// plus it. A mark (a source starts here, ends here) is a stream frame. A change of device
// format keeps counting.
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
constexpr f64 kWantNap     = 0.002; // how soon a decoder waiting for bytes, or a join, is looked at again

enum class CmdKind : u8 { Add, Flush, Jump, Replace, Starve, Param, Quit };
struct Cmd {
    CmdKind    kind = CmdKind::Add;
    HifiSource source;
    u32        epoch = 0;              // a flush or jump's; a parameter's generation
    u32        stage = 0, param = 0;   // a parameter
    f64        value = 0.0;
    u64        sent_ns = 0;            // when the caller sent it
    char       reason[96] = "";
};

constexpr u32 kAligned     = 0x80000000u; // a join request's id: an aligned recut's, not a caller's epoch
constexpr u32 kMaxSegments = 64;          // sources begun within what the source ring holds
constexpr u64 kDitherSeed  = 0x6A09E667F3BCC909ull;

// Where a source begins in the programme: the source ring's frames from `p` on are its,
// starting at its frame `first`, at its depth.
struct Segment { u64 p = 0; u64 first = 0; u32 bits = 16; u64 source = 0; };

// A render's chain: its stages' states, the parameters they were opened with.
struct Chain {
    u8*   mem[kHifiMaxStages]   = {};
    void* state[kHifiMaxStages] = {};
    bool  flat = true;
    u32   gen  = 0;                  // the parameters' generation
};

u64 time_ns() { return (u64)(time_now() * 1e9); }

// What the render thread tells the caller. A mark carries the stream frame it becomes
// true at; the others are true at once. A mark of a render not yet joined carries its
// slot's frame instead, and the caller places it once the callback has said where that
// slot's frame 0 fell.
struct Note {
    HifiEvent event;
    u64       frame = 0;
    u32       epoch = 0;
    bool      relative = false;
    u32       slot = 0;
    bool      cut = false; // not an event: marks at or after `frame` are void, but `event.source`'s
};

template <class T, u32 N> struct Fifo {
    T   items[N];
    u32 head = 0, count = 0;
    bool push(const T& t) { if (count == N) return false; items[(head + count++) % N] = t; return true; }
    bool pop(T* t) { if (!count) return false; *t = items[head]; head = (head + 1) % N; --count; return true; }
    T&   at(u32 i) { return items[(head + i) % N]; }
    void remove(u32 i) { for (u32 k = i; k + 1 < count; ++k) at(k) = at(k + 1); --count; }
};

// A source with its decoder open, or opening (and seeking to where it starts).
struct Playing {
    HifiSource  src;
    void*       state   = nullptr;
    u8*         memory  = nullptr;
    DecoderInfo info;
    bool        opening = false; // its decoder's open, or its seek, is waiting for bytes
    ByteRange   wanted;          // the last Want said for it, while it waits
};

struct Starved { u64 id = 0; char reason[96] = ""; };

// A ring and its counters: one render's lead.
struct Slot {
    u8* data  = nullptr;           // ring_frames * frame_bytes; replaced only while no device is open
    u64 write = 0;                 // atomic, render -> callback: frames written, from the slot's start
    u64 end   = kNever;            // atomic, render -> callback: the slot's stream ends here for now
    u64 read  = 0;                 // atomic, callback -> anyone: frames taken
    u64 base  = 0;                 // atomic: the stream frame of the slot's frame 0, once it is heard
    u32 gen   = 0;                 // atomic, render -> caller: the generation of the parameters it was rendered with
    u32 flat  = 1;                 // atomic, render -> caller: every stage of its chain is flat
    u64 heard_from = 0;            // atomic: the stream frame from which it is the one heard
};

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
    u32 epoch         = 0;             // bumped by a flush or a jump; older marks are dropped
    u64 heard         = 0;
    u32 seen_underruns = 0;
    u64 cur_source    = 0;
    u64 cur_frame     = 0;             // the stream frame the source being heard started at
    u64 cur_first     = 0;             // the frame of it heard there
    u32 param_gen     = 0;             // the last parameter change's generation
    u32 gen_said      = 0;             // the last ParamsHeard given out
    OsThread thread;

    // ---- render <-> callback ----
    Slot slot[2];
    u32  ring_frames  = 0;
    u32  frame_bytes  = 0;
    u32  ramp_frames  = 1;
    u32  fade_frames  = 1;             // a jump's fade, out and in
    u32  start_frames = 1;
    u32  join_frames  = 1;             // the lead a jump waits for before it is joined
    u32  heard_slot   = 0;             // atomic: the callback's while a device is open
    u32  join_pending = 0;             // atomic: the epoch of a render waiting to be joined; 0: none
    u32  join_slot    = 0;             // atomic: its slot, written before join_pending
    u32  join_fade    = 0;             // atomic: 1 to fade it in (a jump), 0 for a clean start (a flush)
    u32  joined       = 0;             // atomic: the last request the callback joined (an epoch, or an aligned recut's id)
    u32  jump_joined  = 0;             // atomic: the last flush or jump joined (recuts leave it), and
    u64  jump_base    = 0;             // atomic: the stream frame its slot's frame 0 fell at: where its relative marks are placed
    u32  join_kind    = 0;             // atomic: 1 for an aligned recut (crossfaded), 0 for a flush or a jump
    u32  xfading      = 0;             // atomic, callback -> anyone: an aligned join's fade is under way
    u32  recuts       = 0;             // atomic: aligned joins done
    u64  req_sent_ns  = 0;             // atomic: when the newest change in the request was sent
    u64  join_ns      = 0;             // atomic: when the callback began the last aligned join
    u64  joined_sent_ns = 0;           // atomic: req_sent_ns of that join
    u64  jump_ns      = 0;             // atomic: when the callback joined the last jump (its first frame), and
    u64  jump_sent_ns = 0;             // atomic: when that jump was asked for
    u64  recut_render_ns = 0;          // atomic: the last recut's rendering of the lead, all of it

    // ---- callback -> anyone ----
    u64 read         = 0;              // atomic: stream frames taken
    u64 silent_run   = 0;              // atomic: frames of silence the callback has written since its last real frame
    u32 underruns    = 0;              // atomic

    // ---- caller -> callback, render ----
    u32 want_pause   = 0;              // atomic
    u32 flushes_done = 0;              // atomic: the last flush or jump the render thread has carried out
    u64 readers      = 0;              // atomic: odd while the render thread is in a step
    bool held        = false;          // under `lock`: the device is held (hifi_device_hold)

    // ---- the callback's alone ----
    bool running  = false;             // taking from the lead
    bool fresh    = true;              // the next start is a clean one: full gain from its first frame
    bool by_join  = false;             // the next start is a joined jump's: at once, fading in
    bool fading   = false;             // the ramp under way is a jump's fade
    bool jump_timing = false;          // a jump was joined: its first frame out is timed
    bool xf       = false;             // crossfading from slot xf_from to the other
    u32  xf_from  = 0, xf_pos = 0, xf_id = 0;
    f32  gain     = 1.0f;

    // ---- the render thread's alone ----
    Fifo<HifiSource, kMaxPending> queue;
    bool         have_cur = false, have_nxt = false;
    Playing      cur;                  // being decoded into the lead
    Playing      nxt;                  // opened early, so the stream knows whether it goes on past cur's last frame
    u64          cur_frames = 0;       // decoded so far
    bool         cur_done   = false;   // the decoder has given its last
    DecodeStatus cur_status = DecodeStatus::End;
    char         cur_reason[96] = "";
    bool         cur_marked = false;   // its Started (or Seeked) mark is out
    bool         draining   = false;   // waiting for the lead to play out before the device changes
    u32          playout_waits = 0;    // looks, since there was work, for the device to have played out
    u32          render_epoch = 0;
    u32          wslot = 0;            // the slot this render writes
    u32          pending_epoch = 0;    // this render waits to be joined: its marks are its slot's frames
    Starved      starved[kMaxStarved]; // sources whose bytes will not come
    u32          nstarved = 0;
    i32*         scratch = nullptr;    // kChunkFrames * 8 decoded samples
    u32          scratch_frames = 0, scratch_at = 0;
    // The chain, and the source: every decoded frame, kept from a little behind the
    // frame heard to the front of the lead, keyed by programme frame (a count of the
    // frames rendered, never reset), so a recut is the chain run again over it.
    Span<const StageDesc* const> stages;
    f64          desired[kHifiMaxStages][kHifiMaxParams] = {};
    u32          desired_gen = 0;
    u64          desired_sent_ns = 0;
    Chain        chain[2];
    u64          p0[2] = {};           // the programme frame of each slot's frame 0
    i32*         src = nullptr;        // src_frames * channels
    u32          src_frames = 0;
    u32          keep_behind = 1;      // frames kept behind the frame heard, for a run-in
    u64          p_valid = 0;          // the first programme frame the source ring holds
    Segment      segs[kMaxSegments];
    u32          nsegs = 0;
    u32          req_id = 0;           // the join request outstanding: a jump's epoch, or an aligned recut's id
    u32          recut_seq = 0;
    f64*         fbuf = nullptr;       // kChunkFrames * 8
};

namespace {

// ---- the callback ---------------------------------------------------------------------

// In double: an f32 cannot hold a 32-bit sample, and the product must not round twice.
template <class T> inline T scaled(T v, f32 g) { return (T)llrint((f64)v * (f64)g); }
template <> inline f32 scaled<f32>(f32 v, f32 g) { return v * g; }

// One frame from a ring to `out`, times `g`. At full gain it is a copy: the ramp's
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

// Switches to the render waiting to be joined, if it has its short lead (or all it will
// have), and the request is still standing. Nothing may be sounding: a jump has faded
// the heard ring out first. Its frame 0 is stream frame `stream`.
bool try_join(Hifi& h, u32 epoch, u64 stream) {
    u32 b = atomic_load(&h.join_slot, MO_RELAXED);
    Slot& s = h.slot[b];
    u64 w = atomic_load(&s.write, MO_ACQUIRE), endf = atomic_load(&s.end, MO_ACQUIRE);
    if (w < h.join_frames && w != endf) return false;
    u32 fade = atomic_load(&h.join_fade, MO_RELAXED);
    u32 expect = epoch;
    if (!atomic_compare_exchange(&h.join_pending, &expect, 0u, false, MO_ACQ_REL, MO_ACQUIRE)) return false; // taken back
    atomic_store(&s.base, stream, MO_RELEASE);
    atomic_store(&h.heard_slot, b, MO_RELEASE);
    atomic_store(&s.heard_from, stream, MO_RELEASE);
    atomic_store(&h.jump_base, stream, MO_RELAXED);
    atomic_store(&h.jump_joined, epoch, MO_RELEASE);
    atomic_store(&h.joined, epoch, MO_RELEASE);
    if (fade) { atomic_store(&h.jump_sent_ns, atomic_load(&h.req_sent_ns, MO_RELAXED), MO_RELAXED); h.jump_timing = true; }
    h.by_join = fade != 0;
    h.fresh   = fade == 0;
    return true;
}

// An aligned recut: the same music in the same stream frames, rendered again. Joined
// when its ring holds the frame the callback is at and a fade's worth after it: then the
// callback crossfades into it from there (or, with nothing sounding, switches). Frames of
// it already in the past are dropped. Until then the heard ring plays on.
bool try_align(Hifi& h, u32 id, u64 stream) {
    u32 b = atomic_load(&h.join_slot, MO_RELAXED);
    Slot& nb = h.slot[b];
    Slot& na = h.slot[b ^ 1u];
    u64 from = atomic_load(&nb.base, MO_ACQUIRE);
    if (stream < from) return false;
    u64 at = stream - from;                      // its frame for where the callback is
    u64 w = atomic_load(&nb.write, MO_ACQUIRE), endf = atomic_load(&nb.end, MO_ACQUIRE);
    u64 fade = h.running ? h.fade_frames : 0;
    u64 left_a = atomic_load(&na.write, MO_ACQUIRE) - atomic_load(&na.read, MO_RELAXED);
    if (left_a < fade) fade = 0;                 // the old one has not a fade's worth left: switch
    bool ready = w >= at + (fade ? fade : 1) || (w == endf && w >= at);
    if (!ready) return false;
    u32 expect = id;
    if (!atomic_compare_exchange(&h.join_pending, &expect, 0u, false, MO_ACQ_REL, MO_ACQUIRE)) return false; // taken back
    atomic_store(&nb.read, at, MO_RELEASE);
    atomic_store(&nb.heard_from, stream, MO_RELEASE);
    atomic_store(&h.join_ns, time_ns(), MO_RELAXED);
    atomic_store(&h.joined_sent_ns, atomic_load(&h.req_sent_ns, MO_RELAXED), MO_RELAXED);
    if (!fade) {
        atomic_store(&h.heard_slot, b, MO_RELEASE);
        atomic_fetch_add(&h.recuts, 1u, MO_RELAXED);
        atomic_store(&h.joined, id, MO_RELEASE);
        return true;
    }
    h.xf = true; h.xf_from = b ^ 1u; h.xf_pos = 0; h.xf_id = id;
    atomic_store(&h.xfading, 1u, MO_RELEASE);
    return true;
}

inline f64 sample_of(const Hifi& h, const u8* frame, u32 c) {
    switch (h.format.sample) {
    case HifiSampleFormat::S16: return ((const i16*)frame)[c];
    case HifiSampleFormat::F32: return ((const f32*)frame)[c];
    default:                    return ((const i32*)frame)[c];
    }
}
inline void put_sample(const Hifi& h, u8* frame, u32 c, f64 v) {
    switch (h.format.sample) {
    case HifiSampleFormat::S16: ((i16*)frame)[c] = (i16)llrint(v); break;
    case HifiSampleFormat::F32: ((f32*)frame)[c] = (f32)v; break;
    default:                    ((i32*)frame)[c] = (i32)llrint(v); break;
    }
}

// The aligned join's fade: old and new, frame for frame, linear over fade_frames, in
// double and rounded, with no dither: two renders that are the same samples join to
// those samples. Returns the frames written; at its end the new ring is the heard one.
u32 crossfade(Hifi& h, u8* dst, u32 frames, u64* stream) {
    Slot& a = h.slot[h.xf_from];
    Slot& b = h.slot[h.xf_from ^ 1u];
    u64 ra = atomic_load(&a.read, MO_RELAXED), rb = atomic_load(&b.read, MO_RELAXED);
    u32 n = h.fade_frames - h.xf_pos < frames ? h.fade_frames - h.xf_pos : frames;
    const u32 fb = h.frame_bytes, ch = h.format.channels;
    for (u32 k = 0; k < n; ++k) {
        const u8* fa = a.data + (usize)((ra + k) % h.ring_frames) * fb;
        const u8* fbm = b.data + (usize)((rb + k) % h.ring_frames) * fb;
        f64 t = (f64)(h.xf_pos + k + 1) / (f64)h.fade_frames;
        u8* o = dst + (usize)k * fb;
        for (u32 c = 0; c < ch; ++c) {
            f64 v = (1.0 - t) * sample_of(h, fa, c) + t * sample_of(h, fbm, c);
            put_sample(h, o, c, h.gain == 1.0f ? v : v * (f64)h.gain);
        }
    }
    h.xf_pos += n;
    atomic_store(&a.read, ra + n, MO_RELEASE);
    atomic_store(&b.read, rb + n, MO_RELEASE);
    *stream += n;
    if (h.xf_pos == h.fade_frames) {
        h.xf = false;
        atomic_store(&h.heard_slot, h.xf_from ^ 1u, MO_RELEASE);
        atomic_store(&h.xfading, 0u, MO_RELEASE);
        atomic_fetch_add(&h.recuts, 1u, MO_RELAXED);
        atomic_store(&h.joined, h.xf_id, MO_RELEASE);
    }
    return n;
}

// The device's callback, on its realtime thread. Takes from the heard ring; writes
// silence when there is nothing to take or it is holding. Its only arithmetic is the
// ramp and the fade.
void hifi_callback(void* user, void* out_v, u32 frames) {
    Hifi& h = *(Hifi*)user;
    u8*   out = (u8*)out_v;
    const u32 fb = h.frame_bytes;
    const bool pause = atomic_load(&h.want_pause, MO_RELAXED) != 0;
    const u32 req = atomic_load(&h.join_pending, MO_ACQUIRE);
    bool aligned = req && atomic_load(&h.join_kind, MO_RELAXED) == 1;
    u32 pend = aligned ? 0 : req; // a flush or a jump waiting: what is heard fades out for it
    u64 stream = atomic_load(&h.read, MO_RELAXED); // only this thread writes it
    u32 produced = 0;

    // A few passes: playing; then, if that faded out for a jump, the joined render, in
    // this same callback when it is ready; or an aligned join's crossfade, and after it.
    for (u32 pass = 0; pass < 6 && produced < frames; ++pass) {
        if (h.xf) {
            produced += crossfade(h, out + (usize)produced * fb, frames - produced, &stream);
            continue;
        }
        if (aligned && !(pause && h.running) && try_align(h, req, stream)) { aligned = false; continue; }
        Slot& s = h.slot[atomic_load(&h.heard_slot, MO_RELAXED)];
        u64 r = atomic_load(&s.read, MO_RELAXED);
        u64 w    = atomic_load(&s.write, MO_ACQUIRE);
        u64 endf = atomic_load(&s.end, MO_ACQUIRE);
        if (!h.running) {
            if (pend && try_join(h, pend, stream)) { pend = 0; continue; } // nothing sounding: switch now
            u64 ready = w - r;
            u64 need  = h.by_join ? h.join_frames : h.start_frames;
            if (pause || pend || ready == 0 || (ready < need && w != endf)) break;
            if (h.jump_timing) { atomic_store(&h.jump_ns, time_ns(), MO_RELAXED); h.jump_timing = false; } // the jump's first frame goes out now
            h.running = true;
            h.fading  = h.by_join;
            h.gain    = h.fresh ? 1.0f : 0.0f;
            h.fresh   = false;
            h.by_join = false;
        }
        const u64  avail  = w - r;
        const bool down   = pause || pend != 0;    // told to stop, or a jump waits: to silence
        const bool more   = (w != endf);           // the stream goes on past what is here
        const u32  want   = frames - produced;
        const u32  take   = want < avail ? want : (u32)avail;
        if (pend && atomic_load(&h.join_fade, MO_RELAXED)) h.fading = true; // a jump's fade out, at once; a flush ramps
        const u32  ramp   = h.fading ? h.fade_frames : h.ramp_frames;
        const f32  step   = 1.0f / (f32)ramp;
        const bool nearly_dry = more && (avail - take) < h.ramp_frames;
        u8* dst = out + (usize)produced * fb;
        u32 took = 0;
        if (h.gain == 1.0f && !down && !nearly_dry) {
            // The whole of normal playback: the ring's bytes, untouched.
            u32 at    = (u32)(r % h.ring_frames);
            u32 first = h.ring_frames - at < take ? h.ring_frames - at : take;
            memcpy(dst, s.data + (usize)at * fb, (usize)first * fb);
            memcpy(dst + (usize)first * fb, s.data, (usize)(take - first) * fb);
            took = take;
            h.fading = false;
        } else {
            for (; took < take; ++took) {
                if (down) {
                    h.gain -= step;
                    if (h.gain <= 0.0f) { h.gain = 0.0f; break; }
                } else {
                    // Running dry with more expected: down to silence by the last frame there is.
                    f32 limit = 1.0f;
                    if (more) { u64 left = avail - took - 1; if (left < h.ramp_frames) limit = (f32)left / (f32)h.ramp_frames; }
                    f32 g = h.gain + step;
                    h.gain = g < limit ? g : limit;
                    if (h.gain >= 1.0f) h.fading = false;
                }
                u32 at = (u32)((r + took) % h.ring_frames);
                copy_scaled(h, s.data + (usize)at * fb, dst + (usize)took * fb, h.gain);
            }
        }
        r += took;
        produced += took;
        stream += took;
        atomic_store(&s.read, r, MO_RELEASE);
        if (down && h.gain <= 0.0f) {
            h.running = false; // and round again: a jump may be ready behind it
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
    atomic_store(&h.read, stream, MO_RELEASE);
}

// ---- the render thread ----------------------------------------------------------------

void push_note(Hifi& h, const Note& n) {
    os_mutex_lock(&h.lock);
    if (!h.notes.push(n)) KV_ERROR("hifi", "the event list is full: an event for source %llu is lost", (unsigned long long)n.event.source);
    os_mutex_unlock(&h.lock);
}

// A note that is true at once.
void note(Hifi& h, HifiEventKind kind, u64 source, const char* reason = "") {
    Note n;
    n.event.kind   = kind;
    n.event.source = source;
    n.epoch = h.render_epoch;
    format(n.event.reason, sizeof n.event.reason, "%s", reason);
    push_note(h, n);
}

// A mark: true when the frame `at` of this render's slot is heard. While the render
// waits to be joined, the slot's place in the stream is not known: the caller places it.
void check_joined(Hifi& h);

void mark(Hifi& h, HifiEventKind kind, u64 at, const char* reason = "") {
    Note n;
    n.event.kind   = kind;
    n.event.source = h.cur.src.id;
    n.event.info   = h.cur.info;
    n.epoch = h.render_epoch;
    format(n.event.reason, sizeof n.event.reason, "%s", reason);
    if (kind == HifiEventKind::Ended) { n.event.status = h.cur_status; n.event.frames = h.cur_frames; }
    else                              n.event.frames = h.cur.src.start;
    check_joined(h); // joined since the step began: its place is known
    if (h.pending_epoch) {
        n.frame = at;
        n.relative = true;
        n.slot = h.wslot;
    } else {
        n.frame = atomic_load(&h.slot[h.wslot].base, MO_ACQUIRE) + at;
    }
    push_note(h, n);
}

// Whether the callback has joined this render: from then on its slot is the heard one.
void check_joined(Hifi& h) {
    if (h.req_id && atomic_load(&h.joined, MO_ACQUIRE) == h.req_id) { h.req_id = 0; h.pending_epoch = 0; }
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
    push_note(h, n);
}

// Lets go of a source: its decoder closed, its memory freed, Released sent.
void release(Hifi& h, Playing& p) {
    if (p.state) p.src.decoder->close(p.state);
    mem_free(p.memory);
    note(h, HifiEventKind::Released, p.src.id);
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

// Opens the next source that will open, as h.nxt, and seeks it to where it starts; the
// ones that will not, fail here. One whose open or seek waits for bytes stays h.nxt,
// opening, and is tried again on the next call.
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
        else if (!p.state) {
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
        if (!why[0] && p.state && p.src.start) {
            SeekResult r = p.src.decoder->seek(p.state, p.src.start);
            if (r.status == SeekStatus::Wait) {
                const char* dead = starved_reason(h, p.src.id);
                if (!dead) { want(h, p, r.need); return; } // still seeking
                why = dead;
            } else if (r.status == SeekStatus::Failed) {
                why = r.reason;
            }
        }
        if (why[0]) { note(h, HifiEventKind::Failed, p.src.id, why); release(h, p); h.have_nxt = false; continue; }
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
    h.by_join = false;
    h.gain    = 1.0f;
}

HifiSampleFormat narrowest_for(u32 bits) {
    return bits <= 16 ? HifiSampleFormat::S16 : bits <= 24 ? HifiSampleFormat::S24 : HifiSampleFormat::S32;
}

// Whether the open device can go straight on with a source of this format.
bool device_fits(const Hifi& h, const AudioFormat& f) {
    return h.device && h.format.rate == f.rate && h.format.channels == f.channels && f.bits <= hifi_sample_bits(h.format.sample);
}

void setup_chain(Hifi& h, u32 b, const HifiDeviceFormat& df);

// Opens the device for the current source's format, with rings to match. False, with
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
    // No callback is running: the rings and what describes them can change. What was in
    // them and never taken is gone with them, and the stream goes on from where it was;
    // this render's slot is the heard one, from the stream frame the device is at.
    h.frame_bytes  = hifi_frame_bytes(o.format);
    u64 want = (u64)(h.config.lead_seconds * (f64)o.format.rate);
    h.ring_frames  = (u32)(want < 2 * kChunkFrames ? 2 * kChunkFrames : want);
    for (Slot& s : h.slot) {
        mem_free(s.data);
        s.data = (u8*)mem_alloc((usize)h.ring_frames * h.frame_bytes);
    }
    auto frames_of = [&](f64 seconds) { u32 n = (u32)(seconds * (f64)o.format.rate); return n ? n : 1u; };
    h.ramp_frames  = frames_of(h.config.ramp_seconds);
    h.fade_frames  = frames_of(h.config.fade_seconds);
    u32 start = frames_of(h.config.start_seconds), join = frames_of(h.config.join_seconds);
    h.start_frames = start < h.ring_frames ? start : h.ring_frames;
    h.join_frames  = join < h.ring_frames ? join : h.ring_frames;
    Slot& s = h.slot[h.wslot];
    u64 w = atomic_load(&s.write, MO_RELAXED);
    atomic_store(&s.base, atomic_load(&h.read, MO_RELAXED) - w, MO_RELEASE); // the frames this render has written still count from where they would have
    atomic_store(&s.read, w, MO_RELEASE);
    atomic_store(&s.heard_from, atomic_load(&h.read, MO_RELAXED), MO_RELEASE);
    atomic_store(&h.heard_slot, h.wslot, MO_RELEASE);
    atomic_store(&h.join_pending, 0u, MO_RELEASE);
    atomic_store(&h.xfading, 0u, MO_RELEASE);
    h.xf = false;
    // The source ring, and the chain, for this format. What the old ring held was all heard.
    h.keep_behind = frames_of(0.5);
    mem_free(h.src);
    h.src_frames = h.ring_frames + h.keep_behind;
    h.src = (i32*)mem_alloc((usize)h.src_frames * o.format.channels * 4);
    h.p_valid = h.p0[h.wslot] + w;
    setup_chain(h, h.wslot, o.format);
    if (h.req_id && h.req_id != h.pending_epoch) h.req_id = 0; // an aligned recut has no callback to join it: the new chain is the heard one
    atomic_store(&h.silent_run, (u64)0, MO_RELEASE);
    check_joined(h);
    if (h.pending_epoch) { // a jump that was never joined: there is no callback to join it, so it is the heard render now
        atomic_store(&h.jump_base, atomic_load(&s.base, MO_RELAXED), MO_RELAXED);
        atomic_store(&h.jump_joined, h.pending_epoch, MO_RELEASE);
        atomic_store(&h.joined, h.pending_epoch, MO_RELEASE);
        check_joined(h);
    }
    h.req_id = 0;
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

// ---- the chain and the source ---------------------------------------------------------

// Opens slot b's chain fresh, for the device's format, at the parameters wanted now.
void setup_chain(Hifi& h, u32 b, const HifiDeviceFormat& df) {
    Chain& c = h.chain[b];
    AudioFormat f{df.rate, df.channels, (u16)hifi_sample_bits(df.sample)};
    c.flat = true;
    for (u32 i = 0; i < h.stages.len; ++i) {
        const StageDesc& d = *h.stages[i];
        if (!c.mem[i]) c.mem[i] = (u8*)mem_alloc(d.memory_bytes ? d.memory_bytes : 1);
        c.state[i] = d.open(f, Span<u8>(c.mem[i], d.memory_bytes ? d.memory_bytes : 1));
        for (u32 j = 0; j < d.params.len && j < kHifiMaxParams; ++j) d.set(c.state[i], j, h.desired[i][j]);
        c.flat = c.flat && d.flat(c.state[i]);
    }
    c.gen = h.desired_gen;
    atomic_store(&h.slot[b].gen, c.gen, MO_RELEASE);
    atomic_store(&h.slot[b].flat, c.flat ? 1u : 0u, MO_RELEASE);
}

u32 chain_run_in(const Hifi& h, u32 b) {
    u32 n = 0;
    for (u32 i = 0; i < h.stages.len; ++i) { u32 r = h.stages[i]->run_in(h.chain[b].state[i]); if (r > n) n = r; }
    return n;
}

// The segment programme frame p is in: the last begun at or before it.
u32 segment_of(const Hifi& h, u64 p) {
    u32 k = 0;
    while (k + 1 < h.nsegs && h.segs[k + 1].p <= p) ++k;
    return k;
}

// A source begins at programme frame p. Segments wholly behind what the source ring
// keeps are let go.
void begin_segment(Hifi& h, u64 p, u64 first, u32 bits, u64 source) {
    const Slot& hs = h.slot[atomic_load(&h.heard_slot, MO_ACQUIRE)];
    u64 heard_p = h.p0[atomic_load(&h.heard_slot, MO_ACQUIRE)] + atomic_load(&hs.read, MO_ACQUIRE);
    u64 oldest = heard_p > h.keep_behind ? heard_p - h.keep_behind : 0;
    while (h.nsegs >= 2 && (h.segs[1].p <= oldest || h.nsegs == kMaxSegments)) {
        for (u32 k = 1; k < h.nsegs; ++k) h.segs[k - 1] = h.segs[k];
        --h.nsegs;
    }
    if (h.nsegs && h.segs[h.nsegs - 1].p == p) --h.nsegs; // a source that gave nothing: replaced
    h.segs[h.nsegs++] = Segment{p, first, bits, source};
}

// Decoded samples into the source ring at programme frame p.
void put_source(Hifi& h, const i32* in, u64 p, u32 frames) {
    const u32 ch = h.format.channels;
    u32 at = (u32)(p % h.src_frames);
    u32 first = h.src_frames - at < frames ? h.src_frames - at : frames;
    memcpy(h.src + (usize)at * ch, in, (usize)first * ch * 4);
    memcpy(h.src, in + (usize)first * ch, (usize)(frames - first) * ch * 4);
}

// TPDF dither, a function of the source's frame and the channel rather than a running
// generator, so two renders of the same frames are the same bits wherever they began:
// two uniform values, their difference, triangular over (-1, 1) of the last bit.
inline u64 mix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline f64 tpdf(u64 frame, u32 channel) {
    u64 r = mix64(kDitherSeed ^ (frame * 8 + channel));
    return (f64)(r & 0xFFFFFFFFull) * (1.0 / 4294967296.0) - (f64)(r >> 32) * (1.0 / 4294967296.0);
}

// Programme frames [p, p + n) from the source ring, through slot b's chain, into its
// ring at its frame w; or, `discard`, through the chain and nowhere (a run-in). Each
// segment's first frame resets the chain: a source is run in from nothing. A flat chain
// takes the integers through, widened and nothing else, and makes no float.
void render_frames(Hifi& h, u32 b, u64 p, u64 n, u64 w, bool discard = false) {
    const Chain& c = h.chain[b];
    const u32 ch = h.format.channels, fb = h.frame_bytes;
    const u32 dev_bits = hifi_sample_bits(h.format.sample);
    u8* ring = h.slot[b].data;
    while (n) {
        u32 k = segment_of(h, p);
        const Segment& sg = h.segs[k];
        u64 seg_end = k + 1 < h.nsegs ? h.segs[k + 1].p : ~0ull;
        u64 m64 = seg_end - p < n ? seg_end - p : n;
        u32 m = m64 < kChunkFrames ? (u32)m64 : kChunkFrames;
        if (p == sg.p) for (u32 i = 0; i < h.stages.len; ++i) h.stages[i]->reset(c.state[i]);
        if (c.flat) {
            if (!discard)
                for (u32 i = 0; i < m; ++i) {
                    const i32* sv = h.src + (usize)((p + i) % h.src_frames) * ch;
                    u8* dst = ring + (usize)((w + i) % h.ring_frames) * fb;
                    switch (h.format.sample) {
                    case HifiSampleFormat::S16: { i16* o = (i16*)dst; for (u32 q = 0; q < ch; ++q) o[q] = (i16)((u32)sv[q] << (16 - sg.bits)); } break;
                    case HifiSampleFormat::S24: { i32* o = (i32*)dst; for (u32 q = 0; q < ch; ++q) o[q] = (i32)((u32)sv[q] << (24 - sg.bits)); } break;
                    case HifiSampleFormat::S32: { i32* o = (i32*)dst; for (u32 q = 0; q < ch; ++q) o[q] = (i32)((u32)sv[q] << (32 - sg.bits)); } break;
                    case HifiSampleFormat::F32: { f32* o = (f32*)dst; f32 kq = 1.0f / (f32)((u64)1 << (sg.bits - 1)); for (u32 q = 0; q < ch; ++q) o[q] = (f32)sv[q] * kq; } break;
                    case HifiSampleFormat::Count: break;
                    }
                }
        } else {
            f64* x = h.fbuf;
            const f64 in_scale = 1.0 / (f64)((u64)1 << (sg.bits - 1));
            for (u32 i = 0; i < m; ++i) {
                const i32* sv = h.src + (usize)((p + i) % h.src_frames) * ch;
                for (u32 q = 0; q < ch; ++q) x[(usize)i * ch + q] = (f64)sv[q] * in_scale;
            }
            for (u32 i = 0; i < h.stages.len; ++i) h.stages[i]->process(c.state[i], x, m);
            if (!discard) {
                const f64 full = (f64)((u64)1 << (dev_bits - 1));
                for (u32 i = 0; i < m; ++i) {
                    u8* dst = ring + (usize)((w + i) % h.ring_frames) * fb;
                    u64 frame = sg.first + (p + i - sg.p);
                    for (u32 q = 0; q < ch; ++q) {
                        f64 y = x[(usize)i * ch + q];
                        if (h.format.sample == HifiSampleFormat::F32) { ((f32*)dst)[q] = (f32)y; continue; }
                        // An exact zero (mute, a stage's silence) stays digital silence, undithered.
                        f64 v = y == 0.0 ? 0.0 : floor(y * full + tpdf(frame, q) + 0.5);
                        if (v > full - 1.0) v = full - 1.0;
                        if (v < -full) v = -full;
                        if (h.format.sample == HifiSampleFormat::S16) ((i16*)dst)[q] = (i16)v; else ((i32*)dst)[q] = (i32)v;
                    }
                }
            }
        }
        p += m; w += m; n -= m;
    }
}

// The current source cannot be played. If nothing follows it, this render's stream ends
// where it is: the callback was told more was coming, and now it is not.
void fail_cur(Hifi& h, const char* why) {
    note(h, HifiEventKind::Failed, h.cur.src.id, why);
    release_cur(h);
    Slot& s = h.slot[h.wslot];
    if (h.queue.count == 0 && !h.have_nxt) atomic_store(&s.end, atomic_load(&s.write, MO_RELAXED), MO_RELEASE);
}

// A flush or a jump: everything this render has goes, and a new one starts, empty, in a
// slot the callback is not reading. With a device open, the callback is asked to join
// it; a request still waiting from an earlier jump is taken back first, and its slot
// used again. With none open, there is nothing to join: the new render is the heard one.
void start_render(Hifi& h, u32 epoch, bool fade, u64 sent_ns) {
    release_cur(h);
    release_nxt(h);
    HifiSource s;
    while (h.queue.pop(&s)) { note(h, HifiEventKind::Released, s.id); forget_starved(h, s.id); }
    h.draining = false;
    h.render_epoch = epoch;
    u64 front = h.p0[h.wslot] + atomic_load(&h.slot[h.wslot].write, MO_RELAXED); // the new programme goes on from here
    if (!h.device) {
        Slot& w = h.slot[h.wslot];
        atomic_store(&w.base, atomic_load(&h.read, MO_RELAXED), MO_RELEASE);
        atomic_store(&w.write, (u64)0, MO_RELEASE);
        atomic_store(&w.read, (u64)0, MO_RELEASE);
        atomic_store(&w.end, (u64)0, MO_RELEASE);
        atomic_store(&h.heard_slot, h.wslot, MO_RELEASE);
        h.p0[h.wslot] = front;
        h.pending_epoch = 0;
        h.req_id = 0;
    } else {
        u32 b = h.wslot ^ 1u;  // the heard slot is this render's: the other is free
        if (h.req_id) {
            u32 expect = h.req_id;
            if (atomic_compare_exchange(&h.join_pending, &expect, 0u, false, MO_ACQ_REL, MO_ACQUIRE)) {
                b = h.wslot;   // taken back: its slot is free again, and the heard one is what it was
            } else {
                // The callback took it: a jump is joined at once; an aligned recut's fade
                // may still be reading the old ring, a fade's length at most.
                for (u32 i = 0; i < 400 && atomic_load(&h.joined, MO_ACQUIRE) != h.req_id; ++i) time_sleep(0.0005);
            }
            h.req_id = 0;
        }
        Slot& w = h.slot[b];
        atomic_store(&w.write, (u64)0, MO_RELEASE);
        atomic_store(&w.read, (u64)0, MO_RELEASE);
        atomic_store(&w.end, (u64)0, MO_RELEASE);   // nothing yet, and nothing expected until a source says
        setup_chain(h, b, h.format);
        h.p0[b] = front;
        atomic_store(&h.join_slot, b, MO_RELAXED);
        atomic_store(&h.join_fade, fade ? 1u : 0u, MO_RELAXED);
        atomic_store(&h.join_kind, 0u, MO_RELAXED);
        atomic_store(&h.req_sent_ns, sent_ns, MO_RELAXED);
        h.wslot = b;
        h.pending_epoch = epoch;
        h.req_id = epoch;
        atomic_store(&h.join_pending, epoch, MO_RELEASE);
    }
    atomic_store(&h.flushes_done, epoch, MO_RELEASE);
}

// The aligned recut. The heard render's chain no longer has the parameters wanted: a new
// render in the other slot takes over from the next frame the callback will take, the
// same programme in the same stream frames, the chain run again over the source ring
// (after a run-in from before it), and from the front of the lead on it is the render
// written. The callback crossfades into it when it has its first frames. One recut is in
// flight at a time; a change that comes meanwhile waits for it, and then the newest
// parameters are the next one's.
void start_recut(Hifi& h) {
    const u64 t0 = time_ns();
    const u32 a = h.wslot, b = a ^ 1u;
    Slot& sa = h.slot[a];
    Slot& sb = h.slot[b];
    u64 ra = atomic_load(&sa.read, MO_ACQUIRE), wa = atomic_load(&sa.write, MO_RELAXED);
    if (ra > wa) ra = wa;
    u64 ps = h.p0[a] + ra, pf = h.p0[a] + wa;
    setup_chain(h, b, h.format);
    atomic_store(&sb.read, (u64)0, MO_RELEASE);
    atomic_store(&sb.write, (u64)0, MO_RELEASE);
    atomic_store(&sb.base, atomic_load(&sa.base, MO_ACQUIRE) + ra, MO_RELEASE);
    if (!h.chain[b].flat) {
        // A run-in: the frames before it, through the chain, to settle it; never back past
        // where its source began, or what the source ring still holds.
        u64 back = chain_run_in(h, b);
        u64 floor_p = h.nsegs ? h.segs[segment_of(h, ps)].p : ps;
        if (floor_p < h.p_valid) floor_p = h.p_valid;
        u64 from = ps - (ps - floor_p < back ? ps - floor_p : back);
        if (from < ps) render_frames(h, b, from, ps - from, 0, true);
    }
    // The first chunk, and the request: the callback can join as soon as it has a fade's
    // worth past where it is. The rest of the lead is rendered behind it, each chunk
    // published as it is done; the callback reads nothing past what is.
    u64 total = pf - ps, first = total < 2 * kChunkFrames ? total : 2 * kChunkFrames;
    render_frames(h, b, ps, first, 0);
    u64 ea = atomic_load(&sa.end, MO_RELAXED);
    u64 eb = ea == kNever ? kNever : ea > ra ? ea - ra : 0;
    atomic_store(&sb.end, first == total ? eb : kNever, MO_RELEASE);
    atomic_store(&sb.write, first, MO_RELEASE);
    h.p0[b] = ps;
    h.wslot = b;
    h.req_id = kAligned | (++h.recut_seq & 0x7FFFFFFFu);
    atomic_store(&h.join_slot, b, MO_RELAXED);
    atomic_store(&h.join_kind, 1u, MO_RELAXED);
    atomic_store(&h.req_sent_ns, h.desired_sent_ns, MO_RELAXED);
    atomic_store(&h.join_pending, h.req_id, MO_RELEASE);
    for (u64 done = first; done < total;) {
        u64 m = total - done < kChunkFrames ? total - done : kChunkFrames;
        render_frames(h, b, ps + done, m, done);
        done += m;
        if (done == total) atomic_store(&sb.end, eb, MO_RELEASE);
        atomic_store(&sb.write, done, MO_RELEASE);
    }
    atomic_store(&h.recut_render_ns, time_ns() - t0, MO_RELAXED);
}

bool wants_recut(const Hifi& h) {
    return h.device && !h.draining && !h.req_id && h.chain[h.wslot].gen != h.desired_gen;
}

// Waits (a fade's length at most) for a join request the callback has taken to be done.
void wait_joined(Hifi& h) {
    for (u32 i = 0; i < 400 && h.req_id && atomic_load(&h.joined, MO_ACQUIRE) != h.req_id; ++i) time_sleep(0.0005);
    check_joined(h);
}

// What follows source `id` is to be other sources (the Adds after this command): `more`
// of them. If `id` is still being decoded, nothing after it has been rendered and this
// costs nothing. If its end is already in the lead, the lead is cut there: a new render
// of the same music up to that frame (the source ring, through the chain), and of the
// new sources after it, crossfaded in like any recut; and a cut note voids the marks the
// old render made past it.
void replace_after(Hifi& h, u64 id, u32 more) {
    release_nxt(h);
    HifiSource q;
    while (h.queue.pop(&q)) { note(h, HifiEventKind::Released, q.id); forget_starved(h, q.id); }
    if (h.have_cur && h.cur.src.id == id) return;
    // Where the next source began after it, in what has been rendered.
    u32 k = h.nsegs;
    while (k-- > 0 && h.segs[k].source != id) {}
    if (k >= h.nsegs || k + 1 >= h.nsegs) { // it ended the stream, or is not in the source ring: nothing after it to cut
        release_cur(h);
        return;
    }
    const u64 pb = h.segs[k + 1].p;
    if (h.req_id && !h.pending_epoch) wait_joined(h); // a recut's fade: let it finish
    if (h.pending_epoch) {
        // A jump not joined yet: nothing of it is heard. Take it back, cut its slot at
        // the boundary, and ask again.
        u32 expect = h.req_id;
        if (atomic_compare_exchange(&h.join_pending, &expect, 0u, false, MO_ACQ_REL, MO_ACQUIRE)) {
            release_cur(h);
            h.nsegs = k + 1;
            Slot& w = h.slot[h.wslot];
            u64 at = pb - h.p0[h.wslot];
            if (atomic_load(&w.write, MO_RELAXED) > at) atomic_store(&w.write, at, MO_RELEASE);
            atomic_store(&w.end, more ? kNever : at, MO_RELEASE);
            Note c; c.cut = true; c.event.source = id; c.frame = at; c.relative = true; c.slot = h.wslot; c.epoch = h.render_epoch;
            push_note(h, c);
            atomic_store(&h.join_pending, h.req_id, MO_RELEASE);
            return;
        }
        wait_joined(h);
    }
    const u32 a = h.wslot, b = a ^ 1u;
    Slot& sa = h.slot[a];
    Slot& sb = h.slot[b];
    u64 ra = atomic_load(&sa.read, MO_ACQUIRE);
    u64 ps = h.p0[a] + ra;
    if (pb <= ps) return; // the callback is past the boundary already: what is heard is the next source
    release_cur(h);
    h.nsegs = k + 1;
    setup_chain(h, b, h.format);
    atomic_store(&sb.read, (u64)0, MO_RELEASE);
    atomic_store(&sb.write, (u64)0, MO_RELEASE);
    u64 base = atomic_load(&sa.base, MO_ACQUIRE);
    atomic_store(&sb.base, base + ra, MO_RELEASE);
    if (!h.chain[b].flat) {
        u64 back = chain_run_in(h, b);
        u64 floor_p = h.segs[segment_of(h, ps)].p;
        if (floor_p < h.p_valid) floor_p = h.p_valid;
        u64 from = ps - (ps - floor_p < back ? ps - floor_p : back);
        if (from < ps) render_frames(h, b, from, ps - from, 0, true);
    }
    render_frames(h, b, ps, pb - ps, 0);
    atomic_store(&sb.end, more ? kNever : pb - ps, MO_RELEASE);
    atomic_store(&sb.write, pb - ps, MO_RELEASE);
    h.p0[b] = ps;
    h.wslot = b;
    Note c; c.cut = true; c.event.source = id; c.frame = base + (pb - h.p0[a]); c.epoch = h.render_epoch;
    push_note(h, c);
    h.req_id = kAligned | (++h.recut_seq & 0x7FFFFFFFu);
    atomic_store(&h.join_slot, b, MO_RELAXED);
    atomic_store(&h.join_kind, 1u, MO_RELAXED);
    atomic_store(&h.req_sent_ns, time_ns(), MO_RELAXED);
    atomic_store(&h.join_pending, h.req_id, MO_RELEASE);
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
        } else if (c.kind == CmdKind::Replace) {
            replace_after(h, c.source.id, c.epoch);
        } else if (c.kind == CmdKind::Param) {
            if (c.stage < h.stages.len && c.param < h.stages[c.stage]->params.len && c.param < kHifiMaxParams) {
                h.desired[c.stage][c.param] = c.value;
                h.desired_gen = c.epoch;
                h.desired_sent_ns = c.sent_ns;
            }
        } else if (c.kind == CmdKind::Starve) {
            if (!starved_reason(h, c.source.id) && h.nstarved < kMaxStarved) {
                h.starved[h.nstarved].id = c.source.id;
                format(h.starved[h.nstarved].reason, sizeof h.starved[0].reason, "%s", c.reason);
                ++h.nstarved;
            }
        } else { // Flush or Jump: everything this render has goes, and a new one starts
            start_render(h, c.epoch, c.kind == CmdKind::Jump, c.sent_ns);
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
// or there is no device: the render thread may then wait for a command. A join waiting
// keeps the device going: the callback must take it up.
bool try_hold(Hifi& h) {
    os_mutex_lock(&h.lock);
    bool done = !h.device || h.held;
    if (!done) {
        u32 delay = hifi_device_delay(h.device);
        bool paused = atomic_load(&h.want_pause, MO_RELAXED) != 0;
        const Slot& s = h.slot[atomic_load(&h.heard_slot, MO_ACQUIRE)];
        bool empty  = atomic_load(&s.read, MO_ACQUIRE) == atomic_load(&s.write, MO_ACQUIRE);
        bool joining = atomic_load(&h.join_pending, MO_ACQUIRE) != 0;
        if ((paused || (empty && !joining)) && atomic_load(&h.silent_run, MO_ACQUIRE) >= (delay ? delay : 1)) {
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
    check_joined(h);
    if (wants_recut(h)) start_recut(h);
    Slot& s = h.slot[h.wslot];
    if (!h.have_cur) {
        prepare_next(h);
        if (!h.have_nxt || h.nxt.opening) {
            // Nothing to play. If sources failed just now, the stream ends where it is.
            atomic_store(&s.end, atomic_load(&s.write, MO_RELAXED), MO_RELEASE);
            if (h.have_nxt) { try_hold(h); return kWantNap; } // its bytes are on their way: look again shortly, the device held meanwhile
            if (h.pending_epoch) return try_hold(h) ? -1.0 : kWantNap; // a flush the callback is to take up; held, it will when it is let go
            return -1.0;
        }
        h.cur = h.nxt; h.nxt = Playing{};
        h.have_cur = true; h.have_nxt = false;
        h.cur_frames = 0; h.cur_done = false; h.cur_marked = false;
        h.cur_status = DecodeStatus::End; h.cur_reason[0] = 0;
        h.draining = h.device && !device_fits(h, h.cur.info.format);
    }

    if (h.draining) {
        // The device must change. Wait for what is in the lead to be taken (a jump to be
        // joined first) and for the device to have played it out (its delay's worth of
        // silence since), then close it.
        u64 w = atomic_load(&s.write, MO_RELAXED);
        atomic_store(&s.end, w, MO_RELEASE);
        if (h.pending_epoch) { release_hold(h); return kWantNap; }
        bool empty = atomic_load(&s.read, MO_ACQUIRE) == w;
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

    u64 w = atomic_load(&s.write, MO_RELAXED);
    u32 left = h.scratch_frames - h.scratch_at;
    if (left) {
        u64 used = w - atomic_load(&s.read, MO_ACQUIRE);
        u32 room = h.ring_frames - (u32)used;
        if (room == 0) return atomic_load(&h.want_pause, MO_RELAXED) ? -1.0 : 0.005; // the lead is full
        u32 n = left < room ? left : room;
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
            if (h.have_nxt && h.nxt.opening && w - atomic_load(&s.read, MO_ACQUIRE) > h.start_frames && !atomic_load(&h.want_pause, MO_RELAXED))
                return kWantNap;
        }
        if (!h.cur_marked) {
            mark(h, h.cur.src.seek ? HifiEventKind::Seeked : HifiEventKind::Started, w);
            begin_segment(h, h.p0[h.wslot] + w, h.cur.src.start, h.cur.info.format.bits, h.cur.src.id);
            h.cur_marked = true;
        }
        u64 p = h.p0[h.wslot] + w;
        put_source(h, h.scratch + (usize)h.scratch_at * h.format.channels, p, n);
        render_frames(h, h.wslot, p, n, w);
        bool goes_on = !last || (h.have_nxt && !h.nxt.opening && device_fits(h, h.nxt.info.format));
        atomic_store(&s.end, goes_on ? kNever : w + n, MO_RELEASE);
        atomic_store(&s.write, w + n, MO_RELEASE);
        if (h.device_bits < h.cur.info.format.bits) { os_mutex_lock(&h.lock); h.device_bits = h.cur.info.format.bits; os_mutex_unlock(&h.lock); }
        h.scratch_at += n;
        w += n;
    }
    if (h.cur_done && h.scratch_at == h.scratch_frames) {
        if (!h.cur_marked) { // a source with no frames at all
            mark(h, h.cur.src.seek ? HifiEventKind::Seeked : HifiEventKind::Started, w);
            begin_segment(h, h.p0[h.wslot] + w, h.cur.src.start, h.cur.info.format.bits, h.cur.src.id);
            h.cur_marked = true;
        }
        mark(h, HifiEventKind::Ended, w, h.cur_reason);
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
    h->fbuf = (f64*)mem_alloc((usize)kChunkFrames * 8 * sizeof(f64));
    h->stages = config.stages;
    if (h->stages.len > kHifiMaxStages) h->stages.len = kHifiMaxStages;
    for (u32 i = 0; i < h->stages.len; ++i)
        for (u32 j = 0; j < h->stages[i]->params.len && j < kHifiMaxParams; ++j) h->desired[i][j] = h->stages[i]->params[j].initial;
    if (!os_thread_start(&h->thread, render_thread, h)) {
        os_cond_destroy(&h->wake); os_mutex_destroy(&h->lock);
        mem_free(h->scratch); mem_free(h->fbuf); mem_free(h);
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
    for (Slot& s : h->slot) mem_free(s.data);
    for (Chain& ch : h->chain) for (u8* m : ch.mem) mem_free(m);
    mem_free(h->src);
    mem_free(h->fbuf);
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

// A flush and a jump, from the caller: the marks not yet reached never will be, and
// nothing is being heard until the new render's first mark is.
void begin_render(Hifi* h, CmdKind kind, const HifiSource* sources, u32 n) {
    ++h->epoch;
    h->pending.count = 0;
    h->cur_source = 0;
    os_mutex_lock(&h->lock);
    Cmd c; c.kind = kind; c.epoch = h->epoch; c.sent_ns = time_ns();
    bool ok = h->cmds.push(c);
    for (u32 i = 0; i < n && ok; ++i) { Cmd a; a.kind = CmdKind::Add; a.source = sources[i]; ok = h->cmds.push(a); }
    if (!ok) KV_ERROR("hifi", "the command list is full: a command is lost");
    os_cond_signal(&h->wake);
    os_mutex_unlock(&h->lock);
}

void hifi_flush(Hifi* h) { begin_render(h, CmdKind::Flush, nullptr, 0); }

void hifi_replace_after(Hifi* h, u64 id, const HifiSource* sources, u32 n) {
    os_mutex_lock(&h->lock);
    Cmd c; c.kind = CmdKind::Replace; c.source.id = id; c.epoch = n;
    bool ok = h->cmds.push(c);
    for (u32 i = 0; i < n && ok; ++i) { Cmd a; a.kind = CmdKind::Add; a.source = sources[i]; ok = h->cmds.push(a); }
    if (!ok) KV_ERROR("hifi", "the command list is full: a command is lost");
    os_cond_signal(&h->wake);
    os_mutex_unlock(&h->lock);
}

void hifi_jump(Hifi* h, const HifiSource* sources, u32 n) { begin_render(h, CmdKind::Jump, sources, n); }

u32 hifi_set_param(Hifi* h, u32 stage, u32 param, f64 value) {
    Cmd c; c.kind = CmdKind::Param; c.stage = stage; c.param = param; c.value = value;
    c.epoch = ++h->param_gen;
    c.sent_ns = time_ns();
    send(*h, c);
    return c.epoch;
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
        bool mark = x.event.kind == HifiEventKind::Started || x.event.kind == HifiEventKind::Seeked || x.event.kind == HifiEventKind::Ended;
        if (x.cut) {
            // The programme past this frame was replaced: its marks never will be, but
            // the source it follows ends there still.
            if (x.epoch == h->epoch)
                for (u32 i = 0; i < h->pending.count;) {
                    Note& m = h->pending.at(i);
                    if (m.relative && !x.relative && atomic_load(&h->jump_joined, MO_ACQUIRE) == m.epoch) { // placed now, to compare like with like
                        m.frame += atomic_load(&h->jump_base, MO_RELAXED);
                        m.relative = false;
                    }
                    if (m.relative == x.relative && m.frame >= x.frame && m.event.source != x.event.source) h->pending.remove(i); else ++i;
                }
        } else if (mark) {
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
        if (x.relative) {
            // Placed once its render was joined. A recut joined since does not move the
            // jump's place; a later jump would have dropped the mark with its epoch.
            if (atomic_load(&h->jump_joined, MO_ACQUIRE) != x.epoch) break; // its place in the stream is not known yet
            x.frame += atomic_load(&h->jump_base, MO_RELAXED);
            x.relative = false;
        }
        bool start = x.event.kind != HifiEventKind::Ended;
        bool reached = start ? h->heard > x.frame : h->heard >= x.frame;
        if (!reached) break;
        if (start) { h->cur_source = x.event.source; h->cur_frame = x.frame; h->cur_first = x.event.frames; }
        else if (h->cur_source == x.event.source) h->cur_source = 0;
        out[n++] = x.event;
        h->pending.remove(0);
    }

    // The parameters heard: the heard ring's, once its first frame has reached the ear.
    const Slot& hs = h->slot[atomic_load(&h->heard_slot, MO_ACQUIRE)];
    u32 gen = atomic_load(&hs.gen, MO_ACQUIRE);
    if (gen != h->gen_said && h->heard >= atomic_load(&hs.heard_from, MO_ACQUIRE) && n < cap) {
        HifiEvent e; e.kind = HifiEventKind::ParamsHeard; e.frames = gen;
        h->gen_said = gen;
        out[n++] = e;
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
    s.joining = atomic_load(&h->join_pending, MO_ACQUIRE) != 0 || atomic_load(&h->xfading, MO_ACQUIRE) != 0;
    s.taken   = atomic_load(&h->read, MO_ACQUIRE);
    const Slot& hs = h->slot[atomic_load(&h->heard_slot, MO_ACQUIRE)];
    s.written = atomic_load(&hs.base, MO_ACQUIRE) + atomic_load(&hs.write, MO_ACQUIRE);
    // The lead that will be heard: a jump's, while it waits to be joined (what is left of
    // the heard one is on its way out).
    const Slot& ls = s.joining ? h->slot[atomic_load(&h->join_slot, MO_RELAXED)] : hs;
    u64 lw = atomic_load(&ls.write, MO_ACQUIRE), lr = atomic_load(&ls.read, MO_ACQUIRE);
    s.lead_frames = lw > lr ? (u32)(lw - lr) : 0;
    s.underruns = atomic_load(&h->underruns, MO_RELAXED);
    s.flat = atomic_load(&hs.flat, MO_ACQUIRE) != 0;
    s.params_heard = h->gen_said;
    s.recuts = atomic_load(&h->recuts, MO_RELAXED);
    u64 jn = atomic_load(&h->join_ns, MO_RELAXED), sn = atomic_load(&h->joined_sent_ns, MO_RELAXED);
    s.recut_seconds = jn > sn && sn ? (f64)(jn - sn) * 1e-9 : 0.0;
    u64 jj = atomic_load(&h->jump_ns, MO_RELAXED), js = atomic_load(&h->jump_sent_ns, MO_RELAXED);
    s.jump_seconds = jj > js && js ? (f64)(jj - js) * 1e-9 : 0.0;
    s.recut_render_seconds = (f64)atomic_load(&h->recut_render_ns, MO_RELAXED) * 1e-9;
    s.exact = s.exact && s.flat && !s.joining;
    s.heard    = h->heard;
    s.source   = h->cur_source;
    s.position = h->cur_source && h->heard > h->cur_frame ? h->cur_first + (h->heard - h->cur_frame) : 0;
    *out = s;
}

} // namespace kv
