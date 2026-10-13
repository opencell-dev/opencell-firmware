#include "unity.h"

#include <math.h>
#include <string.h>

#include "oc_vox_ref_int.h"

void setUp(void) {}
void tearDown(void) {}

static void test_fft_matches_a_naive_dft(void)
{
    float in[256], re[129], im[129];
    uint32_t xs = 0xC0FFEEu;
    for (int i = 0; i < 256; i++) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; in[i] = (float)(xs % 20001u) / 10000.0f - 1.0f; }
    oc_vox_ref_fft256(in, re, im);
    for (int k = 0; k <= 128; k += 7) {
        double sr = 0, si = 0;
        for (int n = 0; n < 256; n++) { sr += in[n] * cos(2 * M_PI * k * n / 256.0); si -= in[n] * sin(2 * M_PI * k * n / 256.0); }
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, (float)sr, re[k]);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, (float)si, im[k]);
    }
    /* a bin-centred sine lands in its bin */
    for (int i = 0; i < 256; i++) in[i] = sinf(2.0f * (float)M_PI * 10.0f * (float)i / 256.0f);
    oc_vox_ref_fft256(in, re, im);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -128.0f, im[10]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, re[10]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, re[11]);
}

static void test_hann_window_sums_to_half(void)
{
    float w[256];
    double s = 0;
    oc_vox_ref_hann256(w);
    for (int i = 0; i < 256; i++) s += w[i];
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 128.0f, (float)s);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, w[0], w[255]);
    TEST_ASSERT_TRUE(w[128] > 0.999f);
}

static void test_ncc_peaks_at_the_period(void)
{
    float x[480];
    for (int i = 0; i < 480; i++) x[i] = (i % 80) < 4 ? 1000.0f : 0.0f; /* 100 Hz pulse train */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, oc_vox_ref_ncc(x + 160, 160, 80));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, oc_vox_ref_ncc(x + 160, 160, 160));
    TEST_ASSERT_TRUE(oc_vox_ref_ncc(x + 160, 160, 40) < 0.1f);
    TEST_ASSERT_TRUE(oc_vox_ref_ncc(x + 160, 160, 77) < 0.6f);
    memset(x, 0, sizeof(x));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, oc_vox_ref_ncc(x + 160, 160, 80));
}

static void test_energy_conversions_round_trip(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, oc_vox_ref_rms_to_e(0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, oc_vox_ref_e_to_rms(0));
    for (int e = 8; e < 128; e += 5) TEST_ASSERT_EQUAL_UINT8(e, oc_vox_ref_rms_to_e(oc_vox_ref_e_to_rms((uint8_t)e)));
    TEST_ASSERT_UINT8_WITHIN(1, 124, oc_vox_ref_rms_to_e(32768.0f * 0.707f)); /* a full-scale sine: -3 dBFS */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -120.0f, oc_vox_ref_db_of_power(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, oc_vox_ref_db_of_power(100.0f));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 173.2f, oc_vox_ref_band_centre_hz(0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fft_matches_a_naive_dft);
    RUN_TEST(test_hann_window_sums_to_half);
    RUN_TEST(test_ncc_peaks_at_the_period);
    RUN_TEST(test_energy_conversions_round_trip);
    return UNITY_END();
}
