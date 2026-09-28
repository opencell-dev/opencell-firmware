#include "unity.h"

#include <string.h>

#include "lc_phy.h"

void setUp(void) {}
void tearDown(void) {}

#define SEED 0x12345678u

static void test_channel_counts(void)
{
    TEST_ASSERT_EQUAL_UINT8(52, lc_num_channels(LC_BAND_915));
    TEST_ASSERT_EQUAL_UINT8(40, lc_num_channels(LC_BAND_2G4));
    TEST_ASSERT_EQUAL_UINT8(0, lc_num_channels(LC_BAND_COUNT));
}

static void test_channel_frequencies_inside_bands(void)
{
    TEST_ASSERT_EQUAL_UINT32(902250000u, lc_channel_freq_hz(LC_BAND_915, 0));
    TEST_ASSERT_EQUAL_UINT32(927750000u, lc_channel_freq_hz(LC_BAND_915, 51));
    TEST_ASSERT_EQUAL_UINT32(2402000000u, lc_channel_freq_hz(LC_BAND_2G4, 0));
    TEST_ASSERT_EQUAL_UINT32(2480000000u, lc_channel_freq_hz(LC_BAND_2G4, 39));
}

static void test_channel_frequency_invalid(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, lc_channel_freq_hz(LC_BAND_915, 52));
    TEST_ASSERT_EQUAL_UINT32(0, lc_channel_freq_hz(LC_BAND_2G4, 40));
    TEST_ASSERT_EQUAL_UINT32(0, lc_channel_freq_hz(LC_BAND_COUNT, 0));
}

/* Golden vectors: these pin the algorithm. If they change, deployed radios on
 * older firmware will stop hearing each other. */
static void test_hop_golden_vectors(void)
{
    static const uint8_t b915_r0[8] = { 36, 44, 40, 37, 48, 5, 17, 27 };
    static const uint8_t b915_r1[8] = { 33, 1, 29, 4, 25, 49, 15, 28 };
    static const uint8_t b2g4_r0[8] = { 5, 30, 21, 3, 25, 39, 9, 28 };
    static const uint8_t b2g4_r1[8] = { 37, 6, 36, 4, 10, 15, 14, 16 };
    static const uint8_t b915_r0_f1001[8] = { 9, 30, 48, 38, 19, 8, 28, 40 };
    for (uint8_t s = 0; s < 8; s++) {
        TEST_ASSERT_EQUAL_UINT8(b915_r0[s], lc_hop_channel(SEED, LC_BAND_915, 0, 1000, s));
        TEST_ASSERT_EQUAL_UINT8(b915_r1[s], lc_hop_channel(SEED, LC_BAND_915, 1, 1000, s));
        TEST_ASSERT_EQUAL_UINT8(b2g4_r0[s], lc_hop_channel(SEED, LC_BAND_2G4, 0, 1000, s));
        TEST_ASSERT_EQUAL_UINT8(b2g4_r1[s], lc_hop_channel(SEED, LC_BAND_2G4, 1, 1000, s));
        TEST_ASSERT_EQUAL_UINT8(b915_r0_f1001[s], lc_hop_channel(SEED, LC_BAND_915, 0, 1001, s));
    }
}

static void test_hop_radios_never_collide_within_frame(void)
{
    for (uint32_t frame = 0; frame < 500; frame++) {
        uint8_t owner[52];
        for (int c = 0; c < 52; c++) {
            owner[c] = 0xFF;
        }
        for (uint8_t r = 0; r < LC_MAX_RADIOS_PER_BAND; r++) {
            for (uint16_t s = 0; s < 256; s++) {
                uint8_t ch = lc_hop_channel(SEED, LC_BAND_915, r, frame, (uint8_t)s);
                TEST_ASSERT_TRUE(owner[ch] == 0xFF || owner[ch] == r);
                owner[ch] = r;
            }
        }
    }
}

