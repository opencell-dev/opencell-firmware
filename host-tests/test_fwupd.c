#include "unity.h"

#include <string.h>

#include "oc_fwupd.h"

static struct {
    int      begins, aborts, finishes;
    uint32_t bytes;
    uint32_t last_offset;
    int      fail_write, fail_finish;
} f;

static int f_begin(void *ctx) { (void)ctx; f.begins++; return 0; }
static int f_write(void *ctx, uint32_t off, const uint8_t *d, uint16_t len)
{
    (void)ctx;
    (void)d;
    f.last_offset = off;
    f.bytes += len;
    return f.fail_write ? -1 : 0;
}
static int f_finish(void *ctx, uint32_t size) { (void)ctx; (void)size; f.finishes++; return f.fail_finish ? -1 : 0; }
static void f_abort(void *ctx) { (void)ctx; f.aborts++; }

static oc_fwupd_t upd;
static uint8_t data[OC_MAX_FW_CHUNK];

void setUp(void)
{
    memset(&f, 0, sizeof(f));
    const oc_fwupd_ops_t ops = { NULL, f_begin, f_write, f_finish, f_abort };
    oc_fwupd_init(&upd, &ops);
}
void tearDown(void) {}

static uint8_t chunk(uint32_t off, uint16_t len)
{
    oc_fw_chunk_t c = { off, len, data };
    return oc_fwupd_chunk(&upd, &c);
}

static uint8_t commit(uint32_t size)
{
    oc_fw_commit_t c = { size };
    return oc_fwupd_commit(&upd, &c);
}

static void test_in_order_update_commits(void)
{
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, chunk(0, 1024));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, chunk(1024, 1024));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, chunk(2048, 100));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, commit(2148));
    TEST_ASSERT_EQUAL_INT(1, f.begins);
    TEST_ASSERT_EQUAL_INT(1, f.finishes);
    TEST_ASSERT_EQUAL_UINT32(2148, f.bytes);
}

static void test_retried_chunk_is_acked_without_rewrite(void)
{
    chunk(0, 1024);
    chunk(1024, 1024);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, chunk(1024, 1024)); /* ACK was lost, host resent */
    TEST_ASSERT_EQUAL_UINT32(2048, f.bytes);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, chunk(2048, 10));
}

static void test_gap_and_straddle_rejected(void)
{
    chunk(0, 1024);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, chunk(4096, 10)); /* gap */
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, chunk(1000, 100)); /* straddles written end */
}

static void test_chunk_without_start_rejected(void)
{
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, chunk(1024, 10));
    TEST_ASSERT_EQUAL_INT(0, f.begins);
}

static void test_restart_at_offset_zero_aborts_previous(void)
{
    chunk(0, 1024);
    chunk(1024, 1024);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, chunk(0, 1024));
    TEST_ASSERT_EQUAL_INT(1, f.aborts);
    TEST_ASSERT_EQUAL_INT(2, f.begins);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, commit(1024));
}

static void test_commit_size_mismatch_aborts(void)
{
    chunk(0, 1024);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, commit(2048));
    TEST_ASSERT_EQUAL_INT(1, f.aborts);
    TEST_ASSERT_EQUAL_INT(0, f.finishes);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, commit(1024)); /* nothing open now */
}

static void test_flash_errors_reported(void)
{
    f.fail_write = 1;
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_FLASH, chunk(0, 1024));
    TEST_ASSERT_EQUAL_INT(1, f.aborts);
    f.fail_write = 0;
    chunk(0, 1024);
    f.fail_finish = 1;
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_FLASH, commit(1024));
}

static void test_empty_chunk_rejected(void)
{
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, chunk(0, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_in_order_update_commits);
    RUN_TEST(test_retried_chunk_is_acked_without_rewrite);
    RUN_TEST(test_gap_and_straddle_rejected);
    RUN_TEST(test_chunk_without_start_rejected);
    RUN_TEST(test_restart_at_offset_zero_aborts_previous);
    RUN_TEST(test_commit_size_mismatch_aborts);
    RUN_TEST(test_flash_errors_reported);
    RUN_TEST(test_empty_chunk_rejected);
    return UNITY_END();
}
