#include "unity.h"

#include <math.h>
#include <string.h>

#include "oc_vox_bits.h"
#include "oc_vox_fec.h"
#include "oc_vox_layout.h"
#include "oc_vox_ref.h"
#include "vox_vectors.h"

void setUp(void) {}
void tearDown(void) {}

static vox_vector_t vec;
static int16_t clean[VOX_MAX_SAMPLES], hurt[VOX_MAX_SAMPLES];
static uint32_t xs;
static uint32_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

static double rms(const int16_t *x, unsigned n)
{
    double acc = 0;
    for (unsigned i = 0; i < n; i++) acc += (double)x[i] * x[i];
    return sqrt(acc / n);
}

typedef void (*hurt_fn)(uint8_t *block, unsigned nbytes, unsigned index, int *drop);

/* Encode the vector `passes` times over, decode it clean and through
 * hurt(); returns the decoder's stats of the hurt run. */
static oc_vox_dec_stats_t run(oc_vox_mode_t m, unsigned passes, hurt_fn fn)
{
    oc_vox_ref_enc_t e;
    oc_vox_ref_dec_t dc, dh;
    uint8_t block[OC_VOX_BLOCK_MAX_BYTES], copy[OC_VOX_BLOCK_MAX_BYTES];
    unsigned blocks = vec.n / 960, nb = oc_vox_block_bytes(m), index = 0;
    oc_vox_ref_enc_init(&e, m);
    oc_vox_ref_dec_init(&dc, m);
    oc_vox_ref_dec_init(&dh, m);
    for (unsigned p = 0; p < passes; p++) {
        for (unsigned b = 0; b < blocks; b++, index++) {
            int drop = 0;
            unsigned at = (index % (VOX_MAX_SAMPLES / 960)) * 960;
            oc_vox_ref_encode(&e, vec.pcm + 960 * b, block);
            oc_vox_ref_decode(&dc, block, clean + at);
            memcpy(copy, block, nb);
            fn(copy, nb, index, &drop);
            oc_vox_ref_decode(&dh, drop ? NULL : copy, hurt + at);
        }
    }
    return *oc_vox_ref_dec_stats(&dh);
}

static void drop_every_fifth(uint8_t *block, unsigned n, unsigned index, int *drop) { (void)block; (void)n; *drop = (index % 5) == 4; }

static void test_a_lost_block_in_five_keeps_the_loudness_and_never_clicks(void)
{
    unsigned blocks, b, bad = 0;
    oc_vox_dec_stats_t st;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    blocks = vec.n / 960;
    st = run(OC_VOX_1200, 1, drop_every_fifth);
    TEST_ASSERT_EQUAL_UINT32(blocks / 5, st.lost_blocks);
    TEST_ASSERT_EQUAL_UINT32(6 * (blocks / 5), st.concealed_frames);
    for (b = 2; b < blocks; b++) {
        double c = rms(clean + 960 * b, 960), h = rms(hurt + 960 * b, 960);
        if (c < 300.0) continue;
        if (fabs(20.0 * log10((h + 1.0) / (c + 1.0))) > 6.0) bad++;
    }
    TEST_ASSERT_TRUE_MESSAGE(bad <= blocks / 10, "a concealed block stays within 6 dB of the clean decode");
    for (unsigned i = 1; i < blocks * 960; i++) TEST_ASSERT_TRUE(abs((int)hurt[i] - (int)hurt[i - 1]) < 8000);
}

static void drop_all(uint8_t *block, unsigned n, unsigned index, int *drop) { (void)block; (void)n; (void)index; *drop = 1; }

