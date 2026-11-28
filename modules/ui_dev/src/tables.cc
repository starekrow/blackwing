// The Tables panel: any table or resource of the player or the library, by name, shown
// from its own description: the field table the engine holds for it. Nothing here names a
// field, so one added to a table next slice appears with no change to the face.
//
// The list is lines. A resource is a line a field. A table is, for each row, a line that
// says which row and a line a field; fifty rows of twelve fields are six hundred and
// fifty lines, of which the list builds the thirty in view and this fills those.
//
// A table added to the player or the library gets a line in k_tables, and its name in
// ui_dev.sync's `reads`: a unit reads what it declared.
#include "face.h"
#include "korvid/core/arena.h"
#include "korvid/core/component.h"
#include "korvid/core/format.h"
#include "korvid/core/ktable.h"
#include <string.h>

namespace kv {

namespace {

struct TableName { const char* name; bool resource; };
constinit const TableName k_tables[] = {
    {"player.transport", true}, {"player.output", true}, {"player.queue", false}, {"player.chain", false}, {"player.params", false},
    {"player.presets", false}, {"player.devices", false}, {"library.scan", true}, {"library.roots", false}, {"library.rows", false},
    {"media.status", true},
};
constexpr u32 kTables = KV_COUNTOF(k_tables);
constexpr u32 kTableLines = 256; // lines handed over at once, at most

// A field's value as text: a string quoted, numbers as they are, a few of an array's.
void field_text(const FieldDesc& fd, const void* row, char* out, usize cap) {
    if (fd.type == FieldType::Char) { schema_field_string(fd, row, out, cap); return; }
    usize n = 0;
    out[0] = 0;
    u32 lanes = fd.count < 12 ? fd.count : 12;
    for (u32 i = 0; i < lanes && n + 24 < cap; ++i) {
        f64 v = 0.0;
        if (i) n += (usize)format(out + n, cap - n, ", ");
        if (fd.type == FieldType::Handle) { n += (usize)format(out + n, cap - n, "(a handle)"); continue; }
        if (!schema_field_lane(fd, row, i, &v)) { n += (usize)format(out + n, cap - n, "?"); continue; }
        if (fd.type == FieldType::Bool) n += (usize)format(out + n, cap - n, "%s", v != 0.0 ? "true" : "false");
        else if (fd.type == FieldType::F32 || fd.type == FieldType::F64) n += (usize)format(out + n, cap - n, "%g", v);
        else if (fd.type == FieldType::U64) { u64 x; memcpy(&x, (const u8*)row + fd.offset + i * 8, 8); n += (usize)format(out + n, cap - n, "%llu", (unsigned long long)x); } // past what a double holds
        else n += (usize)format(out + n, cap - n, "%.0f", v);
    }
    if (fd.count > lanes && n + 16 < cap) format(out + n, cap - n, ", ... (%u)", fd.count);
}

void put_line(JsonBuilder& b, JsonRef list, const char* name, const char* value, bool head) {
    JsonRef o = b.object();
    b.set(o, "name", b.string(name));
    b.set(o, "value", b.string(value));
    b.set(o, "ink", b.string(head ? "#7aa7ff" : "#8b95a7"));
    b.push(list, o);
}

} // namespace

void tables_sync(TickContext& ctx, Face& f) {
    if (f.tab != FaceTab::Tables) return;
    Engine& e = *ctx.engine;
    MemScope scope;
    char text[320];
    {
        JsonBuilder b(&scope.arena->a);
        JsonRef names = b.array();
        for (u32 i = 0; i < kTables; ++i) {
            JsonRef o = b.object();
            b.set(o, "i", b.integer(i));
            b.set(o, "name", b.string(k_tables[i].name));
            b.push(names, o);
        }
        face_set_doc(f, "x_names", b, names);
        b.free();
    }
    if (f.x_name >= kTables) f.x_name = 0;
    const TableName& pick = k_tables[f.x_name];
    u32 first = f.x_first, count = f.x_count ? (f.x_count < kTableLines ? f.x_count : kTableLines) : 40;
    JsonBuilder b(&scope.arena->a);
    JsonRef lines = b.array();
    u32 total = 0;

    if (pick.resource) {
        // The resource's entry in the engine: its field table, and the object published.
        u32 id = engine_resource_id(e, pick.name);
        const ResourceEntry* r = id ? &e.resources[id - 1] : nullptr;
        if (r && r->ptr && r->fields.len) {
            total = r->fields.len;
            for (u32 k = first; k < total && k < first + count; ++k) {
                field_text(r->fields[k], r->ptr, text, sizeof text);
                put_line(b, lines, r->fields[k].name, text, false);
            }
        }
        format(text, sizeof text, "%s: %u fields%s", pick.name, total, r && !r->fields.len ? " (it has no description)" : "");
    } else {
        const KTable* t = ctx.read_ktable(pick.name);
        const Column* col = t && t->columns.len ? &t->columns[0] : nullptr;
        u32 nf = col && col->desc ? col->desc->fields.len : 0;
        u32 live = t ? t->live() : 0;
        if (col && nf) {
            total = live * (nf + 1);
            // The rows the lines in view belong to: the first of them is the (first / (nf + 1))th alive.
            u32 want_row = first / (nf + 1), ordinal = 0, line = want_row * (nf + 1);
            for (u32 slot = 0; slot < t->rows() && line < first + count; ++slot) {
                if (!t->is_alive(slot)) continue;
                if (ordinal++ < want_row) continue;
                const void* row = col->data + (usize)slot * col->stride;
                if (line >= first) {
                    char head[64];
                    format(head, sizeof head, "row %u of %u", ordinal, live);
                    put_line(b, lines, head, col->desc->name, true);
                }
                ++line;
                for (u32 k = 0; k < nf && line < first + count; ++k, ++line) {
                    if (line < first) continue;
                    field_text(col->desc->fields[k], row, text, sizeof text);
                    put_line(b, lines, col->desc->fields[k].name, text, false);
                }
            }
        }
        format(text, sizeof text, "%s: %u rows of %s, %u fields each", pick.name, live, col && col->desc ? col->desc->name : "?", nf);
    }
    face_set(f, "x_summary", text);
    face_set_doc(f, "x_lines", b, lines);
    b.free();
    face_set(f, "x_first", (f64)first);
    face_set(f, "x_total", (f64)total);
}

bool tables_event(TickContext& ctx, Face& f, const KromeEvent& ev) {
    (void)ctx;
    static const Name name = name_intern("x_name");
    u64 i = 0;
    if (Name{ev.event} != name || !face_id(Name{ev.node}, "xn_", &i)) return false;
    f.x_name = (u32)i;
    f.x_first = 0;
    return true;
}

} // namespace kv
