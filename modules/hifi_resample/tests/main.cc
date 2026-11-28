#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_resample(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_resample(c);
    int rc = kv::test_report(c, "hifi_resample_tests");
    kv::mem_shutdown();
    return rc;
}
