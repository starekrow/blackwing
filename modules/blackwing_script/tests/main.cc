#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_script_host(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_script_host(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "blackwing_script_tests");
}
