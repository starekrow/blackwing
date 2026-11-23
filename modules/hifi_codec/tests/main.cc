#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_md5(kv::TestCtx&);
void test_flac_good(kv::TestCtx&);
void test_flac_contract(kv::TestCtx&);
void test_flac_broken(kv::TestCtx&);
void test_flac_spoiled(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_md5(c);
    test_flac_good(c);
    test_flac_contract(c);
    test_flac_broken(c);
    test_flac_spoiled(c);
    int rc = kv::test_report(c, "hifi_codec_tests");
    kv::mem_shutdown();
    return rc;
}
