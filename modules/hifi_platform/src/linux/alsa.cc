// Alone on Linux: the kernel's PCM device (/dev/snd/pcmC<card>D<device>p), spoken to
// through its own interface (sound/asound.h's ioctls) with no library under it, as dbus
// is: alsa-lib is LGPL, and Blackwing's third-party code is permissive. PipeWire is asked
// to let go of the card first (reserve.cc), and has it back on close.
//
// The format is chosen for the file (BR22): its rate if the device takes it, else the
// device's nearest (hifi resamples); the narrowest word that holds its depth, else the
// widest the device has, its frames padded. Packed 24-bit (S24_3LE, common on USB DACs)
// is played from hifi's S24 by packing each sample on the way.
//
// A thread of its own writes a period at a time when the kernel has room, calling hifi's
// callback for it; it is marked realtime, and it neither allocates nor locks. An underrun
// is prepared and played on; the card unplugged, or another program asking for it, is lost.
#include "../backend.h"
#include "reserve.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_error.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <new>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

namespace kv {

namespace {

constexpr u32 F_S16 = (u32)(int)SNDRV_PCM_FORMAT_S16_LE;
constexpr u32 F_S24 = (u32)(int)SNDRV_PCM_FORMAT_S24_LE;   // 24 bits in the low three bytes of four
constexpr u32 F_S24_3 = (u32)(int)SNDRV_PCM_FORMAT_S24_3LE; // 24 bits in three bytes
constexpr u32 F_S32 = (u32)(int)SNDRV_PCM_FORMAT_S32_LE;

snd_mask* mask_of(snd_pcm_hw_params* p, int param) { return &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK]; }
snd_interval* interval_of(snd_pcm_hw_params* p, int param) { return &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]; }

void mask_one(snd_mask* m, u32 v) { memset(m->bits, 0, sizeof m->bits); m->bits[v >> 5] |= 1u << (v & 31); }
bool mask_has(const snd_mask* m, u32 v) { return (m->bits[v >> 5] >> (v & 31)) & 1u; }

// Everything any device could do: what HW_REFINE narrows.
void hw_any(snd_pcm_hw_params* p) {
    memset(p, 0, sizeof *p);
    for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++k) memset(mask_of(p, k)->bits, 0xff, sizeof mask_of(p, k)->bits);
    for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++k) {
        snd_interval* i = interval_of(p, k);
        i->min = 0;
        i->max = UINT_MAX;
    }
    p->rmask = ~0u;
    p->info = ~0u;
}

void set_range(snd_pcm_hw_params* p, int param, u32 lo, u32 hi) {
    snd_interval* i = interval_of(p, param);
    i->min = lo;
    i->max = hi;
    i->openmin = i->openmax = 0;
    i->integer = 1;
    i->empty = 0;
}

bool refine(int fd, snd_pcm_hw_params* p) {
    p->rmask = ~0u;
    return ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, p) == 0;
}

// The words to try for a file of `bits`, best first: the narrowest that holds it, wider
// ones padded, and narrower ones (hifi dithers down) last.
u32 word_order(u32 bits, u32* out) {
    if (bits <= 16) { out[0] = F_S16; out[1] = F_S24; out[2] = F_S24_3; out[3] = F_S32; return 4; }
    if (bits <= 24) { out[0] = F_S24; out[1] = F_S24_3; out[2] = F_S32; out[3] = F_S16; return 4; }
    out[0] = F_S32; out[1] = F_S24; out[2] = F_S24_3; out[3] = F_S16;
    return 4;
}

HifiSampleFormat hifi_word(u32 f) { return f == F_S16 ? HifiSampleFormat::S16 : f == F_S32 ? HifiSampleFormat::S32 : HifiSampleFormat::S24; }
const char* word_name(u32 f) { return f == F_S16 ? "S16_LE" : f == F_S24 ? "S24_LE" : f == F_S24_3 ? "S24_3LE" : f == F_S32 ? "S32_LE" : "?"; }

