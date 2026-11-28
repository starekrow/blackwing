// The connection, three ways:
// - a stand-in bus over a socket pair, played by a thread here, which can send what a real
//   bus never would: a message that breaks the rules, then one that does not;
// - a private dbus-daemon, started for the test, with two connections talking through it:
//   calls, replies, errors, properties, signals, matches, names, introspection, a
//   descriptor; and an idle bus costing its thread nothing;
// - the real session bus, where a name with MPRIS's identity is owned and busctl is asked
//   to see it. Skipped, with a line saying so, where there is no session bus or no busctl.
#include "korvid/dbus/dbus.h"
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/net.h"
#include "korvid/platform/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace kv;

namespace {

// ---- the stand-in bus -------------------------------------------------------------------

struct Fake {
    NetSocket   sock;
    Array<u8>   in;
    u32         serial = 100;
    bool        ok = true;
    const char* step = "";
    u32         client_calls = 0;
    u32         last_call_serial = 0;
};

bool fake_line(Fake& f, char* line, u32 cap) {
    for (;;) {
        for (u32 i = 0; i + 1 < f.in.len; ++i)
            if (f.in.data[i] == '\r' && f.in.data[i + 1] == '\n') {
                u32 n = i < cap - 1 ? i : cap - 1;
                memcpy(line, f.in.data, n);
                line[n] = 0;
                memmove(f.in.data, f.in.data + i + 2, f.in.len - i - 2);
                f.in.len -= i + 2;
                return true;
            }
        char buf[512];
        i32 k = f.sock.read(buf, sizeof buf, 5.0);
        if (k <= 0) return false;
        memcpy(f.in.push_n((u32)k), buf, (u32)k);
    }
}

bool fake_message(Fake& f, DbusMessage* m) {
    for (;;) {
        const char* why;
        i64 size = dbus_message_size(f.in.data, f.in.len, &why);
        if (size > 0 && (u64)size <= f.in.len) {
            bool ok = dbus_parse(f.in.data, (u32)size, nullptr, 0, m, &why);
            memmove(f.in.data, f.in.data + size, f.in.len - (u32)size);
            f.in.len -= (u32)size;
            return ok;
        }
        char buf[4096];
        i32 k = f.sock.read(buf, sizeof buf, 5.0);
        if (k <= 0) return false;
        memcpy(f.in.push_n((u32)k), buf, (u32)k);
    }
}

void fake_send(Fake& f, DbusHeader h, const DbusWriter* body, const Handle* fds = nullptr, u32 nfds = 0) {
    Array<u8> b;
    const char* why;
    if (!dbus_encode(h, ++f.serial, body, &b, &why)) { f.ok = false; f.step = why; return; }
    bool sent = nfds ? f.sock.write_now_with(b.data, b.len, fds, nfds, 5.0) : f.sock.write_now(b.data, b.len, 5.0);
    if (!sent) { f.ok = false; f.step = "send"; }
    b.free();
}

void fake_signal(Fake& f, const char* member, const char* sig, const char* text, const Handle* fds = nullptr, u32 nfds = 0) {
    DbusHeader h;
    h.kind = DbusKind::Signal;
    h.path = "/test";
    h.interface = "net.blackwing.Test";
    h.member = member;
    DbusWriter w;
    w.begin(sig);
    if (sig[0] == 's') w.str(text);
    if (sig[0] == 'h') w.fd(fds[0]);
    fake_send(f, h, &w, fds, nfds);
    w.free();
}

void fake_bus(void* arg) {
    Fake& f = *(Fake*)arg;
    char line[256];
    // The NUL, then AUTH EXTERNAL with the uid in hex.
    char first;
    if (f.sock.read(&first, 1, 5.0) != 1 || first != 0) { f.ok = false; f.step = "the NUL"; return; }
    if (!fake_line(f, line, sizeof line) || strncmp(line, "AUTH EXTERNAL ", 14) != 0) { f.ok = false; f.step = "AUTH"; return; }
    f.sock.write_now("OK 0123456789abcdef0123456789abcdef\r\n", 37);
    if (!fake_line(f, line, sizeof line) || strcmp(line, "NEGOTIATE_UNIX_FD") != 0) { f.ok = false; f.step = "NEGOTIATE"; return; }
    f.sock.write_now("AGREE_UNIX_FD\r\n", 15);
    if (!fake_line(f, line, sizeof line) || strcmp(line, "BEGIN") != 0) { f.ok = false; f.step = "BEGIN"; return; }
    DbusMessage hello;
    if (!fake_message(f, &hello) || strcmp(hello.member, "Hello") != 0) { f.ok = false; f.step = "Hello"; dbus_message_free(&hello); return; }
    DbusHeader r;
    r.kind = DbusKind::Return;
    r.reply_serial = hello.serial;
    DbusWriter w;
    w.begin("s");
    w.str(":1.7");
    fake_send(f, r, &w);
    w.free();
    dbus_message_free(&hello);

    // A signal whose member is not a member name: framed right, its content wrong.
    {
        DbusHeader h;
        h.kind = DbusKind::Signal;
        h.path = "/test"; h.interface = "net.blackwing.Test"; h.member = "Xbad";
        Array<u8> b;
        const char* why;
        dbus_encode(h, ++f.serial, nullptr, &b, &why);
        for (u32 i = 0; i + 4 <= b.len; ++i)
            if (memcmp(b.data + i, "Xbad", 4) == 0) { b.data[i] = '1'; break; }
        f.sock.write_now(b.data, b.len, 5.0);
        b.free();
    }
    // One too big for this client: read past, refused.
    {
        DbusHeader h;
        h.kind = DbusKind::Signal;
        h.path = "/test"; h.interface = "net.blackwing.Test"; h.member = "Huge";
        DbusWriter big;
        big.begin("ay");
        big.array_begin();
        big.array_end();
        Array<u8> b;
        const char* why;
        dbus_encode(h, ++f.serial, &big, &b, &why);
        // Its lengths changed to say 4 MiB and more follow, and then they do: the body is
        // the array's length word, then its bytes.
        u32 extra = kDbusMaxMessage + 4096;
        u32 body_len = extra + 4;
        memcpy(b.data + 4, &body_len, 4);
        memcpy(b.data + b.len - 4, &extra, 4);
        f.sock.write_now(b.data, b.len, 5.0);
        Array<u8> zeros;
        memset(zeros.push_n(65536), 0, 65536);
        for (u32 sent = 0; sent < extra; sent += 65536) f.sock.write_now(zeros.data, extra - sent < 65536 ? extra - sent : 65536, 10.0);
        zeros.free();
        b.free();
        big.free();
    }
    fake_signal(f, "Good", "s", "after the bad ones");
    // A descriptor: one end of a pair, whose other end the test keeps reading.
    NetSocket x, y;
    net_socket_pair(&x, &y);
    fake_signal(f, "Fd", "h", nullptr, &x.fd, 1);
    x.close();
    // A call to the client's object, and its answer read here.
    DbusHeader call;
    call.path = "/obj"; call.interface = "net.blackwing.Thing"; call.member = "Echo";
    DbusWriter cw;
    cw.begin("s");
    cw.str("round");
    fake_send(f, call, &cw);
    cw.free();
    DbusMessage ans;
    if (fake_message(f, &ans)) {
        DbusReader rr = ans.reader();
        const char* s = "";
        if (ans.kind == DbusKind::Return && rr.str(&s) && strcmp(s, "round trip") == 0) f.client_calls++;
        dbus_message_free(&ans);
    }
    // Then y's other end goes too; the test reads "via fd" from what it was given.
    char buf[16];
    if (y.read(buf, sizeof buf, 5.0) == 6 && memcmp(buf, "via fd", 6) == 0) f.client_calls++;
    y.close();
    // Bytes that cannot be framed: the connection is over.
    const u8 junk[16] = {'x', 1, 0, 1};
    f.sock.write_now(junk, sizeof junk, 5.0);
    time_sleep(0.2);
    f.sock.close();
    f.in.free();
}

struct Notified {
    u32 count = 0; // atomic
};
void on_notify(void* user) { atomic_add_fetch(&((Notified*)user)->count, 1u, MO_RELAXED); }

// The next message that is not the bus's own news (NameAcquired, NameLost), which a
// connection that owns a name is sent and its customer may want.
bool next_within(DbusConnection* c, DbusMessage* m, f64 seconds) {
    f64 end = time_now() + seconds;
    while (time_now() < end) {
        if (dbus_next(c, m)) {
            if (m->kind == DbusKind::Signal && strcmp(m->sender, "org.freedesktop.DBus") == 0) { dbus_message_free(m); continue; }
            return true;
        }
        time_sleep(0.002);
    }
    return false;
}

const DbusMethodDesc kEcho[] = {{"Echo", "s", "s"}, {"Quit"}};
const DbusPropertyDesc kThingProps[] = {
    {"Volume", "d", DbusAccess::ReadWrite},
    {"Identity", "s", DbusAccess::Read, DbusEmits::None},
    {"Position", "x", DbusAccess::Read, DbusEmits::None, true},
    {"Tags", "as", DbusAccess::Read, DbusEmits::Invalidates},
};
const DbusSignalDesc kThingSignals[] = {{"Seeked", "x"}};
const DbusInterfaceDesc kThing = {"net.blackwing.Thing", kEcho, 2, kThingProps, 4, kThingSignals, 1};

} // namespace

