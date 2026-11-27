// The player: one system, each tick, on the main thread. It takes the commands that
// arrived, moves each item along (waiting, reading, ready, in hifi's hands), turns what
// hifi says into rows, events and the two resources, and that is all of it.
//
// What the player holds that hifi does not: the files' bytes. An item's file is a sparse
// span: address space reserved at the file's size, committed a piece at a time as its
// pieces are read through the engine's I/O queue. The span is lent to hifi as a source
// once its first pieces are in, and the rest arrive while it plays. What is read, in
// order: the pieces a decoder is waiting for (hifi's wants); the first pieces of the item
// playing and the one after it; then each of those two on from where it is.
//
// The memory cap is on bytes committed, across every item. Past it, pieces are given
// back: first those of items hifi no longer reads, then those of the item playing that
// are behind the frame heard. A piece hifi may be reading is given back in two steps:
// its byte in the map is cleared, and its memory goes once hifi's read fence has passed.
//
// Two things are true of an item in hifi's hands, and they end at different times. It is
// lent: hifi may read its span, until Released, which comes as soon as it is decoded.
// It is in play: hifi is to play it, until it has Ended or Failed or was flushed, which
// for a decoded item is seconds later. An item flushed out of hifi (a stop, a next)
// keeps what it has read, and can go back in.
#include "korvid/blackwing_player/module.h"
#include "korvid/blackwing_player/schema.h"
#include "korvid/core/atomic.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/hifi/hifi.h"
#include "korvid/hifi_codec/decoder.h"
#include "korvid/hifi_eq/stages.h"
#include "korvid/hifi_platform/capture.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <new>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kMaxItems   = 4096;
constexpr u32 kMaxReads   = 8;    // piece reads out at once, across every item
constexpr u32 kMaxReturns = 4096; // pieces given back and waiting for hifi's fence
constexpr u32 kKeepBehind = 4;    // pieces kept behind the frame heard's estimated place
constexpr u32 kNone       = ~0u;

constinit PlayerConfig g_config = {};
constinit u32 g_queue_ktable = 0;
constinit u32 g_chain_ktable = 0;
constinit u32 g_params_ktable = 0;
constinit u32 g_presets_ktable = 0;

// The chain, fixed in this slice: gain, then the equalizer.
constinit const StageDesc* const k_chain[] = {&k_gain_stage, &k_eq_stage};
constexpr u32 kStages = sizeof(k_chain) / sizeof(k_chain[0]);

// What the player knows of one piece of a file.
enum class Piece : u8 { Absent, Reading, There, Returning, Broken };

// The player's own record of an item, beside its row in the ktable.
struct Item {
    u64    id      = 0;
    Handle row;
    Handle stat;              // the stat, while it is out
    u64    size    = 0;
    bool   sized   = false;   // the stat has said
    // The file in pieces: the span hifi reads, and the player's own state of each piece.
    u8*    base    = nullptr;
    usize  reserved = 0;
    u8*    have    = nullptr; // the span's map
    Piece* piece   = nullptr;
    u32*   touched = nullptr; // the tick each piece was last read or asked for: the least recent goes first
    u32    pieces  = 0;
    u64    bytes_there = 0;   // committed and read
    u64    head    = 0;       // bytes from the start decoder_for needs; 0 until the first piece has said
    const DecoderDesc* decoder = nullptr; // what decoder_for said of the head, once: the head may be given back later
    u32    front   = 0;       // where read-ahead looks next; nothing before it from the floor is Absent
    u32    want_lo = kNone, want_hi = 0; // the pieces hifi last said a decoder waits for
    char   broken[96] = "";   // why a piece could not be read
    u32    lends   = 0;       // hifi may be reading its span: added, and not yet Released (a jump can lend it again before the last lend is let go)
    u64    start   = 0;       // the frame its next lend begins at (a seek, or a jump to it)
    bool   seek    = false;   // and that lend goes on from elsewhere in it: Seeked, not Started
    bool   in_play = false;   // hifi is to play it: added, and neither ended, failed nor flushed
    PlayerItemState state = PlayerItemState::Idle;
    bool   alive   = false;   // the slot holds an item
    bool   done    = false;   // it is before the place: heard to its end, or skipped
    bool   removed = false;   // edited out of the list; its slot goes when hifi lets go of it
};

struct PieceRead { Handle request; u32 item = kNone; u32 piece = 0; }; // item kNone: let go of; free what comes
struct Return    { u32 item = 0; u32 piece = 0; u64 fence = 0; };

struct Player {
    Hifi*  hifi = nullptr;
    HifiCapture* capture = nullptr; // the capture device's, when the player made its own
    Item*  items = nullptr;   // kMaxItems slots; an item stays in its slot whatever the list does
    u32*   order = nullptr;   // the list: slot indices, in order
    u32    count = 0;         // items in the list
    u64    next_id = 1;
    u32    cursor = 0;        // the place: the position of the item playing, or next to
    bool   want_play = false;
    bool   paused    = false;
    bool   ended_said = true; // PlayerQueueEnded has been emitted for this run of the queue
    u32    seq = 0;           // the last notification's number
    u64    seek_item  = 0;    // a seek on its way to the ear: the row, and the frame it was sent to
    u64    seek_frame = 0;
    // A cue: a place taken up and not played (PlayerCue). The row, its frame, and what the
    // asker said of its rate and length, shown until it is heard. 0: none.
    u64    cue_item = 0, cue_frame = 0, cue_frames = 0;
    u32    cue_rate = 0;
    u32    shift = 18;        // a piece is 1 << shift bytes
    u64    committed = 0;     // bytes of pieces committed, across every item: there, being read, or being given back
    u64    returning = 0;     // of those, being given back: gone once hifi's fence has passed
    u32    tick = 0;          // pump ticks, for when a piece was last used
    PieceRead reads[kMaxReads];
    u32    nreads = 0;
    Return*   returns = nullptr; // kMaxReturns
    u32    nreturns = 0;
    PlayerTransport transport;
    PlayerOutput    output;
    // The chain as the player sees it: each parameter's value heard, and a change sent
    // and not yet heard (its generation, 0: none).
    Handle chain_rows[kStages];
    Handle param_rows[kStages][kHifiMaxParams];
    f64    heard_value[kStages][kHifiMaxParams] = {};
    f64    sent_value[kStages][kHifiMaxParams] = {};
    u32    sent_gen[kStages][kHifiMaxParams] = {};
    u8*    shadow[kStages] = {};   // a stage of each kind, to say whether the values heard are flat
};

Player* state(Engine& e) { return engine_module_state_as<Player>(e, "player"); }

PlayerItem& row_of(KTable* queue, const Item& it) { return queue->rows_of<PlayerItem>()[it.row.index()]; }

void set_state(KTable* queue, Item& it, PlayerItemState s, const char* reason = nullptr) {
    it.state = s;
    PlayerItem& r = row_of(queue, it);
    r.state = (u8)s;
    if (reason) format(r.reason, sizeof r.reason, "%s", reason);
}

void mark_done(KTable* queue, Item& it, const char* reason) {
    it.done = true;
    if (it.state != PlayerItemState::Failed) set_state(queue, it, PlayerItemState::Idle, reason);
}

// The item at position k of the list.
inline Item& at(Player& p, u32 k) { return p.items[p.order[k]]; }
inline const Item& at(const Player& p, u32 k) { return p.items[p.order[k]]; }

// An item by id, in the list or edited out of it and not yet let go of; null when none.
Item* find(Player& p, u64 id) {
    for (u32 i = 0; i < kMaxItems; ++i) if (p.items[i].alive && p.items[i].id == id) return &p.items[i];
    return nullptr;
}

// Its position in the list; count when it is not in it.
u32 pos_of(const Player& p, const Item& it) {
    u32 slot = (u32)(&it - p.items);
    for (u32 k = 0; k < p.count; ++k) if (p.order[k] == slot) return k;
    return p.count;
}

bool finished(const Item& it) { return it.done || it.removed || it.state == PlayerItemState::Failed; }

// Before the place now: heard, or skipped. Its row says idle; how it ended stays.
void mark_done(KTable* queue, Item& it, const char* reason = nullptr);

// ---- the file in pieces -----------------------------------------------------------------

u64 piece_len(const Player& p, const Item& it, u32 i) {
    u64 from = (u64)i << p.shift;
    return it.size - from < ((u64)1 << p.shift) ? it.size - from : (u64)1 << p.shift;
}
usize piece_commit(const Player& p, const Item& it, u32 i) { return os_page_align((usize)piece_len(p, it, i)); }

SparseSpan span_of(const Player& p, const Item& it) { return SparseSpan{it.base, it.size, p.shift, it.have}; }

