#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_art(kv::TestCtx&);
void test_media(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_art(c);
    test_media(c);
    int rc = kv::test_report(c, "blackwing_media_tests");
    kv::names_shutdown(); // the process's name table, which this engine's names spilled to the heap
    kv::mem_shutdown();
    return rc;
}
