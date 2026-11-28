#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_names(kv::TestCtx&);
void test_wire(kv::TestCtx&);
void test_messages(kv::TestCtx&);
void test_fuzz(kv::TestCtx&);
void test_fake_bus(kv::TestCtx&);
void test_private_bus(kv::TestCtx&);
void test_session_bus(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_names(c);
    test_wire(c);
    test_messages(c);
    test_fuzz(c);
    test_fake_bus(c);
    test_private_bus(c);
    test_session_bus(c);
    int rc = kv::test_report(c, "dbus_tests");
    kv::mem_shutdown();
    return rc;
}
