// A connection: the address, the authentication, Hello, and then a thread of its own that
// owns the socket. It sleeps in net_wait on the socket and a waker; callers post to it
// under the lock and wake it, and take what came with dbus_next or a blocking call. It
// answers Peer, Introspectable and Properties for the objects exported, from what they
// were described as and set to; the rest goes to the caller.
#include "korvid/dbus/dbus.h"
#include "wire_internal.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_error.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/time.h"
#include "korvid/platform/net.h"
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace kv {

namespace dbus_conn {

constexpr u32 kMaxExports = 32;
constexpr u32 kMaxInterfaces = 8;
constexpr u32 kMaxWaits = 16;
constexpr u32 kMaxQueuedFds = 64;
constexpr u32 kAbandoned = 16;

const char* const kBus = "org.freedesktop.DBus";
const char* const kBusPath = "/org/freedesktop/DBus";
const char* const kProps = "org.freedesktop.DBus.Properties";
const char* const kIntro = "org.freedesktop.DBus.Introspectable";
const char* const kPeer = "org.freedesktop.DBus.Peer";

struct Export {
    char                     path[256] = {};
    const DbusInterfaceDesc* ifaces[kMaxInterfaces] = {};
    u32                      count = 0;
    bool                     used = false;
};

struct PropValue {
    u32                      exp = 0;
    const DbusInterfaceDesc* iface = nullptr;
    const DbusPropertyDesc*  prop = nullptr;
    Array<u8>                bytes;
};

struct Wait {
    u32         serial = 0;
    bool        used = false;
    bool        done = false;
    DbusMessage reply;
};

struct Out {
    u32    at = 0, len = 0;
    Handle fds[kDbusMaxFds];
    u32    fd_count = 0;
};

} // namespace dbus_conn

using namespace dbus_conn;

struct DbusConnection {
    NetSocket sock;
    NetWaker  waker;
    OsThread  thread;
    OsMutex   lock;
    OsCond    cond;
    bool      running = false;

    // posted, for the thread to send (under lock)
    Array<u8>  out_bytes;
    Array<Out> out;
    // what came, for the caller (under lock)
    Array<DbusMessage> inbox;
    Wait       waits[kMaxWaits];
    u32        abandoned[kAbandoned] = {};
    u32        abandoned_at = 0;
    // the thread's own
    Array<u8>  rbuf;
    Handle     rfds[kMaxQueuedFds];
    u32        rfd_count = 0;
    u64        discard = 0;
    // objects (under lock)
    Export          exports[kMaxExports];
    Array<PropValue> props;

    u32  serial = 0;        // under lock
    bool open = false;      // under lock
    bool stop = false;      // under lock
    char lost[160] = {};
    char unique[256] = {};
    bool fds_ok = false;
    u32  refused = 0;       // under lock
    char refused_why[160] = {};
    void (*notify)(void*) = nullptr;
    void* notify_user = nullptr;
    u64  wakes = 0;         // atomic
};

namespace {

void set_why(char* why, u32 cap, const char* text) {
    if (why && cap) kv::format(why, cap, "%s", text);
}

u32 next_serial(DbusConnection* c) {
    os_mutex_lock(&c->lock);
    u32 s = ++c->serial;
    if (s == 0) s = ++c->serial;
    os_mutex_unlock(&c->lock);
    return s;
}

void call_notify(DbusConnection* c) {
    void (*fn)(void*);
    void* user;
    os_mutex_lock(&c->lock);
    fn = c->notify;
    user = c->notify_user;
    os_mutex_unlock(&c->lock);
    if (fn) fn(user);
}

// Queues encoded bytes for the thread, and wakes it.
bool post_bytes(DbusConnection* c, const Array<u8>& bytes, const DbusWriter* body) {
    os_mutex_lock(&c->lock);
    if (!c->open) { os_mutex_unlock(&c->lock); return false; }
    Out o;
    o.at = c->out_bytes.len;
    o.len = bytes.len;
    if (body) {
        o.fd_count = body->fd_count;
        for (u32 i = 0; i < body->fd_count; ++i) o.fds[i] = body->fds[i];
    }
    memcpy(c->out_bytes.push_n(bytes.len), bytes.data, bytes.len);
    c->out.push(o);
    os_mutex_unlock(&c->lock);
    c->waker.wake();
    return true;
}

u32 post(DbusConnection* c, const DbusHeader& h, const DbusWriter* body) {
    u32 serial = next_serial(c);
    Array<u8> bytes;
    const char* why = nullptr;
    if (!dbus_encode(h, serial, body, &bytes, &why)) {
        KV_WARN("dbus", "not sent (%s.%s): %s", h.interface ? h.interface : "", h.member ? h.member : "", why);
        bytes.free();
        return 0;
    }
    bool ok = post_bytes(c, bytes, body);
    bytes.free();
    return ok ? serial : 0;
}

void lose(DbusConnection* c, const char* why) {
    os_mutex_lock(&c->lock);
    bool was = c->open;
    if (was) {
        c->open = false;
        kv::format(c->lost, sizeof c->lost, "%s", why);
    }
    os_cond_broadcast(&c->cond);
    os_mutex_unlock(&c->lock);
    if (was) {
        KV_INFO("dbus", "the connection is over: %s", why);
        call_notify(c);
    }
}

void refuse(DbusConnection* c, const char* why) {
    os_mutex_lock(&c->lock);
    ++c->refused;
    kv::format(c->refused_why, sizeof c->refused_why, "%s", why);
    os_mutex_unlock(&c->lock);
    KV_WARN("dbus", "a message refused: %s", why);
}

// ---- answering on the thread ----------------------------------------------------------

void reply_error(DbusConnection* c, const DbusMessage& m, const char* name, const char* text) {
    if (m.flags & kDbusNoReplyExpected) return;
    DbusHeader h;
    h.kind = DbusKind::Error;
    h.error_name = name;
    h.reply_serial = m.serial;
    h.destination = m.sender[0] ? m.sender : nullptr;
    DbusWriter w;
    w.begin("s");
    w.str(text);
    post(c, h, &w);
    w.free();
}

void reply_body(DbusConnection* c, const DbusMessage& m, const DbusWriter* body) {
    if (m.flags & kDbusNoReplyExpected) return;
    DbusHeader h;
    h.kind = DbusKind::Return;
    h.reply_serial = m.serial;
    h.destination = m.sender[0] ? m.sender : nullptr;
    post(c, h, body);
}

// Under lock.
Export* find_export(DbusConnection* c, const char* path) {
    for (Export& e : c->exports)
        if (e.used && strcmp(e.path, path) == 0) return &e;
    return nullptr;
}

const DbusInterfaceDesc* find_iface(const Export& e, const char* name) {
    for (u32 i = 0; i < e.count; ++i)
        if (strcmp(e.ifaces[i]->name, name) == 0) return e.ifaces[i];
    return nullptr;
}

const DbusPropertyDesc* find_prop(const DbusInterfaceDesc* d, const char* name) {
    for (u32 i = 0; i < d->property_count; ++i)
        if (strcmp(d->properties[i].name, name) == 0) return &d->properties[i];
    return nullptr;
}

PropValue* find_value(DbusConnection* c, u32 exp, const DbusPropertyDesc* p) {
    for (PropValue& v : c->props)
        if (v.exp == exp && v.prop == p) return &v;
    return nullptr;
}

bool has_live(const DbusInterfaceDesc* d) {
    for (u32 i = 0; i < d->property_count; ++i)
        if (d->properties[i].live) return true;
    return false;
}

// A stored value into `w` as a variant.
bool put_value(DbusWriter& w, const PropValue& v) {
    DbusReader r;
    r.begin(v.bytes.data, v.bytes.len, v.prop->signature);
    w.variant_begin(v.prop->signature);
    if (!dbus_copy_value(r, w)) return false;
    w.variant_end();
    return !w.error;
}

void put_value_of(DbusWriter& w, const DbusWriter& value) {
    DbusReader r;
    r.begin(value.bytes.data, value.bytes.len, value.sig);
    w.variant_begin(value.sig);
    dbus_copy_value(r, w);
    w.variant_end();
}

// A struct of the readable values set on an interface, as a{sv} entries; under lock.
void put_all(DbusConnection* c, DbusWriter& w, u32 exp, const DbusInterfaceDesc* d, const DbusPropertyValue* live, u32 nlive) {
    for (u32 i = 0; i < d->property_count; ++i) {
        const DbusPropertyDesc* p = &d->properties[i];
        if (p->access == DbusAccess::Write) continue;
        if (p->live) {
            for (u32 k = 0; k < nlive; ++k)
                if (strcmp(live[k].name, p->name) == 0 && live[k].value && live[k].value->done()) {
                    w.entry_begin(); w.str(p->name); put_value_of(w, *live[k].value); w.entry_end();
                }
            continue;
        }
        PropValue* v = find_value(c, exp, p);
        if (!v) continue;
        w.entry_begin(); w.str(p->name); put_value(w, *v); w.entry_end();
    }
}

void append(Array<char>& x, const char* s) {
    u32 n = (u32)strlen(s);
    memcpy(x.push_n(n), s, n);
}

void append_args(Array<char>& x, const char* sig, const DbusArg* args, u32 nargs, u32* used, const char* direction) {
    u32 n = (u32)strlen(sig);
    u32 i = 0;
    while (i < n) {
        u32 t = dbus_complete_type(sig + i, n - i);
        if (!t) break;
        char type[256];
        memcpy(type, sig + i, t);
        type[t] = 0;
        append(x, "<arg type=\"");
        append(x, type);
        append(x, "\"");
        if (args && *used < nargs && args[*used].name) { append(x, " name=\""); append(x, args[*used].name); append(x, "\""); }
        ++*used;
        if (direction) { append(x, " direction=\""); append(x, direction); append(x, "\""); }
        append(x, "/>");
        i += t;
    }
}

// Introspect's XML for `path`; under lock.
void introspect(DbusConnection* c, const char* path, Array<char>& x) {
    append(x, "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
              " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n");
    append(x, " <interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\">"
              "<arg name=\"xml_data\" type=\"s\" direction=\"out\"/></method></interface>\n");
    append(x, " <interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/><method name=\"GetMachineId\">"
              "<arg name=\"machine_uuid\" type=\"s\" direction=\"out\"/></method></interface>\n");
    Export* e = find_export(c, path);
    if (e) {
        append(x, " <interface name=\"org.freedesktop.DBus.Properties\">"
                  "<method name=\"Get\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/><arg name=\"property_name\" type=\"s\" direction=\"in\"/><arg name=\"value\" type=\"v\" direction=\"out\"/></method>"
                  "<method name=\"GetAll\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/><arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/></method>"
                  "<method name=\"Set\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/><arg name=\"property_name\" type=\"s\" direction=\"in\"/><arg name=\"value\" type=\"v\" direction=\"in\"/></method>"
                  "<signal name=\"PropertiesChanged\"><arg name=\"interface_name\" type=\"s\"/><arg name=\"changed_properties\" type=\"a{sv}\"/><arg name=\"invalidated_properties\" type=\"as\"/></signal>"
                  "</interface>\n");
        for (u32 i = 0; i < e->count; ++i) {
            const DbusInterfaceDesc* d = e->ifaces[i];
            append(x, " <interface name=\""); append(x, d->name); append(x, "\">\n");
            for (u32 m = 0; m < d->method_count; ++m) {
                const DbusMethodDesc& md = d->methods[m];
                append(x, "  <method name=\""); append(x, md.name); append(x, "\">");
                u32 used = 0;
                append_args(x, md.in ? md.in : "", md.args, md.arg_count, &used, "in");
                append_args(x, md.out ? md.out : "", md.args, md.arg_count, &used, "out");
                append(x, "</method>\n");
            }
            for (u32 s = 0; s < d->signal_count; ++s) {
                const DbusSignalDesc& sd = d->signals[s];
                append(x, "  <signal name=\""); append(x, sd.name); append(x, "\">");
                u32 used = 0;
                append_args(x, sd.signature ? sd.signature : "", sd.args, sd.arg_count, &used, nullptr);
                append(x, "</signal>\n");
            }
            for (u32 p = 0; p < d->property_count; ++p) {
                const DbusPropertyDesc& pd = d->properties[p];
                append(x, "  <property name=\""); append(x, pd.name); append(x, "\" type=\""); append(x, pd.signature);
                append(x, pd.access == DbusAccess::Read ? "\" access=\"read\"" : pd.access == DbusAccess::Write ? "\" access=\"write\"" : "\" access=\"readwrite\"");
                if (pd.emits == DbusEmits::Changed) append(x, "/>\n");
                else {
                    append(x, "><annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"");
                    append(x, pd.emits == DbusEmits::Invalidates ? "invalidates" : "false");
                    append(x, "\"/></property>\n");
                }
            }
            append(x, " </interface>\n");
        }
    }
    // Children: the next element of every exported path below this one, once each.
    usize plen = strlen(path);
    bool root = plen == 1;
    char seen[kMaxExports][256];
    u32 nseen = 0;
    for (Export& ex : c->exports) {
        if (!ex.used) continue;
        const char* p = ex.path;
        if (root) { if (p[1] == 0) continue; p += 1; }
        else { if (strncmp(p, path, plen) != 0 || p[plen] != '/') continue; p += plen + 1; }
        const char* slash = strchr(p, '/');
        usize n = slash ? (usize)(slash - p) : strlen(p);
        if (n == 0 || n >= 256) continue;
        bool dup = false;
        for (u32 i = 0; i < nseen; ++i)
            if (strlen(seen[i]) == n && memcmp(seen[i], p, n) == 0) dup = true;
        if (dup) continue;
        memcpy(seen[nseen], p, n);
        seen[nseen][n] = 0;
        ++nseen;
        append(x, " <node name=\""); append(x, seen[nseen - 1]); append(x, "\"/>\n");
    }
    append(x, "</node>\n");
    x.push(0);
}

bool machine_id(char* out, u32 cap) {
#if defined(__linux__)
    const char* files[] = {"/etc/machine-id", "/var/lib/dbus/machine-id"};
    for (const char* f : files) {
        FILE* fp = fopen(f, "rb");
        if (!fp) continue;
        usize n = fread(out, 1, cap - 1, fp);
        fclose(fp);
        out[n] = 0;
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
        if (n == 32) return true;
    }
#endif
    (void)out; (void)cap;
    return false;
}

bool sig_is(const DbusMessage& m, const char* s) { return strcmp(m.signature, s) == 0; }

// A call: answered here, refused here, or for the caller (true: put in the inbox).
bool handle_call(DbusConnection* c, const DbusMessage& m) {
    if (strcmp(m.interface, kPeer) == 0) {
        if (strcmp(m.member, "Ping") == 0) { reply_body(c, m, nullptr); return false; }
        if (strcmp(m.member, "GetMachineId") == 0) {
            char id[64];
            if (!machine_id(id, sizeof id)) { reply_error(c, m, "org.freedesktop.DBus.Error.Failed", "no machine id"); return false; }
            DbusWriter w; w.begin("s"); w.str(id); reply_body(c, m, &w); w.free();
            return false;
        }
        reply_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "Peer has Ping and GetMachineId");
        return false;
    }
    bool intro = strcmp(m.member, "Introspect") == 0 && (strcmp(m.interface, kIntro) == 0 || m.interface[0] == 0);
    if (intro) {
        Array<char> x;
        os_mutex_lock(&c->lock);
        introspect(c, m.path, x);
        os_mutex_unlock(&c->lock);
        DbusWriter w; w.begin("s"); w.str(x.data); reply_body(c, m, &w); w.free();
        x.free();
        return false;
    }

    os_mutex_lock(&c->lock);
    Export* e = find_export(c, m.path);
    if (!e) {
        os_mutex_unlock(&c->lock);
        reply_error(c, m, "org.freedesktop.DBus.Error.UnknownObject", "no object at that path");
        return false;
    }
    u32 exp = (u32)(e - c->exports);
    if (strcmp(m.interface, kProps) == 0) {
        DbusReader r = m.reader();
        const char* iname = "";
        const char* pname = "";
        bool get = strcmp(m.member, "Get") == 0 && sig_is(m, "ss");
        bool all = strcmp(m.member, "GetAll") == 0 && sig_is(m, "s");
        bool set = strcmp(m.member, "Set") == 0 && sig_is(m, "ssv");
        if (!get && !all && !set) {
            os_mutex_unlock(&c->lock);
            reply_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "Properties has Get(ss), GetAll(s) and Set(ssv)");
            return false;
        }
        r.str(&iname);
        if (!all) r.str(&pname);
        const DbusInterfaceDesc* d = find_iface(*e, iname);
        if (!d) {
            os_mutex_unlock(&c->lock);
            reply_error(c, m, "org.freedesktop.DBus.Error.UnknownInterface", "no such interface on that object");
            return false;
        }
        if (all) {
            if (has_live(d)) { os_mutex_unlock(&c->lock); return true; }
            DbusWriter w; w.begin("a{sv}"); w.array_begin();
            put_all(c, w, exp, d, nullptr, 0);
            w.array_end();
            os_mutex_unlock(&c->lock);
            reply_body(c, m, &w); w.free();
            return false;
        }
        const DbusPropertyDesc* p = find_prop(d, pname);
        if (!p) {
            os_mutex_unlock(&c->lock);
            reply_error(c, m, "org.freedesktop.DBus.Error.UnknownProperty", "no such property on that interface");
            return false;
        }
        if (set) {
            const char* vs = "";
            r.variant_begin(&vs);
            bool ok_sig = strcmp(vs, p->signature) == 0;
            os_mutex_unlock(&c->lock);
            if (p->access == DbusAccess::Read) { reply_error(c, m, "org.freedesktop.DBus.Error.PropertyReadOnly", "that property is read-only"); return false; }
            if (!ok_sig) { reply_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "a value of the wrong type for that property"); return false; }
            return true;
        }
        if (p->access == DbusAccess::Write) {
            os_mutex_unlock(&c->lock);
            reply_error(c, m, "org.freedesktop.DBus.Error.AccessDenied", "that property is write-only");
            return false;
        }
        if (p->live) { os_mutex_unlock(&c->lock); return true; }
        PropValue* v = find_value(c, exp, p);
        if (!v) {
            os_mutex_unlock(&c->lock);
            reply_error(c, m, "org.freedesktop.DBus.Error.Failed", "that property has no value yet");
            return false;
        }
        DbusWriter w; w.begin("v");
        put_value(w, *v);
        os_mutex_unlock(&c->lock);
        reply_body(c, m, &w); w.free();
        return false;
    }
    // A method of the object's own: described, with the signature it was described with.
    const DbusMethodDesc* found = nullptr;
    bool iface_known = m.interface[0] == 0;
    for (u32 i = 0; i < e->count && !found; ++i) {
        const DbusInterfaceDesc* d = e->ifaces[i];
        if (m.interface[0] && strcmp(d->name, m.interface) != 0) continue;
        iface_known = true;
        for (u32 k = 0; k < d->method_count; ++k)
            if (strcmp(d->methods[k].name, m.member) == 0) { found = &d->methods[k]; break; }
    }
    bool sig_ok = found && strcmp(found->in ? found->in : "", m.signature) == 0;
    os_mutex_unlock(&c->lock);
    if (!iface_known) { reply_error(c, m, "org.freedesktop.DBus.Error.UnknownInterface", "no such interface on that object"); return false; }
    if (!found) { reply_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "no such method on that interface"); return false; }
    if (!sig_ok) { reply_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments of the wrong types for that method"); return false; }
    return true;
}