// Address space for the whole file, and the maps; nothing committed yet.
bool make_span(Player& p, Item& it) {
    it.pieces   = (u32)((it.size + ((u64)1 << p.shift) - 1) >> p.shift);
    it.reserved = (usize)it.pieces << p.shift;
    it.base     = (u8*)os_reserve(it.reserved);
    if (!it.base) return false;
    it.have  = (u8*)mem_alloc(it.pieces);
    it.piece = (Piece*)mem_alloc(it.pieces * sizeof(Piece));
    it.touched = (u32*)mem_alloc(it.pieces * sizeof(u32));
    memset(it.touched, 0, it.pieces * sizeof(u32));
    memset(it.have, 0, it.pieces);
    for (u32 i = 0; i < it.pieces; ++i) it.piece[i] = Piece::Absent;
    it.front = 0;
    return true;
}

// Takes piece i's memory back, at once: nothing may be reading it.
void decommit(Player& p, Item& it, u32 i) {
    if (it.piece[i] == Piece::There) it.bytes_there -= piece_len(p, it, i);
    os_decommit(it.base + ((usize)i << p.shift), piece_commit(p, it, i));
    p.committed -= piece_commit(p, it, i);
    atomic_store(&it.have[i], (u8)0, MO_RELEASE);
    it.piece[i] = Piece::Absent;
}

// Gives piece i back. While the item is lent a decoder may be reading it: the byte is
// cleared now, and the memory goes when hifi's fence has passed (take_returns).
bool give_back(Player& p, Item& it, u32 i) {
    if (it.piece[i] != Piece::There) return false;
    if (!it.lends) { decommit(p, it, i); return true; }
    if (p.nreturns == kMaxReturns) return false;
    atomic_store(&it.have[i], (u8)0, MO_SEQ_CST);
    it.piece[i] = Piece::Returning;
    it.bytes_there -= piece_len(p, it, i);
    p.returns[p.nreturns++] = Return{(u32)(&it - p.items), i, hifi_read_fence(p.hifi)};
    p.returning += piece_commit(p, it, i);
    return true;
}

void take_returns(Player& p) {
    for (u32 k = 0; k < p.nreturns;) {
        Return& r = p.returns[k];
        if (!hifi_read_passed(p.hifi, r.fence)) { ++k; continue; }
        Item& it = p.items[r.item];
        if (it.base && it.piece[r.piece] == Piece::Returning) {
            os_decommit(it.base + ((usize)r.piece << p.shift), piece_commit(p, it, r.piece));
            p.committed -= piece_commit(p, it, r.piece);
            p.returning -= piece_commit(p, it, r.piece);
            it.piece[r.piece] = Piece::Absent;
        }
        p.returns[k] = p.returns[--p.nreturns];
    }
}

// Cancels the item's reads that are out. Their completions are freed when they come.
void cancel_reads(Player& p, IoQueue* io, Item& it) {
    u32 idx = (u32)(&it - p.items);
    for (u32 k = 0; k < p.nreads; ++k) {
        PieceRead& r = p.reads[k];
        if (r.item != idx) continue;
        if (io) io->cancel(r.request);
        if (it.base && it.piece[r.piece] == Piece::Reading) {
            it.piece[r.piece] = Piece::Absent;
            p.committed -= piece_commit(p, it, r.piece);
            os_decommit(it.base + ((usize)r.piece << p.shift), piece_commit(p, it, r.piece));
            if (r.piece < it.front) it.front = r.piece;
        }
        r.item = kNone;
    }
}

// Everything the item holds goes: its reads, its pieces, its address space. Only when
// hifi does not have it.
void drop_span(Player& p, IoQueue* io, Item& it) {
    if (it.lends) return;
    cancel_reads(p, io, it);
    if (!it.base) return;
    for (u32 i = 0; i < it.pieces; ++i)
        if (it.piece[i] == Piece::There || it.piece[i] == Piece::Returning) {
            if (it.piece[i] == Piece::There) it.bytes_there -= piece_len(p, it, i);
            else p.returning -= piece_commit(p, it, i);
            p.committed -= piece_commit(p, it, i);
            it.piece[i] = Piece::Absent;
        }
    for (u32 k = 0; k < p.nreturns;)  // not lent: nobody reads it, and its returns are moot
        if (p.items + p.returns[k].item == &it) p.returns[k] = p.returns[--p.nreturns]; else ++k;
    os_release(it.base, it.reserved);
    mem_free(it.have);
    mem_free(it.piece);
    mem_free(it.touched);
    it.base = nullptr; it.have = nullptr; it.piece = nullptr; it.touched = nullptr;
    it.reserved = 0; it.pieces = 0; it.bytes_there = 0; it.head = 0; it.decoder = nullptr;
    it.want_lo = kNone;
    it.sized = false; // if it comes back (previous, a jump), it is looked at again
}

// The state an item not in play and not finished is in, by what it holds: ready, with its
// head read; reading, with its span made; waiting, with nothing.
PlayerItemState resting(const Item& it) {
    return it.base && it.head ? PlayerItemState::Ready : it.base ? PlayerItemState::Reading : PlayerItemState::Idle;
}

void fail(TickContext& ctx, Player& p, KTable* queue, Item& it, const char* reason) {
    set_state(queue, it, PlayerItemState::Failed, reason);
    PlayerTrackFailed e{};
    e.item = it.id;
    e.seq = ++p.seq;
    format(e.reason, sizeof e.reason, "%s", reason);
    ctx.emit(e);
    it.in_play = false;
    drop_span(p, platform_io(*ctx.engine), it);
}

// The item as a source for hifi, from where its next lend begins.
HifiSource source_of(const Player& p, const Item& it) {
    HifiSource s;
    s.id = it.id;
    s.file = span_of(p, it);
    s.decoder = it.decoder;
    s.start = it.start;
    s.seek = it.seek;
    return s;
}

// Lends a ready item to hifi: added after what it has, or, given `out`, kept there for
// a jump to take.
void lend(Player& p, Item& it, HifiSource* out) {
    HifiSource s = source_of(p, it);
    if (out) *out = s; else hifi_add_source(p.hifi, s);
    ++it.lends;
    it.in_play = true;
    it.start = 0;
    it.seek = false;
}

// Nothing in play any more: what was, and is not finished, is ready again with what it
// has read. hifi may still be letting go of it.
void out_of_play(Player& p, KTable* queue) {
    for (u32 k = 0; k < kMaxItems; ++k) {
        Item& it = p.items[k];
        if (!it.alive) continue;
        it.want_lo = kNone;
        if (!it.in_play) continue;
        it.in_play = false;
        if (!finished(it)) set_state(queue, it, resting(it));
    }
    p.transport.item = 0;
    p.transport.position = 0;
}

// Everything out of hifi, and the playhead held nowhere: after a stop.
void flush(Player& p, KTable* queue) {
    hifi_flush(p.hifi);
    out_of_play(p, queue);
    for (u32 k = p.cursor; k < p.count; ++k) { at(p, k).start = 0; at(p, k).seek = false; }
    p.seek_item = 0;
}

// Moves the place in the queue to item index k, from its frame `frame` (`seek`: within
// the item heard, so Seeked rather than Started), and, if playing, jumps hifi there: what
// is sounding fades out at once, and what is ready from k on goes in its place. What is
// not ready yet follows as it becomes so (advance). What is before k is done; what is
// from k on, and was done, is to come again.
void jump_to(Player& p, KTable* queue, u32 k, u64 frame, bool seek) {
    if (k > p.count) k = p.count;
    for (u32 j = 0; j < p.count; ++j) {
        Item& it = at(p, j);
        if (it.state == PlayerItemState::Failed) continue;
        if (j < k && !it.done) mark_done(queue, it);                                     // skipped
        if (j >= k && it.done) {                                                         // to come again
            it.done = false;
            row_of(queue, it).damaged = 0;
            set_state(queue, it, resting(it), "");
        }
    }
    out_of_play(p, queue);
    p.cursor = k;
    p.seek_item = 0;
    if (k < p.count) {
        Item& it = at(p, k);
        it.start = frame;
        it.seek = seek;
        if (seek) { p.seek_item = it.id; p.seek_frame = frame; }
        // Read from the target on: where it is in the file, by its length in frames.
        u64 frames = row_of(queue, it).frames;
        if (it.base && frames && frame) it.front = (u32)((u64)((f64)it.size * ((f64)frame / (f64)frames)) >> p.shift);
    }
    if (!p.want_play) return;
    HifiSource srcs[2];
    u32 n = 0;
    for (u32 j = k; j < p.count && n < 2; ++j) {
        Item& it = at(p, j);
        if (it.state == PlayerItemState::Failed) continue;
        if (it.state != PlayerItemState::Ready) break; // the rest follow when they are
        lend(p, it, &srcs[n++]);
    }
    hifi_jump(p.hifi, srcs, n);
}

// Whether a move within the item is a seek: it is the one being heard, or a seek to it is
// on its way (a second command in the same tick, before anything is heard).
bool heard_now(const Player& p, const Item& it) { return p.transport.item == it.id || p.seek_item == it.id; }

// The index of the first item at or after k that is not Failed; count when there is none.
u32 playable_from(const Player& p, u32 k) {
    while (k < p.count && at(p, k).state == PlayerItemState::Failed) ++k;
    return k;
}

