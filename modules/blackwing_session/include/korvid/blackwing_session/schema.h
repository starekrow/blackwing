// Korvid blackwing_session: the session's API as the engine reads it: the field tables of
// its two events and its resource.
#pragma once
#include "korvid/blackwing_session/session.h"
#include "korvid/core/module.h"

namespace kv {

inline constexpr FieldDesc k_f_SessionKeep[] = {KV_FIELDS(SessionKeep, owner, key, number, text)};
inline constexpr FieldDesc k_f_SessionKept[] = {KV_FIELDS(SessionKept, owner, key, number, text)};
inline constexpr EventTypeDesc k_session_events[] = {
    KV_EVENT_EX(SessionKeep, k_f_SessionKeep, 0),
    KV_EVENT_EX(SessionKept, k_f_SessionKept, 0),
};
inline constexpr FieldDesc k_session_status_fields[] = {KV_FIELDS(SessionStatus, phase, set_aside, dirty, writing, queue_total, queue_restored, saves, save_bytes, save_seconds, reason)};
inline constexpr ResourceDesc k_session_resources[] = {{.name = ".status", .fields = k_session_status_fields, .cosmetic = true}};

} // namespace kv