void test_fake_bus(TestCtx& c) {
    c.begin("dbus, a stand-in bus");
    Fake f;
    NetSocket mine;
    KV_CHECK(c, net_socket_pair(&mine, &f.sock));
    OsThread t;
    os_thread_start(&t, fake_bus, &f);
    char why[200] = {};
    DbusConnection* conn = dbus_open_socket(&mine, 5.0, why, sizeof why);
    KV_CHECK(c, conn != nullptr);
    if (!conn) { KV_WARN("test", "no connection: %s (the stand-in at %s)", why, f.step); f.sock.close(); os_thread_join(&t); return; }
    KV_CHECK(c, strcmp(dbus_unique_name(conn), ":1.7") == 0 && dbus_can_pass_fds(conn) && !mine.is_open());
    const DbusInterfaceDesc* ifaces[] = {&kThing};
    KV_CHECK(c, dbus_export(conn, "/obj", ifaces, 1));
    Notified n;
    dbus_set_notify(conn, on_notify, &n);

    // The bad ones refused, the good one after them taken: the connection kept.
    DbusMessage m;
    KV_CHECK(c, next_within(conn, &m, 10.0) && m.kind == DbusKind::Signal && strcmp(m.member, "Good") == 0);
    dbus_message_free(&m);
    char rwhy[160];
    KV_CHECK(c, dbus_refused(conn, rwhy, sizeof rwhy) == 2 && dbus_is_open(conn));
    // The descriptor, ours now: written to, and the stand-in reads it.
    KV_CHECK(c, next_within(conn, &m, 5.0) && strcmp(m.member, "Fd") == 0 && m.fd_count == 1);
    DbusReader r = m.reader();
    u32 idx = 99;
    KV_CHECK(c, r.fd(&idx) && idx == 0);
    NetSocket via;
    via.adopt(dbus_message_take_fd(&m, 0));
    dbus_message_free(&m);
    KV_CHECK(c, via.write_now("via fd", 6, 5.0));
    via.close();
    // A call to our object comes here, and is answered.
    KV_CHECK(c, next_within(conn, &m, 5.0) && m.kind == DbusKind::Call && strcmp(m.member, "Echo") == 0);
    DbusWriter w;
    w.begin("s");
    w.str("round trip");
    KV_CHECK(c, dbus_reply(conn, m, &w));
    w.free();
    dbus_message_free(&m);
    // Then bytes that cannot be framed: over, and said why.
    f64 t0 = time_now();
    while (dbus_is_open(conn) && time_now() - t0 < 5.0) time_sleep(0.005);
    KV_CHECK(c, !dbus_is_open(conn) && strstr(dbus_lost_why(conn), "framed") != nullptr);
    KV_CHECK(c, atomic_load(&n.count, MO_RELAXED) >= 4);
    os_thread_join(&t);
    KV_CHECK(c, f.ok && f.client_calls == 2);
    if (!f.ok) KV_WARN("test", "the stand-in failed at %s", f.step);
    dbus_close(conn);
}

