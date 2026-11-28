// Alone on a real card, through the kernel's PCM device: run only when asked, since it
// takes the card from PipeWire (and from anything else playing on it) for a few seconds.
//
//   BLACKWING_ALONE_SINK=alsa_output.usb-... hifi_platform_tests
//
// It plays silence, so nothing is heard. The card's reservation changes hands and comes
// back; frames go at the format granted; a hold stops them; another program asking for
// the card is a loss.
#include "korvid/hifi_platform/device.h"
#include "korvid/dbus/dbus.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <stdlib.h>
#include <string.h>

using namespace kv;

namespace {

struct Quiet { u64 frames = 0; u32 bytes = 0; };
void quiet(void* user, void* out, u32 frames) {
    Quiet& q = *(Quiet*)user;
    memset(out, 0, (usize)frames * q.bytes);
    atomic_add_fetch(&q.frames, (u64)frames, MO_RELAXED);
}

// The reservation's owner, as a unique name; "" when nobody has it.
void owner_of(DbusConnection* c, const char* name, char* out, u32 cap) {
    out[0] = 0;
    DbusHeader h;
    h.destination = "org.freedesktop.DBus";
    h.path = "/org/freedesktop/DBus";
    h.interface = "org.freedesktop.DBus";
    h.member = "GetNameOwner";
    DbusWriter w;
    w.begin("s"); w.str(name);
    DbusMessage r;
    if (dbus_call(c, h, &w, 2.0, &r) && r.kind == DbusKind::Return) {
        DbusReader rd = r.reader();
        const char* s = "";
        if (rd.str(&s)) format(out, cap, "%s", s);
    }
    dbus_message_free(&r);
    w.free();
}

} // namespace

void test_alone(TestCtx& c) {
    const char* sink = getenv("BLACKWING_ALONE_SINK");
    if (!sink || !sink[0]) return; // not asked: it would take a real card
    c.begin("hifi_platform alone, on a real card");
    HifiDeviceInfo list[kHifiMaxDevices];
    u32 n = hifi_devices(list, kHifiMaxDevices);
    const HifiDeviceInfo* d = nullptr;
    for (u32 i = 0; i < n; ++i) if (strcmp(list[i].name, sink) == 0) d = &list[i];
    KV_CHECK(c, d && d->can_exclusive && d->alsa_card >= 0);
    if (!d) return;
    char rname[64], holder[96], during[96], after[96];
    format(rname, sizeof rname, "org.freedesktop.ReserveDevice1.Audio%d", d->alsa_card);
    char why[160];
    DbusConnection* bus = dbus_open(DbusBus::Session, 2.0, why, sizeof why);
    KV_CHECK(c, bus != nullptr);
    if (!bus) return;
    owner_of(bus, rname, holder, sizeof holder);

    // 16 bits at 44.1 kHz: the device's nearest if it has not that rate.
    Quiet q;
    HifiDeviceDesc dd;
    dd.name = sink;
    dd.alone = true;
    dd.want = {44100, 2, HifiSampleFormat::S16};
    dd.callback = quiet;
    dd.user = &q;
    HifiDeviceOpened o = hifi_device_open(dd);
    KV_CHECK(c, o.device && o.alone && !o.shared && o.format.channels == 2);
    if (!o.device) { KV_WARN("test", "alone refused: %s / %s", o.reason, o.alone_why); dbus_close(bus); return; }
    q.bytes = hifi_frame_bytes(o.format);
    owner_of(bus, rname, during, sizeof during);
    KV_CHECK(c, during[0] && strcmp(during, holder) != 0); // ours now
    f64 t0 = time_now();
    hifi_device_start(o.device);
    time_sleep(1.5);
    u64 got = atomic_load(&q.frames, MO_RELAXED);
    f64 took = time_now() - t0;
    u32 delay = hifi_device_delay(o.device);
    KV_INFO("test", "alone: %u Hz %s, %llu frames in %.2f s (%.0f a second), delay %u frames", o.format.rate, hifi_sample_format_name(o.format.sample),
            (unsigned long long)got, took, (f64)got / took, delay);
    KV_CHECK(c, (f64)got > 1.2 * o.format.rate && (f64)got < 1.8 * o.format.rate && delay > 0 && delay < o.format.rate / 2);
    KV_CHECK(c, hifi_device_lost(o.device) == nullptr);
    // Held: no frames asked for; let go: they go on.
    hifi_device_hold(o.device, true);
    time_sleep(0.15);
    u64 h0 = atomic_load(&q.frames, MO_RELAXED);
    time_sleep(0.3);
    KV_CHECK(c, atomic_load(&q.frames, MO_RELAXED) == h0);
    hifi_device_hold(o.device, false);
    time_sleep(0.4);
    KV_CHECK(c, atomic_load(&q.frames, MO_RELAXED) > h0);
    // Another program asks for the card: told yes, and the device is lost.
    DbusHeader rq;
    rq.destination = rname;
    format(why, sizeof why, "/org/freedesktop/ReserveDevice1/Audio%d", d->alsa_card);
    rq.path = why;
    rq.interface = "org.freedesktop.ReserveDevice1";
    rq.member = "RequestRelease";
    DbusWriter pr;
    pr.begin("i"); pr.i32(100);
    DbusMessage ans;
    bool yes = false;
    if (dbus_call(bus, rq, &pr, 2.0, &ans) && ans.kind == DbusKind::Return) { DbusReader rd = ans.reader(); rd.boolean(&yes); }
    dbus_message_free(&ans);
    pr.free();
    f64 asked = time_now();
    while (!hifi_device_lost(o.device) && time_now() - asked < 2.0) time_sleep(0.005);
    KV_CHECK(c, yes && hifi_device_lost(o.device) != nullptr);
    if (hifi_device_lost(o.device)) KV_INFO("test", "alone: lost %.1f ms after another program asked: %s", (time_now() - asked) * 1e3, hifi_device_lost(o.device));
    hifi_device_close(o.device);
    // Given back: WirePlumber's again.
    f64 end = time_now() + 3.0;
    do { owner_of(bus, rname, after, sizeof after); if (after[0] && strcmp(after, during) != 0) break; time_sleep(0.05); } while (time_now() < end);
    KV_CHECK(c, after[0] && strcmp(after, holder) == 0);

    // 24 bits: the word that holds it, packed where the device packs it.
    Quiet q24;
    dd.want = {48000, 2, HifiSampleFormat::S24};
    dd.user = &q24;
    HifiDeviceOpened o24 = hifi_device_open(dd);
    KV_CHECK(c, o24.device && o24.alone && hifi_sample_bits(o24.format.sample) >= 24);
    if (!o24.alone) KV_WARN("test", "24 bits, alone refused: %s%s", o24.alone_why, o24.device ? "" : o24.reason);
    if (o24.device) {
        q24.bytes = hifi_frame_bytes(o24.format);
        hifi_device_start(o24.device);
        time_sleep(0.5);
        KV_CHECK(c, atomic_load(&q24.frames, MO_RELAXED) > o24.format.rate / 4 && !hifi_device_lost(o24.device));
        hifi_device_close(o24.device);
    }
    end = time_now() + 3.0;
    do { owner_of(bus, rname, after, sizeof after); if (strcmp(after, holder) == 0) break; time_sleep(0.05); } while (time_now() < end);
    KV_CHECK(c, strcmp(after, holder) == 0);
    dbus_close(bus);
}
