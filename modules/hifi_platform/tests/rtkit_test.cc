// rtkit: a thread of the test's own made realtime through it, and seen to be. Skipped,
// with a line, where there is no rtkit (a container, a machine without a desktop).
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <string.h>
#if defined(__linux__)
#include "../src/linux/rtkit.h"
#include <sched.h>
#endif

using namespace kv;

#if defined(__linux__)
namespace {

struct Asker {
    i64 tid = 0;      // atomic
    u32 asked = 0;    // atomic
    int policy = -1;
};

void sit(void* arg) {
    Asker& a = *(Asker*)arg;
    atomic_store(&a.tid, rtkit_thread_id(), MO_RELEASE);
    while (!atomic_load(&a.asked, MO_ACQUIRE)) time_sleep(0.001);
    a.policy = sched_getscheduler(0);
}

} // namespace
#endif

void test_rtkit(TestCtx& c) {
#if defined(__linux__)
    c.begin("hifi_platform rtkit");
    Asker a;
    OsThread t;
    KV_CHECK(c, os_thread_start(&t, sit, &a));
    i64 tid = 0;
    while (!(tid = atomic_load(&a.tid, MO_ACQUIRE))) time_sleep(0.001);
    char why[160] = "";
    u32 prio = 0;
    f64 t0 = time_now();
    bool ok = rtkit_make_realtime(tid, why, sizeof why, &prio);
    f64 took = time_now() - t0;
    atomic_store(&a.asked, 1u, MO_RELEASE);
    os_thread_join(&t);
    if (!ok && (strstr(why, "not there") || strstr(why, "no system bus"))) { KV_INFO("test", "skipped: %s", why); return; }
    KV_CHECK(c, ok && prio > 0);
    int p = a.policy & ~SCHED_RESET_ON_FORK;
    KV_CHECK(c, (p == SCHED_RR || p == SCHED_FIFO) && (a.policy & SCHED_RESET_ON_FORK)); // rtkit's is round-robin
    KV_INFO("test", "rtkit: %s %u in %.1f ms", p == SCHED_RR ? "SCHED_RR" : p == SCHED_FIFO ? "SCHED_FIFO" : "?", prio, took * 1e3);
    // A thread that is not this process's is refused, in words.
    KV_CHECK(c, !rtkit_make_realtime(1, why, sizeof why) && why[0]);
#else
    (void)c;
#endif
}
