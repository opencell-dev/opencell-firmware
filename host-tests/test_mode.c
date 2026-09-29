#include "unity.h"

#include "oc_phy.h"

void setUp(void) {}
void tearDown(void) {}

static void test_lora_airtime_matches_semtech_calculator(void)
{
    /* SF12 / BW125 / CR4/5 / preamble 8 / 10 bytes: Semtech calculator 991.23 ms */
    const oc_mode_t sf12 = { OC_MOD_LORA, 12, 1, 8, 125000, 0 };
    TEST_ASSERT_EQUAL_UINT32(991232u, oc_airtime_us(&sf12, 10));

    const oc_mode_t sf7_250 = { OC_MOD_LORA, 7, 1, 8, 250000, 0 };
    TEST_ASSERT_EQUAL_UINT32(33408u, oc_airtime_us(&sf7_250, 28));
}

static void test_tier_airtimes_915(void)
{
    TEST_ASSERT_EQUAL_UINT32(1500u, oc_airtime_us(oc_tier_mode(OC_BAND_915, OC_TIER_NEAR), 28));
    TEST_ASSERT_EQUAL_UINT32(5264u, oc_airtime_us(oc_tier_mode(OC_BAND_915, OC_TIER_MID), 28));
    TEST_ASSERT_EQUAL_UINT32(16704u, oc_airtime_us(oc_tier_mode(OC_BAND_915, OC_TIER_EDGE), 28));
}

static void test_tier_airtimes_2g4(void)
{
    TEST_ASSERT_EQUAL_UINT32(300u, oc_airtime_us(oc_tier_mode(OC_BAND_2G4, OC_TIER_NEAR), 28));
    /* 2.4 GHz mid is LoRa SF7 / 812.5 kHz (bench 2026-09-25: FLRC below 1.3 Mb/s
     * never passes CRC on the 2.4 GHz path). */
    const oc_mode_t *mid = oc_tier_mode(OC_BAND_2G4, OC_TIER_MID);
    TEST_ASSERT_EQUAL_UINT8(OC_MOD_LORA, mid->modulation);
    TEST_ASSERT_EQUAL_UINT8(7, mid->sf);
    TEST_ASSERT_EQUAL_UINT32(812500u, mid->bw_hz);
    TEST_ASSERT_EQUAL_UINT32(10280u, oc_airtime_us(mid, 28));
    TEST_ASSERT_NULL(oc_tier_mode(OC_BAND_2G4, OC_TIER_EDGE));
}

static void test_slot_len_adds_guard(void)
{
    const oc_mode_t *edge = oc_tier_mode(OC_BAND_915, OC_TIER_EDGE);
    TEST_ASSERT_EQUAL_UINT32(16704u + OC_GUARD_US, oc_slot_len_us(edge, 28));
}

static void test_airtime_grows_with_payload(void)
{
    for (int b = 0; b < OC_BAND_COUNT; b++) {
        for (int t = 0; t < OC_TIER_COUNT; t++) {
            const oc_mode_t *m = oc_tier_mode((oc_band_t)b, (oc_tier_t)t);
            if (m == NULL) {
                continue;
            }
            TEST_ASSERT_TRUE(oc_airtime_us(m, 0) > 0);
            TEST_ASSERT_TRUE(oc_airtime_us(m, 255) > oc_airtime_us(m, 28));
        }
    }
}

static void test_invalid_modes_return_zero(void)
{
    const oc_mode_t bad_sf = { OC_MOD_LORA, 4, 1, 8, 500000, 0 };
    const oc_mode_t bad_cr = { OC_MOD_LORA, 7, 5, 8, 500000, 0 };
    const oc_mode_t no_bw = { OC_MOD_LORA, 7, 1, 8, 0, 0 };
    const oc_mode_t bad_flrc = { OC_MOD_FLRC, 0, 3, 16, 0, 1300000 };
    const oc_mode_t bad_mod = { 9, 7, 1, 8, 500000, 0 };
    TEST_ASSERT_EQUAL_UINT32(0, oc_airtime_us(&bad_sf, 28));
    TEST_ASSERT_EQUAL_UINT32(0, oc_airtime_us(&bad_cr, 28));
    TEST_ASSERT_EQUAL_UINT32(0, oc_airtime_us(&no_bw, 28));
    TEST_ASSERT_EQUAL_UINT32(0, oc_airtime_us(&bad_flrc, 28));
    TEST_ASSERT_EQUAL_UINT32(0, oc_airtime_us(&bad_mod, 28));
    TEST_ASSERT_EQUAL_UINT32(0, oc_airtime_us(NULL, 28));
    TEST_ASSERT_EQUAL_UINT32(0, oc_slot_len_us(NULL, 28));
    TEST_ASSERT_NULL(oc_tier_mode(OC_BAND_COUNT, OC_TIER_NEAR));
    TEST_ASSERT_NULL(oc_tier_mode(OC_BAND_915, OC_TIER_COUNT));
}

static void test_edge_tier_fits_spec_frame_budget(void)
{
    /* Spec §4.4: edge slot ~15–20 ms; at least 2 edge calls (DL+UL each) fit
     * in the ~100 ms voice budget. */
    uint32_t slot = oc_slot_len_us(oc_tier_mode(OC_BAND_915, OC_TIER_EDGE), 28);
    TEST_ASSERT_UINT32_WITHIN(2500u, 17500u, slot);
    TEST_ASSERT_TRUE(4u * slot <= 100000u);
}

/* Bench (2026-09-26, two W12s, TX preamble on the slot start): RX_DONE minus
 * (slot start + oc_airtime_us) per tier; the fit stays within 12 us. */
static void test_rx_done_lag_matches_bench(void)
{
    /* 10 000-frame runs (2026-09-26): RX_DONE - ideal end, less the TX start offset */
    static const struct { oc_band_t band; oc_tier_t tier; int32_t us; } bench[] = {
        { OC_BAND_915, OC_TIER_EDGE, 225 }, { OC_BAND_915, OC_TIER_MID, 140 }, { OC_BAND_915, OC_TIER_NEAR, 259 },
        { OC_BAND_2G4, OC_TIER_NEAR, 162 }, { OC_BAND_2G4, OC_TIER_MID, 166 },
    };
    for (unsigned i = 0; i < sizeof(bench) / sizeof(bench[0]); i++) {
        int32_t lag = (int32_t)oc_rx_done_lag_us(oc_tier_mode(bench[i].band, bench[i].tier));
        TEST_ASSERT_INT32_WITHIN(2, bench[i].us, lag); /* tier modes: measured values */
    }
    /* other modes fall back to the fit (within ~12 us of the bench points) */
    oc_mode_t sf6 = { OC_MOD_LORA, 6, 1, 8, 500000, 0 };
    TEST_ASSERT_INT32_WITHIN(15, 170, (int32_t)oc_rx_done_lag_us(&sf6));
    TEST_ASSERT_EQUAL_UINT32(0, oc_rx_done_lag_us(NULL));
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
