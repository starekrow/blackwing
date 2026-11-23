// Korvid hifi_codec: a file as a sparse span. The file's whole address range, cut into
// pieces of one size, and a map that says which pieces are there:
//
//   SparseSpan f{bytes, size, 18, have};          // 256 KB pieces; have: a byte per piece
//   if (sparse_has(f, at, len)) ... read [at, at + len) from f.bytes
//
// Whoever fills the span (the player, through the I/O queue) writes a piece's bytes and
// then sets its byte in `have` with a release store. A reader (a decoder) loads that byte
// with acquire before it reads the piece, and reads nothing it has not seen there. A piece
// that is not there may not be readable at all: the range is reserved address space, and
// a piece's pages are committed when it arrives.
//
// It is plain data, read by one thread and written by another now, and shared between
// processes in P1. modules/hifi_codec/README.md has the rest.
#pragma once
#include "korvid/core/atomic.h"
#include "korvid/core/types.h"

namespace kv {

// A range of a file's bytes.
struct ByteRange {
    u64 at  = 0;
    u64 len = 0;
};

struct SparseSpan {
    const u8* bytes       = nullptr; // `size` bytes of address space; only pieces that are there may be read
    u64       size        = 0;       // the file's length
    u32       piece_shift = 0;       // a piece is 1 << piece_shift bytes; the last may be shorter
    const u8* have        = nullptr; // a byte per piece, nonzero when it is there; load it with acquire
};

// How many pieces the span is cut into.
inline u64 sparse_pieces(const SparseSpan& s) { return s.size ? ((s.size - 1) >> s.piece_shift) + 1 : 0; }

// Whether piece `i` is there.
inline bool sparse_has_piece(const SparseSpan& s, u64 i) { return atomic_load(&s.have[i], MO_ACQUIRE) != 0; }

// Whether every byte of [at, at + len) that is inside the file is there. True for an empty
// range, or one wholly past the end.
bool sparse_has(const SparseSpan& s, u64 at, u64 len);

// [at, at + len) clipped to the file, with its front trimmed to the first piece that is not
// there: what must still arrive before all of the range can be read. Empty when it all can.
ByteRange sparse_missing(const SparseSpan& s, u64 at, u64 len);

// A span over bytes that are all there: one piece, the whole of them. For a caller that
// has a file in memory and nothing to wait for.
SparseSpan sparse_whole(Span<const u8> bytes);

} // namespace kv
