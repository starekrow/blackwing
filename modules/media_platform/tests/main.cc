#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_mpris(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_mpris(c);
    int rc = kv::test_report(c, "media_platform_tests");
    kv::mem_shutdown();
    return rc;
}
