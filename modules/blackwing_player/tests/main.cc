#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"

void test_player(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_player(c);
    kv::names_shutdown();
    kv::mem_shutdown();
    return kv::test_report(c, "blackwing_player_tests");
}