// What came, sorted: a reply waited on, a reply nobody waits for any more, or the inbox.
void dispatch(DbusConnection* c, DbusMessage& m) {
    if (m.kind == DbusKind::Return || m.kind == DbusKind::Error) {
        os_mutex_lock(&c->lock);
        for (Wait& w : c->waits)
            if (w.used && !w.done && w.serial == m.reply_serial) {
                w.reply = m;
                w.done = true;
                os_cond_broadcast(&c->cond);
                os_mutex_unlock(&c->lock);
                return;
            }
        for (u32 a : c->abandoned)
            if (a && a == m.reply_serial) { os_mutex_unlock(&c->lock); dbus_message_free(&m); return; }
        os_mutex_unlock(&c->lock);
    } else if (m.kind == DbusKind::Call) {
        if (!handle_call(c, m)) { dbus_message_free(&m); return; }
    }
    os_mutex_lock(&c->lock);
    c->inbox.push(m);
    os_mutex_unlock(&c->lock);
    call_notify(c);
}

// Takes what has arrived apart into messages.
void take_messages(DbusConnection* c) {
    u32 used = 0;
    for (;;) {
        u8* b = c->rbuf.data + used;
        u32 have = c->rbuf.len - used;
        if (c->discard) {
            u64 n = c->discard < have ? c->discard : have;
            c->discard -= n;
            used += (u32)n;
            if (c->discard) break;
            continue;
        }
        const char* why = nullptr;
        i64 size = dbus_message_size(b, have, &why);
        if (size < 0) {
            char text[160];
            kv::format(text, sizeof text, "the bus sent bytes that cannot be framed as a message (%s)", why);
            lose(c, text);
            return;
        }
        if (size == 0) break;
        if ((u64)size > kDbusMaxMessage) {
            refuse(c, "a message past this client's 4 MiB, read past");
            c->discard = (u64)size;
            continue;
        }
        if ((u64)have < (u64)size) break;
        u32 nfds = 0;
        dbus_header_unix_fds(b, (u32)size, &nfds);
        Handle fds[kDbusMaxFds];
        u32 take = 0;
        while (take < nfds && take < c->rfd_count) {
            if (take < kDbusMaxFds) fds[take] = c->rfds[take];
            else net_close(c->rfds[take]);
            ++take;
        }
        memmove(c->rfds, c->rfds + take, sizeof(Handle) * (c->rfd_count - take));
        c->rfd_count -= take;
        DbusMessage m;
        if (!dbus_parse(b, (u32)size, fds, take < kDbusMaxFds ? take : kDbusMaxFds, &m, &why)) {
            refuse(c, why);
            dbus_message_free(&m);
        } else {
            dispatch(c, m);
        }
        used += (u32)size;
    }
    if (used) {
        memmove(c->rbuf.data, c->rbuf.data + used, c->rbuf.len - used);
        c->rbuf.len -= used;
    }
}

