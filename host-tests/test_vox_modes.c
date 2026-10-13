#include "unity.h"

#include <string.h>

#include "oc_vox.h"

void setUp(void) {}
void tearDown(void) {}

static const uint16_t BPS[9] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2400, 3200 };
static const uint8_t BYTES[9] = { 9, 12, 15, 18, 21, 24, 27, 36, 48 };

static void test_nine_modes_with_their_bitrates_and_block_sizes(void)
{
    TEST_ASSERT_EQUAL_INT(9, OC_VOX_MODE_COUNT);
    for (int m = 0; m < 9; m++) {
        TEST_ASSERT_EQUAL_UINT16(BPS[m], oc_vox_mode_bps((oc_vox_mode_t)m));
        TEST_ASSERT_EQUAL_UINT8(BYTES[m], oc_vox_block_bytes((oc_vox_mode_t)m));
        /* bit/s x 120 ms, whole bytes */
        TEST_ASSERT_EQUAL_UINT16(BPS[m] * 120u / 1000u, oc_vox_block_bits((oc_vox_mode_t)m));
        TEST_ASSERT_EQUAL_UINT16(BYTES[m] * 8u, oc_vox_block_bits((oc_vox_mode_t)m));
        TEST_ASSERT_EQUAL_INT(m, oc_vox_mode_of_block_bytes(BYTES[m]));
    }
    TEST_ASSERT_EQUAL_UINT16(0, oc_vox_mode_bps((oc_vox_mode_t)9));
    TEST_ASSERT_EQUAL_UINT8(0, oc_vox_block_bytes((oc_vox_mode_t)-1));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_mode_of_block_bytes(0));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_mode_of_block_bytes(10));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_mode_of_block_bytes(49));
    TEST_ASSERT_EQUAL_STRING("OVX-1200", oc_vox_mode_name(OC_VOX_1200));
    TEST_ASSERT_EQUAL_STRING("?", oc_vox_mode_name((oc_vox_mode_t)12));
}

static void test_codec_ids_3_to_6_for_the_modes_that_fit_an_app_data_frame(void)
{
    TEST_ASSERT_EQUAL_UINT8(3, oc_vox_codec_id(OC_VOX_600));
    TEST_ASSERT_EQUAL_UINT8(4, oc_vox_codec_id(OC_VOX_800));
    TEST_ASSERT_EQUAL_UINT8(5, oc_vox_codec_id(OC_VOX_1000));
    TEST_ASSERT_EQUAL_UINT8(6, oc_vox_codec_id(OC_VOX_1200));
    for (int m = OC_VOX_1400; m < 9; m++) TEST_ASSERT_EQUAL_UINT8(0, oc_vox_codec_id((oc_vox_mode_t)m));
    TEST_ASSERT_EQUAL_INT(OC_VOX_600, oc_vox_mode_of_codec_id(3));
    TEST_ASSERT_EQUAL_INT(OC_VOX_1200, oc_vox_mode_of_codec_id(6));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_mode_of_codec_id(1)); /* Codec2 1200 */
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_mode_of_codec_id(2));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_mode_of_codec_id(0));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_mode_of_codec_id(7));
    /* every mode's block fits where its id says it fits */
    for (int m = 0; m < 9; m++)
        TEST_ASSERT_EQUAL_INT(oc_vox_block_bytes((oc_vox_mode_t)m) <= 18, oc_vox_codec_id((oc_vox_mode_t)m) != 0);
}

static void test_caps_bytes_round_trip_through_codec_caps(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x3C, oc_vox_caps_to_sig(OC_VOX_CAPS_ALL)); /* bits 2-5: ids 3-6 */
    TEST_ASSERT_EQUAL_HEX8(0x20, oc_vox_caps_to_sig(1u << OC_VOX_1200));
    TEST_ASSERT_EQUAL_HEX8(0x00, oc_vox_caps_to_sig(1u << OC_VOX_3200));
    TEST_ASSERT_EQUAL_HEX16(0x000F, oc_vox_caps_of_sig(0xFF)); /* the four with ids, Codec2's bit ignored */
    TEST_ASSERT_EQUAL_HEX16(0x0000, oc_vox_caps_of_sig(0x03));
    TEST_ASSERT_EQUAL_HEX16(1u << OC_VOX_800, oc_vox_caps_of_sig(0x08));
}

static void test_negotiate_picks_the_highest_mode_that_fits_or_the_lowest(void)
{
    TEST_ASSERT_EQUAL_INT(OC_VOX_1200, oc_vox_negotiate(OC_VOX_CAPS_ALL, OC_VOX_CAPS_ALL, 18, 0));
    TEST_ASSERT_EQUAL_INT(OC_VOX_3200, oc_vox_negotiate(OC_VOX_CAPS_ALL, OC_VOX_CAPS_ALL, 48, 0));
    TEST_ASSERT_EQUAL_INT(OC_VOX_2400, oc_vox_negotiate(OC_VOX_CAPS_ALL, OC_VOX_CAPS_ALL, 47, 0));
    TEST_ASSERT_EQUAL_INT(OC_VOX_600, oc_vox_negotiate(OC_VOX_CAPS_ALL, OC_VOX_CAPS_ALL, 18, 1));
    TEST_ASSERT_EQUAL_INT(OC_VOX_800, oc_vox_negotiate((1u << OC_VOX_800) | (1u << OC_VOX_3200),
                                                       OC_VOX_CAPS_ALL, 18, 0));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_negotiate(1u << OC_VOX_600, 1u << OC_VOX_800, 18, 0));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_negotiate(OC_VOX_CAPS_ALL, OC_VOX_CAPS_ALL, 8, 0));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_negotiate(0, OC_VOX_CAPS_ALL, 48, 0));
}

static void test_init_refuses_a_bad_mode_and_a_block_has_the_modes_size(void)
{
    oc_vox_enc_t e;
    oc_vox_dec_t d;
    TEST_ASSERT_EQUAL_INT(-1, oc_vox_enc_init(&e, (oc_vox_mode_t)9));
    TEST_ASSERT_EQUAL_INT(-1, oc_vox_dec_init(&d, (oc_vox_mode_t)-1));
    TEST_ASSERT_EQUAL_INT(0, oc_vox_enc_init(&e, OC_VOX_1200));
    TEST_ASSERT_EQUAL_INT(0, oc_vox_dec_init(&d, OC_VOX_1200));
    TEST_ASSERT_EQUAL_UINT32(0, oc_vox_dec_stats(&d)->blocks);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nine_modes_with_their_bitrates_and_block_sizes);
    RUN_TEST(test_codec_ids_3_to_6_for_the_modes_that_fit_an_app_data_frame);
    RUN_TEST(test_caps_bytes_round_trip_through_codec_caps);
    RUN_TEST(test_negotiate_picks_the_highest_mode_that_fits_or_the_lowest);
    RUN_TEST(test_init_refuses_a_bad_mode_and_a_block_has_the_modes_size);
    return UNITY_END();
}
