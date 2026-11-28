#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_hifi_exact(kv::TestCtx&);
void test_hifi_join(kv::TestCtx&);
void test_hifi_clock(kv::TestCtx&);
void test_hifi_format_change(kv::TestCtx&);
void test_hifi_pause(kv::TestCtx&);
void test_hifi_flush(kv::TestCtx&);
void test_hifi_underrun(kv::TestCtx&);
void test_hifi_failures(kv::TestCtx&);
void test_hifi_wall_clock(kv::TestCtx&);
void test_hifi_pieces(kv::TestCtx&);
void test_hifi_starved(kv::TestCtx&);
void test_hifi_seek(kv::TestCtx&);
void test_hifi_jump(kv::TestCtx&);
void test_hifi_null_recut(kv::TestCtx&);
void test_hifi_gain_join(kv::TestCtx&);
void test_hifi_drag(kv::TestCtx&);
void test_hifi_replace(kv::TestCtx&);
void test_hifi_marks_across_joins(kv::TestCtx&);
void test_hifi_gapless(kv::TestCtx&);
void test_hifi_lost(kv::TestCtx&);
void test_hifi_resampled(kv::TestCtx&);
void test_hifi_narrower(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_hifi_exact(c);
    test_hifi_join(c);
    test_hifi_clock(c);
    test_hifi_format_change(c);
    test_hifi_pause(c);
    test_hifi_flush(c);
    test_hifi_underrun(c);
    test_hifi_failures(c);
    test_hifi_wall_clock(c);
    test_hifi_pieces(c);
    test_hifi_starved(c);
    test_hifi_seek(c);
    test_hifi_jump(c);
    test_hifi_null_recut(c);
    test_hifi_gain_join(c);
    test_hifi_drag(c);
    test_hifi_replace(c);
    test_hifi_marks_across_joins(c);
    test_hifi_gapless(c);
    test_hifi_lost(c);
    test_hifi_resampled(c);
    test_hifi_narrower(c);
    int rc = kv::test_report(c, "hifi_tests");
    kv::mem_shutdown();
    return rc;
}