void read_some(DbusConnection* c) {
    for (;;) {
        c->rbuf.reserve(c->rbuf.len + 65536);
        Handle got[kNetMaxHandles];
        u32 n = 0;
        i32 k = c->sock.read_with(c->rbuf.data + c->rbuf.len, c->rbuf.cap - c->rbuf.len, got, kNetMaxHandles, &n);
        for (u32 i = 0; i < n; ++i) {
            if (c->rfd_count < kMaxQueuedFds) c->rfds[c->rfd_count++] = got[i];
            else net_close(got[i]);
        }
        if (k < 0) {
            char text[160];
            kv::format(text, sizeof text, "the bus closed the connection%s%s", c->sock.error ? ": " : "",
                       c->sock.error ? os_error_text(c->sock.error) : "");
            lose(c, text);
            return;
        }
        if (k == 0) return;
        c->rbuf.len += (u32)k;
        take_messages(c);
        if (!dbus_is_open(c)) return;
    }
}

// What was posted, onto the socket.
void send_posted(DbusConnection* c) {
    Array<u8> bytes;
    Array<Out> out;
    os_mutex_lock(&c->lock);
    // Swapped out whole: the thread sends from its copies while callers post to fresh ones.
    bytes = c->out_bytes;
    out = c->out;
    c->out_bytes = Array<u8>{};
    c->out = Array<Out>{};
    os_mutex_unlock(&c->lock);
    for (const Out& o : out) {
        bool ok;
        if (o.fd_count) ok = c->sock.write_now_with(bytes.data + o.at, o.len, o.fds, o.fd_count, 5.0);
        else if (c->sock.room() >= o.len) { ok = c->sock.write(bytes.data + o.at, o.len); c->sock.flush(); }
        else ok = c->sock.write_now(bytes.data + o.at, o.len, 5.0);
        if (!ok && !c->sock.is_open()) break;
    }
    bytes.free();
    out.free();
}