// The list, as positions: an item into it at k, and out of it at k.
void insert_at(Player& p, u32 k, u32 slot) {
    for (u32 i = p.count; i > k; --i) p.order[i] = p.order[i - 1];
    p.order[k] = slot;
    ++p.count;
}
u32 take_out(Player& p, u32 k) {
    u32 slot = p.order[k];
    for (u32 i = k; i + 1 < p.count; ++i) p.order[i] = p.order[i + 1];
    --p.count;
    return slot;
}

// Its slot freed and its row gone, once nothing of it is out: hifi has let go of it and
// no read is out for it.
void let_go(Player& p, IoQueue* io, KTable* queue, Item& it) {
    if (!it.removed) return;
    if (!it.row.is_null()) { queue->destroy(it.row); it.row = Handle{}; } // out of the table at once
    if (it.lends) return;                                                   // the slot when hifi lets go
    drop_span(p, io, it);
    it = Item{};
}

// What is before the place is done, and what is from it on is to come: after an edit, or
// a move of the place.
void restamp(Player& p, KTable* queue) {
    for (u32 k = 0; k < p.count; ++k) {
        Item& it = at(p, k);
        if (it.state == PlayerItemState::Failed) continue;
        if (k < p.cursor && !it.done) mark_done(queue, it);
        if (k >= p.cursor && it.done) {
            it.done = false;
            row_of(queue, it).damaged = 0;
            set_state(queue, it, resting(it), "");
        }
    }
}

// After edits: the place follows the row it was at, and what hifi has after that row is
// what the list now has after it. A row at the place that was removed is a jump to what
// is there now. What follows the place, if it changed, goes to hifi as a replacement
// after the place's row: free while that row is still being decoded, a recut of the lead
// when it is not.
void resync(Player& p, KTable* queue, u64 place_id, u32 fallback) {
    Item* place = place_id ? find(p, place_id) : nullptr;
    if (!place || place->removed) {
        u32 k = fallback < p.count ? fallback : p.count;
        if (p.want_play && place) jump_to(p, queue, playable_from(p, k), 0, false);
        else { p.cursor = k; restamp(p, queue); }
        return;
    }
    p.cursor = pos_of(p, *place);
    restamp(p, queue);
    if (!p.want_play || !place->in_play) return; // hifi has nothing of the place: advance lends in order
    u32 nk = playable_from(p, p.cursor + 1);
    Item* want = nk < p.count ? &at(p, nk) : nullptr;
    Item* has = nullptr;
    u32 others = 0;
    for (u32 i = 0; i < kMaxItems; ++i) {
        Item& it = p.items[i];
        if (!it.alive || &it == place || !it.in_play) continue;
        ++others;
        if (!has) has = &it;
    }
    if (others <= 1 && has == want) return; // what follows is what hifi has
    for (u32 i = 0; i < kMaxItems; ++i) if (p.items[i].alive && &p.items[i] != place && p.items[i].in_play) {
        Item& it = p.items[i];
        it.in_play = false;
        it.want_lo = kNone;
        if (!finished(it)) set_state(queue, it, resting(it));
    }
    HifiSource src;
    u32 n = 0;
    if (want && want->state == PlayerItemState::Ready) { lend(p, *want, &src); n = 1; }
    hifi_replace_after(p.hifi, place->id, &src, n);
}

// A parameter sent to hifi, kept within its range: heard a callback or two later.
void set_param(Player& p, u32 stage, u32 param, f64 value) {
    const StageParam& sp = k_chain[stage]->params[param];
    f64 v = value < sp.lo ? sp.lo : value > sp.hi ? sp.hi : value;
    p.sent_value[stage][param] = v;
    p.sent_gen[stage][param] = hifi_set_param(p.hifi, stage, param, v);
}

void commands(TickContext& ctx, Player& p, KTable* queue) {
    bool edited = false;          // the list changed after the place: what hifi has may be wrong
    u64 place_id = p.cursor < p.count ? at(p, p.cursor).id : 0;
    for (const PlayerEnqueue& q : ctx.received<PlayerEnqueue>()) {
        u32 slot = kMaxItems;
        for (u32 i = 0; i < kMaxItems && slot == kMaxItems; ++i) if (!p.items[i].alive) slot = i;
        if (slot == kMaxItems || p.count == kMaxItems) { KV_ERROR("player", "the queue is full (%u items): '%s' is not added", kMaxItems, q.path); continue; }
        Item& it = p.items[slot];
        it = Item{};
        it.id = p.next_id;
        PlayerItem r{};
        r.id = it.id;
        r.state = (u8)PlayerItemState::Idle;
        memcpy(r.path, q.path, sizeof r.path);
        r.path[sizeof r.path - 1] = 0;
        it.row = queue->spawn(r);
        if (it.row.is_null()) { KV_ERROR("player", "the queue table is full: '%s' is not added", q.path); continue; }
        it.alive = true;
        ++p.next_id;
        // Where: before a row, or at the end. Before the place, it is already behind it.
        Item* before = q.before ? find(p, q.before) : nullptr;
        u32 k = before && !before->removed ? pos_of(p, *before) : p.count;
        insert_at(p, k, slot);
        edited = true;
        p.ended_said = false;
    }
    u32 fallback = p.cursor;
    for (const PlayerQueueEdit& x : ctx.received<PlayerQueueEdit>()) {
        if (x.op == (u8)PlayerEditOp::Clear) {
            if (p.want_play || p.cursor < p.count) flush(p, queue);
            p.want_play = false; p.paused = false;
            hifi_play(p.hifi);
            IoQueue* io = platform_io(*ctx.engine);
            while (p.count) {
                Item& it = p.items[take_out(p, p.count - 1)];
                if (!it.stat.is_null() && io) io->cancel(it.stat);
                cancel_reads(p, io, it);
                it.removed = true;
                let_go(p, io, queue, it);
            }
            p.cursor = 0; fallback = 0; place_id = 0;
            edited = false;
            continue;
        }
        Item* it = find(p, x.item);
        if (!it || it->removed) { KV_ERROR("player", "there is no row %llu to edit", (unsigned long long)x.item); continue; }
        u32 k = pos_of(p, *it);
        if (x.op == (u8)PlayerEditOp::Remove) {
            take_out(p, k);
            if (it->id == place_id) fallback = k;
            else if (k < fallback) --fallback;
            IoQueue* io = platform_io(*ctx.engine);
            if (!it->stat.is_null() && io) io->cancel(it->stat);
            cancel_reads(p, io, *it);
            it->removed = true;
            let_go(p, io, queue, *it);
            edited = true;
        } else if (x.op == (u8)PlayerEditOp::Move) {
            Item* before = x.before ? find(p, x.before) : nullptr;
            if (before == it) continue;
            u32 slot = take_out(p, k);
            u32 to = before && !before->removed ? pos_of(p, *before) : p.count;
            insert_at(p, to, slot);
            edited = true;
        }
    }
    for (const PlayerStop& x : ctx.received<PlayerStop>()) {
        (void)x;
        p.want_play = false; p.paused = false;
        p.cue_item = 0;
        flush(p, queue);
        hifi_play(p.hifi); // a pause does not outlive a stop
    }
    for (const PlayerCue& x : ctx.received<PlayerCue>()) {
        Item* it = find(p, x.item);
        if (!it || it->removed || it->state == PlayerItemState::Failed) { KV_ERROR("player", "there is no row %llu to cue", (unsigned long long)x.item); continue; }
        // Nothing sounds, as after a stop; then the place, with nothing sent to hifi.
        p.want_play = false; p.paused = false;
        flush(p, queue);
        hifi_play(p.hifi);
        u64 to = x.frames && x.frame > x.frames ? x.frames : x.frame;
        jump_to(p, queue, pos_of(p, *it), to, false);
        p.cue_item = it->id;
        p.cue_frame = to;
        p.cue_frames = x.frames;
        p.cue_rate = (u32)x.rate;
    }
    for (const PlayerNext& x : ctx.received<PlayerNext>()) {
        (void)x;
        if (p.cursor < p.count) {
            Item& it = at(p, p.cursor);
            // Its reads still out are cancelled. Left to run, they would hold the disk
            // while the next item's waited behind them: on slow media, each skip added
            // reads, and none finished.
            IoQueue* io = platform_io(*ctx.engine);
            if (!it.stat.is_null() && io) io->cancel(it.stat);
            cancel_reads(p, io, it);
            jump_to(p, queue, playable_from(p, p.cursor + 1), 0, false); // skipped: done, with nothing said of how it ended
            drop_span(p, io, it);
        }
    }
    for (const PlayerPrevious& x : ctx.received<PlayerPrevious>()) {
        (void)x;
        if (p.cursor >= p.count) { // past the end: the last row that will play
            u32 k = p.count;
            while (k > 0 && at(p, k - 1).state == PlayerItemState::Failed) --k;
            if (k > 0) jump_to(p, queue, k - 1, 0, false);
            continue;
        }
        Item& it = at(p, p.cursor);
        const PlayerItem& r = row_of(queue, it);
        bool heard_it = p.transport.item == it.id;
        u64 now = p.seek_item == it.id ? p.seek_frame : heard_it ? p.transport.position : 0;
        u32 before = p.cursor;
        while (before > 0 && at(p, before - 1).state == PlayerItemState::Failed) --before;
        // Within the first three seconds, the row before; later, the start of this one.
        if (before > 0 && (!r.rate || now < (u64)r.rate * 3)) jump_to(p, queue, before - 1, 0, false);
        else jump_to(p, queue, p.cursor, 0, heard_now(p, it));
    }
    for (const PlayerJump& x : ctx.received<PlayerJump>()) {
        Item* it = find(p, x.item);
        if (!it || it->removed || it->state == PlayerItemState::Failed) { KV_ERROR("player", "there is no row %llu to play", (unsigned long long)x.item); continue; }
        jump_to(p, queue, pos_of(p, *it), 0, false);
        if (!p.want_play) { p.want_play = true; p.ended_said = false; } // play this row now
    }
    for (const PlayerSeek& x : ctx.received<PlayerSeek>()) {
        Item* it = x.item ? find(p, x.item) : (p.cursor < p.count ? &at(p, p.cursor) : nullptr);
        if (!it || it->removed || it->state == PlayerItemState::Failed) continue;
        u64 frames = row_of(queue, *it).frames, to = frames && x.frame > frames ? frames : x.frame;
        u32 k = pos_of(p, *it);
        jump_to(p, queue, k, to, k == p.cursor && heard_now(p, *it));
    }
    // A preset is its stage's parameters, each set: sent in one tick, so one recut renders
    // them all. Before the single changes, so one made with it in the same tick wins.
    for (const PlayerApplyPreset& x : ctx.received<PlayerApplyPreset>()) {
        if (x.stage >= kStages || x.preset >= k_chain[x.stage]->presets.len) {
            KV_ERROR("player", "there is no preset %u of stage %u", x.preset, x.stage);
            continue;
        }
        const StagePreset& preset = k_chain[x.stage]->presets[x.preset];
        for (u32 j = 0; j < preset.values.len && j < k_chain[x.stage]->params.len; ++j) set_param(p, x.stage, j, preset.values[j]);
    }
    for (const PlayerSetParam& x : ctx.received<PlayerSetParam>()) {
        if (x.stage >= kStages || x.param >= k_chain[x.stage]->params.len) {
            KV_ERROR("player", "there is no parameter %u of stage %u", x.param, x.stage);
            continue;
        }
        set_param(p, x.stage, x.param, x.value);
    }
    for (const PlayerSeekBy& x : ctx.received<PlayerSeekBy>()) {
        if (p.cursor >= p.count) continue;
        Item& it = at(p, p.cursor);
        const PlayerItem& r = row_of(queue, it);
        bool cued = p.cue_item == it.id && !p.want_play;
        u32 rate = r.rate ? r.rate : cued ? p.cue_rate : 0;
        u64 frames = r.frames ? r.frames : cued ? p.cue_frames : 0;
        if (!rate) continue; // not heard yet: nothing to count from
        // From wherever the last seek was going, so repeats add up.
        f64 from = p.seek_item == it.id ? (f64)p.seek_frame : p.transport.item == it.id ? (f64)p.transport.position : 0.0;
        f64 to = from + x.seconds * (f64)rate;
        if (to < 0.0) to = 0.0;
        if (frames && to > (f64)frames) to = (f64)frames;
        jump_to(p, queue, p.cursor, (u64)to, heard_now(p, it));
    }
    for (const PlayerPause& x : ctx.received<PlayerPause>()) {
        (void)x;
        if (p.want_play) { p.paused = true; hifi_pause(p.hifi); }
    }
    if (edited) resync(p, queue, place_id, fallback);
    for (const PlayerPlay& x : ctx.received<PlayerPlay>()) {
        (void)x;
        p.want_play = true;
        if (p.paused) { p.paused = false; hifi_play(p.hifi); }
        if (p.cursor < p.count) p.ended_said = false;
    }
    // A cue lasts while its row is the place and nothing plays. A seek while cued moved the
    // row's start: the cue follows it, and is no seek on its way to anyone's ear.
    if (p.cue_item) {
        Item* c = find(p, p.cue_item);
        if (!c || c->removed || c->state == PlayerItemState::Failed || p.want_play || p.cursor >= p.count || at(p, p.cursor).id != p.cue_item) {
            p.cue_item = 0;
        } else {
            p.cue_frame = c->start;
            c->seek = false;
            p.seek_item = 0;
        }
    }
}

