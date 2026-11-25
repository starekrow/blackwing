// The session: saved from the player's tables, restored through the player's commands.
//
// One file, session.json, in the user's settings directory: the queue's paths in order,
// the place and the frame heard, every parameter of the chain by its stage's and its own
// name, and whatever anyone asked to have kept (SessionKeep). One file is one moment: a
// queue saved at one time and a place at another would not belong to each other.
//
// Saving. What is kept is looked at four times a second; when it is not what was last
// saved, a save is due once nothing more has changed for a few seconds, so a drag is one
// write. While something plays the position is saved every ten seconds, so a kill loses
// no more. Paused or stopped with nothing changing, nothing is written. A write goes to a
// file beside the old one, through the I/O queue below the player's reads, and is renamed
// over it when it is whole: the old session or the new, never half of either. At shutdown
// a session that has changed is written before the module goes.
//
// Restoring. The file is read at start-up. In the ticks after, the kept values are said
// (SessionKept), the parameters set, and the queue enqueued a few dozen rows a tick, so a
// thousand rows do not hold the first frame nor ten thousand a tick. When the player has
// them all, the place is taken up without playing (PlayerCue): it comes back paused. A
// file that will not parse is set aside under another name, with a message, and the
// start is an empty one.
#include "korvid/blackwing_session/module.h"
#include "korvid/blackwing_session/schema.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/json.h"
#include "korvid/core/ktable.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include <new>
#include <string.h>

