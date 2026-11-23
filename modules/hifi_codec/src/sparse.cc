// The sparse span's questions: what is there, and what is not yet.
#include "korvid/hifi_codec/sparse.h"

namespace kv {

namespace {

constexpr u32 kWholeShift = 48;       // one piece of 256 TB: any file sparse_whole is given
constinit const u8 k_all_there = 1;   // the map of that one piece

} // namespace

ByteRange sparse_missing(const SparseSpan& s, u64 at, u64 len) {
    if (at >= s.size || len == 0) return ByteRange{};
    u64 end = s.size - at < len ? s.size : at + len;
    u64 last = (end - 1) >> s.piece_shift;
    for (u64 i = at >> s.piece_shift; i <= last; ++i) {
        if (sparse_has_piece(s, i)) continue;
        u64 from = i << s.piece_shift;
        if (from < at) from = at;
        return ByteRange{from, end - from};
    }
    return ByteRange{};
}

bool sparse_has(const SparseSpan& s, u64 at, u64 len) { return sparse_missing(s, at, len).len == 0; }

SparseSpan sparse_whole(Span<const u8> bytes) {
    SparseSpan s;
    s.bytes       = bytes.data;
    s.size        = bytes.len;
    s.piece_shift = kWholeShift;
    s.have        = &k_all_there;
    return s;
}

} // namespace kv
