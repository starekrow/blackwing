// Fuzzing, two ways. Made: random signatures with random values, written, encoded, parsed
// and copied back, which must agree byte for byte. Mutated: those messages with bytes
// flipped, lengths changed, ends cut and junk added, which must be refused in words or
// parsed into something that reads to its end, and never read out of bounds (run under
// the address sanitizer to be sure of that).
#include "korvid/dbus/dbus.h"
#include "korvid/core/log.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <stdlib.h>
#include <string.h>

using namespace kv;

namespace {

struct Rng {
    u64 s;
    u32 next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (u32)(s >> 11); }
    u32 below(u32 n) { return n ? next() % n : 0; }
};

// One complete type, at most `depth` containers deep, into out.
void gen_type(Rng& r, char* out, u32* n, u32 cap, u32 depth) {
    static const char basic[] = "ybnqiuxtdsog";
    u32 pick = depth == 0 ? 0 : r.below(10);
    if (*n + 8 >= cap) pick = 0;
    if (pick < 5) { out[(*n)++] = basic[r.below(sizeof basic - 1)]; return; }
    if (pick == 5) { out[(*n)++] = 'v'; return; }
    if (pick == 6 || pick == 7) { out[(*n)++] = 'a'; gen_type(r, out, n, cap, depth - 1); return; }
    if (pick == 8) {
        out[(*n)++] = 'a'; out[(*n)++] = '{';
        out[(*n)++] = basic[r.below(sizeof basic - 1)];
        gen_type(r, out, n, cap, depth - 1);
        out[(*n)++] = '}';
        return;
    }
    out[(*n)++] = '(';
    u32 fields = 1 + r.below(3);
    for (u32 i = 0; i < fields && *n + 8 < cap; ++i) gen_type(r, out, n, cap, depth - 1);
    out[(*n)++] = ')';
}

// The length of the complete type at s (which is valid).
u32 type_len(const char* s) {
    if (s[0] == 'a') return 1 + type_len(s + 1);
    if (s[0] == '(' || s[0] == '{') {
        u32 i = 1;
        while (s[i] != ')' && s[i] != '}') i += type_len(s + i);
        return i + 1;
    }
    return 1;
}

const char* const kWords[] = {"", "a", "héllo", "Blackwing", "org.mpris.MediaPlayer2", "ünïcødé ♪"};

// A value of the type at sig[*i], written to w.
void gen_value(Rng& r, DbusWriter& w, const char* sig, u32* i, u32 depth) {
    char ch = sig[(*i)++];
    switch (ch) {
    case 'y': w.byte((u8)r.next()); break;
    case 'b': w.boolean(r.below(2)); break;
    case 'n': w.i16((i16)r.next()); break;
    case 'q': w.u16((u16)r.next()); break;
    case 'i': w.i32((i32)r.next()); break;
    case 'u': w.u32(r.next()); break;
    case 'x': w.i64((i64)(((u64)r.next() << 32) | r.next())); break;
    case 't': w.u64(((u64)r.next() << 32) | r.next()); break;
    case 'd': w.f64((f64)(i32)r.next() / 7.0); break;
    case 's': w.str(kWords[r.below(6)]); break;
    case 'o': w.path(r.below(2) ? "/" : "/org/mpris/MediaPlayer2"); break;
    case 'g': w.signature(r.below(2) ? "" : "a{sv}"); break;
    case 'v': {
        char inner[64];
        u32 n = 0;
        gen_type(r, inner, &n, sizeof inner - 1, depth ? depth - 1 : 0);
        inner[n] = 0;
        w.variant_begin(inner);
        u32 k = 0;
        gen_value(r, w, inner, &k, depth ? depth - 1 : 0);
        w.variant_end();
        break;
    }
    case 'a': {
        u32 start = *i;
        w.array_begin();
        u32 count = r.below(4);
        u32 end = start;
        if (count == 0) end = start + type_len(sig + start);
        for (u32 k = 0; k < count; ++k) {
            u32 at = start;
            if (sig[at] == '{') {
                ++at;
                w.entry_begin();
                gen_value(r, w, sig, &at, depth);
                gen_value(r, w, sig, &at, depth);
                ++at; // '}'
                w.entry_end();
            } else {
                gen_value(r, w, sig, &at, depth);
            }
            end = at;
        }
        w.array_end();
        *i = end;
        break;
    }
    case '(': {
        w.struct_begin();
        while (sig[*i] != ')') gen_value(r, w, sig, i, depth);
        ++*i;
        w.struct_end();
        break;
    }
    default: break;
    }
}

} // namespace

