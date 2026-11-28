// rtkit (rtkit.h): two properties read, the time limit set, one call.
#include "rtkit.h"
#include "korvid/dbus/dbus.h"
#include "korvid/core/format.h"
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace kv {

namespace {

const char* const kRtkit = "org.freedesktop.RealtimeKit1";
const char* const kRtkitPath = "/org/freedesktop/RealtimeKit1";

// One of rtkit's properties, as a number; false when it cannot be read.
bool rtkit_property(DbusConnection* c, const char* name, f64* out) {
    DbusHeader h;
    h.destination = kRtkit;
    h.path = kRtkitPath;
    h.interface = "org.freedesktop.DBus.Properties";
    h.member = "Get";
    DbusWriter w;
    w.begin("ss");
    w.str(kRtkit);
    w.str(name);
    DbusMessage r;
    bool ok = dbus_call(c, h, &w, 1.0, &r) && r.kind == DbusKind::Return;
    if (ok) {
        DbusReader rd = r.reader();
        ok = rd.variant_begin() && rd.number(out);
    }
    dbus_message_free(&r);
    w.free();
    return ok;
}

} // namespace

i64 rtkit_thread_id() { return (i64)syscall(SYS_gettid); }

bool rtkit_make_realtime(i64 tid, char* why, u32 cap, u32* priority) {
    char w[200] = "";
    DbusConnection* c = dbus_open(DbusBus::System, 1.0, w, sizeof w);
    if (!c) { format(why, cap, "no system bus for rtkit (%.80s)", w); return false; }
    f64 max_prio = 0, rttime = 0;
    if (!rtkit_property(c, "MaxRealtimePriority", &max_prio) || !rtkit_property(c, "RTTimeUSecMax", &rttime)) {
        dbus_close(c);
        format(why, cap, "rtkit is not there");
        return false;
    }
    // rtkit grants nothing to a process whose realtime time is not bounded within its own.
    struct rlimit rl;
    if (getrlimit(RLIMIT_RTTIME, &rl) == 0 && (rl.rlim_max == RLIM_INFINITY || rl.rlim_max > (rlim_t)rttime || rl.rlim_cur > (rlim_t)rttime)) {
        rl.rlim_cur = rl.rlim_max = (rlim_t)rttime;
        setrlimit(RLIMIT_RTTIME, &rl);
    }
    u32 prio = max_prio > 0 ? (u32)max_prio : 1;
    DbusHeader h;
    h.destination = kRtkit;
    h.path = kRtkitPath;
    h.interface = kRtkit;
    h.member = "MakeThreadRealtimeWithPID";
    DbusWriter a;
    a.begin("ttu");
    a.u64((u64)getpid());
    a.u64((u64)tid);
    a.u32(prio);
    DbusMessage r;
    char cw[96] = "";
    bool ok = dbus_call(c, h, &a, 1.0, &r, cw, sizeof cw);
    if (ok && r.kind == DbusKind::Error) {
        format(why, cap, "rtkit refused: %s", r.error_text()[0] ? r.error_text() : r.error_name);
        ok = false;
    } else if (!ok) {
        format(why, cap, "rtkit did not answer: %s", cw);
    }
    dbus_message_free(&r);
    a.free();
    dbus_close(c);
    if (ok && priority) *priority = prio;
    return ok;
}

} // namespace kv
