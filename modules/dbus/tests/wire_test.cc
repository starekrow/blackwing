// The wire format against bytes worked out by hand from the specification, names and
// signatures at their edges, and messages each way, in both byte orders.
#include "korvid/dbus/dbus.h"
#include "korvid/core/test.h"
#include <string.h>

using namespace kv;

namespace {

bool bytes_are(const DbusWriter& w, const u8* want, u32 n) {
    return w.bytes.len == n && memcmp(w.bytes.data, want, n) == 0;
}

// A big-endian message, by hand: what a big-endian peer would send.
struct Be {
    u8  b[256] = {};
    u32 n = 0;
    void pad(u32 a) { while (n % a) b[n++] = 0; }
    void y(u8 v) { b[n++] = v; }
    void u(u32 v) { pad(4); b[n++] = (u8)(v >> 24); b[n++] = (u8)(v >> 16); b[n++] = (u8)(v >> 8); b[n++] = (u8)v; }
    void s(const char* t) { u32 l = (u32)strlen(t); u(l); memcpy(b + n, t, l + 1); n += l + 1; }
    void g(const char* t) { u32 l = (u32)strlen(t); y((u8)l); memcpy(b + n, t, l + 1); n += l + 1; }
};

} // namespace

void test_names(TestCtx& c) {
    c.begin("dbus, names and signatures");
    KV_CHECK(c, dbus_valid_path("/") && dbus_valid_path("/org/mpris/MediaPlayer2") && dbus_valid_path("/a_1/B2"));
    KV_CHECK(c, !dbus_valid_path("") && !dbus_valid_path("a") && !dbus_valid_path("/a/") && !dbus_valid_path("//a") && !dbus_valid_path("/a-b"));
    KV_CHECK(c, dbus_valid_interface("org.mpris.MediaPlayer2.Player") && dbus_valid_interface("a._b"));
    KV_CHECK(c, !dbus_valid_interface("org") && !dbus_valid_interface("org.") && !dbus_valid_interface("org.2a") && !dbus_valid_interface("a..b"));
    KV_CHECK(c, dbus_valid_member("Seeked") && dbus_valid_member("_x9") && !dbus_valid_member("9x") && !dbus_valid_member("a.b") && !dbus_valid_member(""));
    KV_CHECK(c, dbus_valid_bus_name(":1.42") && dbus_valid_bus_name("org.mpris.MediaPlayer2.blackwing") && dbus_valid_bus_name("a-b.c"));
    KV_CHECK(c, dbus_valid_bus_name(":1.2a") && !dbus_valid_bus_name("1a.b") && !dbus_valid_bus_name(":1") && !dbus_valid_bus_name("a"));
    char longname[300];
    memset(longname, 'a', sizeof longname);
    longname[1] = '.';
    longname[299] = 0;
    KV_CHECK(c, !dbus_valid_interface(longname) && !dbus_valid_bus_name(longname));

    KV_CHECK(c, dbus_valid_signature("") && dbus_valid_signature("sa{sv}as") && dbus_valid_signature("a(ii)") && dbus_valid_signature("aa{s(xv)}h"));
    KV_CHECK(c, dbus_valid_signature("v", true) && !dbus_valid_signature("ss", true) && !dbus_valid_signature("", true));
    KV_CHECK(c, !dbus_valid_signature("a") && !dbus_valid_signature("()") && !dbus_valid_signature("(i") && !dbus_valid_signature("{sv}"));
    KV_CHECK(c, !dbus_valid_signature("a{vs}") && !dbus_valid_signature("a{s}") && !dbus_valid_signature("a{sss}") && !dbus_valid_signature("z"));
    char deep[80] = {};
    for (u32 i = 0; i < 32; ++i) deep[i] = 'a';
    deep[32] = 'y';
    KV_CHECK(c, dbus_valid_signature(deep)); // 32 arrays: the most
    deep[32] = 'a'; deep[33] = 'y';
    KV_CHECK(c, !dbus_valid_signature(deep));
    char structs[80] = {};
    for (u32 i = 0; i < 33; ++i) { structs[i] = '('; structs[34 + i] = ')'; }
    structs[33] = 'y';
    KV_CHECK(c, !dbus_valid_signature(structs)); // 33 structs
    char too_long[300];
    memset(too_long, 'y', 256);
    too_long[256] = 0;
    KV_CHECK(c, !dbus_valid_signature(too_long));
}

