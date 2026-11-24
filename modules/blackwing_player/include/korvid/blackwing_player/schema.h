// Korvid blackwing_player: the player's API as the engine reads it. The field table of every
// event, row and resource in player.h, and the tables a module record names: what the
// player registers, and what a stand-in for it registers in its place (a face's tests run
// against one, with the player not in the engine). A module that publishes these under the
// name "player" is, to anything that reads tables and emits commands, the player.
//
//   KV_MODULE_DEFINE(standin) = {.name = "player", .events = k_player_events,
//                                .components = k_player_components, .ktables = ..., .resources = k_player_resources};
#pragma once
#include "korvid/blackwing_player/player.h"
#include "korvid/core/module.h"

namespace kv {

inline constexpr FieldDesc k_player_item_fields[] = {KV_FIELDS(PlayerItem, id, pos, state, damaged, channels, bits, rate, frames, size, bytes_read, path, reason)};
inline constexpr FieldDesc k_player_stage_fields[] = {KV_FIELDS(PlayerStage, index, flat, params_len, name, param_names, params)};
inline constexpr FieldDesc k_player_param_fields[] = {KV_FIELDS(PlayerParam, stage, index, value, lo, hi, initial, step, name, unit)};
inline constexpr FieldDesc k_player_preset_fields[] = {KV_FIELDS(PlayerPreset, stage, index, name)};
inline constexpr ComponentDesc k_player_components[] = {
    KV_COMPONENT(PlayerItem, k_player_item_fields, CompCosmetic), KV_COMPONENT(PlayerStage, k_player_stage_fields, CompCosmetic),
    KV_COMPONENT(PlayerParam, k_player_param_fields, CompCosmetic), KV_COMPONENT(PlayerPreset, k_player_preset_fields, CompCosmetic),
};
// Each event describes its fields, so --trace-events (and describe) print them.
inline constexpr FieldDesc k_f_PlayerEnqueue[] = {KV_FIELDS(PlayerEnqueue, path, before)};
inline constexpr FieldDesc k_f_PlayerPlay[] = {KV_FIELDS(PlayerPlay, unused)};
inline constexpr FieldDesc k_f_PlayerPause[] = {KV_FIELDS(PlayerPause, unused)};
inline constexpr FieldDesc k_f_PlayerStop[] = {KV_FIELDS(PlayerStop, unused)};
inline constexpr FieldDesc k_f_PlayerNext[] = {KV_FIELDS(PlayerNext, unused)};
inline constexpr FieldDesc k_f_PlayerPrevious[] = {KV_FIELDS(PlayerPrevious, unused)};
inline constexpr FieldDesc k_f_PlayerJump[] = {KV_FIELDS(PlayerJump, item)};
inline constexpr FieldDesc k_f_PlayerSeek[] = {KV_FIELDS(PlayerSeek, item, frame)};
inline constexpr FieldDesc k_f_PlayerSeekBy[] = {KV_FIELDS(PlayerSeekBy, seconds)};
inline constexpr FieldDesc k_f_PlayerSeeked[] = {KV_FIELDS(PlayerSeeked, item, frame, seq)};
inline constexpr FieldDesc k_f_PlayerSetParam[] = {KV_FIELDS(PlayerSetParam, stage, param, value)};
inline constexpr FieldDesc k_f_PlayerApplyPreset[] = {KV_FIELDS(PlayerApplyPreset, stage, preset)};
inline constexpr FieldDesc k_f_PlayerParamChanged[] = {KV_FIELDS(PlayerParamChanged, stage, param, value, seq)};
inline constexpr FieldDesc k_f_PlayerQueueEdit[] = {KV_FIELDS(PlayerQueueEdit, op, item, before)};
inline constexpr FieldDesc k_f_PlayerTrackStarted[] = {KV_FIELDS(PlayerTrackStarted, item, rate, channels, bits, frames, seq)};
inline constexpr FieldDesc k_f_PlayerTrackEnded[] = {KV_FIELDS(PlayerTrackEnded, item, frames, seq, damaged, reason)};
inline constexpr FieldDesc k_f_PlayerTrackFailed[] = {KV_FIELDS(PlayerTrackFailed, item, seq, reason)};
inline constexpr FieldDesc k_f_PlayerQueueEnded[] = {KV_FIELDS(PlayerQueueEnded, seq)};
inline constexpr FieldDesc k_f_HifiUnderrun[] = {KV_FIELDS(HifiUnderrun, count, seq)};
inline constexpr EventTypeDesc k_player_events[] = {
    KV_EVENT_EX(PlayerEnqueue, k_f_PlayerEnqueue, 0),
    KV_EVENT_EX(PlayerPlay, k_f_PlayerPlay, 0),
    KV_EVENT_EX(PlayerPause, k_f_PlayerPause, 0),
    KV_EVENT_EX(PlayerStop, k_f_PlayerStop, 0),
    KV_EVENT_EX(PlayerNext, k_f_PlayerNext, 0),
    KV_EVENT_EX(PlayerPrevious, k_f_PlayerPrevious, 0),
    KV_EVENT_EX(PlayerJump, k_f_PlayerJump, 0),
    KV_EVENT_EX(PlayerSeek, k_f_PlayerSeek, 0),
    KV_EVENT_EX(PlayerSeekBy, k_f_PlayerSeekBy, 0),
    KV_EVENT_EX(PlayerSeeked, k_f_PlayerSeeked, 0),
    KV_EVENT_EX(PlayerSetParam, k_f_PlayerSetParam, 0),
    KV_EVENT_EX(PlayerApplyPreset, k_f_PlayerApplyPreset, 0),
    KV_EVENT_EX(PlayerParamChanged, k_f_PlayerParamChanged, 0),
    KV_EVENT_EX(PlayerQueueEdit, k_f_PlayerQueueEdit, 0),
    KV_EVENT_EX(PlayerTrackStarted, k_f_PlayerTrackStarted, 0),
    KV_EVENT_EX(PlayerTrackEnded, k_f_PlayerTrackEnded, 0),
    KV_EVENT_EX(PlayerTrackFailed, k_f_PlayerTrackFailed, 0),
    KV_EVENT_EX(PlayerQueueEnded, k_f_PlayerQueueEnded, 0),
    KV_EVENT_EX(HifiUnderrun, k_f_HifiUnderrun, 0),
};
inline constexpr FieldDesc k_player_transport_fields[] = {KV_FIELDS(PlayerTransport, state, item, position, frames, rate, queued, left, place, place_pos, seek_item, seek_frame)};
inline constexpr FieldDesc k_player_output_fields[] = {KV_FIELDS(PlayerOutput, open, shared, exact, sample, channels, bits, float_word, rate, lead_frames, underruns, joining, bytes_held, recut_seconds, recuts, recut_render_seconds, jump_seconds)};
// The two resources: described by their fields, never hashed (they follow a device's clock).
inline constexpr ResourceDesc k_player_resources[] = {
    {.name = ".transport", .fields = k_player_transport_fields, .cosmetic = true},
    {.name = ".output", .fields = k_player_output_fields, .cosmetic = true},
};

// ---- the surface: what a script host needs that a field table does not say ----------------
// Which of the event types are commands (the rest are notifications); the enums' values
// by name, in order from 0; the tables and resources by name, with what describes them;
// the constants. A host builds its binding from these and the tables above.

inline constexpr const char* k_player_surface_version = "0.1.0";
inline constexpr const char* const k_player_commands[] = {
    "PlayerEnqueue", "PlayerQueueEdit", "PlayerPlay", "PlayerPause", "PlayerStop", "PlayerNext", "PlayerPrevious", "PlayerJump",
    "PlayerSeek", "PlayerSeekBy", "PlayerSetParam", "PlayerApplyPreset",
};

struct PlayerSurfaceEnum { const char* name; Span<const char* const> values; };
inline constexpr const char* const k_PlayerEditOp_names[] = {"Remove", "Move", "Clear"};
inline constexpr const char* const k_PlayerItemState_names[] = {"Idle", "Reading", "Ready", "Playing", "Failed"};
inline constexpr const char* const k_PlayerState_names[] = {"Stopped", "Starting", "Playing", "Paused"};
inline constexpr const char* const k_HifiSampleFormat_names[] = {"S16", "S24", "S32", "F32"}; // hifi_platform's; player.output's `sample` is one
static_assert(KV_COUNTOF(k_PlayerItemState_names) == (usize)PlayerItemState::Count && KV_COUNTOF(k_PlayerState_names) == (usize)PlayerState::Count,
              "an enum's names are its values, in order: one was added to player.h and not here");
inline constexpr PlayerSurfaceEnum k_player_enums[] = {
    {"PlayerEditOp", k_PlayerEditOp_names}, {"PlayerItemState", k_PlayerItemState_names}, {"PlayerState", k_PlayerState_names},
    {"HifiSampleFormat", k_HifiSampleFormat_names},
};

// A table (its one component's name and fields) or a resource (its fields), by full name.
struct PlayerSurfaceTable { const char* name; const char* component; Span<const FieldDesc> fields; bool resource; };
inline constexpr PlayerSurfaceTable k_player_tables[] = {
    {"player.queue", "PlayerItem", k_player_item_fields, false},   {"player.chain", "PlayerStage", k_player_stage_fields, false},
    {"player.params", "PlayerParam", k_player_param_fields, false}, {"player.presets", "PlayerPreset", k_player_preset_fields, false},
    {"player.transport", "", k_player_transport_fields, true},      {"player.output", "", k_player_output_fields, true},
};

struct PlayerSurfaceConstant { const char* name; i64 value; };
inline constexpr PlayerSurfaceConstant k_player_constants[] = {{"kPlayerPathMax", kPlayerPathMax}};

} // namespace kv
