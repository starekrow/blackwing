#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_gain(kv::TestCtx&);
void test_eq(kv::TestCtx&);
void test_eq_recut(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_gain(c);
    test_eq(c);
    test_eq_recut(c);
    int rc = kv::test_report(c, "hifi_eq_tests");
    kv::mem_shutdown();
    return rc;
}