struct Alsa {
    int   fd = -1;
    Reserve* reserve = nullptr;
    HifiDeviceCallback callback = nullptr;
    void* user = nullptr;
    HifiDeviceFormat format;      // what hifi gives
    u32   word = 0;               // what the kernel takes
    u32   period = 0, buffer = 0; // frames
    u8*   staging = nullptr;      // a period in hifi's format
    u8*   packed = nullptr;       // a period packed, for S24_3LE
    OsThread thread;
    bool  started = false;
    u32   quit = 0;               // atomic
    u32   hold = 0;               // atomic: asked to hold
    u32   held = 0;               // atomic: the thread has stopped the stream
    u32   lost_set = 0;           // atomic: `lost` is written
    char  lost[96] = "";
    u64   xruns = 0;              // atomic
    i64   last_delay = 0;         // atomic: the delay when it went
    char  path[48] = "";
};

void alsa_lose(Alsa& a, const char* why) {
    if (atomic_load(&a.lost_set, MO_ACQUIRE)) return;
    snd_pcm_sframes_t d = 0;
    if (ioctl(a.fd, SNDRV_PCM_IOCTL_DELAY, &d) == 0) atomic_store(&a.last_delay, (i64)d, MO_RELAXED);
    format(a.lost, sizeof a.lost, "%s", why);
    atomic_store(&a.lost_set, 1u, MO_RELEASE);
}

// One period into the kernel's ring, through hifi's callback. False when the device has
// gone.
bool write_period(Alsa& a) {
    a.callback(a.user, a.staging, a.period);
    const u8* buf = a.staging;
    const u32 ch = a.format.channels;
    if (a.word == F_S24_3) {
        const i32* in = (const i32*)a.staging;
        u8* out = a.packed;
        for (u32 i = 0; i < a.period * ch; ++i) { i32 v = in[i]; out[0] = (u8)v; out[1] = (u8)(v >> 8); out[2] = (u8)(v >> 16); out += 3; }
        buf = a.packed;
    }
    const u32 fb = a.word == F_S16 ? 2 * ch : a.word == F_S24_3 ? 3 * ch : 4 * ch;
    u32 done = 0;
    while (done < a.period && !atomic_load(&a.quit, MO_ACQUIRE)) {
        snd_xferi x{};
        x.buf = (void*)(buf + (usize)done * fb);
        x.frames = a.period - done;
        if (ioctl(a.fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) { done += (u32)x.result; continue; }
        int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN) { struct pollfd p = {a.fd, POLLOUT, 0}; poll(&p, 1, 50); continue; }
        if (e == EPIPE) { atomic_add_fetch(&a.xruns, (u64)1, MO_RELAXED); ioctl(a.fd, SNDRV_PCM_IOCTL_PREPARE); continue; } // an underrun: played on
        if (e == ESTRPIPE) { // suspended: resumed, else prepared again
            for (u32 k = 0; k < 100 && ioctl(a.fd, SNDRV_PCM_IOCTL_RESUME) != 0 && errno == EAGAIN; ++k) time_sleep(0.01);
            ioctl(a.fd, SNDRV_PCM_IOCTL_PREPARE);
            continue;
        }
        alsa_lose(a, e == ENODEV || e == EBADFD ? "the device went (unplugged)" : "the device stopped taking audio");
        return false;
    }
    return true;
}

void alsa_run(void* arg) {
    Alsa& a = *(Alsa*)arg;
    os_thread_enter_audio_class();
    while (!atomic_load(&a.quit, MO_ACQUIRE)) {
        if (reserve_asked(a.reserve)) { alsa_lose(a, "another program asked for the device"); break; }
        if (atomic_load(&a.hold, MO_ACQUIRE)) {
            if (!atomic_load(&a.held, MO_RELAXED)) { ioctl(a.fd, SNDRV_PCM_IOCTL_DROP); atomic_store(&a.held, 1u, MO_RELEASE); }
            time_sleep(0.02);
            continue;
        }
        if (atomic_load(&a.held, MO_RELAXED)) { ioctl(a.fd, SNDRV_PCM_IOCTL_PREPARE); atomic_store(&a.held, 0u, MO_RELEASE); }
        struct pollfd p = {a.fd, POLLOUT, 0};
        int r = poll(&p, 1, 100);
        if (r < 0 && errno != EINTR) { alsa_lose(a, "the device could not be waited on"); break; }
        if (r <= 0) continue;
        if (p.revents & (POLLERR | POLLNVAL | POLLHUP)) {
            // An underrun shows as POLLERR: the write finds out which it is.
        }
        if (!write_period(a)) break;
    }
}

void* alsa_fail(Alsa* a, char* why, u32 cap, const char* text) {
    format(why, cap, "%s", text);
    if (a->fd >= 0) close(a->fd);
    reserve_give(a->reserve);
    mem_free(a->staging);
    mem_free(a->packed);
    a->~Alsa();
    mem_free(a);
    return nullptr;
}

