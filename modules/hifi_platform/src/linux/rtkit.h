// Realtime scheduling for a thread through rtkit (org.freedesktop.RealtimeKit1 on the
// system bus): what a desktop session grants a program whose RLIMIT_RTPRIO is 0, as
// PipeWire's own threads get it. The engine's core does not speak D-Bus; this does,
// through dbus.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The calling thread's kernel id, for rtkit_make_realtime.
i64  rtkit_thread_id();
// Asks rtkit to put thread `tid` of this process in a realtime class (rtkit's is SCHED_RR,
// with SCHED_RESET_ON_FORK) at its highest priority allowed (rtkit's MaxRealtimePriority, 20 by default). Sets the
// process's RLIMIT_RTTIME to rtkit's ceiling first, as rtkit requires: a realtime thread
// that runs that long without sleeping is stopped. Blocking (a second at most); not for a
// realtime thread. False, `why` in words, when it is refused or there is no rtkit.
bool rtkit_make_realtime(i64 tid, char* why, u32 why_cap, u32* priority = nullptr);

} // namespace kv