void test_wire(TestCtx& c) {
    c.begin("dbus, the wire format");
    DbusWriter w;

    // Alignment from the start: a byte, then an int at 4.
    KV_CHECK(c, w.begin("yi"));
    w.byte(1); w.i32(2);
    const u8 yi[] = {1, 0, 0, 0, 2, 0, 0, 0};
    KV_CHECK(c, w.done() && bytes_are(w, yi, sizeof yi));

    // An empty array of int64: its length, then padding to 8 for elements that are not there.
    KV_CHECK(c, w.begin("ax"));
    w.array_begin(); w.array_end();
    const u8 ax[] = {0, 0, 0, 0, 0, 0, 0, 0};
    KV_CHECK(c, w.done() && bytes_are(w, ax, sizeof ax));
    // The same after a byte: the length at 4, the padding to 8, and the length 0 still.
    KV_CHECK(c, w.begin("yax"));
    w.byte(9); w.array_begin(); w.array_end();
    const u8 yax[] = {9, 0, 0, 0, 0, 0, 0, 0};
    KV_CHECK(c, w.done() && bytes_are(w, yax, sizeof yax));
    // An array of int64 at 4: the length counts the elements, not the padding before them.
    KV_CHECK(c, w.begin("ax"));
    w.array_begin(); w.i64(-1); w.array_end();
    const u8 ax1[] = {8, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    KV_CHECK(c, w.done() && bytes_are(w, ax1, sizeof ax1));

    // Strings, a list of them, a signature, a path.
    KV_CHECK(c, w.begin("as"));
    w.array_begin(); w.str("a"); w.str("bc"); w.array_end();
    const u8 as[] = {15, 0, 0, 0, 1, 0, 0, 0, 'a', 0, 0, 0, 2, 0, 0, 0, 'b', 'c', 0};
    KV_CHECK(c, w.done() && bytes_are(w, as, sizeof as));
    KV_CHECK(c, w.begin("go"));
    w.signature("as"); w.path("/a");
    const u8 go[] = {2, 'a', 's', 0, 2, 0, 0, 0, '/', 'a', 0};
    KV_CHECK(c, w.done() && bytes_are(w, go, sizeof go));
    // A variant holding a string.
    KV_CHECK(c, w.begin("v"));
    w.variant_begin("s"); w.str("x"); w.variant_end();
    const u8 vs[] = {1, 's', 0, 0, 1, 0, 0, 0, 'x', 0};
    KV_CHECK(c, w.done() && bytes_are(w, vs, sizeof vs));

    // Misuse is refused, first one kept.
    KV_CHECK(c, w.begin("i")); w.str("no"); KV_CHECK(c, !w.done() && w.error);
    KV_CHECK(c, w.begin("i")); w.i32(1); w.i32(2); KV_CHECK(c, !w.done() && w.error);
    KV_CHECK(c, w.begin("(ii)")); w.struct_begin(); w.i32(1); w.struct_end(); KV_CHECK(c, !w.done());
    KV_CHECK(c, w.begin("s")); w.str("\xff\xfe"); KV_CHECK(c, !w.done());
    KV_CHECK(c, w.begin("o")); w.path("no/slash"); KV_CHECK(c, !w.done());
    KV_CHECK(c, w.begin("v")); w.variant_begin("ss"); KV_CHECK(c, !w.done());
    KV_CHECK(c, w.begin("i")); KV_CHECK(c, !w.done()); // nothing written yet
    KV_CHECK(c, !w.begin("a{vs}") && w.error);

    // Everything, written and read back.
    KV_CHECK(c, w.begin("ybnqiuxtdsogva{sv}(ias)"));
    w.byte(200); w.boolean(true); w.i16(-3); w.u16(65000); w.i32(-70000); w.u32(4000000000u);
    w.i64(-(1ll << 40)); w.u64(1ull << 63); w.f64(0.25); w.str("héllo"); w.path("/org/x"); w.signature("a{sv}");
    w.variant_begin("ai"); w.array_begin(); w.i32(7); w.i32(8); w.array_end(); w.variant_end();
    w.array_begin();
    w.entry_begin(); w.str("Volume"); w.variant_begin("d"); w.f64(0.5); w.variant_end(); w.entry_end();
    w.entry_begin(); w.str("Shuffle"); w.variant_begin("b"); w.boolean(false); w.variant_end(); w.entry_end();
    w.array_end();
    w.struct_begin(); w.i32(1); w.array_begin(); w.array_end(); w.struct_end();
    KV_CHECK(c, w.done());

    DbusReader r;
    r.begin(w.bytes.data, w.bytes.len, w.sig);
    u8 y; bool b; i16 n; u16 q; i32 i; u32 u; i64 x; u64 t; f64 d; const char* s; const char* o; const char* g; const char* vsig;
    KV_CHECK(c, r.byte(&y) && y == 200 && r.boolean(&b) && b && r.i16(&n) && n == -3 && r.u16(&q) && q == 65000);
    KV_CHECK(c, r.i32(&i) && i == -70000 && r.u32(&u) && u == 4000000000u && r.i64(&x) && x == -(1ll << 40) && r.u64(&t) && t == 1ull << 63);
    KV_CHECK(c, r.f64(&d) && d == 0.25 && r.str(&s) && strcmp(s, "héllo") == 0 && r.path(&o) && strcmp(o, "/org/x") == 0);
    KV_CHECK(c, r.signature(&g) && strcmp(g, "a{sv}") == 0);
    KV_CHECK(c, r.variant_begin(&vsig) && strcmp(vsig, "ai") == 0 && r.array_begin());
    i32 sum = 0;
    while (r.array_more()) { i32 e; r.i32(&e); sum += e; }
    KV_CHECK(c, sum == 15 && r.variant_end());
    u32 entries = 0;
    f64 vol = 0;
    KV_CHECK(c, r.array_begin());
    while (r.array_more()) {
        const char* key;
        r.entry_begin(); r.str(&key); r.variant_begin();
        if (strcmp(key, "Volume") == 0) r.number(&vol); else r.skip();
        r.variant_end(); r.entry_end();
        ++entries;
    }
    KV_CHECK(c, entries == 2 && vol == 0.5);
    KV_CHECK(c, r.struct_begin() && r.i32(&i) && i == 1 && r.array_begin() && !r.array_more() && r.struct_end());
    KV_CHECK(c, r.at_end() && !r.failed);

    // Reading the wrong type is refused, sticks, and reads nothing more.
    r.begin(w.bytes.data, w.bytes.len, w.sig);
    KV_CHECK(c, !r.i32(&i) && r.failed && r.error && !r.byte(&y));

    // A copy, re-aligned: the same value at a different place comes out byte for byte as
    // it would have been written there.
    DbusWriter w2;
    KV_CHECK(c, w2.begin("ya(yx)"));
    w2.byte(1); w2.array_begin(); w2.struct_begin(); w2.byte(2); w2.i64(3); w2.struct_end(); w2.array_end();
    DbusWriter w3;
    w3.begin("v");
    DbusReader rr;
    rr.begin(w2.bytes.data, w2.bytes.len, "ya(yx)");
    u8 skip;
    rr.byte(&skip);
    w3.variant_begin("a(yx)");
    KV_CHECK(c, dbus_copy_value(rr, w3));
    w3.variant_end();
    KV_CHECK(c, w3.done());
    DbusReader r3;
    r3.begin(w3.bytes.data, w3.bytes.len, "v");
    i64 got = 0;
    KV_CHECK(c, r3.variant_begin() && r3.array_begin() && r3.array_more() && r3.struct_begin() && r3.byte(&skip) && r3.i64(&got) && got == 3);
    w.free(); w2.free(); w3.free();

    // What a hostile peer might send, refused: padding that is not zero, a boolean of 2, a
    // string with no NUL, an array longer than its bytes.
    const u8 badpad[] = {1, 9, 0, 0, 2, 0, 0, 0};
    r.begin(badpad, sizeof badpad, "yi");
    KV_CHECK(c, r.byte(&y) && !r.i32(&i) && r.failed);
    const u8 badbool[] = {2, 0, 0, 0};
    r.begin(badbool, sizeof badbool, "b");
    KV_CHECK(c, !r.boolean(&b));
    const u8 nonul[] = {1, 0, 0, 0, 'a', 'b'};
    r.begin(nonul, sizeof nonul, "s");
    KV_CHECK(c, !r.str(&s));
    const u8 longarr[] = {100, 0, 0, 0, 1, 0, 0, 0};
    r.begin(longarr, sizeof longarr, "ai");
    KV_CHECK(c, !r.array_begin());
    // Variants nested past 64 deep.
    u8 nest[3 * 70 + 8] = {};
    u32 k = 0;
    for (u32 v = 0; v < 70; ++v) { nest[k++] = 1; nest[k++] = 'v'; nest[k++] = 0; }
    r.begin(nest, k, "v");
    bool ok = true;
    for (u32 v = 0; v < 70 && ok; ++v) ok = r.variant_begin();
    KV_CHECK(c, !ok && r.failed);
}

void test_messages(TestCtx& c) {
    c.begin("dbus, messages");
    DbusWriter body;
    body.begin("su");
    body.str("org.mpris.MediaPlayer2.blackwing");
    body.u32(4);
    DbusHeader h;
    h.destination = "org.freedesktop.DBus";
    h.path = "/org/freedesktop/DBus";
    h.interface = "org.freedesktop.DBus";
    h.member = "RequestName";
    Array<u8> bytes;
    const char* why = nullptr;
    KV_CHECK(c, dbus_encode(h, 7, &body, &bytes, &why));
    KV_CHECK(c, dbus_message_size(bytes.data, 15, &why) == 0 && dbus_message_size(bytes.data, bytes.len, &why) == bytes.len);
    DbusMessage m;
    KV_CHECK(c, dbus_parse(bytes.data, bytes.len, nullptr, 0, &m, &why));
    KV_CHECK(c, m.kind == DbusKind::Call && m.serial == 7 && strcmp(m.member, "RequestName") == 0 && strcmp(m.signature, "su") == 0);
    KV_CHECK(c, strcmp(m.destination, "org.freedesktop.DBus") == 0 && strcmp(m.path, "/org/freedesktop/DBus") == 0 && m.sender[0] == 0);
    DbusReader r = m.reader();
    const char* s; u32 u;
    KV_CHECK(c, r.str(&s) && strcmp(s, "org.mpris.MediaPlayer2.blackwing") == 0 && r.u32(&u) && u == 4 && r.at_end());
    dbus_message_free(&m);

    // A header that is not one is refused before anything is sent.
    DbusHeader bad = h;
    bad.member = nullptr;
    KV_CHECK(c, !dbus_encode(bad, 1, nullptr, &bytes, &why) && why);
    bad = h; bad.path = "nope";
    KV_CHECK(c, !dbus_encode(bad, 1, nullptr, &bytes, &why));
    DbusHeader ret;
    ret.kind = DbusKind::Return;
    KV_CHECK(c, !dbus_encode(ret, 1, nullptr, &bytes, &why)); // replies to no serial

    // Bytes changed after encoding: each refused, in words.
    KV_CHECK(c, dbus_encode(h, 7, &body, &bytes, &why));
    Array<u8> x;
    auto refused = [&](u32 at, u8 v) {
        x.clear();
        memcpy(x.push_n(bytes.len), bytes.data, bytes.len);
        x.data[at] = v;
        DbusMessage mm;
        bool ok = dbus_parse(x.data, x.len, nullptr, 0, &mm, &why);
        dbus_message_free(&mm);
        return !ok && why;
    };
    KV_CHECK(c, refused(0, 'x'));   // byte order
    KV_CHECK(c, refused(1, 9));     // type
    KV_CHECK(c, refused(3, 2));     // version
    x.clear(); memcpy(x.push_n(bytes.len), bytes.data, bytes.len);
    memset(x.data + 8, 0, 4);       // serial 0
    KV_CHECK(c, !dbus_parse(x.data, x.len, nullptr, 0, &m, &why)); dbus_message_free(&m);
    KV_CHECK(c, !dbus_parse(bytes.data, bytes.len - 1, nullptr, 0, &m, &why)); dbus_message_free(&m); // short

    // A descriptor said to be there and not passed, and one passed and not said.
    DbusWriter fb;
    fb.begin("h");
    Handle none;
    fb.fd(none);
    DbusHeader sig;
    sig.kind = DbusKind::Signal;
    sig.path = "/a"; sig.interface = "a.b"; sig.member = "C";
    KV_CHECK(c, dbus_encode(sig, 3, &fb, &bytes, &why));
    KV_CHECK(c, !dbus_parse(bytes.data, bytes.len, nullptr, 0, &m, &why)); dbus_message_free(&m);

    // A big-endian peer's signal: its values read back as they were meant.
    Be be;
    be.y('B'); be.y(4); be.y(0); be.y(1);
    be.u(4); be.u(0x01020304);
    u32 len_at = be.n; be.u(0);
    u32 fields_start = be.n;
    be.pad(8); be.y(1); be.g("o"); be.s("/a");
    be.pad(8); be.y(2); be.g("s"); be.s("a.b");
    be.pad(8); be.y(3); be.g("s"); be.s("Ping");
    be.pad(8); be.y(8); be.g("g"); be.g("u");
    u32 flen = be.n - fields_start;
    be.b[len_at] = (u8)(flen >> 24); be.b[len_at + 1] = (u8)(flen >> 16); be.b[len_at + 2] = (u8)(flen >> 8); be.b[len_at + 3] = (u8)flen;
    be.pad(8);
    be.u(0xA1B2C3D4);
    KV_CHECK(c, dbus_parse(be.b, be.n, nullptr, 0, &m, &why));
    KV_CHECK(c, m.big_endian && m.serial == 0x01020304 && strcmp(m.member, "Ping") == 0);
    r = m.reader();
    KV_CHECK(c, r.u32(&u) && u == 0xA1B2C3D4);
    dbus_message_free(&m);

    // Addresses.
    DbusAddress a[4];
    u32 n = 0;
    KV_CHECK(c, dbus_parse_address("unix:path=/run/user/1000/bus", a, 4, &n, &why) && n == 1 && strcmp(a[0].path, "/run/user/1000/bus") == 0 && !a[0].abstract);
    KV_CHECK(c, dbus_parse_address("tcp:host=x;unix:abstract=/tmp/dbus-ab%2Cc,guid=12", a, 4, &n, &why) && n == 1 && a[0].abstract && strcmp(a[0].path, "/tmp/dbus-ab,c") == 0);
    KV_CHECK(c, !dbus_parse_address("tcp:host=localhost,port=1", a, 4, &n, &why) && why && strstr(why, "tcp"));
    KV_CHECK(c, !dbus_parse_address("unix:tmpdir=/tmp", a, 4, &n, &why) && strstr(why, "listening"));
    KV_CHECK(c, !dbus_parse_address("unix:path=/x%zz", a, 4, &n, &why));
    KV_CHECK(c, !dbus_parse_address("", a, 4, &n, &why));
    body.free(); fb.free(); bytes.free(); x.free();
}