// Rates to try for a file at `rate`, after its own: its family first (the same clock),
// then the rest.
u32 rate_order(u32 rate, u32* out) {
    static const u32 k44[] = {88200, 176400, 44100, 352800, 48000, 96000, 192000, 384000};
    static const u32 k48[] = {96000, 192000, 48000, 384000, 44100, 88200, 176400, 352800};
    const u32* fam = rate % 11025 == 0 ? k44 : k48;
    u32 n = 0;
    out[n++] = rate;
    for (u32 i = 0; i < 8; ++i) if (fam[i] != rate) out[n++] = fam[i];
    return n;
}

} // namespace

void* alsa_open_alone(i32 card, i32 device, const HifiDeviceDesc& desc, HifiDeviceOpened& out, char* why, u32 cap) {
    Alsa* a = (Alsa*)mem_alloc(sizeof(Alsa));
    new (a) Alsa();
    a->callback = desc.callback;
    a->user = desc.user;
    format(a->path, sizeof a->path, "/dev/snd/pcmC%dD%dp", card, device);
    char rwhy[160] = "";
    a->reserve = reserve_take(card, nullptr, 2.0, rwhy, sizeof rwhy);
    // Without a reservation (no session bus), the card may still be free: tried all the same.
    // PipeWire closes the card when it lets go; that may take a moment.
    for (u32 k = 0; k < 50; ++k) {
        a->fd = ::open(a->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (a->fd >= 0 || errno != EBUSY) break;
        time_sleep(0.02);
    }
    if (a->fd < 0) {
        char text[160];
        if (errno == EBUSY) format(text, sizeof text, "another program has the device%s%s", rwhy[0] ? ": " : "", rwhy);
        else format(text, sizeof text, "%s: %s", a->path, os_error_text(errno));
        return alsa_fail(a, why, cap, text);
    }

    snd_pcm_hw_params p;
    hw_any(&p);
    mask_one(mask_of(&p, SNDRV_PCM_HW_PARAM_ACCESS), (u32)SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    if (!refine(a->fd, &p)) return alsa_fail(a, why, cap, "the device does not take interleaved frames");
    // The word.
    u32 order[4];
    u32 bits = hifi_sample_bits(desc.want.sample);
    u32 nw = word_order(bits, order);
    a->word = 0;
    for (u32 i = 0; i < nw && !a->word; ++i)
        if (mask_has(mask_of(&p, SNDRV_PCM_HW_PARAM_FORMAT), order[i])) a->word = order[i];
    if (!a->word) return alsa_fail(a, why, cap, "the device takes no integer word Blackwing writes");
    mask_one(mask_of(&p, SNDRV_PCM_HW_PARAM_FORMAT), a->word);
    // The channels: as the file's, or not at all.
    set_range(&p, SNDRV_PCM_HW_PARAM_CHANNELS, desc.want.channels, desc.want.channels);
    if (!refine(a->fd, &p)) {
        char text[96];
        format(text, sizeof text, "the device does not take %u channels", desc.want.channels);
        return alsa_fail(a, why, cap, text);
    }
    // The rate: the file's, else the nearest of its family, else any.
    u32 rates[9];
    u32 nr = rate_order(desc.want.rate, rates);
    bool rate_ok = false;
    for (u32 i = 0; i < nr && !rate_ok; ++i) {
        snd_pcm_hw_params q = p;
        set_range(&q, SNDRV_PCM_HW_PARAM_RATE, rates[i], rates[i]);
        if (refine(a->fd, &q)) { p = q; rate_ok = true; }
    }
    if (!rate_ok) {
        snd_interval* ri = interval_of(&p, SNDRV_PCM_HW_PARAM_RATE);
        set_range(&p, SNDRV_PCM_HW_PARAM_RATE, ri->min, ri->min);
        if (!refine(a->fd, &p)) return alsa_fail(a, why, cap, "the device takes no rate");
    }
    // About 10 ms periods and 40 to 200 ms of buffer: hifi keeps seconds of lead of its
    // own, so this only has to ride out the thread's scheduling.
    {
        snd_pcm_hw_params q = p;
        set_range(&q, SNDRV_PCM_HW_PARAM_PERIOD_TIME, 5000, 20000);
        set_range(&q, SNDRV_PCM_HW_PARAM_BUFFER_TIME, 40000, 200000);
        if (refine(a->fd, &q)) p = q;
    }
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) != 0) {
        char text[96];
        format(text, sizeof text, "the device would not be set up: %s", os_error_text(errno));
        return alsa_fail(a, why, cap, text);
    }
    a->format.rate = interval_of(&p, SNDRV_PCM_HW_PARAM_RATE)->min;
    a->format.channels = desc.want.channels;
    a->format.sample = hifi_word(a->word);
    a->period = interval_of(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    a->buffer = interval_of(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    if (!a->period || !a->buffer) return alsa_fail(a, why, cap, "the device gave no period");

    snd_pcm_sw_params sw{};
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = a->period;
    sw.xfer_align = 1;
    sw.start_threshold = a->buffer; // starts once it is full
    sw.stop_threshold = a->buffer;  // stops on an underrun
    snd_pcm_uframes_t boundary = a->buffer;
    while (boundary * 2 <= (snd_pcm_uframes_t)LONG_MAX - a->buffer) boundary *= 2;
    sw.boundary = boundary;
    sw.proto = SNDRV_PCM_VERSION;
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) != 0) return alsa_fail(a, why, cap, "the device's software parameters were refused");
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) != 0) return alsa_fail(a, why, cap, "the device would not be prepared");

    a->staging = (u8*)mem_alloc((usize)a->period * hifi_frame_bytes(a->format));
    a->packed = (u8*)mem_alloc((usize)a->period * a->format.channels * 3);
    out.format = a->format;
    out.shared = false;
    KV_INFO("hifi", "alone on %s: %u Hz, %u ch, %s (asked %u Hz, %u bits); %u-frame periods, %u buffered", a->path, a->format.rate, a->format.channels,
            word_name(a->word), desc.want.rate, bits, a->period, a->buffer);
    return a;
}