void run(void* arg) {
    DbusConnection* c = (DbusConnection*)arg;
    if (c->rbuf.len) take_messages(c); // what came in with the authentication's last line
    for (;;) {
        os_mutex_lock(&c->lock);
        bool stop = c->stop;
        bool open = c->open;
        os_mutex_unlock(&c->lock);
        if (stop) break;
        if (!open) {
            // Over: nothing to read, nothing to send. Asleep till dbus_close.
            os_mutex_lock(&c->lock);
            while (!c->stop) os_cond_wait(&c->cond, &c->lock);
            os_mutex_unlock(&c->lock);
            break;
        }
        send_posted(c);
        if (!c->sock.is_open()) {
            lose(c, "the bus closed the connection while we wrote to it");
            continue;
        }
        u8 r = net_wait(c->sock, c->sock.pending() != 0, &c->waker, -1.0);
        atomic_add_fetch(&c->wakes, (u64)1, MO_RELAXED);
        if (r & kNetWritable) c->sock.flush();
        if (r & kNetReadable) read_some(c);
    }
}

// ---- the authentication ---------------------------------------------------------------

bool read_line(NetSocket& s, char* line, u32 cap, f64 deadline, Array<u8>& rest) {
    u32 n = 0;
    for (;;) {
        f64 left = deadline - time_now();
        if (left <= 0.0) return false;
        char buf[256];
        i32 k = s.read(buf, sizeof buf, left);
        if (k < 0) return false;
        for (i32 i = 0; i < k; ++i) {
            if (n + 1 >= cap) return false;
            line[n++] = buf[i];
            if (n >= 2 && line[n - 2] == '\r' && line[n - 1] == '\n') {
                line[n - 2] = 0;
                if (i + 1 < k) memcpy(rest.push_n((u32)(k - i - 1)), buf + i + 1, (u32)(k - i - 1));
                return true;
            }
        }
    }
}

