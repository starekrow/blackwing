// Korvid blackwing_media: the module's API as the engine reads it.
#pragma once
#include "korvid/blackwing_media/media.h"
#include "korvid/core/module.h"

namespace kv {

inline constexpr FieldDesc k_f_MediaRaise[] = {KV_FIELDS(MediaRaise, unused)};
inline constexpr FieldDesc k_f_MediaInterrupted[] = {KV_FIELDS(MediaInterrupted, reason)};
inline constexpr FieldDesc k_f_MediaResumed[] = {KV_FIELDS(MediaResumed, reason)};
inline constexpr EventTypeDesc k_media_events[] = {
    KV_EVENT_EX(MediaRaise, k_f_MediaRaise, 0),
    KV_EVENT_EX(MediaInterrupted, k_f_MediaInterrupted, 0),
    KV_EVENT_EX(MediaResumed, k_f_MediaResumed, 0),
};
inline constexpr FieldDesc k_media_status_fields[] = {KV_FIELDS(MediaStatusRow, open, awake, sleep_watched, playing, commands, interruptions, clients_asked, backend, name, awake_how, why, last_command, title, art)};
inline constexpr ResourceDesc k_media_resources[] = {{.name = ".status", .fields = k_media_status_fields, .cosmetic = true}};

} // namespace kv
