#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_library_walk(kv::TestCtx&);
void test_library_slot(kv::TestCtx&);
void test_library_cancel(kv::TestCtx&);
void test_library_overrides(kv::TestCtx&);
void test_library_changes(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_library_walk(c);
    test_library_slot(c);
    test_library_cancel(c);
    test_library_overrides(c);
    test_library_changes(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "blackwing_library_tests");
}
