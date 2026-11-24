// The model writer. The face writes a value when its source changed, not every tick: each
// key remembers a hash of what it last wrote, and a write of the same value is nothing: no
// event, no layout, no frame. A write that goes through asks the loop for a frame, which
// is the only reason the face ever asks for one.
#include "face.h"
#include "korvid/core/array.h"
#include "korvid/core/arena.h"
#include "korvid/core/json.h"
#include "korvid/core/kbor.h"
#include "korvid/krome/krome.h"
#include <string.h>

namespace kv {

namespace {

u64 hash_bytes(const void* p, usize n, u64 h) {
    const u8* b = (const u8*)p;
    for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h ? h : 1; // 0 means nothing written yet
}

// Whether `key` already shows the value hashed `h`; remembers it if not.
bool shown(Face& f, const char* key, u64 h) {
    for (u32 i = 0; i < f.nkeys; ++i) {
        if (f.keys[i].key != key && strcmp(f.keys[i].key, key) != 0) continue;
        if (f.keys[i].shown == h) return true;
        f.keys[i].shown = h;
        return false;
    }
    if (f.nkeys < kFaceKeys) f.keys[f.nkeys++] = FaceKey{key, h};
    return false; // past the table's end a key is written every time: correct, and counted
}

void wrote(Face& f) {
    ++f.stats.writes;
    engine_want_frame(*f.e);
}

} // namespace

void face_set(Face& f, const char* key, const char* value) {
    if (f.screen.is_null()) return;
    if (!value) value = "";
    usize n = strlen(value);
    if (shown(f, key, hash_bytes(value, n, 14695981039346656037ull))) return;
    if (n < KROME_SET_TEXT) {
        krome_set(*f.e, f.screen, key, value);
    } else {
        // A KromeSet carries 56 bytes; a title is longer. The value goes as a document.
        MemScope scope;
        JsonBuilder b(&scope.arena->a);
        JsonDoc doc;
        Array<u8> out{};
        if (b.finish(b.string(value), doc)) {
            kbor_write(doc, out);
            krome_set_doc(*f.e, f.screen, key, out.data, out.len);
        }
        out.free();
        b.free();
    }
    wrote(f);
}

void face_set(Face& f, const char* key, f64 value) {
    if (f.screen.is_null()) return;
    if (shown(f, key, hash_bytes(&value, sizeof value, 0x9E3779B97F4A7C15ull))) return;
    krome_set(*f.e, f.screen, key, value);
    wrote(f);
}

void face_set_bool(Face& f, const char* key, bool value) {
    if (f.screen.is_null()) return;
    u8 v = value ? 1 : 0;
    if (shown(f, key, hash_bytes(&v, 1, 0xC2B2AE3D27D4EB4Full))) return;
    krome_set_bool(*f.e, f.screen, key, value);
    wrote(f);
}

void face_set_doc(Face& f, const char* key, JsonBuilder& b, JsonRef root) {
    if (f.screen.is_null()) return;
    JsonDoc doc;
    Array<u8> out{};
    if (b.finish(root, doc)) {
        kbor_write(doc, out);
        if (!shown(f, key, hash_bytes(out.data, out.len, 0x27D4EB2F165667C5ull))) {
            krome_set_doc(*f.e, f.screen, key, out.data, out.len);
            wrote(f);
        }
    }
    out.free();
}

void face_forget(Face& f) {
    for (u32 i = 0; i < f.nkeys; ++i) f.keys[i].shown = 0;
}

void face_forget(Face& f, const char* key) {
    for (u32 i = 0; i < f.nkeys; ++i)
        if (f.keys[i].key == key || strcmp(f.keys[i].key, key) == 0) f.keys[i].shown = 0;
}

} // namespace kv
