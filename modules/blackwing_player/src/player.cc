// The player: one system, each tick, on the main thread. It takes the commands that
// arrived, moves each item along (waiting, reading, ready, in hifi's hands), turns what
// hifi says into rows, events and the two resources, and that is all of it.
//
// What the player holds that hifi does not: the files' bytes. An item's bytes are read
// whole through the engine's I/O queue when it is the item playing or the one after, lent
// to hifi as a source, and kept until hifi has let go of them (Released) and the item is
// done. An item flushed out of hifi (a stop, a next) keeps its bytes and can go back in.
//
// Two things are true of an item in hifi's hands, and they end at different times. It is
// lent: hifi may read its bytes, until Released, which comes as soon as it is decoded.
// It is in play: hifi is to play it, until it has Ended or Failed or was flushed, which
// for a decoded item is seconds later.
#include "korvid/blackwing_player/module.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/hifi/hifi.h"
#include "korvid/hifi_codec/decoder.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 kMaxItems = 4096;

constinit PlayerConfig g_config = {};
constinit u32 g_queue_ktable = 0;

// The player's own record of an item, beside its row in the ktable.
struct Item {
    u64    id      = 0;
    Handle row;
    Handle read;              // the I/O request, while Reading
    u8*    data    = nullptr; // the file, once read
    usize  size    = 0;
    bool   lent    = false;   // hifi may be reading its bytes: added, and not yet Released
    bool   in_play = false;   // hifi is to play it: added, and neither ended, failed nor flushed
    PlayerItemState state = PlayerItemState::Waiting;
};

struct Player {
    Hifi*  hifi = nullptr;
    Item*  items = nullptr;   // kMaxItems of them
    u32    count = 0;
    u32    cursor = 0;        // the first item not Done or Failed: the one playing, or next to
    bool   want_play = false;
    bool   paused    = false;
    bool   ended_said = true; // PlayerQueueEnded has been emitted for this run of the queue
    u32    seq = 0;           // the last notification's number
    PlayerTransport transport;
    PlayerOutput    output;
};

Player* state(Engine& e) { return engine_module_state_as<Player>(e, "player"); }

PlayerItem& row_of(KTable* queue, const Item& it) { return queue->rows_of<PlayerItem>()[it.row.index()]; }

void set_state(KTable* queue, Item& it, PlayerItemState s, const char* reason = nullptr) {
    it.state = s;
    PlayerItem& r = row_of(queue, it);
    r.state = (u8)s;
    if (reason) format(r.reason, sizeof r.reason, "%s", reason);
}

// The I/O queue's data is the general allocator's, freed with the size it was read at.
void free_bytes(void* data, usize size) { if (data) free_in(mem_general(), data, size ? size : 1); }

void free_data(Item& it) {
    free_bytes(it.data, it.size);
    it.data = nullptr;
    it.size = 0;
}

Item* find(Player& p, u64 id) { return id >= 1 && id <= p.count ? &p.items[id - 1] : nullptr; }

bool finished(const Item& it) { return it.state == PlayerItemState::Done || it.state == PlayerItemState::Failed; }

void fail(TickContext& ctx, Player& p, KTable* queue, Item& it, const char* reason) {
    set_state(queue, it, PlayerItemState::Failed, reason);
    PlayerTrackFailed e{};
    e.item = it.id;
    e.seq = ++p.seq;
    format(e.reason, sizeof e.reason, "%s", reason);
    ctx.emit(e);
    it.in_play = false;
    if (!it.lent) free_data(it);
}

// Everything out of hifi, and the playhead held nowhere: after a stop or a next. What
// was in play and is not finished is ready again, with its bytes.
void flush(Player& p, KTable* queue) {
    hifi_flush(p.hifi);
    for (u32 k = p.cursor; k < p.count; ++k) {
        Item& it = p.items[k];
        if (!it.in_play) continue;
        it.in_play = false;
        if (!finished(it)) set_state(queue, it, PlayerItemState::Ready);
    }
    p.transport.item = 0;
    p.transport.position = 0;
}