// The chain's rows: the values heard, and whether each stage is flat with them.
void publish_chain(TickContext& ctx, Player& p) {
    KTable* chain = ctx.write_ktable(g_chain_ktable);
    KTable* params = ctx.write_ktable(g_params_ktable);
    if (!chain) return;
    for (u32 i = 0; i < kStages; ++i) {
        PlayerStage& r = chain->rows_of<PlayerStage>()[p.chain_rows[i].index()];
        const StageDesc& d = *k_chain[i];
        AudioFormat f{48000, 2, 24};
        void* st = d.open(f, Span<u8>(p.shadow[i], d.memory_bytes ? d.memory_bytes : 1));
        for (u32 j = 0; j < d.params.len && j < 48; ++j) {
            d.set(st, j, p.heard_value[i][j]);
            r.params[j] = p.heard_value[i][j];
            if (params && !p.param_rows[i][j].is_null()) params->rows_of<PlayerParam>()[p.param_rows[i][j].index()].value = p.heard_value[i][j];
        }
        r.flat = d.flat(st);
    }
}

// hifi says the parameters as of generation `gen` are heard: each change up to it is.
void heard_params(TickContext& ctx, Player& p, u32 gen) {
    bool any = false;
    for (u32 i = 0; i < kStages; ++i)
        for (u32 j = 0; j < k_chain[i]->params.len; ++j) {
            if (!p.sent_gen[i][j] || p.sent_gen[i][j] > gen) continue;
            p.sent_gen[i][j] = 0;
            bool moved = p.heard_value[i][j] != p.sent_value[i][j];
            p.heard_value[i][j] = p.sent_value[i][j];
            ctx.emit(PlayerParamChanged{i, j, p.heard_value[i][j], ++p.seq});
            any = any || moved;
        }
    if (any) publish_chain(ctx, p);
}

// With nothing reaching the ear (stopped, cued or paused), a parameter's new value has
// nobody to be heard by. It is the value that will be heard, and is said now: a readout
// follows the control, a session that sets the chain at start-up sees it set, and the
// player has nothing left to wait for. hifi has the value all the same, for the next
// render; when that is heard, there is nothing more to say.
void settle_params(TickContext& ctx, Player& p) {
    bool any = false;
    for (u32 i = 0; i < kStages; ++i)
        for (u32 j = 0; j < k_chain[i]->params.len; ++j) {
            if (!p.sent_gen[i][j]) continue;
            p.sent_gen[i][j] = 0;
            p.heard_value[i][j] = p.sent_value[i][j];
            ctx.emit(PlayerParamChanged{i, j, p.heard_value[i][j], ++p.seq});
            any = true;
        }
    if (any) publish_chain(ctx, p);
}

// What hifi has to say, as rows and events.
void from_hifi(TickContext& ctx, Player& p, KTable* queue) {
    HifiEvent ev[32];
    IoQueue* io = platform_io(*ctx.engine);
    for (;;) {
        u32 n = hifi_poll(p.hifi, ev, 32);
        for (u32 i = 0; i < n; ++i) {
            const HifiEvent& e = ev[i];
            if (e.kind == HifiEventKind::Underrun) { ctx.emit(HifiUnderrun{(u32)e.frames, ++p.seq}); continue; }
            if (e.kind == HifiEventKind::ParamsHeard) { heard_params(ctx, p, (u32)e.frames); continue; }
            Item* it = find(p, e.source);
            if (!it) continue;
            switch (e.kind) {
            case HifiEventKind::Started:
            case HifiEventKind::Seeked: {
                if (finished(*it) || !it->in_play) break; // skipped or stopped while its start was on its way
                set_state(queue, *it, PlayerItemState::Playing);
                PlayerItem& r = row_of(queue, *it);
                r.rate = e.info.format.rate; r.channels = e.info.format.channels; r.bits = e.info.format.bits; r.frames = e.info.frames;
                if (p.seek_item == it->id) p.seek_item = 0; // the seek has reached the ear
                if (e.kind == HifiEventKind::Seeked) ctx.emit(PlayerSeeked{it->id, e.frames, ++p.seq});
                else ctx.emit(PlayerTrackStarted{it->id, r.rate, r.channels, r.bits, r.frames, ++p.seq});
            } break;
            case HifiEventKind::Ended: {
                if (finished(*it) || !it->in_play) break;
                bool damaged = e.status == DecodeStatus::Damaged;
                mark_done(queue, *it, damaged ? e.reason : nullptr);
                row_of(queue, *it).damaged = damaged;
                it->in_play = false;
                drop_span(p, io, *it);
                PlayerTrackEnded x{};
                x.item = it->id; x.frames = e.frames; x.damaged = damaged; x.seq = ++p.seq;
                format(x.reason, sizeof x.reason, "%s", damaged ? e.reason : "");
                ctx.emit(x);
            } break;
            case HifiEventKind::Failed:
                if (!finished(*it)) fail(ctx, p, queue, *it, e.reason);
                break;
            case HifiEventKind::Released:
                if (it->lends) --it->lends;
                it->want_lo = kNone;
                if (it->removed) let_go(p, io, queue, *it);
                else if (finished(*it)) drop_span(p, io, *it);
                break;
            case HifiEventKind::Want: {
                if (!it->lends || !it->base || e.need.len == 0) break;
                u64 last = e.need.at + e.need.len - 1;
                it->want_lo = (u32)(e.need.at >> p.shift);
                it->want_hi = (u32)((last < it->size ? last : it->size - 1) >> p.shift);
                for (u32 k = it->want_lo; k <= it->want_hi && k < it->pieces; ++k) {
                    it->touched[k] = p.tick;
                    if (it->piece[k] == Piece::Broken) { hifi_starve(p.hifi, it->id, it->broken); break; }
                }
            } break;
            default: break;
            }
        }
        if (n < 32) break;
    }
}

