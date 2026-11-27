// The Scan panel: library.scan, as it stands. Its numbers move all the time while a scan
// runs, and each is written when the text that shows it changes.
#include "face.h"
#include "korvid/core/format.h"

namespace kv {

void scan_sync(TickContext& ctx, Face& f) {
    const LibraryScan* s = ctx.resource<const LibraryScan>("library.scan");
    if (!s) return;
    char text[400];
    face_set(f, "s_state", s->active ? "scanning" : "idle");
    if (s->walking) format(text, sizeof text, "root %u: %s", s->walking, s->dir[0] ? s->dir : "(its top)");
    else            format(text, sizeof text, "nothing");
    face_set(f, "s_walking", text);
    format(text, sizeof text, "%llu files, %.0f a second; %llu directories listed", (unsigned long long)s->files_found, s->names_per_second, (unsigned long long)s->dirs_listed);
    face_set(f, "s_found", text);
    format(text, sizeof text, "%llu tracks, %.0f a second; %.1f MB read", (unsigned long long)s->tags_read, s->tags_per_second, (f64)s->bytes_read / 1048576.0);
    face_set(f, "s_read", text);
    format(text, sizeof text, "%llu tracks", (unsigned long long)s->unread);
    face_set(f, "s_unread", text);
    face_set(f, "s_slot", s->slot ? "something looked at is being scanned first" : "empty");
    format(text, sizeof text, "%llu tracks, generation %llu%s", (unsigned long long)s->tracks, (unsigned long long)s->generation,
           s->rebuilt ? "; made again at start-up (it was another version, or damaged)" : "");
    face_set(f, "s_index", text);
    format(text, sizeof text, "%llu stopped by a newer one", (unsigned long long)s->cancelled);
    face_set(f, "s_queries", text);
    if (s->set_aside) format(text, sizeof text, "%u override file%s would not parse and %s set aside: the Events panel has where", s->set_aside, s->set_aside == 1 ? "" : "s", s->set_aside == 1 ? "was" : "were");
    else              format(text, sizeof text, "none set aside");
    face_set(f, "s_overrides", text);
}

} // namespace kv
