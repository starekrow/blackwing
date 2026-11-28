// What the wire format's parts share inside the module.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The alignment a value of this type code starts at; 0 for a code that is not a type.
u32  dbus_align_of(char code);
// A basic type: one that can be a dict entry's key.
bool dbus_basic(char code);
// The length of the one complete type at s[0, n), within the nesting limits; 0 if none.
u32  dbus_complete_type(const char* s, u32 n);
// The same, where a dict entry ("{sv}") also counts as one: a container about to be opened.
u32  dbus_container_len(const char* s, u32 n);

// The UNIX_FDS header field of a whole message's bytes (0 when it has none): how many of
// the descriptors that came with the stream are its. False if the header cannot be read.
bool dbus_header_unix_fds(const u8* bytes, u32 len, u32* n);

} // namespace kv
