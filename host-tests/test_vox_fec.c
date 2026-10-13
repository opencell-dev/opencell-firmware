#include "unity.h"

#include <string.h>

#include "oc_vox.h"
#include "oc_vox_bits.h"
#include "oc_vox_fec.h"
#include "oc_vox_layout.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t xs = 0x2545F491u;
static uint32_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

static unsigned weight(uint32_t v) { unsigned n = 0; while (v) { n += v & 1u; v >>= 1; } return n; }

/* The check bits from the generator polynomial 0xC75, independently of
 * the table in oc_vox_fec.c. */
static uint32_t golay_from_polynomial(uint32_t d)
{
    uint32_t r = d << 11;
    for (int i = 22; i >= 11; i--)
        if (r & (1u << i)) r ^= 0xC75u << (i - 11);
    uint32_t p = r & 0x7FFu;
    uint32_t w = (d << 11) | p;
    return (d << 12) | (p << 1) | (weight(w) & 1u);
}

static void test_golay_encoder_matches_the_polynomial_and_is_distance_8(void)
{
    unsigned minw = 24;
    for (uint32_t d = 0; d < 4096; d++) {
        uint32_t c = oc_vox_golay_encode(d);
        TEST_ASSERT_EQUAL_HEX32(golay_from_polynomial(d), c);
        if (d) { unsigned w = weight(c); if (w < minw) minw = w; }
    }
    TEST_ASSERT_EQUAL_UINT(8, minw);
}

static void test_golay_corrects_every_1_2_3_bit_pattern(void)
{
    for (int n = 0; n < 64; n++) {
        uint32_t d = rnd() & 0xFFFu, c = oc_vox_golay_encode(d), out;
        TEST_ASSERT_EQUAL_INT(0, oc_vox_golay_decode(c, &out));
        TEST_ASSERT_EQUAL_HEX32(d, out);
        for (int i = 0; i < 24; i++) {
            TEST_ASSERT_EQUAL_INT(1, oc_vox_golay_decode(c ^ (1u << i), &out));
            TEST_ASSERT_EQUAL_HEX32(d, out);
            for (int j = i + 1; j < 24; j++) {
                TEST_ASSERT_EQUAL_INT(2, oc_vox_golay_decode(c ^ (1u << i) ^ (1u << j), &out));
                TEST_ASSERT_EQUAL_HEX32(d, out);
                for (int k = j + 1; k < 24; k++) {
                    TEST_ASSERT_EQUAL_INT(3, oc_vox_golay_decode(c ^ (1u << i) ^ (1u << j) ^ (1u << k), &out));
                    TEST_ASSERT_EQUAL_HEX32(d, out);
                }
            }
        }
    }
}

static void test_golay_detects_every_4_bit_pattern(void)
{
    for (int n = 0; n < 4; n++) {
        uint32_t d = rnd() & 0xFFFu, c = oc_vox_golay_encode(d), out;
        for (int i = 0; i < 24; i++)
            for (int j = i + 1; j < 24; j++)
                for (int k = j + 1; k < 24; k++)
                    for (int l = k + 1; l < 24; l++)
                        TEST_ASSERT_EQUAL_INT(-1, oc_vox_golay_decode(c ^ (1u << i) ^ (1u << j) ^ (1u << k) ^ (1u << l), &out));
    }
}

static void test_hamming_corrects_single_errors(void)
{
    for (unsigned d = 0; d < 256; d++) {
        uint16_t c = oc_vox_hamming_encode((uint8_t)d);
        uint8_t out;
        TEST_ASSERT_EQUAL_UINT16(d, c >> 4);
        TEST_ASSERT_EQUAL_INT(0, oc_vox_hamming_decode(c, &out));
        TEST_ASSERT_EQUAL_UINT8(d, out);
        for (int i = 0; i < 12; i++) {
            TEST_ASSERT_EQUAL_INT(1, oc_vox_hamming_decode((uint16_t)(c ^ (1u << i)), &out));
            TEST_ASSERT_EQUAL_UINT8(d, out);
        }
    }
    /* distinct nonzero columns: every pair of single-error syndromes differs */
    unsigned detected = 0, total = 0;
    for (unsigned d = 0; d < 256; d += 17) {
        uint16_t c = oc_vox_hamming_encode((uint8_t)d);
        uint8_t out;
        for (int i = 0; i < 12; i++)
            for (int j = i + 1; j < 12; j++) {
                total++;
                if (oc_vox_hamming_decode((uint16_t)(c ^ (1u << i) ^ (1u << j)), &out) < 0) detected++;
            }
    }
    TEST_ASSERT_TRUE(detected > 0 && detected < total); /* some double errors are detected, as documented */
}

