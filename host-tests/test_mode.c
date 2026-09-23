#include "unity.h"

#include "lc_phy.h"

void setUp(void) {}
void tearDown(void) {}

static void test_lora_airtime_matches_semtech_calculator(void)
{
    /* SF12 / BW125 / CR4/5 / preamble 8 / 10 bytes: Semtech calculator 991.23 ms */
    const lc_mode_t sf12 = { LC_MOD_LORA, 12, 1, 8, 125000, 0 };
    TEST_ASSERT_EQUAL_UINT32(991232u, lc_airtime_us(&sf12, 10));

    const lc_mode_t sf7_250 = { LC_MOD_LORA, 7, 1, 8, 250000, 0 };
    TEST_ASSERT_EQUAL_UINT32(33408u, lc_airtime_us(&sf7_250, 28));
}

static void test_tier_airtimes_915(void)
{
    TEST_ASSERT_EQUAL_UINT32(1500u, lc_airtime_us(lc_tier_mode(LC_BAND_915, LC_TIER_NEAR), 28));
    TEST_ASSERT_EQUAL_UINT32(5264u, lc_airtime_us(lc_tier_mode(LC_BAND_915, LC_TIER_MID), 28));
    TEST_ASSERT_EQUAL_UINT32(16704u, lc_airtime_us(lc_tier_mode(LC_BAND_915, LC_TIER_EDGE), 28));
}

static void test_tier_airtimes_2g4(void)
{
    TEST_ASSERT_EQUAL_UINT32(300u, lc_airtime_us(lc_tier_mode(LC_BAND_2G4, LC_TIER_NEAR), 28));
    TEST_ASSERT_EQUAL_UINT32(750u, lc_airtime_us(lc_tier_mode(LC_BAND_2G4, LC_TIER_MID), 28));
    TEST_ASSERT_NULL(lc_tier_mode(LC_BAND_2G4, LC_TIER_EDGE));
}

static void test_slot_len_adds_guard(void)
{
    const lc_mode_t *edge = lc_tier_mode(LC_BAND_915, LC_TIER_EDGE);
    TEST_ASSERT_EQUAL_UINT32(16704u + LC_GUARD_US, lc_slot_len_us(edge, 28));
}

static void test_airtime_grows_with_payload(void)
{
    for (int b = 0; b < LC_BAND_COUNT; b++) {
        for (int t = 0; t < LC_TIER_COUNT; t++) {
            const lc_mode_t *m = lc_tier_mode((lc_band_t)b, (lc_tier_t)t);
            if (m == NULL) {
                continue;
            }
            TEST_ASSERT_TRUE(lc_airtime_us(m, 0) > 0);
            TEST_ASSERT_TRUE(lc_airtime_us(m, 255) > lc_airtime_us(m, 28));
        }
    }
}

static void test_invalid_modes_return_zero(void)
{
    const lc_mode_t bad_sf = { LC_MOD_LORA, 4, 1, 8, 500000, 0 };
    const lc_mode_t bad_cr = { LC_MOD_LORA, 7, 5, 8, 500000, 0 };
    const lc_mode_t no_bw = { LC_MOD_LORA, 7, 1, 8, 0, 0 };
    const lc_mode_t bad_flrc = { LC_MOD_FLRC, 0, 3, 16, 0, 1300000 };
    const lc_mode_t bad_mod = { 9, 7, 1, 8, 500000, 0 };
    TEST_ASSERT_EQUAL_UINT32(0, lc_airtime_us(&bad_sf, 28));
    TEST_ASSERT_EQUAL_UINT32(0, lc_airtime_us(&bad_cr, 28));
    TEST_ASSERT_EQUAL_UINT32(0, lc_airtime_us(&no_bw, 28));
    TEST_ASSERT_EQUAL_UINT32(0, lc_airtime_us(&bad_flrc, 28));
    TEST_ASSERT_EQUAL_UINT32(0, lc_airtime_us(&bad_mod, 28));
    TEST_ASSERT_EQUAL_UINT32(0, lc_airtime_us(NULL, 28));
    TEST_ASSERT_EQUAL_UINT32(0, lc_slot_len_us(NULL, 28));
    TEST_ASSERT_NULL(lc_tier_mode(LC_BAND_COUNT, LC_TIER_NEAR));
    TEST_ASSERT_NULL(lc_tier_mode(LC_BAND_915, LC_TIER_COUNT));
}

static void test_edge_tier_fits_spec_frame_budget(void)
{
    /* Spec §4.4: edge slot ~15–20 ms; at least 2 edge calls (DL+UL each) fit
     * in the ~100 ms voice budget. */
    uint32_t slot = lc_slot_len_us(lc_tier_mode(LC_BAND_915, LC_TIER_EDGE), 28);
    TEST_ASSERT_UINT32_WITHIN(2500u, 17500u, slot);
    TEST_ASSERT_TRUE(4u * slot <= 100000u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lora_airtime_matches_semtech_calculator);
    RUN_TEST(test_tier_airtimes_915);
    RUN_TEST(test_tier_airtimes_2g4);
    RUN_TEST(test_slot_len_adds_guard);
    RUN_TEST(test_airtime_grows_with_payload);
    RUN_TEST(test_invalid_modes_return_zero);
    RUN_TEST(test_edge_tier_fits_spec_frame_budget);
    return UNITY_END();
}