void commands(TickContext& ctx, Player& p, KTable* queue) {
    for (const PlayerEnqueue& q : ctx.received<PlayerEnqueue>()) {
        if (p.count == kMaxItems) { KV_ERROR("player", "the queue is full (%u items): '%s' is not added", kMaxItems, q.path); continue; }
        Item& it = p.items[p.count];
        it = Item{};
        it.id = p.count + 1;
        PlayerItem r{};
        r.id = it.id;
        r.state = (u8)PlayerItemState::Waiting;
        memcpy(r.path, q.path, sizeof r.path);
        r.path[sizeof r.path - 1] = 0;
        it.row = queue->spawn(r);
        if (it.row.is_null()) { KV_ERROR("player", "the queue table is full: '%s' is not added", q.path); continue; }
        ++p.count;
        p.ended_said = false;
    }
    for (const PlayerStop& x : ctx.received<PlayerStop>()) {
        (void)x;
        p.want_play = false; p.paused = false;
        flush(p, queue);
        hifi_play(p.hifi); // a pause does not outlive a stop
    }
    for (const PlayerNext& x : ctx.received<PlayerNext>()) {
        (void)x;
        if (p.cursor < p.count) {
            Item& it = p.items[p.cursor];
            set_state(queue, it, PlayerItemState::Done); // skipped: done, with nothing said of how it ended
            it.in_play = false;
            if (!it.lent) free_data(it);
            flush(p, queue);
        }
    }
    for (const PlayerPause& x : ctx.received<PlayerPause>()) {
        (void)x;
        if (p.want_play) { p.paused = true; hifi_pause(p.hifi); }
    }
    for (const PlayerPlay& x : ctx.received<PlayerPlay>()) {
        (void)x;
        p.want_play = true;
        if (p.paused) { p.paused = false; hifi_play(p.hifi); }
        if (p.cursor < p.count) p.ended_said = false;
    }
}

// What hifi has to say, as rows and events.
void from_hifi(TickContext& ctx, Player& p, KTable* queue) {
    HifiEvent ev[32];
    for (;;) {
        u32 n = hifi_poll(p.hifi, ev, 32);
        for (u32 i = 0; i < n; ++i) {
            const HifiEvent& e = ev[i];
            if (e.kind == HifiEventKind::Underrun) { ctx.emit(HifiUnderrun{(u32)e.frames, ++p.seq}); continue; }
            Item* it = find(p, e.source);
            if (!it) continue;
            switch (e.kind) {
            case HifiEventKind::Started: {
                if (finished(*it) || !it->in_play) break; // skipped or stopped while its start was on its way
                set_state(queue, *it, PlayerItemState::Playing);
                PlayerItem& r = row_of(queue, *it);
                r.rate = e.info.format.rate; r.channels = e.info.format.channels; r.bits = e.info.format.bits; r.frames = e.info.frames;
                ctx.emit(PlayerTrackStarted{it->id, r.rate, r.channels, r.bits, r.frames, ++p.seq});
            } break;
            case HifiEventKind::Ended: {
                if (finished(*it) || !it->in_play) break;
                bool damaged = e.status == DecodeStatus::Damaged;
                set_state(queue, *it, PlayerItemState::Done, damaged ? e.reason : nullptr);
                row_of(queue, *it).damaged = damaged;
                it->in_play = false;
                if (!it->lent) free_data(*it);
                PlayerTrackEnded x{};
                x.item = it->id; x.frames = e.frames; x.damaged = damaged; x.seq = ++p.seq;
                format(x.reason, sizeof x.reason, "%s", damaged ? e.reason : "");
                ctx.emit(x);
            } break;
            case HifiEventKind::Failed:
                if (!finished(*it)) fail(ctx, p, queue, *it, e.reason);
                break;
            case HifiEventKind::Released:
                it->lent = false;
                if (finished(*it)) free_data(*it);
                break;
            default: break;
            }
        }
        if (n < 32) break;
    }
}

// Reads that have landed.
void from_io(TickContext& ctx, Player& p, KTable* queue) {
    IoQueue* io = platform_io(*ctx.engine);
    if (!io) return;
    for (u32 k = 0; k < p.count; ++k) {
        Item& it = p.items[k];
        if (it.read.is_null()) continue;
        for (const IoCompletion& c : io->completions()) {
            if (c.request != it.read) continue;
            IoCompletion done = c;
            io->reap(it.read);
            it.read = Handle{};
            if (finished(it)) { free_bytes(done.data, done.size); break; } // skipped while it was being read
            if (done.status != IoStatus::Done) {
                free_bytes(done.data, done.size);
                char why[96];
                format(why, sizeof why, "the file could not be read: %s", done.error ? strerror(done.error) : "the read was cancelled");
                fail(ctx, p, queue, it, why);
                break;
            }
            it.data = (u8*)done.data;
            it.size = done.size;
            if (!decoder_for(Span<const u8>(it.data, (u32)it.size))) fail(ctx, p, queue, it, "not a kind of audio file this build plays");
            else set_state(queue, it, PlayerItemState::Ready);
            break;
        }
    }
}

