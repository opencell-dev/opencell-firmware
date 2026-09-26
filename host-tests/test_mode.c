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
    /* 2.4 GHz mid is LoRa SF7 / 812.5 kHz (bench 2026-09-25: FLRC below 1.3 Mb/s
     * never passes CRC on the 2.4 GHz path). */
    const lc_mode_t *mid = lc_tier_mode(LC_BAND_2G4, LC_TIER_MID);
    TEST_ASSERT_EQUAL_UINT8(LC_MOD_LORA, mid->modulation);
    TEST_ASSERT_EQUAL_UINT8(7, mid->sf);
    TEST_ASSERT_EQUAL_UINT32(812500u, mid->bw_hz);
    TEST_ASSERT_EQUAL_UINT32(10280u, lc_airtime_us(mid, 28));
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

/* Bench (2026-09-26, two W12s, TX preamble on the slot start): RX_DONE minus
 * (slot start + lc_airtime_us) per tier; the fit stays within 12 us. */
static void test_rx_done_lag_matches_bench(void)
{
    /* 10 000-frame runs (2026-09-26): RX_DONE - ideal end, less the TX start offset */
    static const struct { lc_band_t band; lc_tier_t tier; int32_t us; } bench[] = {
        { LC_BAND_915, LC_TIER_EDGE, 225 }, { LC_BAND_915, LC_TIER_MID, 140 }, { LC_BAND_915, LC_TIER_NEAR, 259 },
        { LC_BAND_2G4, LC_TIER_NEAR, 162 }, { LC_BAND_2G4, LC_TIER_MID, 166 },
    };
    for (unsigned i = 0; i < sizeof(bench) / sizeof(bench[0]); i++) {
        int32_t lag = (int32_t)lc_rx_done_lag_us(lc_tier_mode(bench[i].band, bench[i].tier));
        TEST_ASSERT_INT32_WITHIN(2, bench[i].us, lag); /* tier modes: measured values */
    }
    /* other modes fall back to the fit (within ~12 us of the bench points) */
    lc_mode_t sf6 = { LC_MOD_LORA, 6, 1, 8, 500000, 0 };
    TEST_ASSERT_INT32_WITHIN(15, 170, (int32_t)lc_rx_done_lag_us(&sf6));
    TEST_ASSERT_EQUAL_UINT32(0, lc_rx_done_lag_us(NULL));
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
    RUN_TEST(test_rx_done_lag_matches_bench);
    return UNITY_END();
}
