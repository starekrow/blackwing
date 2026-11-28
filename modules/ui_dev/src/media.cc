// The Media panel: media.status, as blackwing_media publishes it. With no media module in
// the engine (a test's), it says so.
#include "face.h"
#include "korvid/blackwing_media/media.h"
#include "korvid/core/format.h"

namespace kv {

void media_sync(TickContext& ctx, Face& f) {
    if (f.tab != FaceTab::Media) return;
    const MediaStatusRow* m = ctx.resource<const MediaStatusRow>("media.status");
    char text[320];
    if (!m || !m->open) {
        face_set(f, "m_summary", "no surface");
        face_set(f, "m_why", m ? m->why : "no media module in this engine");
        face_set(f, "m_name", "");
        face_set(f, "m_shown", "");
        face_set(f, "m_art", "");
        face_set(f, "m_awake", "");
        face_set(f, "m_sleep", "");
        face_set(f, "m_commands", "");
        return;
    }
    format(text, sizeof text, "%s, %s", m->backend, m->playing ? "playing" : "not playing");
    face_set(f, "m_summary", text);
    face_set(f, "m_name", m->name);
    face_set(f, "m_shown", m->title[0] ? m->title : "nothing");
    face_set(f, "m_art", m->art[0] ? face_file_name(m->art) : "none");
    format(text, sizeof text, "%s", m->awake ? (m->awake_how[0] ? m->awake_how : "held") : "not held");
    face_set(f, "m_awake", text);
    face_set(f, "m_sleep", m->sleep_watched ? "heard: paused before it" : "not heard");
    format(text, sizeof text, "%u commands (last: %s); %u reads; %u interruptions", m->commands, m->last_command[0] ? m->last_command : "none",
           m->clients_asked, m->interruptions);
    face_set(f, "m_commands", text);
    face_set(f, "m_why", m->why);
}

} // namespace kv
