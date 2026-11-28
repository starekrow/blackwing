// Messages: the fixed header, the header's fields, and the body, checked whole before the
// message is given to anyone. And bus addresses, which are text.
#include "korvid/dbus/dbus.h"
#include "wire_internal.h"
#include "korvid/core/memory.h"
#include "korvid/platform/net.h"
#include <string.h>

namespace kv {

namespace {

constexpr u8 kFieldPath = 1, kFieldInterface = 2, kFieldMember = 3, kFieldErrorName = 4, kFieldReplySerial = 5,
             kFieldDestination = 6, kFieldSender = 7, kFieldSignature = 8, kFieldUnixFds = 9;

u32 read32(const u8* p, bool big) {
    u32 v;
    memcpy(&v, p, 4);
    return big ? __builtin_bswap32(v) : v;
}

bool refuse(const char** why, const char* text) {
    if (why) *why = text;
    return false;
}

} // namespace

i64 dbus_message_size(const u8* b, u32 have, const char** why) {
    if (have < 16) return 0;
    if (b[0] != 'l' && b[0] != 'B') { if (why) *why = "a byte order that is neither 'l' nor 'B'"; return -1; }
    if (b[3] != 1) { if (why) *why = "a protocol version that is not 1"; return -1; }
    bool big = b[0] == 'B';
    u64 body = read32(b + 4, big);
    u64 fields = read32(b + 12, big);
    if (fields > kDbusMaxArray) { if (why) *why = "header fields past the spec's 64 MiB"; return -1; }
    u64 header_end = 16 + fields;
    u64 body_start = (header_end + 7) & ~(u64)7;
    u64 total = body_start + body;
    if (total > kDbusSpecMaxMessage) { if (why) *why = "a message past the spec's 128 MiB"; return -1; }
    return (i64)total;
}

bool dbus_parse(const u8* bytes, u32 len, const Handle* fds, u32 fd_count, DbusMessage* out, const char** why) {
    *out = DbusMessage{};
    for (u32 i = 0; i < fd_count; ++i) {
        if (out->fd_count < kDbusMaxFds) out->fds[out->fd_count++] = fds[i];
        else net_close(fds[i]);
    }
    const char* w = nullptr;
    i64 size = dbus_message_size(bytes, len, &w);
    if (size < 0) return refuse(why, w);
    if (size == 0 || (u64)size != len) return refuse(why, "a message whose lengths do not add up to its bytes");
    if (len > kDbusMaxMessage) return refuse(why, "a message past this client's 4 MiB");
    out->storage = (u8*)mem_alloc(len);
    out->storage_len = len;
    memcpy(out->storage, bytes, len);
    const u8* b = out->storage;
    bool big = b[0] == 'B';
    out->big_endian = big;
    if (b[1] < 1 || b[1] > 4) return refuse(why, "a message type this client does not know");
    out->kind = (DbusKind)b[1];
    out->flags = b[2];
    out->body_len = read32(b + 4, big);
    out->serial = read32(b + 8, big);
    if (out->serial == 0) return refuse(why, "a serial of 0");
    u32 fields = read32(b + 12, big);
    u32 header_end = 16 + fields;
    u32 body_start = (header_end + 7) & ~7u;

    bool little_host = true; // every machine Blackwing runs on
    bool swap = big == little_host;
    DbusReader r;
    r.begin(b, header_end, "a(yv)", swap, 12);
    u32 seen = 0;
    u32 unix_fds = 0;
    bool has_sig = false;
    if (r.array_begin()) {
        while (r.array_more()) {
            u8 code = 0;
            const char* vs = nullptr;
            if (!r.struct_begin() || !r.byte(&code) || !r.variant_begin(&vs)) break;
            if (code >= 1 && code <= 9) {
                if (seen & (1u << code)) return refuse(why, "a header field given twice");
                seen |= 1u << code;
            }
            const char* want = code == kFieldPath ? "o"
                             : code == kFieldReplySerial || code == kFieldUnixFds ? "u"
                             : code == kFieldSignature ? "g"
                             : code >= 1 && code <= 9 ? "s" : nullptr;
            if (want && strcmp(vs, want) != 0) return refuse(why, "a header field of the wrong type");
            const char* s = nullptr;
            switch (code) {
            case kFieldPath: if (r.path(&s)) out->path = s; break;
            case kFieldInterface:
                if (r.str(&s)) { if (!dbus_valid_interface(s)) return refuse(why, "an interface name that is not one"); out->interface = s; }
                break;
            case kFieldMember:
                if (r.str(&s)) { if (!dbus_valid_member(s)) return refuse(why, "a member name that is not one"); out->member = s; }
                break;
            case kFieldErrorName:
                if (r.str(&s)) { if (!dbus_valid_interface(s)) return refuse(why, "an error name that is not one"); out->error_name = s; }
                break;
            case kFieldReplySerial: r.u32(&out->reply_serial); break;
            case kFieldDestination:
                if (r.str(&s)) { if (!dbus_valid_bus_name(s)) return refuse(why, "a destination that is not a bus name"); out->destination = s; }
                break;
            case kFieldSender:
                if (r.str(&s)) { if (!dbus_valid_bus_name(s)) return refuse(why, "a sender that is not a bus name"); out->sender = s; }
                break;
            case kFieldSignature: if (r.signature(&s)) { out->signature = s; has_sig = true; } break;
            case kFieldUnixFds: r.u32(&unix_fds); break;
            default: r.skip(); break; // a field this client does not know: the spec says to let it be
            }
            if (!r.variant_end() || !r.struct_end()) break;
        }
    }
    if (r.failed) return refuse(why, r.error);
    if (r.pos != header_end) return refuse(why, "header fields that do not fill their length");
    for (u32 i = header_end; i < body_start; ++i)
        if (b[i] != 0) return refuse(why, "padding before the body that is not zero");
    (void)has_sig;

    switch (out->kind) {
    case DbusKind::Call:
        if (!(seen & (1u << kFieldPath)) || !(seen & (1u << kFieldMember))) return refuse(why, "a call with no path or member");
        break;
    case DbusKind::Signal:
        if (!(seen & (1u << kFieldPath)) || !(seen & (1u << kFieldInterface)) || !(seen & (1u << kFieldMember)))
            return refuse(why, "a signal with no path, interface or member");
        break;
    case DbusKind::Error:
        if (!(seen & (1u << kFieldErrorName))) return refuse(why, "an error with no name");
        [[fallthrough]];
    case DbusKind::Return:
        if (!(seen & (1u << kFieldReplySerial)) || out->reply_serial == 0) return refuse(why, "a reply with no serial it replies to");
        break;
    default: break;
    }
    if (unix_fds != out->fd_count) return refuse(why, "a message whose descriptors are not the number it says");

    out->body = b + body_start;
    DbusReader body;
    body.begin(out->body, out->body_len, out->signature, swap, 0, out->fd_count);
    while (body.peek())
        if (!body.skip()) break;
    if (body.failed) return refuse(why, body.error);
    if (!body.at_end()) return refuse(why, "a body longer than its signature");
    return true;
}

bool dbus_header_unix_fds(const u8* b, u32 len, u32* n) {
    *n = 0;
    if (len < 16) return false;
    bool big = b[0] == 'B';
    u32 header_end = 16 + read32(b + 12, big);
    if (header_end > len) return false;
    DbusReader r;
    r.begin(b, header_end, "a(yv)", big, 12);
    if (!r.array_begin()) return false;
    while (r.array_more()) {
        u8 code = 0;
        const char* vs = nullptr;
        if (!r.struct_begin() || !r.byte(&code) || !r.variant_begin(&vs)) return false;
        if (code == kFieldUnixFds && strcmp(vs, "u") == 0) { if (!r.u32(n)) return false; }
        else if (!r.skip()) return false;
        if (!r.variant_end() || !r.struct_end()) return false;
    }
    return !r.failed;
}

DbusReader DbusMessage::reader() const {
    DbusReader r;
    r.begin(body, body_len, signature, big_endian, 0, fd_count);
    return r;
}

const char* DbusMessage::error_text() const {
    if (signature[0] != 's') return "";
    DbusReader r = reader();
    const char* s = "";
    return r.str(&s) ? s : "";
}

Handle dbus_message_take_fd(DbusMessage* m, u32 i) {
    if (i >= m->fd_count) return Handle{};
    Handle h = m->fds[i];
    m->fds[i] = Handle{};
    return h;
}

void dbus_message_free(DbusMessage* m) {
    for (u32 i = 0; i < m->fd_count; ++i)
        if (os_handle_open(m->fds[i])) net_close(m->fds[i]);
    if (m->storage) mem_free(m->storage);
    *m = DbusMessage{};
}

bool dbus_encode(const DbusHeader& h, u32 serial, const DbusWriter* body, Array<u8>* out, const char** why) {
    if (body && !body->done()) return refuse(why, body->error ? body->error : "a body not finished");
    if (serial == 0) return refuse(why, "a serial of 0");
    switch (h.kind) {
    case DbusKind::Call: if (!h.path || !h.member) return refuse(why, "a call needs a path and a member"); break;
    case DbusKind::Signal: if (!h.path || !h.interface || !h.member) return refuse(why, "a signal needs a path, an interface and a member"); break;
    case DbusKind::Error: if (!h.error_name) return refuse(why, "an error needs a name"); [[fallthrough]];
    case DbusKind::Return: if (!h.reply_serial) return refuse(why, "a reply needs the serial it replies to"); break;
    default: return refuse(why, "a message of no kind");
    }
    if (h.path && !dbus_valid_path(h.path)) return refuse(why, "a path that is not one");
    if (h.interface && !dbus_valid_interface(h.interface)) return refuse(why, "an interface name that is not one");
    if (h.member && !dbus_valid_member(h.member)) return refuse(why, "a member name that is not one");
    if (h.error_name && !dbus_valid_interface(h.error_name)) return refuse(why, "an error name that is not one");
    if (h.destination && !dbus_valid_bus_name(h.destination)) return refuse(why, "a destination that is not a bus name");

    DbusWriter w;
    w.begin("yyyyuua(yv)");
    w.byte('l');
    w.byte((u8)h.kind);
    w.byte(h.flags);
    w.byte(1);
    w.u32(body ? body->bytes.len : 0);
    w.u32(serial);
    w.array_begin();
    auto field_s = [&](u8 code, const char* sig, const char* v) {
        if (!v) return;
        w.struct_begin(); w.byte(code); w.variant_begin(sig);
        if (sig[0] == 'o') w.path(v); else if (sig[0] == 'g') w.signature(v); else w.str(v);
        w.variant_end(); w.struct_end();
    };
    auto field_u = [&](u8 code, u32 v) {
        w.struct_begin(); w.byte(code); w.variant_begin("u"); w.u32(v); w.variant_end(); w.struct_end();
    };
    field_s(kFieldPath, "o", h.path);
    field_s(kFieldInterface, "s", h.interface);
    field_s(kFieldMember, "s", h.member);
    field_s(kFieldErrorName, "s", h.error_name);
    if (h.reply_serial) field_u(kFieldReplySerial, h.reply_serial);
    field_s(kFieldDestination, "s", h.destination);
    if (body && body->sig_len) field_s(kFieldSignature, "g", body->sig);
    if (body && body->fd_count) field_u(kFieldUnixFds, body->fd_count);
    w.array_end();
    if (!w.done()) { w.free(); return refuse(why, w.error ? w.error : "the header would not encode"); }
    while (w.bytes.len % 8) w.bytes.push(0);
    u32 total = w.bytes.len + (body ? body->bytes.len : 0);
    if (total > kDbusMaxMessage) { w.free(); return refuse(why, "a message past this client's 4 MiB"); }
    out->clear();
    memcpy(out->push_n(w.bytes.len), w.bytes.data, w.bytes.len);
    if (body && body->bytes.len) memcpy(out->push_n(body->bytes.len), body->bytes.data, body->bytes.len);
    w.free();
    return true;
}

// ---- addresses ------------------------------------------------------------------------

namespace {

int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Undoes %XX in [p, e) into out; false for a bad escape or a value too long.
bool unescape(const char* p, const char* e, char* out, u32 cap) {
    u32 n = 0;
    while (p < e) {
        char ch = *p++;
        if (ch == '%') {
            if (e - p < 2 || hexval(p[0]) < 0 || hexval(p[1]) < 0) return false;
            ch = (char)(hexval(p[0]) * 16 + hexval(p[1]));
            p += 2;
        }
        if (ch == 0 || n + 1 >= cap) return false;
        out[n++] = ch;
    }
    out[n] = 0;
    return true;
}

} // namespace

bool dbus_parse_address(const char* address, DbusAddress* out, u32 cap, u32* count, const char** why) {
    *count = 0;
    const char* last_why = "an empty address";
    const char* p = address ? address : "";
    while (*p) {
        const char* end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        const char* colon = (const char*)memchr(p, ':', (usize)(end - p));
        if (!colon) { last_why = "an address entry with no transport"; p = *end ? end + 1 : end; continue; }
        bool unix_ = (colon - p) == 4 && memcmp(p, "unix", 4) == 0;
        if (!unix_) { last_why = "a transport other than unix: (tcp, unixexec, launchd), which this client does not take"; p = *end ? end + 1 : end; continue; }
        DbusAddress a;
        bool have = false, bad = false, listening = false;
        const char* k = colon + 1;
        while (k < end) {
            const char* comma = (const char*)memchr(k, ',', (usize)(end - k));
            if (!comma) comma = end;
            const char* eq = (const char*)memchr(k, '=', (usize)(comma - k));
            if (!eq) { bad = true; break; }
            usize kl = (usize)(eq - k);
            bool is_path = kl == 4 && memcmp(k, "path", 4) == 0;
            bool is_abs = kl == 8 && memcmp(k, "abstract", 8) == 0;
            if (is_path || is_abs) {
                if (have || !unescape(eq + 1, comma, a.path, sizeof a.path) || !a.path[0]) { bad = true; break; }
                a.abstract = is_abs;
                have = true;
            } else if ((kl == 3 && memcmp(k, "dir", 3) == 0) || (kl == 6 && memcmp(k, "tmpdir", 6) == 0) ||
                       (kl == 7 && memcmp(k, "runtime", 7) == 0)) {
                listening = bad = true;
                break;
            } // guid=, and keys this client does not know, are let be
            k = comma < end ? comma + 1 : end;
        }
        if (!bad && have) {
            if (*count < cap) out[(*count)++] = a;
        } else if (listening) last_why = "a unix: address for listening (dir=, tmpdir=, runtime=), not for connecting";
        else if (bad) last_why = "a unix: address that cannot be read";
        else last_why = "a unix: address with no path= or abstract=";
        p = *end ? end + 1 : end;
    }
    if (*count == 0) return refuse(why, last_why);
    return true;
}

} // namespace kv