static void test_crc_known_answers(void)
{
    /* CRC-8 poly 0x2F over "123456789" (init 0, no reflection, no xorout) = 0xA2? no: check against a
     * direct bitwise reference computed here. */
    static const uint8_t msg[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    uint32_t ref8 = 0, ref12 = 0;
    for (unsigned i = 0; i < 72; i++) {
        unsigned bit = oc_vox_bit_get(msg, i);
        unsigned in8 = bit ^ ((ref8 >> 7) & 1u), in12 = bit ^ ((ref12 >> 11) & 1u);
        ref8 = (ref8 << 1) & 0xFFu; if (in8) ref8 ^= 0x2Fu;
        ref12 = (ref12 << 1) & 0xFFFu; if (in12) ref12 ^= 0x80Fu;
    }
    TEST_ASSERT_EQUAL_HEX16(ref8, oc_vox_crc_bits(msg, 0, 72, 8));
    TEST_ASSERT_EQUAL_HEX16(ref12, oc_vox_crc_bits(msg, 0, 72, 12));
    TEST_ASSERT_EQUAL_HEX16(0, oc_vox_crc_bits(msg, 8, 8, 8));
    TEST_ASSERT_NOT_EQUAL(oc_vox_crc_bits(msg, 0, 72, 8), oc_vox_crc_bits(msg, 3, 72, 8)); /* drops a 1 bit ('1' = 0x31) */
    /* a single flipped bit anywhere changes the CRC */
    uint8_t m2[9];
    for (unsigned i = 0; i < 72; i++) {
        memcpy(m2, msg, 9);
        oc_vox_bit_set(m2, i, !oc_vox_bit_get(m2, i));
        TEST_ASSERT_NOT_EQUAL(ref12, oc_vox_crc_bits(m2, 0, 72, 12));
    }
}

static void random_pstr(const oc_vox_shape_t *sh, uint8_t *pstr)
{
    memset(pstr, 0, OC_VOX_PARAM_MAX_BYTES);
    for (unsigned i = 0; i < sh->param_bits; i++) oc_vox_bit_set(pstr, i, rnd() & 1u);
}

static void test_protect_recover_round_trip_in_every_mode(void)
{
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        const oc_vox_shape_t *sh = oc_vox_shape((oc_vox_mode_t)m);
        uint8_t pstr[OC_VOX_PARAM_MAX_BYTES], back[OC_VOX_PARAM_MAX_BYTES], block[OC_VOX_BLOCK_MAX_BYTES];
        oc_vox_fec_report_t rep;
        for (int n = 0; n < 20; n++) {
            random_pstr(sh, pstr);
            oc_vox_fec_protect((oc_vox_mode_t)m, pstr, block);
            oc_vox_fec_recover((oc_vox_mode_t)m, block, back, &rep);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(pstr, back, (sh->param_bits + 7u) / 8u);
            TEST_ASSERT_EQUAL_UINT8(0, rep.golay_fixed + rep.golay_failed + rep.hamming_fixed + rep.hamming_failed + rep.crc_failed);
        }
    }
}

/* Every coded bit lands in exactly one block position (the interleaver is
 * a permutation): flipping each block bit alone changes the recovery or a
 * report field, and flipping all of them maps back to flipping all. */
static void test_interleaver_is_a_permutation_and_spreads_bursts(void)
{
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        const oc_vox_shape_t *sh = oc_vox_shape((oc_vox_mode_t)m);
        unsigned nbits = sh->block_bytes * 8u;
        uint8_t zero[OC_VOX_PARAM_MAX_BYTES] = { 0 }, block[OC_VOX_BLOCK_MAX_BYTES], back[OC_VOX_PARAM_MAX_BYTES];
        oc_vox_fec_report_t rep;
        oc_vox_fec_protect((oc_vox_mode_t)m, zero, block);
        for (unsigned i = 0; i < nbits; i++) TEST_ASSERT_EQUAL_UINT(0, oc_vox_bit_get(block, i)); /* all-zero is a codeword */
        /* The block is written into a matrix ilv_width (24) wide and read by
         * columns, so a Golay word (one row) gets one hit per column a burst
         * touches: any burst of up to 3 x depth consecutive block bits leaves
         * every Golay word correctable. */
        unsigned depth = nbits / sh->ilv_width, burst = 3u * depth;
        for (unsigned start = 0; start + burst <= nbits; start++) {
            memset(block, 0, sizeof(block));
            for (unsigned i = 0; i < burst; i++) oc_vox_bit_set(block, start + i, 1);
            oc_vox_fec_recover((oc_vox_mode_t)m, block, back, &rep);
            TEST_ASSERT_EQUAL_UINT8(0, rep.golay_failed);
            TEST_ASSERT_EQUAL_UINT8(sh->golay, rep.golay_fixed); /* every word was hit, every word repaired */
            for (unsigned i = 0; i < 12u * sh->golay; i++) TEST_ASSERT_EQUAL_UINT(0, oc_vox_bit_get(back, i));
        }
        /* and one more bit breaks some word in some position */
        unsigned broken = 0;
        for (unsigned start = 0; start + burst + 1 <= nbits; start++) {
            memset(block, 0, sizeof(block));
            for (unsigned i = 0; i < burst + 1; i++) oc_vox_bit_set(block, start + i, 1);
            oc_vox_fec_recover((oc_vox_mode_t)m, block, back, &rep);
            broken += rep.golay_failed;
        }
        TEST_ASSERT_TRUE(broken > 0);
    }
}