// Moves the cursor past what is finished, starts reads for the item playing and the one
// after it, and lends hifi each ready item in order.
void advance(TickContext& ctx, Player& p, KTable* queue) {
    while (p.cursor < p.count && finished(p.items[p.cursor])) ++p.cursor;
    if (!p.want_play) return;
    IoQueue* io = platform_io(*ctx.engine);
    u32 ahead = 0;
    bool gap = false; // an item before this one is not in hifi yet: later ones must wait their turn
    for (u32 k = p.cursor; k < p.count && ahead < 2; ++k) {
        Item& it = p.items[k];
        if (finished(it)) continue;
        ++ahead;
        if (it.state == PlayerItemState::Waiting && it.read.is_null() && !it.data) {
            const char* path = row_of(queue, it).path;
            IoStat st;
            if (!io)                                 { fail(ctx, p, queue, it, "there is no I/O queue to read with"); --ahead; continue; }
            if (!io_stat(path, &st) || !st.is_file)  { fail(ctx, p, queue, it, "there is no such file"); --ahead; continue; }
            if (st.size > g_config.read_ahead_bytes) { fail(ctx, p, queue, it, "the file is larger than the read-ahead cap"); --ahead; continue; }
            if (st.size >= 0xFFFFFFFFull)            { fail(ctx, p, queue, it, "the file is too large to hold in memory"); --ahead; continue; }
            IoRequest rq;
            rq.path = path;
            rq.priority = IoPriority::High;
            it.read = io->read_file(rq);
            if (it.read.is_null()) { fail(ctx, p, queue, it, "the I/O queue is full"); --ahead; continue; }
            set_state(queue, it, PlayerItemState::Reading);
        }
        if (it.state == PlayerItemState::Ready && !it.in_play && !it.lent && !gap) {
            HifiSource s;
            s.id = it.id;
            s.bytes = Span<const u8>(it.data, (u32)it.size);
            s.decoder = decoder_for(s.bytes);
            hifi_add_source(p.hifi, s);
            it.lent = it.in_play = true;
        }
        if (!it.in_play) gap = true;
    }
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
    u32 left = 0;
    bool busy = false; // anything of ours still in hifi's hands
    for (u32 k = 0; k < p.count; ++k) { left += !finished(p.items[k]); busy = busy || p.items[k].lent || p.items[k].in_play; }
    t.left = left;

    // The queue has run out when nothing is left to play and the last of it has been heard.
    if (p.want_play && left == 0 && !busy && s.source == 0 && !s.flushing) {
        p.want_play = false;
        if (!p.ended_said) { ctx.emit(PlayerQueueEnded{++p.seq}); p.ended_said = true; }
    }
    t.state = (u8)(!p.want_play ? PlayerState::Stopped : p.paused ? PlayerState::Paused : cur ? PlayerState::Playing : PlayerState::Starting);

    PlayerOutput& o = p.output;
    o.open = s.device_open; o.shared = s.shared; o.exact = s.exact;
    o.sample = (u8)s.format.sample; o.channels = s.format.channels; o.rate = s.format.rate;
    o.lead_frames = s.lead_frames; o.underruns = s.underruns;
}

// Phase PreSim, main thread, every tick, paused or not.
void player_pump(TickContext& ctx) {
    Player& p = ctx.state_as<Player>();
    KTable* queue = ctx.write_ktable(g_queue_ktable);
    commands(ctx, p, queue);
    from_io(ctx, p, queue);
    from_hifi(ctx, p, queue);
    advance(ctx, p, queue);
    publish(ctx, p, queue);
}