// Stats and piece reads that have landed.
void from_io(TickContext& ctx, Player& p, KTable* queue) {
    IoQueue* io = platform_io(*ctx.engine);
    if (!io) return;
    // Stats.
    for (u32 k = 0; k < kMaxItems; ++k) {
        Item& it = p.items[k];
        if (!it.alive || it.stat.is_null()) continue;
        IoCompletion done;
        if (!io->take(it.stat, &done)) continue; // still on its way
        it.stat = Handle{};
        if (finished(it)) continue;
        if (done.status != IoStatus::Done) {
            char why[96];
            format(why, sizeof why, "the file could not be looked at: %s", done.error ? os_error_text(done.error) : "the stat was cancelled");
            fail(ctx, p, queue, it, why);
        } else if (!done.stat.is_file()) {
            fail(ctx, p, queue, it, "there is no such file");
        } else if (done.stat.size == 0) {
            fail(ctx, p, queue, it, "not a kind of audio file this build plays");
        } else {
            it.size = done.stat.size;
            it.sized = true;
            row_of(queue, it).size = it.size;
            if (!make_span(p, it)) fail(ctx, p, queue, it, "there is no address space for the file");
        }
    }
    // Pieces.
    bool arrived = false; // a piece of a file hifi has came in: a decoder may be waiting for it
    for (u32 k = 0; k < p.nreads;) {
        PieceRead r = p.reads[k];
        IoCompletion done;
        if (!io->take(r.request, &done)) { ++k; continue; } // still on its way
        p.reads[k] = p.reads[--p.nreads];
        if (r.item == kNone) { done.bytes.free(); continue; } // let go of meanwhile
        Item& it = p.items[r.item];
        if (!it.base || it.piece[r.piece] != Piece::Reading) { done.bytes.free(); continue; }
        u64 len = piece_len(p, it, r.piece);
        if (done.status == IoStatus::Done && done.bytes.size == len) {
            memcpy(it.base + ((usize)r.piece << p.shift), done.bytes.data, (usize)len);
            atomic_store(&it.have[r.piece], (u8)1, MO_RELEASE);
            it.piece[r.piece] = Piece::There;
            it.touched[r.piece] = p.tick;
            it.bytes_there += len;
            if (it.lends) arrived = true;
        } else if (done.status == IoStatus::Cancelled) {
            it.piece[r.piece] = Piece::Absent;
            p.committed -= piece_commit(p, it, r.piece);
            os_decommit(it.base + ((usize)r.piece << p.shift), piece_commit(p, it, r.piece));
            if (r.piece < it.front) it.front = r.piece;
        } else {
            // A piece that will not come: the source ends where it is needed (from_hifi's
            // Want), or, before it was lent, the item fails.
            it.piece[r.piece] = Piece::Broken;
            p.committed -= piece_commit(p, it, r.piece);
            os_decommit(it.base + ((usize)r.piece << p.shift), piece_commit(p, it, r.piece));
            if (done.status == IoStatus::Done) format(it.broken, sizeof it.broken, "the file was cut short while it was read");
            else format(it.broken, sizeof it.broken, "the file could not be read: %s", done.error ? os_error_text(done.error) : "unknown error");
            if (it.lends && it.want_lo != kNone && r.piece >= it.want_lo && r.piece <= it.want_hi) hifi_starve(p.hifi, it.id, it.broken);
            if (!it.lends && !it.in_play && !finished(it)) fail(ctx, p, queue, it, it.broken);
        }
        done.bytes.free();
    }
    if (arrived) hifi_wake(p.hifi);
}

// The piece below which the item playing has been heard, less a margin: what is behind
// it may be given back. Estimated by the file's length in frames, so it is kept loose.
u32 heard_floor(const Player& p, const Item& it, KTable* queue) {
    if (p.transport.item != it.id) return 0;
    u64 frames = row_of(queue, it).frames;
    if (!frames || !it.pieces) return 0;
    u64 at = (u64)((f64)it.size * ((f64)p.transport.position / (f64)frames));
    u32 piece = (u32)(at >> p.shift);
    return piece > kKeepBehind ? piece - kKeepBehind : 0;
}

// Makes room for one more piece under the cap. `urgent`: a decoder waits for it, and
// pieces read ahead may go too, the farthest first. What is being given back counts as
// gone when choosing what else must go, and as there until hifi's fence has passed, so
// the room may come only on a later tick. False until it has.
bool make_room(Player& p, KTable* queue, u64 need, bool urgent) {
    const u64 cap = g_config.memory_bytes;
    auto over = [&] { return p.committed - p.returning + need > cap; };
    if (!over()) return p.committed + need <= cap;
    // Items hifi does not read, the last first.
    for (u32 k = kMaxItems; k-- > 0 && over();) {
        Item& it = p.items[k];
        if (!it.alive || it.lends || !it.base) continue;
        for (u32 i = it.pieces; i-- > 0 && over();) give_back(p, it, i);
    }
    // The item playing, behind the frame heard.
    for (u32 k = p.cursor; k < p.count && over(); ++k) {
        Item& it = at(p, k);
        if (!it.lends || !it.base) continue;
        u32 floor = heard_floor(p, it, queue);
        for (u32 i = 0; i < floor && over(); ++i) give_back(p, it, i);
    }
    // A decoder waits: what has gone longest unused, read ahead or behind, but never what
    // is wanted. Least recent first, so a seek's search keeps what it has just looked at.
    while (over() && urgent) {
        Item* best = nullptr;
        u32 bi = 0, bt = ~0u;
        for (u32 k = p.cursor; k < p.count; ++k) {
            Item& it = at(p, k);
            if (!it.lends || !it.base) continue;
            for (u32 i = 0; i < it.pieces; ++i) {
                if (it.piece[i] != Piece::There) continue;
                if (it.want_lo != kNone && i >= it.want_lo && i <= it.want_hi) continue;
                if (it.touched[i] < bt) { bt = it.touched[i]; best = &it; bi = i; }
            }
        }
        if (!best || !give_back(p, *best, bi)) break;
        if (bi < best->front) best->front = bi;
    }
    return p.committed + need <= cap;
}

// Starts a read of piece i of the item.
bool start_read(Player& p, IoQueue* io, KTable* queue, Item& it, u32 i, IoPriority prio, bool urgent) {
    if (p.nreads == kMaxReads) return false;
    usize commit = piece_commit(p, it, i);
    if (!make_room(p, queue, commit, urgent)) return false;
    if (!os_commit(it.base + ((usize)i << p.shift), commit)) return false;
    IoRequest rq;
    rq.path = row_of(queue, it).path;
    rq.priority = prio;
    rq.offset = (u64)i << p.shift;
    rq.length = (usize)piece_len(p, it, i);
    rq.owner = &p; // the player's own list: drained at shutdown
    Handle h = io->read_file(rq);
    if (h.is_null()) { os_decommit(it.base + ((usize)i << p.shift), commit); return false; }
    p.committed += commit;
    it.piece[i] = Piece::Reading;
    p.reads[p.nreads++] = PieceRead{h, (u32)(&it - p.items), i};
    return true;
}

// The next piece of `it` to read, from `from` on; kNone when there is none.
u32 next_absent(const Item& it, u32 from) {
    for (u32 i = from; i < it.pieces; ++i) if (it.piece[i] == Piece::Absent) return i;
    return kNone;
}