DbusConnection* start(NetSocket* s, f64 seconds, char* why, u32 cap) {
    f64 deadline = time_now() + (seconds > 0 ? seconds : 5.0);
    DbusConnection* c = (DbusConnection*)mem_alloc(sizeof(DbusConnection));
    new (c) DbusConnection();
    c->sock.adopt(s->fd);
    s->fd = Handle{};
    s->close();
    c->sock.out_limit = 4 * kDbusMaxMessage;
    os_mutex_init(&c->lock);
    os_cond_init(&c->cond);
    auto give_up = [&](const char* text) -> DbusConnection* {
        set_why(why, cap, text);
        c->sock.close();
        c->waker.close();
        c->rbuf.free();
        os_cond_destroy(&c->cond);
        os_mutex_destroy(&c->lock);
        c->~DbusConnection();
        mem_free(c);
        return nullptr;
    };
    i32 err = 0;
    if (!c->waker.open(&err)) return give_up("no waker for the connection's thread");

    // EXTERNAL: the bus knows who we are from the socket; we say the same, as hex digits
    // of our uid's decimal digits.
    char auth[96];
    char uid[24];
#if defined(_WIN32)
    kv::format(uid, sizeof uid, "0");
#else
    kv::format(uid, sizeof uid, "%u", (unsigned)getuid());
#endif
    char hex[48] = {};
    for (u32 i = 0; uid[i]; ++i) kv::format(hex + i * 2, 3, "%02x", (unsigned)(u8)uid[i]);
    i32 an = kv::format(auth, sizeof auth, "%cAUTH EXTERNAL %s\r\n", 0, hex);
    char line[512];
    if (!c->sock.write_now(auth, (u32)an, deadline - time_now())) return give_up("the bus would not take the authentication");
    if (!read_line(c->sock, line, sizeof line, deadline, c->rbuf)) return give_up("no answer to the authentication in time");
    if (strncmp(line, "OK ", 3) != 0) {
        char text[160];
        kv::format(text, sizeof text, "the bus refused EXTERNAL authentication: %.100s", line);
        return give_up(text);
    }
    const char* neg = "NEGOTIATE_UNIX_FD\r\n";
    if (!c->sock.write_now(neg, (u32)strlen(neg), deadline - time_now())) return give_up("the bus went during the authentication");
    if (!read_line(c->sock, line, sizeof line, deadline, c->rbuf)) return give_up("no answer to NEGOTIATE_UNIX_FD in time");
    c->fds_ok = strcmp(line, "AGREE_UNIX_FD") == 0;
    const char* begin = "BEGIN\r\n";
    if (!c->sock.write_now(begin, (u32)strlen(begin), deadline - time_now())) return give_up("the bus went during the authentication");

    c->open = true;
    if (!os_thread_start(&c->thread, run, c)) { c->open = false; return give_up("no thread for the connection"); }
    c->running = true;

    DbusHeader h;
    h.destination = kBus;
    h.path = kBusPath;
    h.interface = kBus;
    h.member = "Hello";
    DbusMessage reply;
    char hw[160] = {};
    if (!dbus_call(c, h, nullptr, deadline - time_now(), &reply, hw, sizeof hw)) {
        char text[200];
        kv::format(text, sizeof text, "Hello was not answered: %s", hw);
        set_why(why, cap, text);
        dbus_close(c);
        return nullptr;
    }
    const char* name = nullptr;
    DbusReader r = reply.reader();
    bool ok = reply.kind == DbusKind::Return && r.str(&name) && dbus_valid_bus_name(name);
    if (ok) kv::format(c->unique, sizeof c->unique, "%s", name);
    dbus_message_free(&reply);
    if (!ok) {
        set_why(why, cap, "Hello was answered with no unique name");
        dbus_close(c);
        return nullptr;
    }
    return c;
}

} // namespace