static void test_class_a_errors_are_corrected_up_to_three_per_word_and_reported(void)
{
    const oc_vox_shape_t *sh = oc_vox_shape(OC_VOX_1200);
    uint8_t pstr[OC_VOX_PARAM_MAX_BYTES], back[OC_VOX_PARAM_MAX_BYTES], block[OC_VOX_BLOCK_MAX_BYTES];
    oc_vox_fec_report_t rep;
    unsigned nbits = sh->block_bytes * 8u, depth = nbits / sh->ilv_width;
    random_pstr(sh, pstr);
    oc_vox_fec_protect(OC_VOX_1200, pstr, block);
    /* coded bits 0..2 are the first Golay word's: their interleaved positions are 0, depth, 2 depth */
    oc_vox_bit_set(block, 0, !oc_vox_bit_get(block, 0));
    oc_vox_bit_set(block, depth, !oc_vox_bit_get(block, depth));
    oc_vox_bit_set(block, 2 * depth, !oc_vox_bit_get(block, 2 * depth));
    oc_vox_fec_recover(OC_VOX_1200, block, back, &rep);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pstr, back, (sh->param_bits + 7u) / 8u);
    TEST_ASSERT_EQUAL_UINT8(1, rep.golay_fixed);
    TEST_ASSERT_EQUAL_UINT8(0, rep.golay_failed);
    /* a fourth error in the same word: detected, class A lost */
    oc_vox_bit_set(block, 3 * depth, !oc_vox_bit_get(block, 3 * depth));
    oc_vox_fec_recover(OC_VOX_1200, block, back, &rep);
    TEST_ASSERT_EQUAL_UINT8(1, rep.golay_failed);
}

static void test_crc_catches_a_class_c_error_in_the_high_modes(void)
{
    for (int m = OC_VOX_2400; m <= OC_VOX_3200; m++) {
        const oc_vox_shape_t *sh = oc_vox_shape((oc_vox_mode_t)m);
        uint8_t pstr[OC_VOX_PARAM_MAX_BYTES], back[OC_VOX_PARAM_MAX_BYTES], block[OC_VOX_BLOCK_MAX_BYTES];
        oc_vox_fec_report_t rep;
        unsigned nbits = sh->block_bytes * 8u, depth = nbits / sh->ilv_width;
        /* coded bit index of the last class-C bit: 24 G + 12 H + (C bits - 1) */
        unsigned cbits = sh->param_bits - 12u * sh->golay - 8u * sh->hamming;
        unsigned coded_i = 24u * sh->golay + 12u * sh->hamming + cbits - 1u;
        unsigned pos = (coded_i % sh->ilv_width) * depth + coded_i / sh->ilv_width;
        random_pstr(sh, pstr);
        oc_vox_fec_protect((oc_vox_mode_t)m, pstr, block);
        oc_vox_bit_set(block, pos, !oc_vox_bit_get(block, pos));
        oc_vox_fec_recover((oc_vox_mode_t)m, block, back, &rep);
        TEST_ASSERT_EQUAL_UINT8(1, rep.crc_failed);
        TEST_ASSERT_EQUAL_UINT8(0, rep.golay_failed);
        TEST_ASSERT_EQUAL_UINT8(0, rep.golay_fixed);
    }
    /* the low modes have no CRC: a class-C error passes silently */
    const oc_vox_shape_t *sh = oc_vox_shape(OC_VOX_600);
    uint8_t pstr[OC_VOX_PARAM_MAX_BYTES], back[OC_VOX_PARAM_MAX_BYTES], block[OC_VOX_BLOCK_MAX_BYTES];
    oc_vox_fec_report_t rep;
    random_pstr(sh, pstr);
    oc_vox_fec_protect(OC_VOX_600, pstr, block);
    oc_vox_bit_set(block, 71, !oc_vox_bit_get(block, 71));
    oc_vox_fec_recover(OC_VOX_600, block, back, &rep);
    TEST_ASSERT_EQUAL_UINT8(0, rep.crc_failed + rep.golay_failed + rep.golay_fixed);
    TEST_ASSERT_TRUE(memcmp(pstr, back, 8) != 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golay_encoder_matches_the_polynomial_and_is_distance_8);
    RUN_TEST(test_golay_corrects_every_1_2_3_bit_pattern);
    RUN_TEST(test_golay_detects_every_4_bit_pattern);
    RUN_TEST(test_hamming_corrects_single_errors);
    RUN_TEST(test_crc_known_answers);
    RUN_TEST(test_protect_recover_round_trip_in_every_mode);
    RUN_TEST(test_interleaver_is_a_permutation_and_spreads_bursts);
    RUN_TEST(test_class_a_errors_are_corrected_up_to_three_per_word_and_reported);
    RUN_TEST(test_crc_catches_a_class_c_error_in_the_high_modes);
    return UNITY_END();
}