// Starts what reads there is room for, in order: what decoders wait for; the first pieces
// of the two items ahead; then each of them on from where it is.
void schedule_reads(Player& p, IoQueue* io, KTable* queue, Item** ahead, u32 nahead) {
    for (u32 a = 0; a < nahead; ++a) {
        Item& it = *ahead[a];
        if (!it.base || it.want_lo == kNone) continue;
        for (u32 i = it.want_lo; i <= it.want_hi && i < it.pieces; ++i)
            if (it.piece[i] == Piece::Absent && !start_read(p, io, queue, it, i, IoPriority::High, true)) return;
    }
    for (u32 a = 0; a < nahead; ++a) {
        Item& it = *ahead[a];
        if (!it.base) continue;
        u64 head = it.head ? it.head : 1;
        u32 last = (u32)((head - 1) >> p.shift);
        for (u32 i = 0; i <= last && i < it.pieces; ++i)
            if (it.piece[i] == Piece::Absent && !start_read(p, io, queue, it, i, IoPriority::High, true)) return;
    }
    for (u32 a = 0; a < nahead; ++a) {
        Item& it = *ahead[a];
        if (!it.base || !it.lends) continue; // read ahead only of what hifi has
        u32 floor = heard_floor(p, it, queue);
        if (it.front < floor) it.front = floor;
        for (;;) {
            u32 i = next_absent(it, it.front);
            if (i == kNone) { it.front = it.pieces; break; }
            it.front = i;
            if (!start_read(p, io, queue, it, i, IoPriority::Normal, false)) return;
        }
    }
}

// Moves the cursor past what is finished, looks at the item playing and the one after
// it and reads their pieces, and lends hifi each ready item in order.
void advance(TickContext& ctx, Player& p, KTable* queue) {
    while (p.cursor < p.count && finished(at(p, p.cursor))) ++p.cursor;
    if (!p.want_play) return;
    IoQueue* io = platform_io(*ctx.engine);
    Item* ahead[2];
    u32 nahead = 0;
    bool gap = false; // an item before this one is not in hifi yet: later ones must wait their turn
    for (u32 k = p.cursor; k < p.count && nahead < 2; ++k) {
        Item& it = at(p, k);
        if (finished(it)) continue;
        if (it.state == PlayerItemState::Idle && it.stat.is_null() && !it.sized) {
            if (!io) { fail(ctx, p, queue, it, "there is no I/O queue to read with"); continue; }
            IoRequest rq;
            rq.path = row_of(queue, it).path;
            rq.priority = IoPriority::High;
            rq.owner = &p;
            it.stat = io->stat_file(rq);
            if (it.stat.is_null()) { fail(ctx, p, queue, it, "the I/O queue is full"); continue; }
            set_state(queue, it, PlayerItemState::Reading);
        }
        if (it.state == PlayerItemState::Reading && it.base && it.piece[0] == Piece::There) {
            // The head: as much as decoder_for needs to say, which the first piece tells.
            if (!it.head) it.head = decoder_head_bytes(Span<const u8>(it.base, (u32)piece_len(p, it, 0)), it.size);
            // Tags in a row in front: the head grows as each one's header is seen.
            while (it.head <= 0xFFFFFFFFull && sparse_has(span_of(p, it), 0, it.head)) {
                u64 more = decoder_head_bytes(Span<const u8>(it.base, (u32)it.head), it.size);
                if (more <= it.head) break;
                it.head = more;
            }
            if (sparse_has(span_of(p, it), 0, it.head)) {
                if (it.head <= 0xFFFFFFFFull) it.decoder = decoder_for(Span<const u8>(it.base, (u32)it.head));
                if (!it.decoder) {
                    fail(ctx, p, queue, it, "not a kind of audio file this build plays");
                    continue;
                }
                set_state(queue, it, PlayerItemState::Ready);
            }
        }
        ahead[nahead++] = &it;
        if (it.state == PlayerItemState::Ready && !it.in_play && !gap) lend(p, it, nullptr);
        if (!it.in_play) gap = true;
    }
    if (io) schedule_reads(p, io, queue, ahead, nahead);
}

void publish(TickContext& ctx, Player& p, KTable* queue) {
    HifiStatus s;
    hifi_status(p.hifi, &s);
    PlayerTransport& t = p.transport;
    Item* cur = find(p, s.source);
    if (cur && finished(*cur)) cur = nullptr; // skipped: what is still sounding of it is on its way out
    t.item     = cur ? cur->id : 0;
    t.position = cur ? s.position : 0;
    t.frames   = cur ? row_of(queue, *cur).frames : 0;
    t.rate     = cur ? row_of(queue, *cur).rate : 0;
    t.queued   = p.count;
    t.place    = p.cursor < p.count ? at(p, p.cursor).id : 0;
    t.place_pos = p.cursor + 1;
    t.seek_item  = p.seek_item;
    t.seek_frame = p.seek_item ? p.seek_frame : 0;
    u32 left = 0;
    bool busy = false; // anything of ours still in hifi's hands
    for (u32 k = 0; k < p.count; ++k) {
        Item& it = at(p, k);
        left += !finished(it);
        PlayerItem& r = row_of(queue, it);
        r.pos = k + 1;
        r.bytes_read = it.bytes_there;
    }
    for (u32 k = 0; k < kMaxItems; ++k) busy = busy || (p.items[k].alive && (p.items[k].lends || p.items[k].in_play));
    t.left = left;

    // The queue has run out when nothing is left to play and the last of it has been heard.
    if (p.want_play && left == 0 && !busy && s.source == 0 && !s.flushing) {
        p.want_play = false;
        if (!p.ended_said) { ctx.emit(PlayerQueueEnded{++p.seq}); p.ended_said = true; }
    }
    t.state = (u8)(!p.want_play ? PlayerState::Stopped : p.paused ? PlayerState::Paused : cur ? PlayerState::Playing : PlayerState::Starting);
    // Cued: held at a row and a frame that have not been heard. The row's rate and length
    // are the file's once it has been played, and until then what the cue said.
    if (p.cue_item && !p.want_play) {
        if (Item* c = find(p, p.cue_item)) {
            const PlayerItem& r = row_of(queue, *c);
            t.state = (u8)PlayerState::Paused;
            t.item = c->id;
            t.position = p.cue_frame;
            t.frames = r.frames ? r.frames : p.cue_frames;
            t.rate = r.rate ? r.rate : p.cue_rate;
        }
    }

    PlayerOutput& o = p.output;
    o.open = s.device_open; o.shared = s.shared; o.exact = s.exact;
    o.sample = (u8)s.format.sample; o.channels = s.format.channels; o.rate = s.format.rate;
    o.bits = s.device_open ? (u8)hifi_sample_bits(s.format.sample) : 0;
    o.float_word = s.device_open && s.format.sample == HifiSampleFormat::F32;
    o.lead_frames = s.lead_frames; o.underruns = s.underruns;
    o.bytes_held = p.committed;
    o.joining = s.joining;
    o.recut_seconds = s.recut_seconds;
    o.recut_render_seconds = s.recut_render_seconds;
    o.jump_seconds = s.jump_seconds;
    o.recuts = s.recuts;
}

// Phase PreSim, main thread, every tick, paused or not.
// Whether the next tick is wanted at the clock's rate (engine_want_ticks): while something
// is playing or about to, and while anything asked for has yet to be heard or read. A loop
// that paces ticks apart from frames, or sleeps between them, reads this; paused or
// stopped with nothing on its way, the player asks for nothing and costs nothing.
bool wants_ticks(const Player& p) {
    if (p.want_play && !p.paused) return true;
    if (p.nreads || p.nreturns || p.output.joining) return true;
    if (p.seek_item && !p.paused) return true;
    for (u32 i = 0; i < kStages; ++i)
        for (u32 j = 0; j < k_chain[i]->params.len; ++j)
            if (p.sent_gen[i][j]) return true;
    return false;
}

void player_pump(TickContext& ctx) {
    Player& p = ctx.state_as<Player>();
    KTable* queue = ctx.write_ktable(g_queue_ktable);
    ++p.tick;
    commands(ctx, p, queue);
    from_io(ctx, p, queue);
    from_hifi(ctx, p, queue);
    if (!p.want_play || p.paused) settle_params(ctx, p);
    take_returns(p);
    advance(ctx, p, queue);
    publish(ctx, p, queue);
    if (wants_ticks(p)) engine_want_ticks(*ctx.engine);
}