static void test_hop_uses_channels_evenly(void)
{
    uint32_t counts[52] = { 0 };
    for (uint32_t frame = 0; frame < 20000; frame++) {
        for (uint8_t s = 0; s < 20; s++) {
            counts[lc_hop_channel(0xCAFEF00Du, LC_BAND_915, 0, frame, s)]++;
        }
    }
    const uint32_t mean = 20000u * 20u / 52u; /* 7692 */
    for (int c = 0; c < 52; c++) {
        TEST_ASSERT_UINT32_WITHIN(mean * 6u / 100u, mean, counts[c]);
    }
}

static void test_hop_frame_number_extremes_stay_in_range(void)
{
    const uint32_t frames[] = { 0u, 1u, 0x7FFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu };
    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        for (uint16_t s = 0; s < 256; s++) {
            TEST_ASSERT_LESS_THAN_UINT8(52, lc_hop_channel(SEED, LC_BAND_915, 3, frames[i], (uint8_t)s));
            TEST_ASSERT_LESS_THAN_UINT8(40, lc_hop_channel(SEED, LC_BAND_2G4, 3, frames[i], (uint8_t)s));
        }
    }
}

static void test_hop_invalid_inputs(void)
{
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_hop_channel(SEED, LC_BAND_915, LC_MAX_RADIOS_PER_BAND, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_hop_channel(SEED, LC_BAND_COUNT, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_sync_channel(SEED, LC_BAND_COUNT, 0));
}

static void test_sync_channels_golden_and_cycle(void)
{
    static const uint8_t b915[8] = { 0, 6, 13, 19, 26, 32, 39, 45 };
    static const uint8_t b2g4[8] = { 1, 6, 11, 16, 21, 26, 31, 36 };
    for (uint32_t f = 0; f < 8; f++) {
        TEST_ASSERT_EQUAL_UINT8(b915[f], lc_sync_channel(SEED, LC_BAND_915, f));
        TEST_ASSERT_EQUAL_UINT8(b2g4[f], lc_sync_channel(SEED, LC_BAND_2G4, f));
        TEST_ASSERT_EQUAL_UINT8(b915[f], lc_sync_channel(SEED, LC_BAND_915, f + 8));
    }
}

static void test_sync_channels_in_range_for_any_seed(void)
{
    for (uint32_t seed = 0; seed < 1000; seed++) {
        for (uint32_t f = 0; f < 8; f++) {
            TEST_ASSERT_LESS_THAN_UINT8(52, lc_sync_channel(seed * 2654435761u, LC_BAND_915, f));
            TEST_ASSERT_LESS_THAN_UINT8(40, lc_sync_channel(seed * 2654435761u, LC_BAND_2G4, f));
        }
    }
}

/* Channel-list spec §3.1: the seed-derived anchor is today's sync channel. */
static void test_sync_channel_at_seed_anchor_is_legacy(void)
{
    for (uint32_t seed = 0; seed < 600; seed++) {
        uint32_t s = seed * 2654435761u;
        for (uint32_t f = 0; f < 16; f++) {
            TEST_ASSERT_EQUAL_UINT8(lc_sync_channel(s, LC_BAND_915, f),
                                    lc_sync_channel_at((uint8_t)(s % 6u), LC_BAND_915, f));
            TEST_ASSERT_EQUAL_UINT8(lc_sync_channel(s, LC_BAND_2G4, f),
                                    lc_sync_channel_at((uint8_t)(s % 5u), LC_BAND_2G4, f));
        }
    }
}

static void test_sync_channel_at_wraps_and_rejects(void)
{
    static const uint8_t a30[8] = { 30, 36, 43, 49, 4, 10, 17, 23 };
    static const uint8_t a51[8] = { 51, 5, 12, 18, 25, 31, 38, 44 };
    for (uint32_t f = 0; f < 8; f++) {
        TEST_ASSERT_EQUAL_UINT8(a30[f], lc_sync_channel_at(30, LC_BAND_915, f));
        TEST_ASSERT_EQUAL_UINT8(a51[f], lc_sync_channel_at(51, LC_BAND_915, f + 800));
    }
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_sync_channel_at(52, LC_BAND_915, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_sync_channel_at(40, LC_BAND_2G4, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_sync_channel_at(0, LC_BAND_COUNT, 0));
}

/* Distinct anchors never collide: in every frame each of the 52 anchors has
 * its own channel. */
static void test_distinct_anchors_never_share_a_frame(void)
{
    for (uint32_t f = 0; f < 8; f++) {
        uint8_t owner[52];
        memset(owner, 0xFF, sizeof(owner));
        for (uint8_t a = 0; a < 52; a++) {
            uint8_t ch = lc_sync_channel_at(a, LC_BAND_915, f);
            TEST_ASSERT_LESS_THAN_UINT8(52, ch);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, owner[ch], "two anchors on one channel");
            owner[ch] = a;
        }
    }
}

static void test_channel_of_freq(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, lc_channel_of_freq(LC_BAND_915, 902250000u));
    TEST_ASSERT_EQUAL_UINT8(30, lc_channel_of_freq(LC_BAND_915, 917250000u));
    TEST_ASSERT_EQUAL_UINT8(51, lc_channel_of_freq(LC_BAND_915, 927750000u));
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_channel_of_freq(LC_BAND_915, 917300000u)); /* off grid */
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_channel_of_freq(LC_BAND_915, 928250000u)); /* past ch 51 */
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_channel_of_freq(LC_BAND_915, 0));
}