// ---- a private bus ----------------------------------------------------------------------

namespace {

bool start_daemon(Process& p, char* address, u32 cap) {
    const char* argv[] = {"dbus-daemon", "--session", "--nofork", "--print-address=1"};
    ProcessDesc d;
    d.argv = Span<const char* const>(argv, 4);
    if (!p.spawn(d)) return false;
    u32 n = 0;
    f64 end = time_now() + 5.0;
    while (time_now() < end) {
        p.wait(0.1);
        i32 k = p.read_out(address + n, cap - 1 - n);
        if (k > 0) n += (u32)k;
        address[n] = 0;
        char* nl = strchr(address, '\n');
        if (nl) { *nl = 0; return true; }
        if (k < 0) break;
    }
    return false;
}

} // namespace

void test_private_bus(TestCtx& c) {
    c.begin("dbus, a private bus");
    Process daemon;
    char address[512] = {};
    if (!start_daemon(daemon, address, sizeof address)) {
        KV_INFO("test", "skipped: dbus-daemon would not start here (%s)", daemon.error);
        daemon.close();
        return;
    }
    char why[200] = {};
    DbusConnection* a = dbus_open_address(address, 5.0, why, sizeof why);
    DbusConnection* b = dbus_open_address(address, 5.0, why, sizeof why);
    KV_CHECK(c, a && b);
    if (!a || !b) { KV_WARN("test", "%s", why); dbus_close(a); dbus_close(b); daemon.close(); return; }
    KV_CHECK(c, dbus_unique_name(a)[0] == ':' && strcmp(dbus_unique_name(a), dbus_unique_name(b)) != 0);
    KV_CHECK(c, dbus_can_pass_fds(a) && dbus_can_pass_fds(b));

    // A's name and object.
    const char* name = "net.blackwing.Test";
    KV_CHECK(c, dbus_request_name(a, name, kDbusNameDoNotQueue, 2.0) == DbusNameReply::PrimaryOwner);
    KV_CHECK(c, dbus_request_name(b, name, kDbusNameDoNotQueue, 2.0) == DbusNameReply::Exists);
    KV_CHECK(c, dbus_request_name(a, name, kDbusNameDoNotQueue, 2.0) == DbusNameReply::AlreadyOwner);
    const DbusInterfaceDesc* ifaces[] = {&kThing};
    KV_CHECK(c, dbus_export(a, "/net/blackwing/Thing", ifaces, 1));
    DbusWriter v;
    v.begin("d"); v.f64(0.5);
    KV_CHECK(c, dbus_property_set(a, "/net/blackwing/Thing", "net.blackwing.Thing", "Volume", v, true));
    v.begin("s"); v.str("Blackwing (test)");
    KV_CHECK(c, dbus_property_set(a, "/net/blackwing/Thing", "net.blackwing.Thing", "Identity", v));
    v.begin("s"); v.str("not a double");
    KV_CHECK(c, !dbus_property_set(a, "/net/blackwing/Thing", "net.blackwing.Thing", "Volume", v));
    KV_CHECK(c, !dbus_property_set(a, "/net/blackwing/Thing", "net.blackwing.Thing", "Nothing", v));

    DbusHeader h;
    h.destination = name;
    h.path = "/net/blackwing/Thing";
    DbusMessage reply, m;

    // A described method goes to A, whose answer comes back to B.
    h.interface = "net.blackwing.Thing";
    h.member = "Echo";
    DbusWriter w;
    w.begin("s"); w.str("hello");
    u32 serial = dbus_send(b, h, &w);
    KV_CHECK(c, serial != 0 && next_within(a, &m, 2.0) && m.kind == DbusKind::Call && strcmp(m.member, "Echo") == 0);
    KV_CHECK(c, strcmp(m.sender, dbus_unique_name(b)) == 0);
    DbusReader r = m.reader();
    const char* s = "";
    r.str(&s);
    DbusWriter echo;
    echo.begin("s"); echo.str(s);
    dbus_reply(a, m, &echo);
    dbus_message_free(&m);
    KV_CHECK(c, next_within(b, &reply, 2.0) && reply.kind == DbusKind::Return && reply.reply_serial == serial);
    r = reply.reader();
    KV_CHECK(c, r.str(&s) && strcmp(s, "hello") == 0);
    dbus_message_free(&reply);

    // Refused by A's thread, in words: no such method, the wrong arguments, no such object.
    h.member = "Nope";
    KV_CHECK(c, dbus_call(b, h, nullptr, 2.0, &reply) && reply.kind == DbusKind::Error && strcmp(reply.error_name, "org.freedesktop.DBus.Error.UnknownMethod") == 0);
    dbus_message_free(&reply);
    h.member = "Echo";
    w.begin("i"); w.i32(3);
    KV_CHECK(c, dbus_call(b, h, &w, 2.0, &reply) && strcmp(reply.error_name, "org.freedesktop.DBus.Error.InvalidArgs") == 0);
    dbus_message_free(&reply);
    DbusHeader nowhere = h;
    nowhere.path = "/nowhere";
    KV_CHECK(c, dbus_call(b, nowhere, nullptr, 2.0, &reply) && strcmp(reply.error_name, "org.freedesktop.DBus.Error.UnknownObject") == 0);
    dbus_message_free(&reply);

    // Properties: Get answered by A's thread; a live one, and GetAll with one, by A.
    h.interface = "org.freedesktop.DBus.Properties";
    h.member = "Get";
    w.begin("ss"); w.str("net.blackwing.Thing"); w.str("Volume");
    f64 vol = 0;
    KV_CHECK(c, dbus_call(b, h, &w, 2.0, &reply) && reply.kind == DbusKind::Return);
    r = reply.reader();
    KV_CHECK(c, r.variant_begin() && r.f64(&vol) && vol == 0.5);
    dbus_message_free(&reply);
    w.begin("ss"); w.str("net.blackwing.Thing"); w.str("Position");
    serial = dbus_send(b, h, &w);
    KV_CHECK(c, next_within(a, &m, 2.0) && strcmp(m.member, "Get") == 0);
    DbusWriter pos;
    pos.begin("v"); pos.variant_begin("x"); pos.i64(1234567); pos.variant_end();
    dbus_reply(a, m, &pos);
    dbus_message_free(&m);
    i64 p = 0;
    KV_CHECK(c, next_within(b, &reply, 2.0) && reply.reply_serial == serial);
    r = reply.reader();
    KV_CHECK(c, r.variant_begin() && r.i64(&p) && p == 1234567);
    dbus_message_free(&reply);
    h.member = "GetAll";
    w.begin("s"); w.str("net.blackwing.Thing");
    serial = dbus_send(b, h, &w);
    KV_CHECK(c, next_within(a, &m, 2.0) && strcmp(m.member, "GetAll") == 0);
    DbusWriter live;
    live.begin("x"); live.i64(42);
    DbusPropertyValue lv{"Position", &live};
    dbus_reply_get_all(a, m, &lv, 1);
    dbus_message_free(&m);
    KV_CHECK(c, next_within(b, &reply, 2.0) && reply.reply_serial == serial && strcmp(reply.signature, "a{sv}") == 0);
    u32 keys = 0;
    r = reply.reader();
    if (r.array_begin())
        while (r.array_more()) { const char* k; r.entry_begin(); r.str(&k); r.skip(); r.entry_end(); ++keys; }
    KV_CHECK(c, keys == 3 && !r.failed); // Volume, Identity, Position: Tags has no value yet
    dbus_message_free(&reply);
    // Set: to A, which takes it and sets it; PropertiesChanged reaches B through its match.
    KV_CHECK(c, dbus_add_match(b, "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", 2.0));
    h.member = "Set";
    w.begin("ssv"); w.str("net.blackwing.Thing"); w.str("Volume"); w.variant_begin("d"); w.f64(0.75); w.variant_end();
    serial = dbus_send(b, h, &w);
    KV_CHECK(c, next_within(a, &m, 2.0) && strcmp(m.member, "Set") == 0);
    r = m.reader();
    const char* i1; const char* i2; f64 nv = 0;
    r.str(&i1); r.str(&i2); r.variant_begin(); r.f64(&nv);
    v.begin("d"); v.f64(nv);
    KV_CHECK(c, dbus_property_set(a, "/net/blackwing/Thing", "net.blackwing.Thing", "Volume", v));
    dbus_reply(a, m, nullptr);
    dbus_message_free(&m);
    bool changed = false, answered = false;
    f64 end = time_now() + 2.0;
    while ((!changed || !answered) && time_now() < end) {
        if (!dbus_next(b, &m)) { time_sleep(0.002); continue; }
        if (m.kind == DbusKind::Return && m.reply_serial == serial) answered = true;
        if (m.kind == DbusKind::Signal && strcmp(m.member, "PropertiesChanged") == 0) {
            r = m.reader();
            const char* iface;
            r.str(&iface);
            if (r.array_begin() && r.array_more()) {
                const char* k; f64 val = 0;
                r.entry_begin(); r.str(&k); r.variant_begin(); r.f64(&val);
                changed = strcmp(k, "Volume") == 0 && val == 0.75;
            }
        }
        dbus_message_free(&m);
    }
    KV_CHECK(c, changed && answered);
    w.begin("ssv"); w.str("net.blackwing.Thing"); w.str("Identity"); w.variant_begin("s"); w.str("x"); w.variant_end();
    KV_CHECK(c, dbus_call(b, h, &w, 2.0, &reply) && strcmp(reply.error_name, "org.freedesktop.DBus.Error.PropertyReadOnly") == 0);
    dbus_message_free(&reply);

    // A signal of A's own, heard by B's match.
    KV_CHECK(c, dbus_add_match(b, "type='signal',interface='net.blackwing.Thing'", 2.0));
    DbusWriter seek;
    seek.begin("x"); seek.i64(99);
    KV_CHECK(c, dbus_signal(a, "/net/blackwing/Thing", "net.blackwing.Thing", "Seeked", &seek) != 0);
    bool heard = false;
    end = time_now() + 2.0;
    while (!heard && next_within(b, &m, end - time_now())) {
        heard = m.kind == DbusKind::Signal && strcmp(m.member, "Seeked") == 0;
        dbus_message_free(&m);
    }
    KV_CHECK(c, heard);

    // Introspection: the object's interfaces, and the way down to it from "/".
    h.interface = "org.freedesktop.DBus.Introspectable";
    h.member = "Introspect";
    KV_CHECK(c, dbus_call(b, h, nullptr, 2.0, &reply) && reply.kind == DbusKind::Return);
    r = reply.reader();
    const char* xml = "";
    r.str(&xml);
    KV_CHECK(c, strstr(xml, "<interface name=\"net.blackwing.Thing\">") && strstr(xml, "<property name=\"Volume\" type=\"d\" access=\"readwrite\"/>") &&
                strstr(xml, "<method name=\"Echo\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"out\"/></method>") &&
                strstr(xml, "<signal name=\"Seeked\">"));
    dbus_message_free(&reply);
    DbusHeader root = h;
    root.path = "/net";
    KV_CHECK(c, dbus_call(b, root, nullptr, 2.0, &reply) && reply.kind == DbusKind::Return);
    r = reply.reader();
    r.str(&xml);
    KV_CHECK(c, strstr(xml, "<node name=\"blackwing\"/>") != nullptr);
    dbus_message_free(&reply);
    h.interface = "org.freedesktop.DBus.Peer";
    h.member = "Ping";
    KV_CHECK(c, dbus_call(b, h, nullptr, 2.0, &reply) && reply.kind == DbusKind::Return);
    dbus_message_free(&reply);
    // A round trip through the bus, B to A's thread and back, timed.
    f64 times[200];
    u32 good = 0;
    for (u32 i = 0; i < 200; ++i) {
        f64 t0 = time_now();
        if (dbus_call(b, h, nullptr, 2.0, &reply) && reply.kind == DbusKind::Return) times[good++] = time_now() - t0;
        dbus_message_free(&reply);
    }
    for (u32 i = 1; i < good; ++i) for (u32 k = i; k > 0 && times[k - 1] > times[k]; --k) { f64 t = times[k]; times[k] = times[k - 1]; times[k - 1] = t; }
    KV_CHECK(c, good == 200);
    if (good) KV_INFO("test", "Ping through the bus, 200 round trips: median %.0f us, 99th %.0f us, worst %.0f us",
                      times[good / 2] * 1e6, times[good * 99 / 100] * 1e6, times[good - 1] * 1e6);

    // A descriptor through the bus: A sends one end of a pair; B writes to what it got.
    NetSocket x, y;
    net_socket_pair(&x, &y);
    DbusWriter fdw;
    fdw.begin("h");
    fdw.fd(x.fd);
    DbusHeader fh;
    fh.kind = DbusKind::Signal;
    fh.destination = dbus_unique_name(b);
    fh.path = "/net/blackwing/Thing";
    fh.interface = "net.blackwing.Thing";
    fh.member = "Fd";
    KV_CHECK(c, dbus_send(a, fh, &fdw) != 0);
    bool got_fd = false;
    end = time_now() + 2.0;
    while (!got_fd && next_within(b, &m, end - time_now())) {
        if (strcmp(m.member, "Fd") == 0 && m.fd_count == 1) {
            NetSocket z;
            z.adopt(dbus_message_take_fd(&m, 0));
            char buf[8];
            got_fd = z.write_now("fd", 2, 2.0) && y.read(buf, sizeof buf, 2.0) == 2;
            z.close();
        }
        dbus_message_free(&m);
    }
    KV_CHECK(c, got_fd);
    x.close(); y.close();

    // A call that nobody answers in time: false, and said why. A's queue had it, and
    // A answers late: the late answer goes nowhere.
    h.interface = "net.blackwing.Thing";
    h.member = "Quit";
    char cwhy[96] = {};
    KV_CHECK(c, !dbus_call(b, h, nullptr, 0.2, &reply, cwhy, sizeof cwhy) && strstr(cwhy, "no answer"));
    KV_CHECK(c, next_within(a, &m, 2.0) && strcmp(m.member, "Quit") == 0);
    dbus_reply(a, m, nullptr);
    dbus_message_free(&m);
    time_sleep(0.1);
    bool late = false;
    while (dbus_next(b, &m)) { late |= m.kind == DbusKind::Return; dbus_message_free(&m); }
    KV_CHECK(c, !late);

    // Idle, the threads do not wake.
    time_sleep(0.1);
    u64 wa = dbus_thread_wakes(a), wb = dbus_thread_wakes(b);
    time_sleep(1.0);
    u64 da = dbus_thread_wakes(a) - wa, db = dbus_thread_wakes(b) - wb;
    KV_INFO("test", "idle for a second: %llu and %llu wakeups", (unsigned long long)da, (unsigned long long)db);
    KV_CHECK(c, da == 0 && db == 0);

    // The name let go; the bus gone, both connections say so.
    KV_CHECK(c, dbus_release_name(a, name, 2.0));
    KV_CHECK(c, dbus_request_name(b, name, kDbusNameDoNotQueue, 2.0) == DbusNameReply::PrimaryOwner);
    daemon.kill();
    end = time_now() + 3.0;
    while ((dbus_is_open(a) || dbus_is_open(b)) && time_now() < end) time_sleep(0.005);
    KV_CHECK(c, !dbus_is_open(a) && !dbus_is_open(b) && dbus_lost_why(a)[0]);
    KV_CHECK(c, !dbus_call(a, h, nullptr, 1.0, &reply) && dbus_send(a, h, nullptr) == 0);
    v.free(); w.free(); echo.free(); pos.free(); live.free(); seek.free(); fdw.free();
    dbus_close(a);
    dbus_close(b);
    daemon.close();
}