static void test_twelve_lost_blocks_fade_to_silence(void)
{
    oc_vox_ref_enc_t e;
    oc_vox_ref_dec_t d;
    uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
    double first, later;
    (void)drop_all;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    oc_vox_ref_enc_init(&e, OC_VOX_1200);
    oc_vox_ref_dec_init(&d, OC_VOX_1200);
    for (unsigned b = 0; b < 4; b++) { oc_vox_ref_encode(&e, vec.pcm + 960 * b, block); oc_vox_ref_decode(&d, block, hurt); }
    oc_vox_ref_decode(&d, NULL, hurt);          /* lost block 1: held, then -3 dB a frame */
    first = rms(hurt, 960);
    TEST_ASSERT_TRUE(first > 100.0);
    oc_vox_ref_decode(&d, NULL, hurt);          /* lost block 2: frames 7-12, silence at 12 */
    later = rms(hurt, 960);
    TEST_ASSERT_TRUE(later < first * 0.5);
    TEST_ASSERT_EQUAL_INT16(0, hurt[959]);      /* the 12th lost frame is silent */
    oc_vox_ref_decode(&d, NULL, hurt);
    TEST_ASSERT_EQUAL_INT16(0, hurt[0]);
    TEST_ASSERT_EQUAL_INT16(0, hurt[959]);
    TEST_ASSERT_EQUAL_UINT32(3, oc_vox_ref_dec_stats(&d)->lost_blocks);
    TEST_ASSERT_EQUAL_UINT32(18, oc_vox_ref_dec_stats(&d)->concealed_frames);
}

static double ber_p;
static void flip_at_ber(uint8_t *block, unsigned n, unsigned index, int *drop)
{
    (void)index; (void)drop;
    for (unsigned i = 0; i < n * 8; i++)
        if ((double)(rnd() % 1000000u) / 1000000.0 < ber_p) oc_vox_bit_set(block, i, !oc_vox_bit_get(block, i));
}

static void test_bit_errors_at_one_per_thousand_are_repaired(void)
{
    unsigned total, bad = 0;
    oc_vox_dec_stats_t st;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    xs = 0xBEEF1234u;
    ber_p = 1e-3;
    st = run(OC_VOX_1200, 10, flip_at_ber); /* 500 blocks, 72000 bits: ~72 flipped */
    total = 10 * (vec.n / 960);
    TEST_ASSERT_EQUAL_UINT32(total, st.blocks);
    TEST_ASSERT_EQUAL_UINT32(0, st.golay_failed);
    TEST_ASSERT_EQUAL_UINT32(0, st.lost_blocks);
    TEST_ASSERT_TRUE(st.golay_fixed > 10);
    TEST_ASSERT_TRUE(st.hamming_fixed > 3);
    /* the last pass is in the buffers: within 3 dB on 90 % of loud blocks */
    for (unsigned b = 2; b < vec.n / 960; b++) {
        double c = rms(clean + 960 * b, 960), h = rms(hurt + 960 * b, 960);
        if (c < 300.0) continue;
        if (fabs(20.0 * log10((h + 1.0) / (c + 1.0))) > 3.0) bad++;
    }
    TEST_ASSERT_TRUE_MESSAGE(bad <= (vec.n / 960) / 10, "loudness within 3 dB at BER 1e-3");
}

static void test_heavy_bit_errors_stay_bounded_and_are_counted(void)
{
    oc_vox_dec_stats_t st;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    xs = 0x5EED5EEDu;
    ber_p = 5e-2;
    st = run(OC_VOX_1200, 10, flip_at_ber);
    TEST_ASSERT_TRUE(st.golay_failed > 5);
    TEST_ASSERT_TRUE(st.lost_blocks > 5);
    TEST_ASSERT_TRUE(st.lost_blocks <= st.golay_failed);
    TEST_ASSERT_EQUAL_UINT32(6 * st.lost_blocks, st.concealed_frames);
    for (unsigned i = 0; i < VOX_MAX_SAMPLES / 960 * 960; i++) TEST_ASSERT_TRUE(hurt[i] >= -32768 && hurt[i] <= 32767);
    TEST_ASSERT_TRUE(rms(hurt, vec.n) < 2.0 * rms(clean, vec.n) + 1000.0);
}

/* Dropping every third block without telling the decoder (as the app's
 * DOWN path would): the toggle bit repeats and the gap is counted. */