void test_fuzz(TestCtx& c) {
    c.begin("dbus, fuzzed");
    Rng r{0x9E3779B97F4A7C15ull};
    Array<u8> bytes;
    Array<u8> mutated;
    DbusWriter w, back;
    u32 made = 0, made_ok = 0, copied_same = 0;
    u32 parsed = 0, refused = 0, read_whole = 0, unexplained = 0;
    // DBUS_FUZZ_ROUNDS asks for a longer run (each round is a made message and 40 mutations).
    const char* env = getenv("DBUS_FUZZ_ROUNDS");
    u32 rounds = env && atoi(env) > 0 ? (u32)atoi(env) : 3000;
    if (env) r.s ^= os_ticks();
    for (u32 round = 0; round < rounds; ++round) {
        char sig[200];
        u32 n = 0;
        u32 types = 1 + r.below(4);
        for (u32 t = 0; t < types && n < 150; ++t) gen_type(r, sig, &n, 150, 1 + r.below(5));
        sig[n] = 0;
        if (!dbus_valid_signature(sig)) continue;
        ++made;
        w.begin(sig);
        u32 i = 0;
        while (sig[i]) gen_value(r, w, sig, &i, 3);
        if (!w.done()) continue;
        DbusHeader h;
        h.kind = DbusKind::Signal;
        h.path = "/org/x";
        h.interface = "org.x.Y";
        h.member = "Z";
        const char* why = nullptr;
        if (!dbus_encode(h, round + 1, &w, &bytes, &why)) continue;
        DbusMessage m;
        bool ok = dbus_parse(bytes.data, bytes.len, nullptr, 0, &m, &why);
        if (ok) {
            ++made_ok;
            DbusReader rd = m.reader();
            back.begin(m.signature);
            bool all = true;
            while (rd.peek() && all) all = dbus_copy_value(rd, back);
            if (all && back.done() && back.bytes.len == w.bytes.len && memcmp(back.bytes.data, w.bytes.data, w.bytes.len) == 0) ++copied_same;
        }
        dbus_message_free(&m);

        // Mutations of this message.
        for (u32 k = 0; k < 40; ++k) {
            mutated.clear();
            memcpy(mutated.push_n(bytes.len), bytes.data, bytes.len);
            u32 edits = 1 + r.below(4);
            for (u32 e = 0; e < edits && mutated.len; ++e) {
                u32 at = r.below(mutated.len);
                switch (r.below(6)) {
                case 0: mutated.data[at] ^= (u8)(1u << r.below(8)); break;
                case 1: { static const u8 v[] = {0, 1, 0x7f, 0x80, 0xff, 'a', '(', ')', '{', 'v'}; mutated.data[at] = v[r.below(10)]; break; }
                case 2: { u32 a4 = at & ~3u; if (a4 + 4 <= mutated.len) { u32 v = r.below(2) ? r.below(64) : r.next(); memcpy(mutated.data + a4, &v, 4); } break; }
                case 3: mutated.len = at; break;
                case 4: { u32 add = 1 + r.below(16); u8* p = mutated.push_n(add); for (u32 b = 0; b < add; ++b) p[b] = (u8)r.next(); break; }
                default: mutated.data[at] = (u8)r.next(); break;
                }
            }
            i64 size = dbus_message_size(mutated.data, mutated.len, &why);
            u32 take = size > 0 && (u64)size <= mutated.len ? (u32)size : mutated.len;
            DbusMessage mm;
            if (dbus_parse(mutated.data, take, nullptr, 0, &mm, &why)) {
                ++parsed;
                DbusReader rd = mm.reader();
                bool all = true;
                while (rd.peek() && all) all = rd.skip();
                if (all && rd.at_end()) ++read_whole;
            } else {
                ++refused;
                if (!why) ++unexplained; // a refusal must say why
            }
            dbus_message_free(&mm);
        }
    }
    // Pure noise too.
    for (u32 k = 0; k < 20000; ++k) {
        u32 len = 16 + r.below(200);
        mutated.clear();
        u8* p = mutated.push_n(len);
        for (u32 b = 0; b < len; ++b) p[b] = (u8)r.next();
        p[0] = r.below(2) ? 'l' : 'B';
        p[3] = 1;
        const char* why = nullptr;
        DbusMessage mm;
        if (dbus_parse(mutated.data, len, nullptr, 0, &mm, &why)) ++parsed; else ++refused;
        dbus_message_free(&mm);
    }
    KV_INFO("test", "fuzz: %u made, %u parsed back, %u copied the same; mutated: %u parsed, %u read whole, %u refused",
            made, made_ok, copied_same, parsed, read_whole, refused);
    KV_CHECK(c, made > rounds * 2 / 3 && made_ok == copied_same && made_ok + 50 > made);
    KV_CHECK(c, parsed == read_whole && refused > 0 && unexplained == 0);
    bytes.free(); mutated.free(); w.free(); back.free();
}
