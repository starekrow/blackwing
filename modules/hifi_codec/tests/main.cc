#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_md5(kv::TestCtx&);
void test_flac_good(kv::TestCtx&);
void test_flac_contract(kv::TestCtx&);
void test_flac_broken(kv::TestCtx&);
void test_flac_spoiled(kv::TestCtx&);
void test_sparse_span(kv::TestCtx&);
void test_flac_pieces(kv::TestCtx&);
void test_flac_seek(kv::TestCtx&);
void test_flac_seek_broken(kv::TestCtx&);
void test_mp3_reference(kv::TestCtx&);
void test_mp3_tags(kv::TestCtx&);
void test_mp3_pieces(kv::TestCtx&);
void test_mp3_seek(kv::TestCtx&);
void test_mp3_broken(kv::TestCtx&);
void test_mp3_spoiled(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_md5(c);
    test_flac_good(c);
    test_flac_contract(c);
    test_flac_broken(c);
    test_flac_spoiled(c);
    test_sparse_span(c);
    test_flac_pieces(c);
    test_flac_seek(c);
    test_flac_seek_broken(c);
    test_mp3_reference(c);
    test_mp3_tags(c);
    test_mp3_pieces(c);
    test_mp3_seek(c);
    test_mp3_broken(c);
    test_mp3_spoiled(c);
    int rc = kv::test_report(c, "hifi_codec_tests");
    kv::mem_shutdown();
    return rc;
}
