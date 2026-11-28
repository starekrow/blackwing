#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_device_names(kv::TestCtx&);
void test_pipewire(kv::TestCtx&);
void test_capture_unplug(kv::TestCtx&);
void test_pipewire_devices(kv::TestCtx&);
void test_alone(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_device_names(c);
    test_pipewire(c);
    test_capture_unplug(c);
    test_pipewire_devices(c);
    test_alone(c);
    int rc = kv::test_report(c, "hifi_platform_tests");
    kv::mem_shutdown();
    return rc;
}