// ---- the real session bus ---------------------------------------------------------------

void test_session_bus(TestCtx& c) {
    c.begin("dbus, the session bus");
    char why[200] = {};
    DbusConnection* s = dbus_open(DbusBus::Session, 2.0, why, sizeof why);
    if (!s) { KV_INFO("test", "skipped: no session bus (%s)", why); return; }
    // MPRIS's identity under a test's own name, so a Blackwing playing now is not disturbed.
    char name[128];
    snprintf(name, sizeof name, "org.mpris.MediaPlayer2.blackwing.test%d", (int)getpid());
    KV_CHECK(c, dbus_request_name(s, name, kDbusNameDoNotQueue, 2.0) == DbusNameReply::PrimaryOwner);
    static const DbusPropertyDesc props[] = {
        {"Identity", "s"}, {"CanQuit", "b"}, {"CanRaise", "b"}, {"HasTrackList", "b"},
        {"SupportedUriSchemes", "as"}, {"SupportedMimeTypes", "as"},
    };
    static const DbusMethodDesc methods[] = {{"Raise"}, {"Quit"}};
    static const DbusInterfaceDesc root = {"org.mpris.MediaPlayer2", methods, 2, props, 6};
    const DbusInterfaceDesc* ifaces[] = {&root};
    KV_CHECK(c, dbus_export(s, "/org/mpris/MediaPlayer2", ifaces, 1));
    DbusWriter v;
    v.begin("s"); v.str("Blackwing (dbus test)");
    dbus_property_set(s, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2", "Identity", v, true);
    v.begin("b"); v.boolean(false);
    dbus_property_set(s, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2", "CanQuit", v, true);
    // busctl, a client we did not write, sees the name and reads the property.
    char cmd[256];
    snprintf(cmd, sizeof cmd, "busctl --user get-property %s /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2 Identity", name);
    Process p;
    if (!p.shell(cmd)) {
        KV_INFO("test", "busctl not run: %s", p.error);
    } else {
        char out[256] = {};
        u32 n = 0;
        f64 end = time_now() + 5.0;
        // This thread waits here while the connection's thread answers busctl.
        while (time_now() < end) {
            p.wait(0.1);
            i32 k = p.read_out(out + n, sizeof out - 1 - n);
            if (k > 0) n += (u32)k;
            if (k < 0 && !p.running()) break;
        }
        out[n] = 0;
        KV_INFO("test", "busctl says: %s", out);
        KV_CHECK(c, strstr(out, "s \"Blackwing (dbus test)\"") != nullptr);
        p.close();
    }
    v.free();
    dbus_release_name(s, name, 2.0);
    dbus_close(s);
}
