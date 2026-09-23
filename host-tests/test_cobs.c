#include "unity.h"

#include "lc_cobs.h"

void setUp(void) {}
void tearDown(void) {}

static void check_vector(const uint8_t *in, size_t in_len, const uint8_t *expected, size_t exp_len)
{
    uint8_t enc[LC_COBS_MAX_ENCODED(300)];
    size_t n = lc_cobs_encode(in, in_len, enc);
    TEST_ASSERT_EQUAL_size_t(exp_len, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, enc, exp_len);

    uint8_t dec[300];
    int d = lc_cobs_decode(enc, n, dec, sizeof(dec));
    TEST_ASSERT_EQUAL_INT((int)in_len, d);
    if (in_len > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(in, dec, in_len);
    }
}

static void test_cobs_single_zero(void)
{
    const uint8_t in[] = { 0x00 };
    const uint8_t out[] = { 0x01, 0x01 };
    check_vector(in, sizeof(in), out, sizeof(out));
}

static void test_cobs_two_zeros(void)
{
    const uint8_t in[] = { 0x00, 0x00 };
    const uint8_t out[] = { 0x01, 0x01, 0x01 };
    check_vector(in, sizeof(in), out, sizeof(out));
}

static void test_cobs_embedded_zero(void)
{
    const uint8_t in[] = { 0x11, 0x22, 0x00, 0x33 };
    const uint8_t out[] = { 0x03, 0x11, 0x22, 0x02, 0x33 };
    check_vector(in, sizeof(in), out, sizeof(out));
}

static void test_cobs_no_zero(void)
{
    const uint8_t in[] = { 0x11, 0x22, 0x33, 0x44 };
    const uint8_t out[] = { 0x05, 0x11, 0x22, 0x33, 0x44 };
    check_vector(in, sizeof(in), out, sizeof(out));
}

static void test_cobs_trailing_zeros(void)
{
    const uint8_t in[] = { 0x11, 0x00, 0x00, 0x00 };
    const uint8_t out[] = { 0x02, 0x11, 0x01, 0x01, 0x01 };
    check_vector(in, sizeof(in), out, sizeof(out));
}

static void test_cobs_254_nonzero_bytes(void)
{
    uint8_t in[254];
    uint8_t out[255];
    out[0] = 0xFF;
    for (int i = 0; i < 254; i++) {
        in[i] = (uint8_t)(i + 1);
        out[i + 1] = (uint8_t)(i + 1);
    }
    check_vector(in, sizeof(in), out, sizeof(out));
}

static void test_cobs_255_nonzero_bytes_splits_block(void)
{
    uint8_t in[255];
    uint8_t out[257];
    out[0] = 0xFF;
    for (int i = 0; i < 254; i++) {
        in[i] = (uint8_t)(i + 1);
        out[i + 1] = (uint8_t)(i + 1);
    }
    in[254] = 0xFF;
    out[255] = 0x02;
    out[256] = 0xFF;
    check_vector(in, sizeof(in), out, sizeof(out));
}

static void test_cobs_decode_rejects_embedded_zero(void)
{
    const uint8_t bad[] = { 0x03, 0x11, 0x00 };
    uint8_t dec[8];
    TEST_ASSERT_EQUAL_INT(-1, lc_cobs_decode(bad, sizeof(bad), dec, sizeof(dec)));
}

static void test_cobs_decode_rejects_code_past_end(void)
{
    const uint8_t bad[] = { 0x05, 0x11, 0x22 };
    uint8_t dec[8];
    TEST_ASSERT_EQUAL_INT(-1, lc_cobs_decode(bad, sizeof(bad), dec, sizeof(dec)));
}

static void test_cobs_decode_respects_out_cap(void)
{
    const uint8_t enc[] = { 0x05, 0x11, 0x22, 0x33, 0x44 };
    uint8_t dec[3];
    TEST_ASSERT_EQUAL_INT(-1, lc_cobs_decode(enc, sizeof(enc), dec, sizeof(dec)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cobs_single_zero);
    RUN_TEST(test_cobs_two_zeros);
    RUN_TEST(test_cobs_embedded_zero);
    RUN_TEST(test_cobs_no_zero);
    RUN_TEST(test_cobs_trailing_zeros);
    RUN_TEST(test_cobs_254_nonzero_bytes);
    RUN_TEST(test_cobs_255_nonzero_bytes_splits_block);
    RUN_TEST(test_cobs_decode_rejects_embedded_zero);
    RUN_TEST(test_cobs_decode_rejects_code_past_end);
    RUN_TEST(test_cobs_decode_respects_out_cap);
    return UNITY_END();
}
