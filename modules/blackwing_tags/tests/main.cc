#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_tags_id3v2(kv::TestCtx&);
void test_tags_vorbis(kv::TestCtx&);
void test_tags_ape_id3v1(kv::TestCtx&);
void test_tags_rank(kv::TestCtx&);
void test_tags_path(kv::TestCtx&);
void test_tags_pieces(kv::TestCtx&);
void test_tags_hostile(kv::TestCtx&);
void test_tags_spoiled(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_tags_id3v2(c);
    test_tags_vorbis(c);
    test_tags_ape_id3v1(c);
    test_tags_rank(c);
    test_tags_path(c);
    test_tags_pieces(c);
    test_tags_hostile(c);
    test_tags_spoiled(c);
    int rc = kv::test_report(c, "blackwing_tags_tests");
    kv::mem_shutdown();
    return rc;
}