bool player_startup(Engine& e, ModuleInstance& self) {
    Player* p = arena_push<Player>(self.arena);
    self.set_state(p);
    p->items = (Item*)mem_alloc(sizeof(Item) * kMaxItems);
    p->order = (u32*)mem_alloc(sizeof(u32) * kMaxItems);
    for (u32 i = 0; i < kMaxItems; ++i) new (&p->items[i]) Item();
    p->returns = (Return*)mem_alloc(sizeof(Return) * kMaxReturns);
    u32 piece = g_config.piece_bytes;
    if (piece < os_page_size() || (piece & (piece - 1))) {
        KV_ERROR("player", "a piece of %u bytes is not a power of two of a page or more", piece);
        return false;
    }
    p->shift = 0;
    while ((1u << p->shift) < piece) ++p->shift;
    HifiConfig hc;
    hc.stages = k_chain;
    hc.device = g_config.device ? g_config.device : "default";
    hc.device_backend = g_config.device_backend;
    // The capture device with none given: one of the player's own, on the wall clock,
    // keeping nothing. A run with no hardware, for as long as the player lasts.
    if (!hc.device_backend && strcmp(hc.device, "capture") == 0) {
        HifiCaptureDesc cd;
        cd.wall_clock = true;
        cd.keep_bytes = 0;
        p->capture = hifi_capture_create(cd);
        hc.device_backend = p->capture;
    }
    hc.lead_seconds = g_config.lead_seconds;
    p->hifi = hifi_create(hc);
    if (!p->hifi) { KV_ERROR("player", "the audio path's thread could not start"); return false; }
    // The chain's rows, one per stage, at the stages' initial values.
    KTable* chain = engine_ktable(e, g_chain_ktable);
    KTable* params = engine_ktable(e, g_params_ktable);
    KTable* presets = engine_ktable(e, g_presets_ktable);
    for (u32 i = 0; i < kStages; ++i) {
        const StageDesc& d = *k_chain[i];
        p->shadow[i] = (u8*)mem_alloc(d.memory_bytes ? d.memory_bytes : 1);
        PlayerStage r{};
        r.index = i;
        r.flat = 1;
        r.params_len = (u8)(d.params.len < 48 ? d.params.len : 48);
        format(r.name, sizeof r.name, "%s", d.name);
        for (u32 j = 0; j < r.params_len; ++j) {
            format(r.param_names[j], sizeof r.param_names[j], "%s", d.params[j].name);
            r.params[j] = p->heard_value[i][j] = p->sent_value[i][j] = d.params[j].initial;
        }
        if (chain) p->chain_rows[i] = chain->spawn(r);
        for (u32 j = 0; params && j < d.params.len && j < kHifiMaxParams; ++j) {
            const StageParam& sp = d.params[j];
            PlayerParam pr{};
            pr.stage = i; pr.index = j;
            pr.value = pr.initial = sp.initial;
            pr.lo = sp.lo; pr.hi = sp.hi; pr.step = sp.step;
            format(pr.name, sizeof pr.name, "%s", sp.name);
            format(pr.unit, sizeof pr.unit, "%s", sp.unit);
            p->param_rows[i][j] = params->spawn(pr);
        }
        for (u32 j = 0; presets && j < d.presets.len; ++j) {
            PlayerPreset pr{};
            pr.stage = i; pr.index = j;
            format(pr.name, sizeof pr.name, "%s", d.presets[j].name);
            presets->spawn(pr);
        }
    }
    return engine_publish(e, self, ".transport", &p->transport) && engine_publish(e, self, ".output", &p->output);
}

void player_shutdown(Engine& e, ModuleInstance& self) {
    (void)self;
    Player* p = state(e);
    if (!p) return;
    hifi_destroy(p->hifi); // first: after this nobody reads the items' spans
    p->hifi = nullptr;
    if (p->capture) hifi_capture_destroy(p->capture);
    p->capture = nullptr;
    IoQueue* io = platform_io(e);
    for (u32 k = 0; k < kMaxItems; ++k) {
        Item& it = p->items[k];
        if (!it.alive) continue;
        it.stat = Handle{}; // drained below
        it.lends = 0;
        drop_span(*p, io, it);
    }
    // Stats and reads still out: cancelled and waited for, and their bytes freed if they landed anyway.
    if (io) io->drain(p);
    p->nreads = 0;
    mem_free(p->items);
    mem_free(p->order);
    mem_free(p->returns);
    for (u8* m : p->shadow) mem_free(m);
    p->items = nullptr;
}

// ---- the command line -------------------------------------------------------------------
// The same events anything else emits, from words.

void cmd_add(CliContext& ctx, Span<const char* const> args) {
    if (args.len == 0) { ctx.fail("player add <file>..."); return; }
    for (const char* a : args) {
        PlayerEnqueue q{};
        if (strlen(a) >= sizeof q.path) { ctx.fail("the path is too long: %s", a); return; }
        format(q.path, sizeof q.path, "%s", a);
        ctx.engine->events.emit(q);
    }
    ctx.print("queued %u", args.len);
}
void cmd_play(CliContext& ctx, Span<const char* const> args)  { if (args.len) { ctx.fail("player play"); return; } ctx.engine->events.emit(PlayerPlay{}); }
void cmd_pause(CliContext& ctx, Span<const char* const> args) { if (args.len) { ctx.fail("player pause"); return; } ctx.engine->events.emit(PlayerPause{}); }
void cmd_stop(CliContext& ctx, Span<const char* const> args)  { if (args.len) { ctx.fail("player stop"); return; } ctx.engine->events.emit(PlayerStop{}); }
void cmd_next(CliContext& ctx, Span<const char* const> args)  { if (args.len) { ctx.fail("player next"); return; } ctx.engine->events.emit(PlayerNext{}); }
void cmd_previous(CliContext& ctx, Span<const char* const> args) { if (args.len) { ctx.fail("player previous"); return; } ctx.engine->events.emit(PlayerPrevious{}); }

// A number from the command line; false when it is not one.
bool number(const char* a, f64* out) {
    char* end = nullptr;
    f64 v = strtod(a, &end);
    if (!a[0] || !end || *end) return false;
    *out = v;
    return true;
}

void cmd_jump(CliContext& ctx, Span<const char* const> args) {
    f64 row = 0.0;
    if (args.len != 1 || !number(args[0], &row) || row < 1.0) { ctx.fail("player jump <row>"); return; }
    ctx.engine->events.emit(PlayerJump{(u64)row});
}
void cmd_seek(CliContext& ctx, Span<const char* const> args) {
    f64 secs = 0.0;
    if (args.len != 1 || !number(args[0], &secs) || secs < 0.0) { ctx.fail("player seek <seconds>"); return; }
    Player* p = state(*ctx.engine);
    if (!p || p->cursor >= p->count) { ctx.fail("nothing to seek in"); return; }
    KTable* queue = engine_ktable(*ctx.engine, g_queue_ktable);
    u32 rate = queue ? queue->rows_of<PlayerItem>()[at(*p, p->cursor).row.index()].rate : 0;
    if (!rate) { ctx.fail("the row has not been heard yet: its rate is not known"); return; }
    ctx.engine->events.emit(PlayerSeek{0, (u64)(secs * rate)});
}
void cmd_volume(CliContext& ctx, Span<const char* const> args) {
    f64 db = 0.0;
    if (args.len != 1 || !number(args[0], &db)) { ctx.fail("player volume <dB, 0 at most>"); return; }
    ctx.engine->events.emit(PlayerSetParam{0, kGainDb, db});
}
void cmd_mute(CliContext& ctx, Span<const char* const> args) {
    bool on = args.len == 1 && strcmp(args[0], "on") == 0, off = args.len == 1 && strcmp(args[0], "off") == 0;
    if (!on && !off) { ctx.fail("player mute on|off"); return; }
    ctx.engine->events.emit(PlayerSetParam{0, kGainMute, on ? 1.0 : 0.0});
}
void cmd_set(CliContext& ctx, Span<const char* const> args) {
    f64 st = 0.0, pa = 0.0, v = 0.0;
    if (args.len != 3 || !number(args[0], &st) || !number(args[1], &pa) || !number(args[2], &v)) { ctx.fail("player set <stage> <param> <value>"); return; }
    ctx.engine->events.emit(PlayerSetParam{(u32)st, (u32)pa, v});
}
// player eq <band> <dB>; player eq preset <name>; player eq presets.
void cmd_eq(CliContext& ctx, Span<const char* const> args) {
    // The equalizer is stage 1; its presets are data it carries, which player.presets shows.
    Span<const StagePreset> presets = k_chain[1]->presets;
    if (args.len == 1 && strcmp(args[0], "presets") == 0) {
        for (const StagePreset& p : presets) ctx.print("%s", p.name);
        return;
    }
    if (args.len == 2 && strcmp(args[0], "preset") == 0) {
        for (u32 i = 0; i < presets.len; ++i)
            if (strcmp(presets[i].name, args[1]) == 0) { ctx.engine->events.emit(PlayerApplyPreset{1, i}); return; }
        ctx.fail("there is no preset '%s' (player eq presets)", args[1]);
        return;
    }
    f64 band = 0.0, db = 0.0;
    if (args.len != 2 || !number(args[0], &band) || !number(args[1], &db) || band < 0.0 || band >= kEqBands) {
        ctx.fail("player eq <band 0-9> <dB> | player eq preset <name> | player eq presets");
        return;
    }
    ctx.engine->events.emit(PlayerSetParam{1, (u32)band * 4 + 2, db});
}
void cmd_remove(CliContext& ctx, Span<const char* const> args) {
    f64 row = 0.0;
    if (args.len != 1 || !number(args[0], &row) || row < 1.0) { ctx.fail("player remove <row>"); return; }
    ctx.engine->events.emit(PlayerQueueEdit{(u8)PlayerEditOp::Remove, (u64)row, 0});
}
void cmd_move(CliContext& ctx, Span<const char* const> args) {
    f64 row = 0.0, before = 0.0;
    if (args.len < 1 || args.len > 2 || !number(args[0], &row) || row < 1.0 || (args.len == 2 && !number(args[1], &before))) {
        ctx.fail("player move <row> [<before row>]: to before that row, or to the end");
        return;
    }
    ctx.engine->events.emit(PlayerQueueEdit{(u8)PlayerEditOp::Move, (u64)row, (u64)before});
}
void cmd_clear(CliContext& ctx, Span<const char* const> args) {
    if (args.len) { ctx.fail("player clear"); return; }
    ctx.engine->events.emit(PlayerQueueEdit{(u8)PlayerEditOp::Clear, 0, 0});
}
void cmd_insert(CliContext& ctx, Span<const char* const> args) {
    f64 before = 0.0;
    if (args.len < 2 || !number(args[0], &before) || before < 1.0) { ctx.fail("player insert <before row> <file>..."); return; }
    for (u32 i = 1; i < args.len; ++i) {
        PlayerEnqueue q{};
        if (strlen(args[i]) >= sizeof q.path) { ctx.fail("the path is too long: %s", args[i]); return; }
        format(q.path, sizeof q.path, "%s", args[i]);
        q.before = (u64)before;
        ctx.engine->events.emit(q);
    }
}
void cmd_seekby(CliContext& ctx, Span<const char* const> args) {
    f64 by = 0.0;
    if (args.len != 1 || !number(args[0], &by)) { ctx.fail("player seekby <seconds>"); return; }
    ctx.engine->events.emit(PlayerSeekBy{by});
}

