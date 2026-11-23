#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_library_walk(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_library_walk(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "blackwing_library_tests");
}