// ---- the interface --------------------------------------------------------------------

DbusConnection* dbus_open_socket(NetSocket* s, f64 seconds, char* why, u32 cap) {
    if (!s->is_open()) { set_why(why, cap, "the socket is not open"); return nullptr; }
    return start(s, seconds, why, cap);
}

DbusConnection* dbus_open_address(const char* address, f64 seconds, char* why, u32 cap) {
#if !defined(__linux__)
    (void)address; (void)seconds;
    set_why(why, cap, "D-Bus is Linux's here: this OS's media surface is its own");
    return nullptr;
#else
    DbusAddress list[8];
    u32 n = 0;
    const char* w = nullptr;
    if (!dbus_parse_address(address, list, 8, &n, &w)) { set_why(why, cap, w); return nullptr; }
    char last[160] = "no address to try";
    f64 deadline = time_now() + (seconds > 0 ? seconds : 5.0);
    for (u32 i = 0; i < n; ++i) {
        NetSocket s;
        i32 err = 0;
        if (!net_connect_unix_now(list[i].path, list[i].abstract ? NetUnixName::Abstract : NetUnixName::Path, &s,
                                  deadline - time_now(), &err)) {
            kv::format(last, sizeof last, "%s%s: %s", list[i].abstract ? "@" : "", list[i].path, os_error_text(err));
            continue;
        }
        return start(&s, deadline - time_now(), why, cap);
    }
    set_why(why, cap, last);
    return nullptr;
#endif
}

DbusConnection* dbus_open(DbusBus bus, f64 seconds, char* why, u32 cap) {
    char address[512];
    const char* env = getenv(bus == DbusBus::Session ? "DBUS_SESSION_BUS_ADDRESS" : "DBUS_SYSTEM_BUS_ADDRESS");
    if (env && env[0]) kv::format(address, sizeof address, "%s", env);
    else if (bus == DbusBus::System) kv::format(address, sizeof address, "unix:path=/var/run/dbus/system_bus_socket");
    else {
        const char* run = getenv("XDG_RUNTIME_DIR");
        if (!run || !run[0]) { set_why(why, cap, "no session bus: neither DBUS_SESSION_BUS_ADDRESS nor XDG_RUNTIME_DIR is set"); return nullptr; }
        kv::format(address, sizeof address, "unix:path=%s/bus", run);
    }
    return dbus_open_address(address, seconds, why, cap);
}

void dbus_close(DbusConnection* c) {
    if (!c) return;
    os_mutex_lock(&c->lock);
    c->stop = true;
    os_cond_broadcast(&c->cond);
    os_mutex_unlock(&c->lock);
    c->waker.wake();
    if (c->running) os_thread_join(&c->thread);
    c->sock.close();
    c->waker.close();
    for (DbusMessage& m : c->inbox) dbus_message_free(&m);
    c->inbox.free();
    for (Wait& w : c->waits)
        if (w.used && w.done) dbus_message_free(&w.reply);
    for (u32 i = 0; i < c->rfd_count; ++i) net_close(c->rfds[i]);
    for (PropValue& v : c->props) v.bytes.free();
    c->props.free();
    c->rbuf.free();
    c->out_bytes.free();
    c->out.free();
    os_cond_destroy(&c->cond);
    os_mutex_destroy(&c->lock);
    c->~DbusConnection();
    mem_free(c);
}

const char* dbus_unique_name(DbusConnection* c) { return c->unique; }
u64         dbus_thread_wakes(DbusConnection* c) { return atomic_load(&c->wakes, MO_RELAXED); }
bool        dbus_can_pass_fds(DbusConnection* c) { return c->fds_ok; }

bool dbus_is_open(DbusConnection* c) {
    os_mutex_lock(&c->lock);
    bool o = c->open;
    os_mutex_unlock(&c->lock);
    return o;
}

const char* dbus_lost_why(DbusConnection* c) { return dbus_is_open(c) ? "" : c->lost; }

u32 dbus_refused(DbusConnection* c, char* why, u32 cap) {
    os_mutex_lock(&c->lock);
    u32 n = c->refused;
    if (why && cap) kv::format(why, cap, "%s", c->refused_why);
    os_mutex_unlock(&c->lock);
    return n;
}

void dbus_set_notify(DbusConnection* c, void (*fn)(void*), void* user) {
    os_mutex_lock(&c->lock);
    c->notify = fn;
    c->notify_user = user;
    os_mutex_unlock(&c->lock);
}