bool player_startup(Engine& e, ModuleInstance& self) {
    Player* p = arena_push<Player>(self.arena);
    self.set_state(p);
    p->items = (Item*)mem_alloc(sizeof(Item) * kMaxItems);
    HifiConfig hc;
    hc.device = g_config.device ? g_config.device : "default";
    hc.device_backend = g_config.device_backend;
    hc.lead_seconds = g_config.lead_seconds;
    p->hifi = hifi_create(hc);
    if (!p->hifi) { KV_ERROR("player", "the audio path's thread could not start"); return false; }
    return engine_publish(e, self, ".transport", &p->transport) && engine_publish(e, self, ".output", &p->output);
}

void player_shutdown(Engine& e, ModuleInstance& self) {
    (void)self;
    Player* p = state(e);
    if (!p) return;
    hifi_destroy(p->hifi); // first: after this nobody reads the items' bytes
    p->hifi = nullptr;
    IoQueue* io = platform_io(e);
    for (u32 k = 0; k < p->count; ++k) {
        Item& it = p->items[k];
        if (!it.read.is_null() && io) {
            // A read still out: cancel it, and take its bytes if it landed anyway.
            io->cancel(it.read);
            for (const IoCompletion& c : io->completions())
                if (c.request == it.read) { free_bytes(c.data, c.size); io->reap(it.read); break; }
        }
        free_data(it);
    }
    mem_free(p->items);
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

void cmd_status(CliContext& ctx, Span<const char* const> args) {
    if (args.len) { ctx.fail("player status"); return; }
    Player* p = state(*ctx.engine);
    if (!p) { ctx.fail("the player is not running"); return; }
    const PlayerTransport& t = p->transport;
    const PlayerOutput& o = p->output;
    f64 at = t.rate ? (f64)t.position / t.rate : 0.0, of = t.rate ? (f64)t.frames / t.rate : 0.0;
    ctx.print("%s; item %llu at %.1f of %.1f s; %u queued, %u left", player_state_name((PlayerState)t.state),
              (unsigned long long)t.item, at, of, t.queued, t.left);
    if (o.open) ctx.print("device: %u Hz, %u ch, %s, %s%s; lead %.2f s; %u underruns", o.rate, o.channels,
                          hifi_sample_format_name((HifiSampleFormat)o.sample), o.shared ? "shared" : "unshared", o.exact ? ", exact" : "",
                          o.rate ? (f64)o.lead_frames / o.rate : 0.0, o.underruns);
    else        ctx.print("device: none open");
    KTable* queue = engine_ktable(*ctx.engine, g_queue_ktable);
    if (!queue) return;
    for (u32 k = 0; k < p->count; ++k) {
        const PlayerItem& r = queue->rows_of<PlayerItem>()[p->items[k].row.index()];
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
    {.name = "player status", .help = "the transport, the device and the queue", .fn = cmd_status},
};

constinit const FieldDesc k_item_fields[] = {KV_FIELDS(PlayerItem, id, state, damaged, channels, bits, rate, frames, path, reason)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(PlayerItem, k_item_fields, CompCosmetic)};
constinit const KTableDesc k_ktables[] = {
    {.name = ".queue", .components = "PlayerItem", .capacity = kMaxItems, .id = &g_queue_ktable, .cosmetic = true},
};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT(PlayerEnqueue), KV_EVENT(PlayerPlay), KV_EVENT(PlayerPause), KV_EVENT(PlayerStop), KV_EVENT(PlayerNext),
    KV_EVENT(PlayerTrackStarted), KV_EVENT(PlayerTrackEnded), KV_EVENT(PlayerTrackFailed), KV_EVENT(PlayerQueueEnded), KV_EVENT(HifiUnderrun),
};
constinit const ResourceDesc k_resources[] = {{".transport"}, {".output"}};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = player_pump,
     .reads = "PlayerEnqueue, PlayerPlay, PlayerPause, PlayerStop, PlayerNext",
     .writes = ".queue, .transport, .output",
     .emits = "PlayerTrackStarted, PlayerTrackEnded, PlayerTrackFailed, PlayerQueueEnded, HifiUnderrun",
     .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(blackwing_player_module) = {
    .name       = "player",
    .events     = k_events,
    .components = k_components,
    .ktables    = k_ktables,
    .resources  = k_resources,
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
    case PlayerItemState::Waiting: return "waiting";
    case PlayerItemState::Reading: return "reading";
    case PlayerItemState::Ready:   return "ready";
    case PlayerItemState::Playing: return "playing";
    case PlayerItemState::Done:    return "done";
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
