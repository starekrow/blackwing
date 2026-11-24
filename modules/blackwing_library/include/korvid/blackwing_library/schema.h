// Korvid blackwing_library: the library's API as the engine reads it. The field table of
// every event, row and resource in library.h, and the tables a module record names: what
// the library registers, and what a stand-in for it registers in its place (a face's tests
// run against one, with the library not in the engine).
#pragma once
#include "korvid/blackwing_library/library.h"
#include "korvid/core/module.h"

namespace kv {

inline constinit const FieldDesc k_library_root_fields[] = {KV_FIELDS(LibraryRoot, id, state, dirs, tracks, failed, unread, path, reason)};
inline constinit const FieldDesc k_library_row_fields[] = {KV_FIELDS(LibraryRow, query, index, id, kind, missing, offline, root, track_number, disc_number, size, frames, rate, name, title, artist), // sixteen at most a list
                                            KV_FIELDS(LibraryRow, album, path, reason, count, albums, date)};
inline constinit const ComponentDesc k_library_components[] = {KV_COMPONENT(LibraryRoot, k_library_root_fields, CompCosmetic), KV_COMPONENT(LibraryRow, k_library_row_fields, CompCosmetic)};
inline constinit const FieldDesc k_f_LibraryAddRoot[] = {KV_FIELDS(LibraryAddRoot, path)};
inline constinit const FieldDesc k_f_LibraryRemoveRoot[] = {KV_FIELDS(LibraryRemoveRoot, root)};
inline constinit const FieldDesc k_f_LibraryQuery[] = {KV_FIELDS(LibraryQuery, id, view, sort, parent, text, first, count)};
inline constinit const FieldDesc k_f_LibraryClose[] = {KV_FIELDS(LibraryClose, id)};
inline constinit const FieldDesc k_f_LibraryPrioritize[] = {KV_FIELDS(LibraryPrioritize, path, dir, query, count, tracks)};
inline constinit const FieldDesc k_f_LibraryOverride[] = {KV_FIELDS(LibraryOverride, track, field, value, clear)};
inline constinit const FieldDesc k_f_LibraryResult[] = {KV_FIELDS(LibraryResult, id, total, first, count, generation, seconds)};
inline constinit const FieldDesc k_f_LibraryChanged[] = {KV_FIELDS(LibraryChanged, generation)};
inline constinit const FieldDesc k_f_LibraryScanFinished[] = {KV_FIELDS(LibraryScanFinished, root, tracks, seconds)};
inline constinit const EventTypeDesc k_library_events[] = {
    KV_EVENT_EX(LibraryAddRoot, k_f_LibraryAddRoot, 0),
    KV_EVENT_EX(LibraryRemoveRoot, k_f_LibraryRemoveRoot, 0),
    KV_EVENT_EX(LibraryQuery, k_f_LibraryQuery, 0),
    KV_EVENT_EX(LibraryClose, k_f_LibraryClose, 0),
    KV_EVENT_EX(LibraryPrioritize, k_f_LibraryPrioritize, 0),
    KV_EVENT_EX(LibraryOverride, k_f_LibraryOverride, 0),
    KV_EVENT_EX(LibraryResult, k_f_LibraryResult, 0),
    KV_EVENT_EX(LibraryChanged, k_f_LibraryChanged, 0),
    KV_EVENT_EX(LibraryScanFinished, k_f_LibraryScanFinished, 0),
};
inline constinit const FieldDesc k_library_scan_fields[] = {KV_FIELDS(LibraryScan, active, rebuilt, walking, generation, tracks, dirs_listed, cancelled, files_found, tags_read, bytes_read, unread, names_per_second, tags_per_second, slot, dir)};
// The resource: described by its fields, never hashed (it follows a disk).
inline constinit const ResourceDesc k_library_resources[] = {{.name = ".scan", .fields = k_library_scan_fields, .cosmetic = true}};

} // namespace kv