u32 dbus_send(DbusConnection* c, const DbusHeader& h, const DbusWriter* body) { return post(c, h, body); }

bool dbus_next(DbusConnection* c, DbusMessage* out) {
    os_mutex_lock(&c->lock);
    bool have = c->inbox.len != 0;
    if (have) {
        *out = c->inbox.data[0];
        memmove(c->inbox.data, c->inbox.data + 1, sizeof(DbusMessage) * (c->inbox.len - 1));
        c->inbox.len--;
    }
    os_mutex_unlock(&c->lock);
    return have;
}

bool dbus_call(DbusConnection* c, const DbusHeader& h, const DbusWriter* body, f64 seconds, DbusMessage* reply,
               char* why, u32 cap) {
    *reply = DbusMessage{};
    u32 serial = next_serial(c);
    Array<u8> bytes;
    const char* w = nullptr;
    DbusHeader hh = h;
    hh.kind = DbusKind::Call;
    hh.flags &= (u8)~kDbusNoReplyExpected;
    if (!dbus_encode(hh, serial, body, &bytes, &w)) { set_why(why, cap, w); bytes.free(); return false; }
    os_mutex_lock(&c->lock);
    Wait* slot = nullptr;
    for (Wait& x : c->waits)
        if (!x.used) { slot = &x; break; }
    if (!slot || !c->open) {
        bool open = c->open;
        os_mutex_unlock(&c->lock);
        bytes.free();
        set_why(why, cap, open ? "too many calls waiting at once" : c->lost);
        return false;
    }
    slot->used = true;
    slot->done = false;
    slot->serial = serial;
    os_mutex_unlock(&c->lock);
    bool posted = post_bytes(c, bytes, body);
    bytes.free();

    f64 deadline = time_now() + (seconds > 0 ? seconds : 0.0);
    os_mutex_lock(&c->lock);
    while (posted && !slot->done && c->open) {
        f64 left = deadline - time_now();
        if (left <= 0.0) break;
        os_cond_timed_wait(&c->cond, &c->lock, left);
    }
    bool done = slot->done;
    if (done) *reply = slot->reply;
    else {
        c->abandoned[c->abandoned_at++ % kAbandoned] = serial;
        if (!c->open) set_why(why, cap, c->lost);
        else {
            char text[96];
            kv::format(text, sizeof text, "no answer in %.1f s", seconds);
            set_why(why, cap, text);
        }
    }
    slot->used = false;
    slot->done = false;
    slot->reply = DbusMessage{};
    os_mutex_unlock(&c->lock);
    return done;
}

bool dbus_reply(DbusConnection* c, const DbusMessage& call, const DbusWriter* body) {
    if (call.kind != DbusKind::Call || (call.flags & kDbusNoReplyExpected)) return true;
    DbusHeader h;
    h.kind = DbusKind::Return;
    h.reply_serial = call.serial;
    h.destination = call.sender[0] ? call.sender : nullptr;
    return post(c, h, body) != 0;
}

bool dbus_reply_error(DbusConnection* c, const DbusMessage& call, const char* name, const char* text) {
    if (call.kind != DbusKind::Call || (call.flags & kDbusNoReplyExpected)) return true;
    DbusHeader h;
    h.kind = DbusKind::Error;
    h.error_name = name;
    h.reply_serial = call.serial;
    h.destination = call.sender[0] ? call.sender : nullptr;
    DbusWriter w;
    w.begin("s");
    w.str(text ? text : "");
    bool ok = post(c, h, &w) != 0;
    w.free();
    return ok;
}

u32 dbus_signal(DbusConnection* c, const char* path, const char* interface, const char* member, const DbusWriter* body) {
    DbusHeader h;
    h.kind = DbusKind::Signal;
    h.path = path;
    h.interface = interface;
    h.member = member;
    return post(c, h, body);
}

namespace {

bool bus_call(DbusConnection* c, const char* member, const DbusWriter* body, f64 seconds, DbusMessage* reply) {
    DbusHeader h;
    h.destination = kBus;
    h.path = kBusPath;
    h.interface = kBus;
    h.member = member;
    char why[160];
    if (!dbus_call(c, h, body, seconds, reply, why, sizeof why)) {
        KV_WARN("dbus", "%s: %s", member, why);
        return false;
    }
    if (reply->kind == DbusKind::Error) {
        KV_WARN("dbus", "%s: %s: %s", member, reply->error_name, reply->error_text());
        dbus_message_free(reply);
        return false;
    }
    return true;
}

} // namespace

DbusNameReply dbus_request_name(DbusConnection* c, const char* name, u32 flags, f64 seconds) {
    if (!dbus_valid_bus_name(name) || name[0] == ':') return DbusNameReply::Failed;
    DbusWriter w;
    w.begin("su");
    w.str(name);
    w.u32(flags);
    DbusMessage reply;
    bool ok = bus_call(c, "RequestName", &w, seconds, &reply);
    w.free();
    if (!ok) return DbusNameReply::Failed;
    u32 v = 0;
    DbusReader r = reply.reader();
    ok = r.u32(&v) && v >= 1 && v <= 4;
    dbus_message_free(&reply);
    return ok ? (DbusNameReply)v : DbusNameReply::Failed;
}

bool dbus_release_name(DbusConnection* c, const char* name, f64 seconds) {
    DbusWriter w;
    w.begin("s");
    w.str(name);
    DbusMessage reply;
    bool ok = bus_call(c, "ReleaseName", &w, seconds, &reply);
    w.free();
    if (ok) dbus_message_free(&reply);
    return ok;
}

bool dbus_add_match(DbusConnection* c, const char* rule, f64 seconds) {
    DbusWriter w;
    w.begin("s");
    w.str(rule);
    DbusMessage reply;
    bool ok = bus_call(c, "AddMatch", &w, seconds, &reply);
    w.free();
    if (ok) dbus_message_free(&reply);
    return ok;
}