/* §3.3 mode table: both modes allow the 915 grid; FIXED only in Part 97. */
static void test_sync_anchor_ok_table(void)
{
    TEST_ASSERT_TRUE(lc_sync_anchor_ok(LC_PHY_MODE_PART15, 902250000u, LC_SYNC_CYCLE));
    TEST_ASSERT_TRUE(lc_sync_anchor_ok(LC_PHY_MODE_PART15, 927750000u, LC_SYNC_CYCLE));
    TEST_ASSERT_FALSE(lc_sync_anchor_ok(LC_PHY_MODE_PART15, 917250000u, LC_SYNC_FIXED));
    TEST_ASSERT_TRUE(lc_sync_anchor_ok(LC_PHY_MODE_PART97, 917250000u, LC_SYNC_CYCLE));
    TEST_ASSERT_TRUE(lc_sync_anchor_ok(LC_PHY_MODE_PART97, 917250000u, LC_SYNC_FIXED));
    TEST_ASSERT_FALSE(lc_sync_anchor_ok(LC_PHY_MODE_PART97, 917300000u, LC_SYNC_CYCLE)); /* off grid */
    TEST_ASSERT_FALSE(lc_sync_anchor_ok(LC_PHY_MODE_PART15, 901750000u, LC_SYNC_CYCLE)); /* below the band */
    TEST_ASSERT_FALSE(lc_sync_anchor_ok(LC_PHY_MODE_PART97, 928250000u, LC_SYNC_CYCLE)); /* above it */
    TEST_ASSERT_FALSE(lc_sync_anchor_ok(LC_PHY_MODE_PART15, 2402000000u, LC_SYNC_CYCLE)); /* no 2.4 row */
    TEST_ASSERT_FALSE(lc_sync_anchor_ok(0, 902250000u, LC_SYNC_CYCLE)); /* no mode */
    TEST_ASSERT_FALSE(lc_sync_anchor_ok(3, 902250000u, LC_SYNC_CYCLE));
    TEST_ASSERT_FALSE(lc_sync_anchor_ok(LC_PHY_MODE_PART97, 902250000u, 2)); /* no such pattern */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_channel_counts);
    RUN_TEST(test_channel_frequencies_inside_bands);
    RUN_TEST(test_channel_frequency_invalid);
    RUN_TEST(test_hop_golden_vectors);
    RUN_TEST(test_hop_radios_never_collide_within_frame);
    RUN_TEST(test_hop_uses_channels_evenly);
    RUN_TEST(test_hop_frame_number_extremes_stay_in_range);
    RUN_TEST(test_hop_invalid_inputs);
    RUN_TEST(test_sync_channels_golden_and_cycle);
    RUN_TEST(test_sync_channels_in_range_for_any_seed);
    RUN_TEST(test_sync_channel_at_seed_anchor_is_legacy);
    RUN_TEST(test_sync_channel_at_wraps_and_rejects);
    RUN_TEST(test_distinct_anchors_never_share_a_frame);
    RUN_TEST(test_channel_of_freq);
    RUN_TEST(test_sync_anchor_ok_table);
    return UNITY_END();
}
