// The device reservation (org.freedesktop.ReserveDevice1), which PipeWire's WirePlumber
// honours: a program that wants a sound card for itself owns the name
// org.freedesktop.ReserveDevice1.Audio<card> on the session bus, asking the owner first to
// let go (RequestRelease); WirePlumber closes the card when it does, and opens it again when
// the name is free. Blackwing lets go the same way to a program of a higher priority: it
// answers yes at once, and the device is reported lost, which the player takes as a pull.
//
// On a thread of its own, asleep on the bus: the device's thread is realtime and does
// nothing that allocates.
#include "reserve.h"
#include "korvid/dbus/dbus.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include <new>
#include <string.h>

namespace kv {

namespace {

constexpr i32 kPriority = 0; // WirePlumber's own: it lets go to us, and we keep the card when it asks back
const char* const kIface = "org.freedesktop.ReserveDevice1";

const DbusArg kReleaseArgs[] = {{"priority", "i"}, {"result", "b"}};
const DbusMethodDesc kMethods[] = {{"RequestRelease", "i", "b", kReleaseArgs, 2}};
const DbusPropertyDesc kProps[] = {{"Priority", "i"}, {"ApplicationName", "s"}, {"ApplicationDeviceName", "s"}};
const DbusInterfaceDesc kReserveIface = {kIface, kMethods, 1, kProps, 3};

} // namespace

struct Reserve {
    DbusConnection* c = nullptr;
    char     name[64] = "";
    char     path[64] = "";
    OsThread thread;
    OsEvent  event;
    u32      stop = 0;   // atomic
    u32      asked = 0;  // atomic: another program asked for the card, and was told yes
};

namespace {

void reserve_wake(void* user) { os_event_set(&((Reserve*)user)->event); }

void reserve_run(void* arg) {
    Reserve& r = *(Reserve*)arg;
    while (!atomic_load(&r.stop, MO_ACQUIRE)) {
        os_event_wait(&r.event, 3600.0);
        DbusMessage m;
        while (dbus_next(r.c, &m)) {
            if (m.kind == DbusKind::Call && strcmp(m.member, "RequestRelease") == 0) {
                // Let go to a program of a higher priority, as the protocol has it; not to
                // one of the same or lower, which is WirePlumber asking for the card back
                // the moment it has let it go.
                i32 priority = 0;
                DbusReader rd = m.reader();
                rd.i32(&priority);
                bool yes = priority > kPriority;
                DbusWriter w;
                w.begin("b");
                w.boolean(yes);
                dbus_reply(r.c, m, &w);
                w.free();
                if (yes) atomic_store(&r.asked, 1u, MO_RELEASE); // the player pauses as on a pull
            } else if (m.kind == DbusKind::Signal && strcmp(m.member, "NameLost") == 0) {
                DbusReader rd = m.reader();
                const char* lost = "";
                rd.str(&lost);
                if (strcmp(lost, r.name) == 0) atomic_store(&r.asked, 1u, MO_RELEASE); // taken from us without asking
            }
            dbus_message_free(&m);
        }
    }
}

} // namespace

Reserve* reserve_take(i32 card, const char* address, f64 seconds, char* why, u32 cap) {
    char w[200] = "";
    DbusConnection* c = address ? dbus_open_address(address, seconds, w, sizeof w) : dbus_open(DbusBus::Session, seconds, w, sizeof w);
    if (!c) { format(why, cap, "no session bus to reserve the card on (%.60s)", w); return nullptr; }
    Reserve* r = (Reserve*)mem_alloc(sizeof(Reserve));
    new (r) Reserve();
    r->c = c;
    format(r->name, sizeof r->name, "org.freedesktop.ReserveDevice1.Audio%d", card);
    format(r->path, sizeof r->path, "/org/freedesktop/ReserveDevice1/Audio%d", card);
    const DbusInterfaceDesc* ifaces[] = {&kReserveIface};
    dbus_export(c, r->path, ifaces, 1);
    DbusWriter v;
    v.begin("i"); v.i32(kPriority);
    dbus_property_set(c, r->path, kIface, "Priority", v, true);
    v.begin("s"); v.str("Blackwing");
    dbus_property_set(c, r->path, kIface, "ApplicationName", v, true);
    char dev[32];
    format(dev, sizeof dev, "hw:%d", card);
    v.begin("s"); v.str(dev);
    dbus_property_set(c, r->path, kIface, "ApplicationDeviceName", v, true);
    v.free();

    const u32 flags = kDbusNameDoNotQueue | kDbusNameAllowReplacement;
    DbusNameReply got = dbus_request_name(c, r->name, flags, seconds);
    if (got == DbusNameReply::Exists) {
        // Its holder is asked to let go, at our priority.
        DbusHeader h;
        h.destination = r->name;
        h.path = r->path;
        h.interface = kIface;
        h.member = "RequestRelease";
        DbusWriter p;
        p.begin("i");
        p.i32(kPriority);
        DbusMessage reply;
        char cw[96] = "";
        bool yes = false;
        if (dbus_call(c, h, &p, seconds, &reply, cw, sizeof cw)) {
            DbusReader rd = reply.reader();
            yes = reply.kind == DbusKind::Return && rd.boolean(&yes) && yes;
            if (reply.kind == DbusKind::Error) format(cw, sizeof cw, "%s", reply.error_text());
            dbus_message_free(&reply);
        }
        p.free();
        if (!yes) {
            format(why, cap, "the card's holder would not let it go%s%s", cw[0] ? ": " : "", cw);
            dbus_close(c);
            r->~Reserve();
            mem_free(r);
            return nullptr;
        }
        got = dbus_request_name(c, r->name, flags | kDbusNameReplaceExisting, seconds);
    }
    if (got != DbusNameReply::PrimaryOwner && got != DbusNameReply::AlreadyOwner) {
        format(why, cap, "the card's reservation could not be had");
        dbus_close(c);
        r->~Reserve();
        mem_free(r);
        return nullptr;
    }
    os_event_init(&r->event);
    dbus_set_notify(c, reserve_wake, r);
    if (!os_thread_start(&r->thread, reserve_run, r)) {
        format(why, cap, "no thread for the card's reservation");
        dbus_close(c);
        os_event_destroy(&r->event);
        r->~Reserve();
        mem_free(r);
        return nullptr;
    }
    os_event_set(&r->event);
    return r;
}

bool reserve_asked(const Reserve* r) { return r && atomic_load(&r->asked, MO_ACQUIRE) != 0; }

void reserve_give(Reserve* r) {
    if (!r) return;
    atomic_store(&r->stop, 1u, MO_RELEASE);
    os_event_set(&r->event);
    os_thread_join(&r->thread);
    dbus_set_notify(r->c, nullptr, nullptr);
    dbus_release_name(r->c, r->name, 2.0); // the holder before us takes it again
    dbus_close(r->c);
    os_event_destroy(&r->event);
    r->~Reserve();
    mem_free(r);
}

} // namespace kv
