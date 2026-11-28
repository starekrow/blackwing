// The device reservation (reserve.cc): a sound card asked of its holder (WirePlumber), and
// given back.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Reserve;

// Reserves card `card` on the session bus (`address`: null for the session's own; tests),
// asking its holder to let go. Null with `why` when it cannot be had.
Reserve* reserve_take(i32 card, const char* address, f64 seconds, char* why, u32 cap);
// Another program has asked for the card since (and been told yes). Any thread; no
// allocation, so the device's realtime thread may ask.
bool     reserve_asked(const Reserve* r);
// Gives it back: the holder before takes it again.
void     reserve_give(Reserve* r);

} // namespace kv