void cmd_status(CliContext& ctx, Span<const char* const> args) {
    if (args.len) { ctx.fail("player status"); return; }
    Player* p = state(*ctx.engine);
    if (!p) { ctx.fail("the player is not running"); return; }
    const PlayerTransport& t = p->transport;
    const PlayerOutput& o = p->output;
    f64 secs = t.rate ? (f64)t.position / t.rate : 0.0, of = t.rate ? (f64)t.frames / t.rate : 0.0;
    ctx.print("%s; item %llu at %.1f of %.1f s; %u queued, %u left", player_state_name((PlayerState)t.state),
              (unsigned long long)t.item, secs, of, t.queued, t.left);
    if (o.open) ctx.print("device: %u Hz, %u ch, %s, %s%s; lead %.2f s; %u underruns", o.rate, o.channels,
                          hifi_sample_format_name((HifiSampleFormat)o.sample), o.shared ? "shared" : "unshared", o.exact ? ", exact" : "",
                          o.rate ? (f64)o.lead_frames / o.rate : 0.0, o.underruns);
    else        ctx.print("device: none open");
    ctx.print("files: %.1f MB held of %.1f", (f64)o.bytes_held / (1 << 20), (f64)g_config.memory_bytes / (1 << 20));
    if (o.recuts) ctx.print("recuts: %u; the last %.1f ms from the change reaching the path to the join, the lead rendered again in %.1f ms", o.recuts, o.recut_seconds * 1e3, o.recut_render_seconds * 1e3);
    if (o.jump_seconds > 0.0) ctx.print("jumps: the last %.1f ms from reaching the path to its first frame out", o.jump_seconds * 1e3);
    for (u32 i = 0; i < kStages; ++i) {
        char line[256];
        usize len = (usize)format(line, sizeof line, "chain %u %s:", i, k_chain[i]->name);
        u32 shown = 0;
        for (u32 j = 0; j < k_chain[i]->params.len && len + 32 < sizeof line; ++j) {
            if (p->heard_value[i][j] == k_chain[i]->params[j].initial) continue; // only what has been changed
            len += (usize)format(line + len, sizeof line - len, " %s %g", k_chain[i]->params[j].name, p->heard_value[i][j]);
            ++shown;
        }
        if (!shown) format(line + len, sizeof line - len, " as it starts");
        ctx.print("%s", line);
    }
    KTable* queue = engine_ktable(*ctx.engine, g_queue_ktable);
    if (!queue) return;
    for (u32 k = 0; k < p->count; ++k) {
        const PlayerItem& r = queue->rows_of<PlayerItem>()[at(*p, k).row.index()];
        ctx.print("%4llu %-8s %s%s%s", (unsigned long long)r.id, player_item_state_name((PlayerItemState)r.state), r.path,
                  r.reason[0] ? ": " : "", r.reason);
    }
}

constinit const CliCommandDesc k_commands[] = {
    {.name = "player add",    .help = "<file>... adds files to the end of the queue", .fn = cmd_add},
    {.name = "player play",   .help = "plays, or goes on from a pause", .fn = cmd_play},
    {.name = "player pause",  .help = "holds the playhead", .fn = cmd_pause},
    {.name = "player stop",   .help = "stops; the item playing starts over next time", .fn = cmd_stop},
    {.name = "player next",   .help = "ends the item playing and goes on to the next", .fn = cmd_next},
    {.name = "player previous", .help = "the row before, within three seconds of a start; otherwise this one from its start", .fn = cmd_previous},
    {.name = "player jump",   .help = "<row> plays that row now", .fn = cmd_jump},
    {.name = "player seek",   .help = "<seconds> goes to that point of the row playing", .fn = cmd_seek},
    {.name = "player seekby", .help = "<seconds> goes that far on (or back, negative) from where the last seek was going", .fn = cmd_seekby},
    {.name = "player volume", .help = "<dB> the volume: 0 at most", .fn = cmd_volume},
    {.name = "player mute",   .help = "on|off", .fn = cmd_mute},
    {.name = "player set",    .help = "<stage> <param> <value> a parameter of the chain", .fn = cmd_set},
    {.name = "player eq",     .help = "<band> <dB>, preset <name>, presets: the equalizer", .fn = cmd_eq},
    {.name = "player remove", .help = "<row> takes a row out of the queue", .fn = cmd_remove},
    {.name = "player move",   .help = "<row> [<before row>] moves a row: before another, or to the end", .fn = cmd_move},
    {.name = "player insert", .help = "<before row> <file>... adds files before a row", .fn = cmd_insert},
    {.name = "player clear",  .help = "empties the queue, and stops", .fn = cmd_clear},
    {.name = "player status", .help = "the transport, the device and the queue", .fn = cmd_status},
};

constinit const KTableDesc k_ktables[] = {
    {.name = ".queue", .components = "PlayerItem", .capacity = kMaxItems, .id = &g_queue_ktable, .cosmetic = true},
    {.name = ".chain", .components = "PlayerStage", .capacity = kHifiMaxStages, .id = &g_chain_ktable, .cosmetic = true},
    {.name = ".params", .components = "PlayerParam", .capacity = kHifiMaxStages * kHifiMaxParams, .id = &g_params_ktable, .cosmetic = true},
    {.name = ".presets", .components = "PlayerPreset", .capacity = 64, .id = &g_presets_ktable, .cosmetic = true},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = player_pump,
     .reads = "PlayerEnqueue, PlayerPlay, PlayerPause, PlayerStop, PlayerNext, PlayerPrevious, PlayerJump, PlayerSeek, PlayerSeekBy, PlayerCue, PlayerSetParam, PlayerApplyPreset, PlayerQueueEdit",
     .writes = ".queue, .chain, .params, .presets, .transport, .output",
     .emits = "PlayerTrackStarted, PlayerTrackEnded, PlayerTrackFailed, PlayerQueueEnded, PlayerSeeked, PlayerParamChanged, HifiUnderrun",
     .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(blackwing_player_module) = {
    .name       = "player",
    .events     = k_player_events,
    .components = k_player_components,
    .ktables    = k_ktables,
    .resources  = k_player_resources,
    .systems    = k_systems,
    .commands   = k_commands,
    .startup    = player_startup,
    .shutdown   = player_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

void player_configure(const PlayerConfig& config) { g_config = config; }

const PlayerTransport* player_transport(Engine& e) { Player* p = state(e); return p ? &p->transport : nullptr; }
const PlayerOutput*    player_output(Engine& e)    { Player* p = state(e); return p ? &p->output : nullptr; }

const char* player_item_state_name(PlayerItemState s) {
    switch (s) {
    case PlayerItemState::Idle:    return "idle";
    case PlayerItemState::Reading: return "reading";
    case PlayerItemState::Ready:   return "ready";
    case PlayerItemState::Playing: return "playing";
    case PlayerItemState::Failed:  return "failed";
    case PlayerItemState::Count:   break;
    }
    return "?";
}

const char* player_state_name(PlayerState s) {
    switch (s) {
    case PlayerState::Stopped:  return "stopped";
    case PlayerState::Starting: return "starting";
    case PlayerState::Playing:  return "playing";
    case PlayerState::Paused:   return "paused";
    case PlayerState::Count:    break;
    }
    return "?";
}

} // namespace kv
