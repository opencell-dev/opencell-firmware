#include "unity.h"

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
    return UNITY_END();
}
