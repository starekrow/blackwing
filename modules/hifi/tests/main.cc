#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_hifi_exact(kv::TestCtx&);
void test_hifi_join(kv::TestCtx&);
void test_hifi_clock(kv::TestCtx&);
void test_hifi_format_change(kv::TestCtx&);
void test_hifi_pause(kv::TestCtx&);
void test_hifi_flush(kv::TestCtx&);
void test_hifi_underrun(kv::TestCtx&);
void test_hifi_failures(kv::TestCtx&);
void test_hifi_wall_clock(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_hifi_exact(c);
    test_hifi_join(c);
    test_hifi_clock(c);
    test_hifi_format_change(c);
    test_hifi_pause(c);
    test_hifi_flush(c);
    test_hifi_underrun(c);
    test_hifi_failures(c);
    test_hifi_wall_clock(c);
    int rc = kv::test_report(c, "hifi_tests");
    kv::mem_shutdown();
    return rc;
}
