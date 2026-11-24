// Korvid blackwing_library: the library's API as the engine reads it. The field table of
// every event, row and resource in library.h, and the tables a module record names: what
// the library registers, and what a stand-in for it registers in its place (a face's tests
// run against one, with the library not in the engine).
#pragma once
#include "korvid/blackwing_library/library.h"
#include "korvid/core/module.h"

namespace kv {

inline constexpr FieldDesc k_library_root_fields[] = {KV_FIELDS(LibraryRoot, id, state, dirs, tracks, failed, unread, path, reason)};
inline constexpr FieldDesc k_library_row_fields[] = {KV_FIELDS(LibraryRow, query, index, id, kind, missing, offline, root, track_number, disc_number, size, frames, rate, name, title, artist), // sixteen at most a list
                                            KV_FIELDS(LibraryRow, album, path, reason, count, albums, date)};
inline constexpr ComponentDesc k_library_components[] = {KV_COMPONENT(LibraryRoot, k_library_root_fields, CompCosmetic), KV_COMPONENT(LibraryRow, k_library_row_fields, CompCosmetic)};
inline constexpr FieldDesc k_f_LibraryAddRoot[] = {KV_FIELDS(LibraryAddRoot, path)};
inline constexpr FieldDesc k_f_LibraryRemoveRoot[] = {KV_FIELDS(LibraryRemoveRoot, root)};
inline constexpr FieldDesc k_f_LibraryQuery[] = {KV_FIELDS(LibraryQuery, id, view, sort, parent, text, first, count)};
inline constexpr FieldDesc k_f_LibraryClose[] = {KV_FIELDS(LibraryClose, id)};
inline constexpr FieldDesc k_f_LibraryPrioritize[] = {KV_FIELDS(LibraryPrioritize, path, dir, query, count, tracks)};
inline constexpr FieldDesc k_f_LibraryOverride[] = {KV_FIELDS(LibraryOverride, track, field, value, clear)};
inline constexpr FieldDesc k_f_LibraryPath[] = {KV_FIELDS(LibraryPath, id, index, path)};
inline constexpr FieldDesc k_f_LibraryCollect[] = {KV_FIELDS(LibraryCollect, id, kind, sort, row, text)};
inline constexpr FieldDesc k_f_LibraryCollected[] = {KV_FIELDS(LibraryCollected, id, index, total, path)};
inline constexpr FieldDesc k_f_LibraryResult[] = {KV_FIELDS(LibraryResult, id, total, first, count, generation, seconds)};
inline constexpr FieldDesc k_f_LibraryChanged[] = {KV_FIELDS(LibraryChanged, generation)};
inline constexpr FieldDesc k_f_LibraryScanFinished[] = {KV_FIELDS(LibraryScanFinished, root, tracks, seconds)};
inline constexpr EventTypeDesc k_library_events[] = {
    KV_EVENT_EX(LibraryAddRoot, k_f_LibraryAddRoot, 0),
    KV_EVENT_EX(LibraryRemoveRoot, k_f_LibraryRemoveRoot, 0),
    KV_EVENT_EX(LibraryQuery, k_f_LibraryQuery, 0),
    KV_EVENT_EX(LibraryClose, k_f_LibraryClose, 0),
    KV_EVENT_EX(LibraryPrioritize, k_f_LibraryPrioritize, 0),
    KV_EVENT_EX(LibraryOverride, k_f_LibraryOverride, 0),
    KV_EVENT_EX(LibraryPath, k_f_LibraryPath, 0),
    KV_EVENT_EX(LibraryCollect, k_f_LibraryCollect, 0),
    KV_EVENT_EX(LibraryCollected, k_f_LibraryCollected, 0),
    KV_EVENT_EX(LibraryResult, k_f_LibraryResult, 0),
    KV_EVENT_EX(LibraryChanged, k_f_LibraryChanged, 0),
    KV_EVENT_EX(LibraryScanFinished, k_f_LibraryScanFinished, 0),
};
inline constexpr FieldDesc k_library_scan_fields[] = {KV_FIELDS(LibraryScan, active, rebuilt, walking, generation, tracks, dirs_listed, cancelled, files_found, tags_read, bytes_read, unread, names_per_second, tags_per_second, slot, dir)};
// The resource: described by its fields, never hashed (it follows a disk).
inline constexpr ResourceDesc k_library_resources[] = {{.name = ".scan", .fields = k_library_scan_fields, .cosmetic = true}};

// ---- the surface: what a script host needs that a field table does not say ----------------
// Which of the event types are commands (the rest are notifications); the enums' values
// by name, in order from 0; the tables and the resource by name; the constants.

inline constexpr const char* k_library_surface_version = "0.1.0";
inline constexpr const char* const k_library_commands[] = {
    "LibraryAddRoot", "LibraryRemoveRoot", "LibraryQuery", "LibraryClose", "LibraryPrioritize", "LibraryOverride", "LibraryPath", "LibraryCollect",
};

struct LibrarySurfaceEnum { const char* name; Span<const char* const> values; };
inline constexpr const char* const k_LibraryView_names[] = {"Folders", "Tracks", "Search", "Artists", "Albums", "AlbumTracks", "Genres", "Paths"};
inline constexpr const char* const k_LibrarySort_names[] = {"Default", "Title", "Artist", "Album", "Date", "Path"};
inline constexpr const char* const k_LibraryRootState_names[] = {"Walking", "Reading", "Idle", "Offline"};
inline constexpr const char* const k_LibraryRowKind_names[] = {"Directory", "Track", "Artist", "Album", "Genre"};
static_assert(KV_COUNTOF(k_LibraryView_names) == (usize)LibraryView::Count && KV_COUNTOF(k_LibrarySort_names) == (usize)LibrarySort::Count &&
              KV_COUNTOF(k_LibraryRootState_names) == (usize)LibraryRootState::Count && KV_COUNTOF(k_LibraryRowKind_names) == (usize)LibraryRowKind::Count,
              "an enum's names are its values, in order: one was added to library.h and not here");
inline constexpr LibrarySurfaceEnum k_library_enums[] = {
    {"LibraryView", k_LibraryView_names}, {"LibrarySort", k_LibrarySort_names}, {"LibraryRootState", k_LibraryRootState_names},
    {"LibraryRowKind", k_LibraryRowKind_names},
};

struct LibrarySurfaceTable { const char* name; const char* component; Span<const FieldDesc> fields; bool resource; };
inline constexpr LibrarySurfaceTable k_library_tables[] = {
    {"library.roots", "LibraryRoot", k_library_root_fields, false}, {"library.rows", "LibraryRow", k_library_row_fields, false},
    {"library.scan", "", k_library_scan_fields, true},
};

struct LibrarySurfaceConstant { const char* name; i64 value; };
inline constexpr LibrarySurfaceConstant k_library_constants[] = {
    {"kLibraryMaxWindow", kLibraryMaxWindow}, {"kLibraryMaxRows", kLibraryMaxRows}, {"kLibraryMaxCollect", kLibraryMaxCollect}, {"kLibraryPathMax", kLibraryPathMax},
};

} // namespace kv
