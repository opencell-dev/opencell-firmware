#include "unity.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "oc_vox_fx_int.h"
#include "oc_vox_ref_int.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t xs = 0xF00DF00Du;
static uint32_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

static void test_fixed_fft_matches_the_float_fft_within_a_tenth_of_a_db(void)
{
    float in[256], re[129], im[129];
    int32_t xr[256], xi[256];
    for (int i = 0; i < 256; i++) {
        float s = 6000.0f * sinf(2.0f * OC_VOX_PI * 440.0f * (float)i / 8000.0f) + 2000.0f * sinf(2.0f * OC_VOX_PI * 1333.0f * (float)i / 8000.0f)
                  + (float)((int)(rnd() % 400u) - 200);
        float w = 0.5f - 0.5f * cosf(2.0f * OC_VOX_PI * ((float)i + 0.5f) / 256.0f);
        in[i] = s * w;
        xr[i] = (int32_t)((int32_t)s * oc_vox_fx_hann256[i]) >> 7; /* Q8 samples */
        xi[i] = 0;
    }
    oc_vox_ref_fft256(in, re, im);
    oc_vox_fx_fft256(xr, xi);
    for (int k = 0; k <= 128; k++) {
        /* the fixed output is DFT / 256 in Q8: x 256 / 256 = sample units */
        double pf = (double)re[k] * re[k] + (double)im[k] * im[k];
        double px = (double)xr[k] * xr[k] + (double)xi[k] * xi[k];
        if (pf < 1e6) continue; /* only bins above -60 dB of the peak (~1e12) */
        TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, (float)(10.0 * log10(px / pf)));
    }
}

static void test_log2_exp2_and_sqrt_tables(void)
{
    for (uint32_t v = 1; v < 0xFFFFFFFFu / 3u; v = v * 3u + 1u) {
        TEST_ASSERT_INT32_WITHIN(5, (int32_t)(256.0 * log2((double)v) + 0.5), oc_vox_fx_log2_q8(v)); /* 0.02 */
        TEST_ASSERT_UINT32_WITHIN(1, (uint32_t)sqrt((double)v), oc_vox_fx_sqrt32(v));
    }
    TEST_ASSERT_EQUAL_INT32(0, oc_vox_fx_log2_q8(0));
    TEST_ASSERT_EQUAL_INT32(0, oc_vox_fx_log2_q8(1));
    TEST_ASSERT_EQUAL_INT32(8 * 256, oc_vox_fx_log2_q8(256));
    TEST_ASSERT_INT32_WITHIN(5, 40 * 256, oc_vox_fx_log2_q8_64((uint64_t)1 << 40));
    for (int32_t x = -15 * 256; x <= 15 * 256; x += 37)
        TEST_ASSERT_FLOAT_WITHIN((float)(0.002 * 32768.0 * pow(2.0, x / 256.0) + 1.0), (float)(32768.0 * pow(2.0, x / 256.0)), (float)oc_vox_fx_exp2_q15(x));
    TEST_ASSERT_EQUAL_UINT32(32768, oc_vox_fx_exp2_q15(0));
    TEST_ASSERT_EQUAL_UINT32(65536, oc_vox_fx_exp2_q15(256));
    TEST_ASSERT_EQUAL_UINT32(1000000, oc_vox_fx_sqrt64((uint64_t)1000000 * 1000000));
    TEST_ASSERT_EQUAL_UINT32(0, oc_vox_fx_sqrt64(0));
}

static void test_sine_table_and_phase_accumulator(void)
{
    for (uint32_t ph = 0; ph < 0xFFFFFF00u; ph += 0x01234567u)
        TEST_ASSERT_FLOAT_WITHIN(1e-4f * 32768.0f + 2.0f, 32767.0f * sinf(2.0f * OC_VOX_PI * (float)ph / 4294967296.0f), (float)oc_vox_fx_sin_q15(ph));
    TEST_ASSERT_EQUAL_INT16(0, oc_vox_fx_sin_q15(0));
    TEST_ASSERT_EQUAL_INT16(32767, oc_vox_fx_sin_q15(0x40000000u));
}

static void test_ncc_matches_the_float_ncc(void)
{
    int16_t x[480];
    float xf[480];
    for (int i = 0; i < 480; i++) {
        float v = 8000.0f * sinf(2.0f * OC_VOX_PI * 125.0f * (float)i / 8000.0f) + (float)((int)(rnd() % 2000u) - 1000);
        x[i] = (int16_t)v;
        xf[i] = (float)x[i];
    }
    for (unsigned lag = 20; lag <= 160; lag += 7)
        TEST_ASSERT_FLOAT_WITHIN(0.01f, oc_vox_ref_ncc(xf + 160, 160, lag), (float)oc_vox_fx_ncc_q15(x + 160, 160, lag) / 32768.0f);
    TEST_ASSERT_INT32_WITHIN(400, 32768, oc_vox_fx_ncc_q15(x + 160, 160, 64)); /* the period */
    memset(x, 0, sizeof(x));
    TEST_ASSERT_EQUAL_INT32(0, oc_vox_fx_ncc_q15(x + 160, 160, 64));
}

static void test_units_of_a_log2_ratio(void)
{
    /* 10 dB = 13.33 units = 3.3219 x 256 Q8 */
    TEST_ASSERT_INT32_WITHIN(1, 13, oc_vox_fx_units_of_log2_q8(850));
    TEST_ASSERT_INT32_WITHIN(1, -13, oc_vox_fx_units_of_log2_q8(-850));
    TEST_ASSERT_EQUAL_INT32(4, oc_vox_fx_units_of_log2_q8(256)); /* 6 dB = 4 units */
    TEST_ASSERT_EQUAL_INT32(0, oc_vox_fx_units_of_log2_q8(0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixed_fft_matches_the_float_fft_within_a_tenth_of_a_db);
    RUN_TEST(test_log2_exp2_and_sqrt_tables);
    RUN_TEST(test_sine_table_and_phase_accumulator);
    RUN_TEST(test_ncc_matches_the_float_ncc);
    RUN_TEST(test_units_of_a_log2_ratio);
    return UNITY_END();
}