bool dbus_remove_match(DbusConnection* c, const char* rule, f64 seconds) {
    DbusWriter w;
    w.begin("s");
    w.str(rule);
    DbusMessage reply;
    bool ok = bus_call(c, "RemoveMatch", &w, seconds, &reply);
    w.free();
    if (ok) dbus_message_free(&reply);
    return ok;
}

bool dbus_export(DbusConnection* c, const char* path, const DbusInterfaceDesc* const* ifaces, u32 count) {
    if (!dbus_valid_path(path) || strlen(path) >= 256 || count > kMaxInterfaces) return false;
    for (u32 i = 0; i < count; ++i)
        if (!ifaces[i] || !dbus_valid_interface(ifaces[i]->name)) return false;
    os_mutex_lock(&c->lock);
    Export* e = find_export(c, path);
    if (!e)
        for (Export& x : c->exports)
            if (!x.used) { e = &x; break; }
    bool ok = e != nullptr;
    if (ok) {
        u32 exp = (u32)(e - c->exports);
        // A value set for an interface it no longer has goes.
        for (u32 i = 0; i < c->props.len;) {
            PropValue& v = c->props.data[i];
            bool keep = v.exp != exp;
            for (u32 k = 0; k < count && !keep; ++k) keep = v.iface == ifaces[k];
            if (keep) { ++i; continue; }
            v.bytes.free();
            c->props.data[i] = c->props.data[--c->props.len];
        }
        kv::format(e->path, sizeof e->path, "%s", path);
        for (u32 i = 0; i < count; ++i) e->ifaces[i] = ifaces[i];
        e->count = count;
        e->used = true;
    }
    os_mutex_unlock(&c->lock);
    return ok;
}

bool dbus_unexport(DbusConnection* c, const char* path) {
    os_mutex_lock(&c->lock);
    Export* e = find_export(c, path);
    if (e) {
        u32 exp = (u32)(e - c->exports);
        for (u32 i = 0; i < c->props.len;) {
            if (c->props.data[i].exp != exp) { ++i; continue; }
            c->props.data[i].bytes.free();
            c->props.data[i] = c->props.data[--c->props.len];
        }
        *e = Export{};
    }
    os_mutex_unlock(&c->lock);
    return e != nullptr;
}

bool dbus_properties_set(DbusConnection* c, const char* path, const char* interface, const DbusPropertyValue* values,
                         u32 count, bool quiet) {
    DbusWriter sig;
    sig.begin("sa{sv}as");
    sig.str(interface);
    const char* invalidated[64];
    u32 ninval = 0;
    u32 changed = 0;
    os_mutex_lock(&c->lock);
    Export* e = find_export(c, path);
    const DbusInterfaceDesc* d = e ? find_iface(*e, interface) : nullptr;
    bool ok = d != nullptr;
    for (u32 i = 0; i < count && ok; ++i) {
        const DbusPropertyDesc* p = find_prop(d, values[i].name);
        ok = p && values[i].value && values[i].value->done() && strcmp(values[i].value->sig, p->signature) == 0 && !p->live;
    }
    if (ok) {
        u32 exp = (u32)(e - c->exports);
        sig.array_begin();
        for (u32 i = 0; i < count; ++i) {
            const DbusPropertyDesc* p = find_prop(d, values[i].name);
            const DbusWriter& v = *values[i].value;
            PropValue* pv = find_value(c, exp, p);
            bool same = pv && pv->bytes.len == v.bytes.len && memcmp(pv->bytes.data, v.bytes.data, v.bytes.len) == 0;
            if (same) continue;
            if (!pv) {
                PropValue n;
                n.exp = exp;
                n.iface = d;
                n.prop = p;
                pv = &c->props.push(n);
            }
            pv->bytes.clear();
            if (v.bytes.len) memcpy(pv->bytes.push_n(v.bytes.len), v.bytes.data, v.bytes.len);
            if (p->emits == DbusEmits::Changed) {
                sig.entry_begin(); sig.str(p->name); put_value(sig, *pv); sig.entry_end();
                ++changed;
            } else if (p->emits == DbusEmits::Invalidates && ninval < 64) {
                invalidated[ninval++] = p->name;
            }
        }
        sig.array_end();
    }
    os_mutex_unlock(&c->lock);
    if (ok && !quiet && (changed || ninval)) {
        sig.array_begin();
        for (u32 i = 0; i < ninval; ++i) sig.str(invalidated[i]);
        sig.array_end();
        dbus_signal(c, path, kProps, "PropertiesChanged", &sig);
    }
    sig.free();
    return ok;
}

bool dbus_property_set(DbusConnection* c, const char* path, const char* interface, const char* name,
                       const DbusWriter& value, bool quiet) {
    DbusPropertyValue v{name, &value};
    return dbus_properties_set(c, path, interface, &v, 1, quiet);
}

bool dbus_reply_get_all(DbusConnection* c, const DbusMessage& call, const DbusPropertyValue* live, u32 count) {
    DbusReader r = call.reader();
    const char* iname = "";
    if (!sig_is(call, "s") || !r.str(&iname)) return dbus_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "GetAll takes an interface's name");
    DbusWriter w;
    w.begin("a{sv}");
    w.array_begin();
    os_mutex_lock(&c->lock);
    Export* e = find_export(c, call.path);
    const DbusInterfaceDesc* d = e ? find_iface(*e, iname) : nullptr;
    if (d) put_all(c, w, (u32)(e - c->exports), d, live, count);
    os_mutex_unlock(&c->lock);
    w.array_end();
    bool ok = d ? dbus_reply(c, call, &w) : dbus_reply_error(c, call, "org.freedesktop.DBus.Error.UnknownInterface", "no such interface on that object");
    w.free();
    return ok;
}

} // namespace kv
