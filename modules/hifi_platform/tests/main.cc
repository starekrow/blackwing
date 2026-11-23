#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_device_names(kv::TestCtx&);
void test_pipewire(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_device_names(c);
    test_pipewire(c);
    int rc = kv::test_report(c, "hifi_platform_tests");
    kv::mem_shutdown();
    return rc;
}
