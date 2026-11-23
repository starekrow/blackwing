// A FLAC's metadata blocks (Vorbis comments, pictures) and APEv2. Both are lists of
// sized items, walked by their sizes; a picture's bytes are stepped over.
#include "reader.h"
#include <string.h>

namespace kv::tags {

namespace {

constexpr u64 kMaxComments = 4u << 20; // a Vorbis comment block read whole up to this

// A value whose key named `f`: numbers split, genres as names, the rest as they are.
void value(Out& o, TagSource s, TagField f, const u8* p, u64 n) {
    if (f == TagField::TrackNumber || f == TagField::DiscNumber || f == TagField::TrackTotal || f == TagField::DiscTotal ||
        f == TagField::Genre) {
        char buf[512];
        u64 m = n < sizeof buf ? n : sizeof buf;
        memcpy(buf, p, (usize)m);
        if (f == TagField::Genre) genres(o, s, buf, (u32)m);
        else if (f == TagField::TrackNumber) o.numbers(s, f, TagField::TrackTotal, buf, (u32)m);
        else if (f == TagField::DiscNumber) o.numbers(s, f, TagField::DiscTotal, buf, (u32)m);
        else o.numbers(s, f, TagField::Count, buf, (u32)m);
        return;
    }
    if (f == TagField::Compilation && n && (p[0] == 't' || p[0] == 'T' || p[0] == 'y' || p[0] == 'Y')) { // "true", "yes"
        o.utf8(s, f, (const u8*)"1", 1);
        return;
    }
    o.utf8(s, f, p, n);
}

void comments(Out& o, const u8* p, u64 n) {
    if (n < 8) { o.fail("a Vorbis comment block is cut short"); return; }
    u64 vendor = le32(p);
    if (vendor > n - 8) { o.fail("a Vorbis comment block's vendor runs past it"); return; }
    u64 at = 4 + vendor;
    u32 count = le32(p + at);
    at += 4;
    for (u32 i = 0; i < count; ++i) {
        if (i == kMaxItems) { o.fail("more than %u Vorbis comments: the rest are not read", kMaxItems); return; }
        if (n - at < 4) { o.fail("a Vorbis comment block says it has more comments than it holds"); return; }
        u64 len = le32(p + at);
        at += 4;
        if (len > n - at) { o.fail("a Vorbis comment runs past its block"); return; }
        const u8* c = p + at;
        at += len;
        const u8* eq = (const u8*)memchr(c, '=', (usize)len);
        if (!eq) continue;
        u32 klen = (u32)(eq - c);
        TagField f = field_for_key((const char*)c, klen);
        if (f == TagField::Count) continue;
        // Several values may share a key, one comment each; some writers put several in one, split by NULs.
        const u8* v = eq + 1;
        u64 vn = len - klen - 1;
        while (vn) {
            const u8* z = (const u8*)memchr(v, 0, (usize)vn);
            u64 one = z ? (u64)(z - v) : vn;
            value(o, TagSource::Vorbis, f, v, one);
            if (!z) break;
            v += one + 1; vn -= one + 1;
        }
    }
}

} // namespace

bool read_flac(File& f, Out& o, u64 at) {
    u64 pos = at + 4;
    for (u32 k = 0;; ++k) {
        if (k == kMaxItems) { o.fail("a FLAC file with more than %u metadata blocks", kMaxItems); return true; }
        const u8* h = f.get(pos, 4);
        if (!h) { if (f.waiting) return false; o.fail("a FLAC metadata block header could not be read"); return true; }
        const bool last = (h[0] & 0x80) != 0;
        const u32 type = h[0] & 0x7F;
        const u64 len = be24(h + 1), body = pos + 4;
        if (type == 127) { o.fail("a FLAC metadata block of a kind that is not one"); return true; }
        if (len > f.size() - body) { o.fail("a FLAC metadata block runs past the end of the file"); return true; }
        if (type == 4) {
            o.sources |= 1u << (u32)TagSource::Vorbis;
            if (len > kMaxComments) {
                o.fail("a Vorbis comment block of %llu bytes is larger than is read", (unsigned long long)len);
            } else {
                const u8* p = f.get(body, len);
                if (!p) { if (f.waiting) return false; o.fail("a Vorbis comment block could not be read"); }
                else comments(o, p, len);
            }
        } else if (type == 6 && len >= 32) {
            // A picture: its head says what it is and how long; its bytes are not read.
            const u8* p = f.get(body, 8);
            if (!p) { if (f.waiting) return false; }
            else {
                u64 mime = be32(p + 4);
                const u8* q = mime <= len - 32 ? f.get(body, 8 + mime + 4) : nullptr;
                if (!q && f.waiting) return false;
                u64 desc = q ? be32(q + 8 + mime) : ~0ull;
                u64 head = 8 + mime + 4 + desc + 20;
                if (q && desc <= len && head <= len) {
                    const u8* r = f.get(body + head - 4, 4);
                    if (!r && f.waiting) return false;
                    u64 n = r ? be32(r) : ~0ull;
                    if (r && n <= len - head) {
                        TagPicture pic;
                        pic.source = TagSource::Vorbis;
                        pic.kind = (u8)be32(p);
                        u64 c = mime < sizeof pic.mime - 1 ? mime : sizeof pic.mime - 1;
                        memcpy(pic.mime, q + 8, (usize)c);
                        pic.mime[c] = 0;
                        pic.at = body + head;
                        pic.len = n;
                        o.picture(pic);
                    }
                }
            }
        }
        pos = body + len;
        if (last) return true;
    }
}

bool read_ape(File& f, Out& o, u64 end, u64* length) {
    *length = 0;
    const u8* foot = f.get(end - 32, 32);
    if (!foot) return !f.waiting;
    o.sources |= 1u << (u32)TagSource::Ape;
    const u64 size = le32(foot + 12), count = le32(foot + 16), flags = le32(foot + 20);
    if (size < 32 || size > end) { o.fail("an APE tag's size is not one"); return true; }
    *length = size + ((flags & 0x80000000u) && size + 32 <= end ? 32u : 0u);
    const u64 items_end = end - 32;
    u64 pos = end - size;
    for (u64 i = 0; i < count; ++i) {
        if (i == kMaxItems) { o.fail("an APE tag with more than %u items: the rest are not read", kMaxItems); break; }
        if (items_end - pos < 10) { o.fail("an APE tag says it has more items than it holds"); break; }
        const u8* ih = f.get(pos, 8);
        if (!ih) { if (f.waiting) return false; break; }
        const u64 vlen = le32(ih), iflags = le32(ih + 4);
        u64 kmax = items_end - pos - 8 < 256 ? items_end - pos - 8 : 256;
        const u8* key = f.get(pos + 8, kmax);
        if (!key) { if (f.waiting) return false; break; }
        const u8* z = (const u8*)memchr(key, 0, (usize)kmax);
        if (!z) { o.fail("an APE item's key is not ended"); break; }
        const u32 klen = (u32)(z - key);
        const u64 vpos = pos + 8 + klen + 1;
        if (vlen > items_end - vpos) { o.fail("an APE item runs past the end of its tag"); break; }
        const u32 type = (u32)(iflags >> 1) & 3;
        if (type == 0) {
            TagField fld = field_for_key((const char*)key, klen);
            if (fld != TagField::Count) {
                if (vlen > kMaxItemBytes) { o.fail("an APE item of %llu bytes is larger than is read", (unsigned long long)vlen); }
                else {
                    const u8* v = f.get(vpos, vlen);
                    if (!v) { if (f.waiting) return false; }
                    else {
                        // Several values are split by NULs.
                        u64 vn = vlen;
                        while (vn) {
                            const u8* nz = (const u8*)memchr(v, 0, (usize)vn);
                            u64 one = nz ? (u64)(nz - v) : vn;
                            value(o, TagSource::Ape, fld, v, one);
                            if (!nz) break;
                            v += one + 1; vn -= one + 1;
                        }
                    }
                }
            }
        } else if (type == 1 && klen >= 9 && memcmp(key, "Cover Art", 9) == 0) {
            // A picture: a file name, a NUL, the bytes.
            u64 hn = vlen < 256 ? vlen : 256;
            const u8* v = f.get(vpos, hn);
            if (!v) { if (f.waiting) return false; }
            else if (const u8* nz = (const u8*)memchr(v, 0, (usize)hn)) {
                TagPicture pic;
                pic.source = TagSource::Ape;
                pic.kind = (klen >= 17 && memcmp(key + 9, " (Front)", 8) == 0) ? 3 : 0;
                u64 name = (u64)(nz - v);
                pic.at = vpos + name + 1;
                pic.len = vlen - name - 1;
                o.picture(pic);
            }
        }
        pos = vpos + vlen;
    }
    return true;
}

} // namespace kv::tags