namespace {

void alsa_start(void* impl) {
    Alsa& a = *(Alsa*)impl;
    if (a.started) return;
    a.started = os_thread_start(&a.thread, alsa_run, &a);
    if (!a.started) alsa_lose(a, "no thread for the device");
}

u32 alsa_delay(void* impl) {
    Alsa& a = *(Alsa*)impl;
    if (atomic_load(&a.lost_set, MO_ACQUIRE)) return (u32)atomic_load(&a.last_delay, MO_RELAXED);
    if (atomic_load(&a.held, MO_ACQUIRE)) return 0;
    snd_pcm_sframes_t d = 0;
    if (ioctl(a.fd, SNDRV_PCM_IOCTL_DELAY, &d) != 0 || d < 0) return 0;
    return (u32)d;
}

void alsa_hold(void* impl, bool hold) { atomic_store(&((Alsa*)impl)->hold, hold ? 1u : 0u, MO_RELEASE); }

void alsa_close(void* impl) {
    Alsa* a = (Alsa*)impl;
    atomic_store(&a->quit, 1u, MO_RELEASE);
    if (a->started) os_thread_join(&a->thread);
    ioctl(a->fd, SNDRV_PCM_IOCTL_DROP);
    ioctl(a->fd, SNDRV_PCM_IOCTL_HW_FREE);
    close(a->fd);
    a->fd = -1;
    u64 x = atomic_load(&a->xruns, MO_RELAXED);
    if (x) KV_INFO("hifi", "%s: %llu underruns while it was alone", a->path, (unsigned long long)x);
    reserve_give(a->reserve); // PipeWire has the card back
    mem_free(a->staging);
    mem_free(a->packed);
    a->~Alsa();
    mem_free(a);
}

const char* alsa_lost(void* impl) {
    Alsa& a = *(Alsa*)impl;
    return atomic_load(&a.lost_set, MO_ACQUIRE) ? a.lost : nullptr;
}

bool alsa_has(const char*) { return false; } // opened only through another backend's alone_target

void* alsa_open(const HifiDeviceDesc&, HifiDeviceOpened& out) { out.reason = "the kernel's device is opened alone, through its sink"; return nullptr; }

} // namespace

constinit const HifiBackend k_alsa_backend = {
    .name = "alsa", .has = alsa_has, .open = alsa_open, .start = alsa_start, .delay = alsa_delay, .hold = alsa_hold,
    .close = alsa_close, .lost = alsa_lost, .list = nullptr, .alone_target = nullptr,
};

} // namespace kv