static void test_a_repeated_toggle_reports_an_unannounced_gap(void)
{
    oc_vox_ref_enc_t e;
    oc_vox_ref_dec_t d;
    uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
    unsigned blocks, b, delivered = 0;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    blocks = vec.n / 960;
    oc_vox_ref_enc_init(&e, OC_VOX_800);
    oc_vox_ref_dec_init(&d, OC_VOX_800);
    for (b = 0; b < blocks; b++) {
        oc_vox_ref_encode(&e, vec.pcm + 960 * b, block);
        if (b % 3 == 2) continue; /* vanishes */
        oc_vox_ref_decode(&d, block, hurt);
        delivered++;
    }
    TEST_ASSERT_EQUAL_UINT32(delivered, oc_vox_ref_dec_stats(&d)->blocks);
    TEST_ASSERT_EQUAL_UINT32(0, oc_vox_ref_dec_stats(&d)->lost_blocks);
    /* every drop sits between two delivered blocks with the same toggle */
    TEST_ASSERT_EQUAL_UINT32(blocks / 3 - (blocks % 3 == 0 ? 1 : 0), oc_vox_ref_dec_stats(&d)->toggle_gaps);
    /* and a stream with nothing missing reports none */
    oc_vox_ref_enc_init(&e, OC_VOX_800);
    oc_vox_ref_dec_init(&d, OC_VOX_800);
    for (b = 0; b < blocks; b++) { oc_vox_ref_encode(&e, vec.pcm + 960 * b, block); oc_vox_ref_decode(&d, block, hurt); }
    TEST_ASSERT_EQUAL_UINT32(0, oc_vox_ref_dec_stats(&d)->toggle_gaps);
}

static void flip_last_c_bit(uint8_t *block, unsigned n, unsigned index, int *drop)
{
    const oc_vox_shape_t *sh = oc_vox_shape(OC_VOX_2400);
    unsigned nbits = n * 8, depth = nbits / sh->ilv_width;
    unsigned cbits = sh->param_bits - 12u * sh->golay - 8u * sh->hamming;
    unsigned coded_i = 24u * sh->golay + 12u * sh->hamming + cbits - 1u;
    unsigned pos = (coded_i % sh->ilv_width) * depth + coded_i / sh->ilv_width;
    (void)index; (void)drop;
    oc_vox_bit_set(block, pos, !oc_vox_bit_get(block, pos));
}

static void test_a_class_c_error_in_2400_drops_the_fine_detail_only(void)
{
    oc_vox_dec_stats_t st;
    unsigned blocks, b, bad = 0;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    blocks = vec.n / 960;
    st = run(OC_VOX_2400, 1, flip_last_c_bit);
    TEST_ASSERT_EQUAL_UINT32(blocks, st.crc_failed);
    TEST_ASSERT_EQUAL_UINT32(0, st.lost_blocks);
    TEST_ASSERT_EQUAL_UINT32(0, st.golay_fixed);
    for (b = 2; b < blocks; b++) {
        double c = rms(clean + 960 * b, 960), h = rms(hurt + 960 * b, 960);
        if (c < 300.0) continue;
        if (fabs(20.0 * log10((h + 1.0) / (c + 1.0))) > 2.0) bad++;
    }
    TEST_ASSERT_TRUE_MESSAGE(bad <= blocks / 10, "loudness within 2 dB with the fine stages dropped");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_lost_block_in_five_keeps_the_loudness_and_never_clicks);
    RUN_TEST(test_twelve_lost_blocks_fade_to_silence);
    RUN_TEST(test_bit_errors_at_one_per_thousand_are_repaired);
    RUN_TEST(test_heavy_bit_errors_stay_bounded_and_are_counted);
    RUN_TEST(test_a_repeated_toggle_reports_an_unannounced_gap);
    RUN_TEST(test_a_class_c_error_in_2400_drops_the_fine_detail_only);
    return UNITY_END();
}
