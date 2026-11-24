#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_face(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_face(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "ui_dev_tests");
}