namespace kv {

namespace {

constinit SessionConfig g_session_config = {};
constexpr u32 kSessionVersion = 1;
constexpr f64 kLookSeconds = 0.25; // how often what is kept is looked at
constexpr const char* kFileName = "session.json";

struct Kept { char owner[16]; char key[32]; f64 number; char text[256]; };
struct SavedParam { char stage[32]; char name[16]; f64 value; };

struct Session {
    Engine* e = nullptr;
    char    path[4200] = "";   // the file
    SessionStatus state;
    Array<Kept> kept;
    bool    kept_changed = false;
    // What the file held, until it has been given to the player.
    Array<char*>      queue;   // paths, each from mem_alloc
    Array<SavedParam> params;
    u32     place = 0;         // the saved place: a position in the queue from 1; 0 none
    u64     frame = 0, frames = 0, rate = 0;
    bool    said_kept = false, set_params = false, cued = false;
    // Saving.
    u64     saved_sig = 0;     // what was last written, as a hash of it
    u64     seen_sig = 0;      // what was last seen
    f64     changed_at = 0.0;  // when it was last seen to change
    f64     looked_at = 0.0;
    f64     position_at = 0.0; // when the position was last saved
    Handle  write;             // a save on the I/O queue's task workers
    struct SaveJob* job = nullptr; // its file and bytes, the task's until the completion is taken
    f64     write_asked = 0.0;
    u64     write_sig = 0;
    usize   write_bytes = 0;
};

Session* session_of(Engine& e) { return engine_module_state_as<Session>(e, "session"); }

u64 mix(u64 h, const void* p, usize n) {
    const u8* b = (const u8*)p;
    for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

char* keep_string(const char* s) {
    usize n = strlen(s);
    char* out = (char*)mem_alloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

void free_saved(Session& s) {
    for (char* p : s.queue) mem_free(p);
    s.queue.free();
    s.queue = Array<char*>{};
    s.params.free();
    s.params = Array<SavedParam>{};
}

// ---- reading ---------------------------------------------------------------------------------

// The file's text into the session. False when it is not a session: not JSON, or with no
// version. Anything it has that is not known is passed over, and anything it lacks keeps
// its default: an older file is read, and a newer one as far as it is understood.
bool parse(Session& s, const char* text, usize size) {
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(text, size, &scope.arena->a, doc)) { format(s.state.reason, sizeof s.state.reason, "line %u: %s", doc.error_line, doc.error ? doc.error : "not JSON"); return false; }
    const JsonValue& root = doc.root();
    if (!root.is_object() || !json_get(doc, root, "blackwing_session")) { format(s.state.reason, sizeof s.state.reason, "it is not a session: it has no version"); return false; }
    if (const JsonValue* q = json_get(doc, root, "queue"))
        if (q->is_array())
            for (const JsonValue* v = json_first(doc, *q); v; v = json_next(doc, *v))
                if (v->is_string() && strlen(json_string(v, "")) < kPlayerPathMax) s.queue.push(keep_string(json_string(v, "")));
    s.place = (u32)json_u64(json_get(doc, root, "place"), 0);
    s.frame = json_u64(json_get(doc, root, "frame"), 0);
    s.frames = json_u64(json_get(doc, root, "frames"), 0);
    s.rate = json_u64(json_get(doc, root, "rate"), 0);
    if (s.place > s.queue.len) s.place = 0;
    if (const JsonValue* list = json_get(doc, root, "params"))
        if (list->is_array())
            for (const JsonValue* v = json_first(doc, *list); v; v = json_next(doc, *v)) {
                if (!v->is_object()) continue;
                SavedParam p{};
                format(p.stage, sizeof p.stage, "%s", json_string(json_get(doc, *v, "stage"), ""));
                format(p.name, sizeof p.name, "%s", json_string(json_get(doc, *v, "name"), ""));
                p.value = json_number(json_get(doc, *v, "value"), 0.0);
                if (p.stage[0] && p.name[0]) s.params.push(p);
            }
    if (const JsonValue* list = json_get(doc, root, "kept"))
        if (list->is_array())
            for (const JsonValue* v = json_first(doc, *list); v; v = json_next(doc, *v)) {
                if (!v->is_object()) continue;
                Kept k{};
                format(k.owner, sizeof k.owner, "%s", json_string(json_get(doc, *v, "owner"), ""));
                format(k.key, sizeof k.key, "%s", json_string(json_get(doc, *v, "key"), ""));
                k.number = json_number(json_get(doc, *v, "number"), 0.0);
                format(k.text, sizeof k.text, "%s", json_string(json_get(doc, *v, "text"), ""));
                if (k.owner[0] && k.key[0]) s.kept.push(k);
            }
    return true;
}

// The file at start-up. One that is there and will not parse is set aside under another
// name, not overwritten, and said; the start is then an empty one.
void read_file(Session& s) {
    IoBytes file;
    if (!io_read_file(s.path, &file)) return; // none: a first run
    bool ok = parse(s, (const char*)file.data, file.size);
    file.free();
    if (ok) return;
    free_saved(s);
    s.kept.clear();
    s.place = 0;
    char aside[4300];
    format(aside, sizeof aside, "%s.unreadable-%llu", s.path, (unsigned long long)(time_now() * 1000.0));
    bool moved = io_rename(s.path, aside);
    s.state.set_aside = 1;
    KV_WARN("session", "%s would not be read (%s): %s", s.path, s.state.reason, moved ? "it is set aside, and this start is an empty one" : "and could not be set aside");
    print_err("blackwing: the saved session would not be read (%s); it is kept as %s, and this start is an empty one\n", s.state.reason, moved ? aside : s.path);
}

// ---- writing ---------------------------------------------------------------------------------

struct Snapshot {
    Array<const PlayerItem*> rows; // by place
    u32 place = 0;
    u64 frame = 0, frames = 0, rate = 0;
    u64 sig = 0;                   // of everything but the frame
    bool playing = false;
};

// What there is to keep, read from the tables: the queue in the list's order, the place,
// the frame heard, and the parameters. `sig` is a hash of all of it but the frame, which
// moves all the time and is saved on a clock.
void look(TickContext& ctx, Session& s, Snapshot& snap) {
    const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
    const KTable* q = ctx.read_ktable("player.queue");
    const KTable* params = ctx.read_ktable("player.params");
    u64 sig = 14695981039346656037ull;
    if (t && q) {
        snap.rows.resize_fill(t->queued, nullptr);
        Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
        for (u32 i = 0; i < rows.len; ++i)
            if (q->is_alive(i) && rows[i].pos >= 1 && rows[i].pos <= snap.rows.len) snap.rows[rows[i].pos - 1] = &rows[i];
        // A row's id stands for its path: a row never changes the file it is of.
        for (const PlayerItem* r : snap.rows) { u64 id = r ? r->id : 0; sig = mix(sig, &id, sizeof id); }
        snap.place = t->place_pos <= t->queued ? t->place_pos : 0;
        snap.frame = t->item ? t->position : 0;
        snap.frames = t->item ? t->frames : 0;
        snap.rate = t->item ? t->rate : 0;
        snap.playing = t->state == (u8)PlayerState::Playing || t->state == (u8)PlayerState::Starting;
        sig = mix(sig, &snap.place, sizeof snap.place);
        // Stopped or paused, where it is held is part of what is kept; playing, it is the clock's.
        if (!snap.playing) sig = mix(sig, &snap.frame, sizeof snap.frame);
    }
    if (params) {
        Span<const PlayerParam> rows = params->rows_of<PlayerParam>();
        for (u32 i = 0; i < rows.len; ++i)
            if (params->is_alive(i)) { sig = mix(sig, &rows[i].stage, sizeof(u32) * 2); sig = mix(sig, &rows[i].value, sizeof(f64)); }
    }
    for (const Kept& k : s.kept) sig = mix(sig, &k, sizeof k);
    snap.sig = sig ? sig : 1;
}

// The session as text: the queue's paths in order, the place and the frame, the parameters
// by their stage's name and their own (so a chain put in another order next year still
// finds them), and the kept values.
void write_text(Session& s, Span<const PlayerItem* const> rows, u32 place, u64 frame, u64 frames, u64 rate, const KTable* chain, const KTable* table, Array<u8>& out) {
    MemScope scope;
    JsonBuilder b(&scope.arena->a);
    JsonRef root = b.object();
    b.set(root, "blackwing_session", b.integer(kSessionVersion));
    JsonRef queue = b.array();
    for (const PlayerItem* r : rows) b.push(queue, b.string(r ? r->path : ""));
    b.set(root, "queue", queue);
    b.set(root, "place", b.integer(place));
    b.set(root, "frame", b.unsigned_integer(frame));
    b.set(root, "frames", b.unsigned_integer(frames));
    b.set(root, "rate", b.unsigned_integer(rate));
    JsonRef params = b.array();
    if (chain && table) {
        Span<const PlayerStage> stages = chain->rows_of<PlayerStage>();
        Span<const PlayerParam> prow = table->rows_of<PlayerParam>();
        for (u32 i = 0; i < prow.len; ++i) {
            if (!table->is_alive(i)) continue;
            const char* stage = "";
            for (u32 k = 0; k < stages.len; ++k) if (chain->is_alive(k) && stages[k].index == prow[i].stage) stage = stages[k].name;
            JsonRef o = b.object();
            b.set(o, "stage", b.string(stage));
            b.set(o, "name", b.string(prow[i].name));
            b.set(o, "value", b.number(prow[i].value));
            b.push(params, o);
        }
    }
    b.set(root, "params", params);
    JsonRef kept = b.array();
    for (const Kept& k : s.kept) {
        JsonRef o = b.object();
        b.set(o, "owner", b.string(k.owner));
        b.set(o, "key", b.string(k.key));
        b.set(o, "number", b.number(k.number));
        b.set(o, "text", b.string(k.text));
        b.push(kept, o);
    }
    b.set(root, "kept", kept);
    JsonDoc doc;
    if (b.finish(root, doc)) json_write(doc, out);
    b.free();
}

void temp_path(const Session& s, char* out, usize cap) { format(out, cap, "%s.new", s.path); }

// A save, off the main thread: the whole file beside the old one, then renamed over it, so a
// kill leaves the old file or the new. Both calls block, which is a task's to do and a tick's
// not to (a dev build says so of a tick that does).
struct SaveJob {
    char     tmp[4300] = "";
    char     to[4200] = "";
    Array<u8> text;
};
i32 save_task(void* arg, const IoTaskCtl&) {
    SaveJob& j = *(SaveJob*)arg;
    i32 err = 0;
    if (!io_write_file(j.tmp, j.text.data, j.text.len, &err)) return err ? err : (i32)Err::Io;
    if (!io_rename(j.tmp, j.to, &err)) return err ? err : (i32)Err::Io;
    return 0;
}
void free_job(Session& s) {
    if (!s.job) return;
    s.job->text.free();
    s.job->~SaveJob();
    mem_free(s.job);
    s.job = nullptr;
}

// A save, asked of the I/O queue's task workers, below the player's reads.
void save(TickContext& ctx, Session& s, const Snapshot& snap) {
    IoQueue* io = platform_io(*ctx.engine);
    if (!io || !s.write.is_null()) return;
    SaveJob* j = new (mem_alloc(sizeof(SaveJob))) SaveJob();
    write_text(s, Span<const PlayerItem* const>(snap.rows.data, snap.rows.len), snap.place, snap.frame, snap.frames, snap.rate, ctx.read_ktable("player.chain"),
               ctx.read_ktable("player.params"), j->text);
    s.job = j;
    if (!j->text.len) { free_job(s); return; }
    temp_path(s, j->tmp, sizeof j->tmp);
    format(j->to, sizeof j->to, "%s", s.path);
    IoTaskRequest t;
    t.fn = save_task;
    t.arg = j;
    t.name = "session save";
    t.priority = IoPriority::Low;
    t.owner = &s;
    s.write = io->run_task(t);
    s.write_asked = time_now();
    s.write_sig = snap.sig;
    s.write_bytes = j->text.len;
    s.state.writing = s.write.is_null() ? 0 : 1;
    s.position_at = s.write_asked;
    if (s.write.is_null()) { free_job(s); KV_WARN("session", "the session could not be handed to the I/O queue"); }
}

// The save that was asked for, when it is whole: put in the old one's place.
void finish_write(TickContext& ctx, Session& s) {
    IoQueue* io = platform_io(*ctx.engine);
    if (!io || s.write.is_null()) return;
    IoCompletion c;
    if (!io->take(s.write, &c)) { engine_want_ticks(*ctx.engine); return; } // on its way: looked for again next tick
    if (c.status == IoStatus::Done) {
        s.saved_sig = s.write_sig;
        ++s.state.saves;
        s.state.save_bytes = s.write_bytes;
        s.state.save_seconds = time_now() - s.write_asked;
    } else {
        KV_WARN("session", "the session was not saved to %s: %s; the old one stands", s.path,
                c.status == IoStatus::Cancelled ? "cancelled" : os_error_text(c.error));
    }
    s.write = Handle{};
    s.state.writing = 0;
    free_job(s);
}

// ---- the units --------------------------------------------------------------------------------

void on_keep(TickContext& ctx, Span<const SessionKeep> events) {
    Session& s = ctx.state_as<Session>();
    for (const SessionKeep& k : events) {
        if (!k.owner[0] || !k.key[0]) continue;
        Kept* have = nullptr;
        for (Kept& x : s.kept) if (strcmp(x.owner, k.owner) == 0 && strcmp(x.key, k.key) == 0) have = &x;
        if (!have) { s.kept.push(Kept{}); have = &s.kept.last(); }
        Kept next{};
        format(next.owner, sizeof next.owner, "%s", k.owner);
        format(next.key, sizeof next.key, "%s", k.key);
        next.number = k.number;
        format(next.text, sizeof next.text, "%s", k.text);
        *have = next;
    }
}

void session_tick(TickContext& ctx) {
    Session& s = ctx.state_as<Session>();
    if (s.state.phase == (u8)SessionPhase::Off) return;
    Engine& e = *ctx.engine;
    f64 now = time_now();
    finish_write(ctx, s);

    if (s.state.phase == (u8)SessionPhase::Restoring) {
        engine_want_ticks(e); // until it is back, at the clock's rate
        // What was kept for others, once; the parameters, once the player's tables say what there is to set.
        if (!s.said_kept) {
            s.said_kept = true;
            for (const Kept& k : s.kept) {
                SessionKept out{};
                memcpy(out.owner, k.owner, sizeof out.owner);
                memcpy(out.key, k.key, sizeof out.key);
                out.number = k.number;
                memcpy(out.text, k.text, sizeof out.text);
                ctx.emit(out);
            }
        }
        const KTable* chain = ctx.read_ktable("player.chain");
        const KTable* table = ctx.read_ktable("player.params");
        if (!s.set_params && chain && table) {
            s.set_params = true;
            Span<const PlayerStage> stages = chain->rows_of<PlayerStage>();
            Span<const PlayerParam> rows = table->rows_of<PlayerParam>();
            for (const SavedParam& p : s.params)
                for (u32 i = 0; i < rows.len; ++i) {
                    if (!table->is_alive(i) || strcmp(rows[i].name, p.name) != 0) continue;
                    bool stage = false;
                    for (u32 k = 0; k < stages.len; ++k) stage = stage || (chain->is_alive(k) && stages[k].index == rows[i].stage && strcmp(stages[k].name, p.stage) == 0);
                    if (stage && rows[i].value != p.value) ctx.emit(PlayerSetParam{rows[i].stage, rows[i].index, p.value});
                }
        }
        // The queue, a few dozen rows a tick, in order.
        u32 per = g_session_config.rows_per_tick ? g_session_config.rows_per_tick : 64;
        for (u32 n = 0; n < per && s.state.queue_restored < s.queue.len; ++n, ++s.state.queue_restored) {
            PlayerEnqueue q{};
            format(q.path, sizeof q.path, "%s", s.queue[s.state.queue_restored]);
            ctx.emit(q);
        }
        if (s.state.queue_restored < s.queue.len) return;
        // All asked for. When the player has them, the place: taken up, not played.
        const PlayerTransport* t = ctx.resource<const PlayerTransport>("player.transport");
        const KTable* q = ctx.read_ktable("player.queue");
        if (!t || !q || t->queued < s.queue.len) return;
        if (!s.cued) {
            s.cued = true;
            if (s.place) {
                Span<const PlayerItem> rows = q->rows_of<PlayerItem>();
                for (u32 i = 0; i < rows.len; ++i)
                    if (q->is_alive(i) && rows[i].pos == s.place) { ctx.emit(PlayerCue{rows[i].id, s.frame, s.frames, s.rate}); break; }
                return; // the cue is the next tick's
            }
        }
        // Back as it was left. What the tables say now is what was saved: nothing to write.
        Snapshot snap;
        look(ctx, s, snap);
        snap.rows.free();
        s.saved_sig = s.seen_sig = snap.sig;
        s.looked_at = s.position_at = now;
        free_saved(s);
        s.state.phase = (u8)SessionPhase::Ready;
        return;
    }

    // Ready: looked at four times a second, not every tick.
    if (now - s.looked_at < kLookSeconds) return;
    s.looked_at = now;
    Snapshot snap;
    look(ctx, s, snap);
    if (snap.sig != s.seen_sig) { s.seen_sig = snap.sig; s.changed_at = now; }
    bool changed = s.seen_sig != s.saved_sig;
    s.state.dirty = changed ? 1 : 0;
    bool due = changed && now - s.changed_at >= g_session_config.change_seconds;
    // While it plays, the position on a clock: what a kill can lose.
    if (snap.playing && now - s.position_at >= g_session_config.position_seconds) due = true;
    if (due && s.write.is_null()) save(ctx, s, snap);
    snap.rows.free();
}

bool session_startup(Engine& e, ModuleInstance& self) {
    Session* s = arena_push<Session>(self.arena);
    if (!s) return false;
    self.set_state(s);
    s->e = &e;
    if (!engine_publish(e, self, ".status", &s->state)) return false;
    if (!g_session_config.enabled) return true;
    char dir[4200];
    if (g_session_config.dir && g_session_config.dir[0]) format(dir, sizeof dir, "%s", g_session_config.dir);
    else format(dir, sizeof dir, "%s", platform_user_dir(e, UserDir::Settings, "blackwing", "blackwing"));
    if (!dir[0]) { KV_WARN("session", "there is no settings directory to keep the session in: it is not kept"); return true; }
    io_make_dirs(dir);
    session_file_in(dir, s->path, sizeof s->path);
    // Saves run on the queue's task workers; one is enough, and another module may have started them.
    if (IoQueue* io = platform_io(e)) if (!io->tasks) io->start_tasks(1);
    read_file(*s);
    s->state.queue_total = s->queue.len;
    s->state.phase = (u8)SessionPhase::Restoring;
    engine_want_ticks(e);
    return true;
}

// At exit: a session that has changed since it was last written is written now, whole, as
// every write is. The tables are the player's, which shuts down after this module.
void session_shutdown(Engine& e, ModuleInstance& self) {
    Session* s = self.state_as<Session>();
    if (!s) return;
    // A save still on its way is cancelled (or, under way, waited for) for the one made here.
    if (IoQueue* io = platform_io(e)) { if (!s->write.is_null()) io->cancel(s->write); io->drain(s); }
    s->write = Handle{};
    free_job(*s);
    if (s->state.phase == (u8)SessionPhase::Ready) {
        const PlayerTransport* t = player_transport(e);
        KTable* q = engine_ktable(e, "player.queue");
        if (t && q) {
            Array<const PlayerItem*> rows{};
            rows.resize_fill(t->queued, nullptr);
            Span<PlayerItem> all = q->rows_of<PlayerItem>();
            for (u32 i = 0; i < all.len; ++i)
                if (q->is_alive(i) && all[i].pos >= 1 && all[i].pos <= rows.len) rows[all[i].pos - 1] = &all[i];
            Array<u8> text{};
            write_text(*s, Span<const PlayerItem* const>(rows.data, rows.len), t->place_pos <= t->queued ? t->place_pos : 0, t->item ? t->position : 0,
                       t->item ? t->frames : 0, t->item ? t->rate : 0, engine_ktable(e, "player.chain"), engine_ktable(e, "player.params"), text);
            rows.free();
            char tmp[4300];
            temp_path(*s, tmp, sizeof tmp);
            if (text.len && io_write_file(tmp, text.data, text.len) && io_rename(tmp, s->path)) ++s->state.saves;
            else KV_WARN("session", "the session was not saved to %s at exit", s->path);
            text.free();
        }
    }
    free_saved(*s);
    s->kept.free();
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".keep", KV_ON(SessionKeep, on_keep), .phase = Phase::PreSim},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::PostSim, .fn = session_tick, .reads = "player.transport, player.queue, player.params, player.chain",
     .writes = ".status", .emits = "PlayerEnqueue, PlayerCue, PlayerSetParam, SessionKept", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

void session_configure(const SessionConfig& config) { g_session_config = config; }

const SessionStatus* session_status(Engine& e) {
    Session* s = session_of(e);
    return s ? &s->state : nullptr;
}

void session_file_in(const char* dir, char* out, usize cap) {
    usize n = dir ? strlen(dir) : 0;
    bool slash = n && (dir[n - 1] == '/' || dir[n - 1] == '\\');
    format(out, cap, "%s%s%s", dir ? dir : "", n && !slash ? "/" : "", kFileName);
}

f64 session_peek(const char* dir, const char* owner, const char* key, f64 fallback) {
    char path[4200];
    session_file_in(dir, path, sizeof path);
    IoBytes file;
    if (!io_read_file(path, &file)) return fallback;
    f64 out = fallback;
    {
        MemScope scope;
        JsonDoc doc;
        if (json_parse((const char*)file.data, file.size, &scope.arena->a, doc) && doc.root().is_object())
            if (const JsonValue* list = json_get(doc, doc.root(), "kept"))
                if (list->is_array())
                    for (const JsonValue* v = json_first(doc, *list); v; v = json_next(doc, *v))
                        if (v->is_object() && strcmp(json_string(json_get(doc, *v, "owner"), ""), owner) == 0 && strcmp(json_string(json_get(doc, *v, "key"), ""), key) == 0)
                            out = json_number(json_get(doc, *v, "number"), fallback);
    }
    file.free();
    return out;
}

KV_MODULE_DEFINE(blackwing_session_module) = {
    .name      = "session",
    .events    = k_session_events,
    .resources = k_session_resources,
    .systems   = k_systems,
    .handlers  = k_handlers,
    .startup   = session_startup,
    .shutdown  = session_shutdown,
    .memory    = {.reserve = 64 * 1024},
};

} // namespace kv
