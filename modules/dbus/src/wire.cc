// The wire format: names, signatures, and values written and read to a signature, as the
// D-Bus Specification has them. Every value is aligned to its size from the start of the
// message (a body starts 8-aligned, so from the body's start too); an array's length counts
// its elements' bytes and not the padding before the first, which is there even when it
// is empty. The reader checks before it reads: everything it is given came from another
// process.
#include "korvid/dbus/dbus.h"
#include "wire_internal.h"
#include "korvid/core/utf8.h"
#include <string.h>

namespace kv {

// ---- names ----------------------------------------------------------------------------

namespace {

bool alpha_(char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; }
bool digit(char ch) { return ch >= '0' && ch <= '9'; }

// Dot-separated elements: at least two, none empty, each of [A-Za-z_] then [A-Za-z0-9_],
// with '-' too where `dash`, and a digit first too where `digit_first`.
bool dotted(const char* s, bool dash, bool digit_first) {
    usize n = strlen(s);
    if (n == 0 || n > kDbusMaxName) return false;
    u32 elements = 0;
    const char* p = s;
    for (;;) {
        const char* e = p;
        while (*e && *e != '.') {
            char ch = *e;
            bool ok = alpha_(ch) || (dash && ch == '-') || (digit(ch) && (e != p || digit_first));
            if (!ok) return false;
            ++e;
        }
        if (e == p) return false;
        ++elements;
        if (!*e) break;
        p = e + 1;
    }
    return elements >= 2;
}

} // namespace

bool dbus_valid_path(const char* s) {
    if (!s || s[0] != '/') return false;
    if (s[1] == 0) return true;
    const char* p = s + 1;
    for (;;) {
        const char* e = p;
        while (*e && *e != '/') {
            if (!alpha_(*e) && !digit(*e)) return false;
            ++e;
        }
        if (e == p) return false; // "//", or a trailing '/'
        if (!*e) return true;
        p = e + 1;
    }
}

bool dbus_valid_interface(const char* s) { return s && dotted(s, false, false); }

bool dbus_valid_member(const char* s) {
    if (!s || !alpha_(s[0])) return false;
    usize n = 0;
    for (const char* p = s; *p; ++p, ++n)
        if (!alpha_(*p) && !digit(*p)) return false;
    return n <= kDbusMaxName;
}

bool dbus_valid_bus_name(const char* s) {
    if (!s) return false;
    if (s[0] == ':') return strlen(s) <= kDbusMaxName && dotted(s + 1, true, true);
    return dotted(s, true, false);
}

// ---- signatures -----------------------------------------------------------------------

u32 dbus_align_of(char code) {
    switch (code) {
    case 'y': case 'g': case 'v': return 1;
    case 'n': case 'q': return 2;
    case 'b': case 'i': case 'u': case 's': case 'o': case 'a': case 'h': return 4;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 0;
    }
}

bool dbus_basic(char code) {
    return code && strchr("ybnqiuxtdsogh", code) != nullptr;
}

namespace {

// The length of the one complete type at s[0, n), or 0 if there is none.
u32 complete(const char* s, u32 n, u32 arrays, u32 structs) {
    if (n == 0) return 0;
    char ch = s[0];
    if (dbus_basic(ch) || ch == 'v') return 1;
    if (ch == 'a') {
        if (arrays + 1 > kDbusMaxNesting) return 0;
        if (n >= 2 && s[1] == '{') {
            // a dict entry: a basic key and one complete value, only here
            if (structs + 1 > kDbusMaxNesting || n < 5 || !dbus_basic(s[2])) return 0;
            u32 v = complete(s + 3, n - 3, arrays + 1, structs + 1);
            if (!v || 3 + v >= n || s[3 + v] != '}') return 0;
            return 3 + v + 1;
        }
        u32 e = complete(s + 1, n - 1, arrays + 1, structs);
        return e ? 1 + e : 0;
    }
    if (ch == '(') {
        if (structs + 1 > kDbusMaxNesting) return 0;
        u32 i = 1;
        u32 fields = 0;
        while (i < n && s[i] != ')') {
            u32 f = complete(s + i, n - i, arrays, structs + 1);
            if (!f) return 0;
            i += f;
            ++fields;
        }
        if (i >= n || fields == 0) return 0;
        return i + 1;
    }
    return 0; // ')', '{' or '}' out of place, or not a type
}

} // namespace

u32 dbus_complete_type(const char* s, u32 n) { return complete(s, n, 0, 0); }

u32 dbus_container_len(const char* s, u32 n) {
    if (n && s[0] == '{') {
        // a dict entry, which is complete only as an array's element: measured as one
        if (n < 4 || !dbus_basic(s[1])) return 0;
        u32 v = complete(s + 2, n - 2, 0, 0);
        if (!v || 2 + v >= n || s[2 + v] != '}') return 0;
        return 2 + v + 1;
    }
    return complete(s, n, 0, 0);
}

bool dbus_valid_signature(const char* s, bool single) {
    if (!s) return false;
    usize n = strlen(s);
    if (n > kDbusMaxSignature) return false;
    u32 i = 0, types = 0;
    while (i < n) {
        u32 t = complete(s + i, (u32)n - i, 0, 0);
        if (!t) return false;
        i += t;
        ++types;
    }
    return single ? types == 1 : true;
}

// ---- writing --------------------------------------------------------------------------

namespace {

void pad(Array<u8>& b, u32 align) {
    while (b.len % align) b.push(0);
}

} // namespace

bool DbusWriter::begin(const char* s) {
    bytes.clear();
    fd_count = 0;
    error = nullptr;
    depth = 0;
    stack[0] = Frame{};
    if (!s || !dbus_valid_signature(s)) {
        sig[0] = 0;
        sig_len = 0;
        error = "the signature is not a valid one";
        return false;
    }
    sig_len = (kv::u32)strlen(s);
    memcpy(sig, s, sig_len + 1);
    stack[0].at = 0;
    stack[0].end = sig_len;
    return true;
}

namespace {

const char* frame_sig(const DbusWriter& w, const DbusWriter::Frame& f) {
    return f.in_bytes ? (const char*)w.bytes.data : w.sig;
}

// The next value is to be `code`: checks it against the signature, and starts an array's
// next element. Returns the frame, or null (and sets error).
DbusWriter::Frame* expect(DbusWriter& w, char code) {
    if (w.error) return nullptr;
    DbusWriter::Frame& f = w.stack[w.depth];
    if (f.code == 'a' && f.at == f.end) { f.at = f.elem_at; } // the next element
    if (f.code == 'a' && f.at == f.elem_at) ++f.count;
    if (f.at >= f.end) { w.error = "a value past the end of the signature"; return nullptr; }
    if (frame_sig(w, f)[f.at] != code) { w.error = "a value that is not the signature's type there"; return nullptr; }
    return &f;
}

template <class T>
void put(DbusWriter& w, char code, T v) {
    DbusWriter::Frame* f = expect(w, code);
    if (!f) return;
    pad(w.bytes, (u32)sizeof(T));
    memcpy(w.bytes.push_n((u32)sizeof(T)), &v, sizeof(T));
    f->at++;
}

void put_string(DbusWriter& w, char code, const char* s, u32 len) {
    DbusWriter::Frame* f = expect(w, code);
    if (!f) return;
    if (memchr(s, 0, len) || !utf8_valid(s, len)) { w.error = "a string with a NUL in it, or not UTF-8"; return; }
    if (code == 'o' && !dbus_valid_path(s)) { w.error = "an object path that is not one"; return; }
    if (code == 'g') {
        if (len > kDbusMaxSignature || !dbus_valid_signature(s)) { w.error = "a signature that is not one"; return; }
        w.bytes.push((u8)len);
    } else {
        pad(w.bytes, 4);
        memcpy(w.bytes.push_n(4), &len, 4);
    }
    if (len) memcpy(w.bytes.push_n(len), s, len);
    w.bytes.push(0);
    f->at++;
}

bool push_frame(DbusWriter& w, const DbusWriter::Frame& f) {
    if (w.depth + 1 > kDbusMaxDepth) { w.error = "values nested past the spec's 64"; return false; }
    w.stack[++w.depth] = f;
    return true;
}

} // namespace

void DbusWriter::byte(kv::u8 v)    { put(*this, 'y', v); }
void DbusWriter::boolean(bool v)   { put(*this, 'b', (kv::u32)(v ? 1 : 0)); }
void DbusWriter::i16(kv::i16 v)    { put(*this, 'n', v); }
void DbusWriter::u16(kv::u16 v)    { put(*this, 'q', v); }
void DbusWriter::i32(kv::i32 v)    { put(*this, 'i', v); }
void DbusWriter::u32(kv::u32 v)    { put(*this, 'u', v); }
void DbusWriter::i64(kv::i64 v)    { put(*this, 'x', v); }
void DbusWriter::u64(kv::u64 v)    { put(*this, 't', v); }
void DbusWriter::f64(kv::f64 v)    { put(*this, 'd', v); }
void DbusWriter::str(const char* s) { str_n(s ? s : "", s ? (kv::u32)strlen(s) : 0); }
void DbusWriter::str_n(const char* s, kv::u32 len) { put_string(*this, 's', s, len); }
void DbusWriter::path(const char* s) { put_string(*this, 'o', s ? s : "", s ? (kv::u32)strlen(s) : 0); }
void DbusWriter::signature(const char* s) { put_string(*this, 'g', s ? s : "", s ? (kv::u32)strlen(s) : 0); }

void DbusWriter::fd(Handle h) {
    if (!error && fd_count >= kDbusMaxFds) { error = "more descriptors than one message carries"; return; }
    kv::u32 at = fd_count;
    put(*this, 'h', at);
    if (!error) fds[fd_count++] = h;
}

void DbusWriter::array_begin() {
    Frame* f = expect(*this, 'a');
    if (!f) return;
    const char* s = frame_sig(*this, *f);
    Frame a;
    a.code = 'a';
    a.in_bytes = f->in_bytes;
    a.elem_at = f->at + 1;
    a.at = a.elem_at;
    a.end = a.elem_at + dbus_container_len(s + a.elem_at, f->end - a.elem_at);
    kv::u32 elem_align = dbus_align_of(s[a.elem_at]); // before `bytes` grows: a variant's signature is in it
    pad(bytes, 4);
    a.len_at = bytes.len;
    memset(bytes.push_n(4), 0, 4);
    pad(bytes, elem_align);
    a.start = bytes.len;
    a.count = 0;
    push_frame(*this, a);
}

void DbusWriter::array_end() {
    if (error) return;
    Frame& a = stack[depth];
    if (a.code != 'a' || depth == 0) { error = "array_end with no array begun"; return; }
    if (a.count != 0 && a.at != a.end) { error = "an array's last element is not finished"; return; }
    kv::u32 len = bytes.len - a.start;
    if (len > kDbusMaxArray) { error = "an array past the spec's 64 MiB"; return; }
    memcpy(bytes.data + a.len_at, &len, 4);
    kv::u32 end = a.end;
    --depth;
    stack[depth].at = end;
}

namespace {

void open_struct(DbusWriter& w, char open, char close) {
    DbusWriter::Frame* f = expect(w, open);
    if (!f) return;
    if (open == '{' && w.stack[w.depth].code != 'a') { w.error = "a dict entry outside an array"; return; }
    const char* s = frame_sig(w, *f);
    u32 n = dbus_container_len(s + f->at, f->end - f->at);
    DbusWriter::Frame st;
    st.code = (u8)open;
    st.in_bytes = f->in_bytes;
    st.at = f->at + 1;
    st.end = f->at + n - 1; // the closing ')' or '}'
    (void)close;
    pad(w.bytes, 8);
    push_frame(w, st);
}

void close_struct(DbusWriter& w, char open) {
    if (w.error) return;
    DbusWriter::Frame& st = w.stack[w.depth];
    if (st.code != (u8)open || w.depth == 0) { w.error = "a struct's end with none begun"; return; }
    if (st.at != st.end) { w.error = "a struct ended before its fields were all written"; return; }
    u32 after = st.end + 1;
    --w.depth;
    w.stack[w.depth].at = after;
}

} // namespace

void DbusWriter::struct_begin() { open_struct(*this, '(', ')'); }
void DbusWriter::struct_end()   { close_struct(*this, '('); }
void DbusWriter::entry_begin()  { open_struct(*this, '{', '}'); }
void DbusWriter::entry_end()    { close_struct(*this, '{'); }

void DbusWriter::variant_begin(const char* s) {
    Frame* f = expect(*this, 'v');
    if (!f) return;
    if (!s || !dbus_valid_signature(s, true)) { error = "a variant's signature that is not one complete type"; return; }
    kv::u32 n = (kv::u32)strlen(s);
    bytes.push((kv::u8)n);
    Frame v;
    v.code = 'v';
    v.in_bytes = 1;
    v.at = bytes.len;
    v.end = bytes.len + n;
    memcpy(bytes.push_n(n), s, n);
    bytes.push(0);
    push_frame(*this, v);
}

void DbusWriter::variant_end() {
    if (error) return;
    Frame& v = stack[depth];
    if (v.code != 'v' || depth == 0) { error = "variant_end with no variant begun"; return; }
    if (v.at != v.end) { error = "a variant ended with no value written"; return; }
    --depth;
    stack[depth].at++;
}

bool DbusWriter::done() const {
    return !error && depth == 0 && stack[0].at == stack[0].end;
}

void DbusWriter::free() { bytes.free(); }

// ---- reading --------------------------------------------------------------------------

void DbusReader::begin(const u8* d, kv::u32 len, const char* s, bool sw, kv::u32 at, kv::u32 fds) {
    data = d;
    limit = len;
    pos = at;
    swap = sw;
    fd_count = fds;
    depth = 0;
    arrays = structs = 0;
    failed = false;
    error = nullptr;
    stack[0] = Frame{};
    stack[0].at = s ? s : "";
    stack[0].end = stack[0].at + strlen(stack[0].at);
    stack[0].data_end = len;
}

namespace {

bool fail(DbusReader& r, const char* why) {
    if (!r.failed) { r.failed = true; r.error = why; }
    return false;
}

// Where values of this frame may go no further.
u32 frame_limit(const DbusReader& r) {
    for (u32 i = r.depth + 1; i-- > 0;)
        if (r.stack[i].code == 'a' || i == 0) return r.stack[i].data_end;
    return r.limit;
}

bool align_to(DbusReader& r, u32 a) {
    u32 to = (r.pos + a - 1) & ~(a - 1);
    if (to > frame_limit(r)) return fail(r, "a value past the end of its bytes");
    for (u32 i = r.pos; i < to; ++i)
        if (r.data[i] != 0) return fail(r, "alignment padding that is not zero");
    r.pos = to;
    return true;
}

DbusReader::Frame* expect(DbusReader& r, char code) {
    if (r.failed) return nullptr;
    DbusReader::Frame& f = r.stack[r.depth];
    if (f.code == 'a' && !f.started) { fail(r, "an array element read without array_more"); return nullptr; }
    if (f.at >= f.end) { fail(r, "a value past the end of the signature"); return nullptr; }
    if (*f.at != code) { fail(r, "a value read as a type the signature does not have there"); return nullptr; }
    return &f;
}

template <class T>
bool get(DbusReader& r, char code, T* out) {
    DbusReader::Frame* f = expect(r, code);
    if (!f) return false;
    if (!align_to(r, (u32)sizeof(T))) return false;
    if (r.pos + sizeof(T) > frame_limit(r)) return fail(r, "a value past the end of its bytes");
    u8 b[sizeof(T)];
    memcpy(b, r.data + r.pos, sizeof(T));
    if (r.swap)
        for (u32 i = 0; i < sizeof(T) / 2; ++i) { u8 t = b[i]; b[i] = b[sizeof(T) - 1 - i]; b[sizeof(T) - 1 - i] = t; }
    memcpy(out, b, sizeof(T));
    r.pos += (u32)sizeof(T);
    f->at++;
    return true;
}

bool get_string(DbusReader& r, char code, const char** s, u32* len_out) {
    DbusReader::Frame* f = expect(r, code);
    if (!f) return false;
    u32 len;
    if (code == 'g') {
        if (r.pos + 1 > frame_limit(r)) return fail(r, "a signature past the end of its bytes");
        len = r.data[r.pos++];
    } else {
        if (!align_to(r, 4)) return false;
        if (r.pos + 4 > frame_limit(r)) return fail(r, "a string's length past the end of its bytes");
        memcpy(&len, r.data + r.pos, 4);
        if (r.swap) len = __builtin_bswap32(len);
        r.pos += 4;
    }
    u32 lim = frame_limit(r);
    if (len >= lim || r.pos + len + 1 > lim || r.pos + len + 1 < r.pos) return fail(r, "a string longer than its bytes");
    const char* p = (const char*)r.data + r.pos;
    if (p[len] != 0) return fail(r, "a string not ended by a NUL");
    if (memchr(p, 0, len)) return fail(r, "a string with a NUL in it");
    if (!utf8_valid(p, len)) return fail(r, "a string that is not UTF-8");
    if (code == 'o' && !dbus_valid_path(p)) return fail(r, "an object path that is not one");
    if (code == 'g' && !dbus_valid_signature(p)) return fail(r, "a signature that is not one");
    r.pos += len + 1;
    f->at++;
    if (s) *s = p;
    if (len_out) *len_out = len;
    return true;
}

bool push(DbusReader& r, const DbusReader::Frame& f) {
    if (r.depth + 1 > kDbusMaxDepth) return fail(r, "values nested past the spec's 64");
    r.stack[++r.depth] = f;
    return true;
}

bool open_struct(DbusReader& r, char open) {
    DbusReader::Frame* f = expect(r, open);
    if (!f) return false;
    if (open == '{' && r.stack[r.depth].code != 'a') return fail(r, "a dict entry outside an array");
    if (r.structs + 1 > kDbusMaxNesting) return fail(r, "structs nested past the spec's 32");
    u32 n = dbus_container_len(f->at, (u32)(f->end - f->at));
    if (!n) return fail(r, "a struct whose signature is not complete");
    if (!align_to(r, 8)) return false;
    DbusReader::Frame st;
    st.code = (u8)open;
    st.at = f->at + 1;
    st.end = f->at + n - 1;
    if (!push(r, st)) return false;
    ++r.structs;
    return true;
}

bool close_struct(DbusReader& r, char open) {
    if (r.failed) return false;
    DbusReader::Frame& st = r.stack[r.depth];
    if (st.code != (u8)open || r.depth == 0) return fail(r, "a struct's end with none begun");
    if (st.at != st.end) return fail(r, "a struct ended before its fields were read");
    const char* after = st.end + 1;
    --r.depth;
    --r.structs;
    r.stack[r.depth].at = after;
    return true;
}

} // namespace

char DbusReader::peek() const {
    if (failed) return 0;
    const Frame& f = stack[depth];
    if (f.code == 'a' && !f.started) return 0;
    return f.at < f.end ? *f.at : 0;
}

bool DbusReader::byte(kv::u8* v)   { return get(*this, 'y', v); }
bool DbusReader::boolean(bool* v) {
    kv::u32 b;
    if (!get(*this, 'b', &b)) return false;
    if (b > 1) return fail(*this, "a boolean that is neither 0 nor 1");
    *v = b != 0;
    return true;
}
bool DbusReader::i16(kv::i16* v)   { return get(*this, 'n', v); }
bool DbusReader::u16(kv::u16* v)   { return get(*this, 'q', v); }
bool DbusReader::i32(kv::i32* v)   { return get(*this, 'i', v); }
bool DbusReader::u32(kv::u32* v)   { return get(*this, 'u', v); }
bool DbusReader::i64(kv::i64* v)   { return get(*this, 'x', v); }
bool DbusReader::u64(kv::u64* v)   { return get(*this, 't', v); }
bool DbusReader::f64(kv::f64* v)   { return get(*this, 'd', v); }
bool DbusReader::str(const char** s, kv::u32* len) { return get_string(*this, 's', s, len); }
bool DbusReader::path(const char** s)      { return get_string(*this, 'o', s, nullptr); }
bool DbusReader::signature(const char** s) { return get_string(*this, 'g', s, nullptr); }
bool DbusReader::fd(kv::u32* index) {
    if (!get(*this, 'h', index)) return false;
    if (*index >= fd_count) return fail(*this, "a descriptor's index past those the message carries");
    return true;
}

bool DbusReader::array_begin() {
    Frame* f = expect(*this, 'a');
    if (!f) return false;
    if (arrays + 1 > kDbusMaxNesting) return fail(*this, "arrays nested past the spec's 32");
    kv::u32 n = dbus_container_len(f->at + 1, (kv::u32)(f->end - f->at - 1));
    if (!n) return fail(*this, "an array whose element type is not complete");
    if (!align_to(*this, 4)) return false;
    if (pos + 4 > frame_limit(*this)) return fail(*this, "an array's length past the end of its bytes");
    kv::u32 len;
    memcpy(&len, data + pos, 4);
    if (swap) len = __builtin_bswap32(len);
    pos += 4;
    if (len > kDbusMaxArray) return fail(*this, "an array past the spec's 64 MiB");
    if (!align_to(*this, dbus_align_of(f->at[1]))) return false; // even when empty
    kv::u32 lim = frame_limit(*this);
    if (len > lim - pos) return fail(*this, "an array longer than its bytes");
    Frame a;
    a.code = 'a';
    a.elem_at = f->at + 1;
    a.at = a.elem_at;
    a.end = a.elem_at + n;
    a.data_end = pos + len;
    a.started = false;
    if (!push(*this, a)) return false;
    ++arrays;
    return true;
}

bool DbusReader::array_more() {
    if (failed) return false;
    Frame& a = stack[depth];
    if (a.code != 'a' || depth == 0) return fail(*this, "array_more with no array begun");
    if (a.started && a.at != a.end) return fail(*this, "an array element not read to its end");
    if (pos < a.data_end) {
        a.at = a.elem_at;
        a.started = true;
        return true;
    }
    if (pos > a.data_end) return fail(*this, "an array's elements past its length");
    const char* after = a.end;
    --depth;
    --arrays;
    stack[depth].at = after;
    return false;
}

bool DbusReader::struct_begin() { return open_struct(*this, '('); }
bool DbusReader::struct_end()   { return close_struct(*this, '('); }
bool DbusReader::entry_begin()  { return open_struct(*this, '{'); }
bool DbusReader::entry_end()    { return close_struct(*this, '{'); }

bool DbusReader::variant_begin(const char** sig_out) {
    Frame* f = expect(*this, 'v');
    if (!f) return false;
    if (pos + 1 > frame_limit(*this)) return fail(*this, "a variant past the end of its bytes");
    kv::u32 len = data[pos];
    kv::u32 lim = frame_limit(*this);
    if (pos + 1 + len + 1 > lim) return fail(*this, "a variant's signature longer than its bytes");
    const char* s = (const char*)data + pos + 1;
    if (s[len] != 0 || memchr(s, 0, len)) return fail(*this, "a variant's signature not ended by a NUL");
    // One complete type, and nested within what is left of the limits.
    kv::u32 n = dbus_complete_type(s, len);
    if (!n || n != len) return fail(*this, "a variant's signature that is not one complete type");
    pos += len + 2;
    Frame v;
    v.code = 'v';
    v.at = s;
    v.end = s + len;
    if (!push(*this, v)) return false;
    if (sig_out) *sig_out = s;
    return true;
}

bool DbusReader::variant_end() {
    if (failed) return false;
    Frame& v = stack[depth];
    if (v.code != 'v' || depth == 0) return fail(*this, "variant_end with no variant begun");
    if (v.at != v.end) return fail(*this, "a variant ended before its value was read");
    --depth;
    stack[depth].at++;
    return true;
}

bool DbusReader::skip() {
    switch (peek()) {
    case 'y': { kv::u8 v; return byte(&v); }
    case 'b': { bool v; return boolean(&v); }
    case 'n': { kv::i16 v; return i16(&v); }
    case 'q': { kv::u16 v; return u16(&v); }
    case 'i': { kv::i32 v; return i32(&v); }
    case 'u': { kv::u32 v; return u32(&v); }
    case 'x': { kv::i64 v; return i64(&v); }
    case 't': { kv::u64 v; return u64(&v); }
    case 'd': { kv::f64 v; return f64(&v); }
    case 'h': { kv::u32 v; return fd(&v); }
    case 's': return str(nullptr);
    case 'o': return path(nullptr);
    case 'g': return signature(nullptr);
    case 'a':
        if (!array_begin()) return false;
        while (array_more())
            while (peek()) if (!skip()) return false;
        return !failed;
    case '(': case '{': {
        char open = peek();
        if (!open_struct(*this, open)) return false;
        while (peek()) if (!skip()) return false;
        return close_struct(*this, open);
    }
    case 'v':
        if (!variant_begin()) return false;
        if (!skip()) return false;
        return variant_end();
    default:
        return fail(*this, failed ? error : "nothing to read there");
    }
}

bool DbusReader::at_end() const {
    if (failed) return false;
    const Frame& f = stack[depth];
    if (depth == 0) return f.at == f.end && pos == limit;
    return f.at == f.end;
}

bool DbusReader::number(kv::f64* v) {
    switch (peek()) {
    case 'y': { kv::u8 x;  if (!byte(&x)) return false; *v = x; return true; }
    case 'n': { kv::i16 x; if (!i16(&x)) return false; *v = x; return true; }
    case 'q': { kv::u16 x; if (!u16(&x)) return false; *v = x; return true; }
    case 'i': { kv::i32 x; if (!i32(&x)) return false; *v = x; return true; }
    case 'u': { kv::u32 x; if (!u32(&x)) return false; *v = x; return true; }
    case 'x': { kv::i64 x; if (!i64(&x)) return false; *v = (kv::f64)x; return true; }
    case 't': { kv::u64 x; if (!u64(&x)) return false; *v = (kv::f64)x; return true; }
    case 'd': return f64(v);
    default: return fail(*this, "a number asked for where there is none");
    }
}

// ---- copying --------------------------------------------------------------------------

bool dbus_copy_value(DbusReader& r, DbusWriter& w) {
    switch (r.peek()) {
    case 'y': { u8 v;  if (!r.byte(&v)) return false; w.byte(v); break; }
    case 'b': { bool v; if (!r.boolean(&v)) return false; w.boolean(v); break; }
    case 'n': { kv::i16 v; if (!r.i16(&v)) return false; w.i16(v); break; }
    case 'q': { kv::u16 v; if (!r.u16(&v)) return false; w.u16(v); break; }
    case 'i': { kv::i32 v; if (!r.i32(&v)) return false; w.i32(v); break; }
    case 'u': { kv::u32 v; if (!r.u32(&v)) return false; w.u32(v); break; }
    case 'x': { kv::i64 v; if (!r.i64(&v)) return false; w.i64(v); break; }
    case 't': { kv::u64 v; if (!r.u64(&v)) return false; w.u64(v); break; }
    case 'd': { kv::f64 v; if (!r.f64(&v)) return false; w.f64(v); break; }
    case 's': { const char* s; u32 n; if (!r.str(&s, &n)) return false; w.str_n(s, n); break; }
    case 'o': { const char* s; if (!r.path(&s)) return false; w.path(s); break; }
    case 'g': { const char* s; if (!r.signature(&s)) return false; w.signature(s); break; }
    case 'h': return false; // a descriptor is the message's: not copied into another
    case 'a':
        if (!r.array_begin()) return false;
        w.array_begin();
        while (r.array_more())
            while (r.peek()) if (!dbus_copy_value(r, w)) return false;
        if (r.failed) return false;
        w.array_end();
        break;
    case '(': case '{': {
        char open = r.peek();
        if (open == '(') { if (!r.struct_begin()) return false; w.struct_begin(); }
        else { if (!r.entry_begin()) return false; w.entry_begin(); }
        while (r.peek()) if (!dbus_copy_value(r, w)) return false;
        if (open == '(') { if (!r.struct_end()) return false; w.struct_end(); }
        else { if (!r.entry_end()) return false; w.entry_end(); }
        break;
    }
    case 'v': {
        const char* s;
        if (!r.variant_begin(&s)) return false;
        w.variant_begin(s);
        if (!dbus_copy_value(r, w)) return false;
        if (!r.variant_end()) return false;
        w.variant_end();
        break;
    }
    default: return false;
    }
    return !w.error;
}

} // namespace kv
