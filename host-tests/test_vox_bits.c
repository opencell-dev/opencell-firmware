#include "unity.h"

#include <string.h>

#include "oc_vox.h"
#include "oc_vox_bits.h"
#include "oc_vox_int.h"

void setUp(void) {}
void tearDown(void) {}

/* Each mode's parameter string plus its FEC overhead is exactly the block
 * (PLAN.md §1.3); classes A and B fit inside the string. */
static void test_every_mode_sums_to_its_block(void)
{
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        const oc_vox_shape_t *sh = oc_vox_shape((oc_vox_mode_t)m);
        TEST_ASSERT_NOT_NULL(sh);
        unsigned sum = 0;
        for (unsigned i = 0; i < sh->n; i++) {
            TEST_ASSERT_TRUE(sh->f[i].kind < OC_VOX_F_COUNT);
            TEST_ASSERT_TRUE(sh->f[i].frame < OC_VOX_FRAMES);
            TEST_ASSERT_TRUE(sh->f[i].bits >= 1 && sh->f[i].bits <= 10);
            sum += sh->f[i].bits;
        }
        TEST_ASSERT_EQUAL_UINT(sh->param_bits, sum);
        unsigned fec = 12u * sh->golay + 4u * sh->hamming + sh->crc_bits;
        TEST_ASSERT_EQUAL_UINT(oc_vox_block_bits((oc_vox_mode_t)m), sum + fec);
        TEST_ASSERT_EQUAL_UINT8(oc_vox_block_bytes((oc_vox_mode_t)m), sh->block_bytes);
        TEST_ASSERT_TRUE(12u * sh->golay + 8u * sh->hamming <= sum);
        TEST_ASSERT_EQUAL_UINT(0, oc_vox_block_bits((oc_vox_mode_t)m) % sh->ilv_width);
        TEST_ASSERT_TRUE(sh->n <= OC_VOX_MAX_FIELDS);
        TEST_ASSERT_TRUE((sum + 7u) / 8u <= OC_VOX_PARAM_MAX_BYTES);
        /* the toggle leads, then the first frame's absolute energy */
        TEST_ASSERT_EQUAL_UINT8(OC_VOX_F_T, sh->f[0].kind);
        TEST_ASSERT_EQUAL_UINT8(OC_VOX_F_E_ABS, sh->f[1].kind);
    }
    TEST_ASSERT_NULL(oc_vox_shape((oc_vox_mode_t)9));
}

static void test_bit_writer_and_reader_round_trip_and_bound(void)
{
    uint8_t buf[4];
    oc_vox_bw_t w;
    oc_vox_br_t r;
    oc_vox_bw_init(&w, buf, 29);
    oc_vox_bw_put(&w, 0x5, 3);
    oc_vox_bw_put(&w, 0xABCD, 16);
    oc_vox_bw_put(&w, 0x3FF, 10); /* 29 bits: full */
    TEST_ASSERT_FALSE(oc_vox_bw_overrun(&w));
    oc_vox_bw_put(&w, 1, 1);
    TEST_ASSERT_TRUE(oc_vox_bw_overrun(&w));
    TEST_ASSERT_EQUAL_HEX8(0xB5, buf[0]); /* 101 10101 */
    TEST_ASSERT_EQUAL_HEX8(0x79, buf[1]); /* 0111 1001 */
    TEST_ASSERT_EQUAL_HEX8(0xBF, buf[2]); /* 101 11111: the 0xABCD tail, then five ones */
    TEST_ASSERT_EQUAL_HEX8(0xF8, buf[3]); /* 11111 000: the 30th bit was not written */
    oc_vox_br_init(&r, buf, 29);
    TEST_ASSERT_EQUAL_HEX32(0x5, oc_vox_br_get(&r, 3));
    TEST_ASSERT_EQUAL_HEX32(0xABCD, oc_vox_br_get(&r, 16));
    TEST_ASSERT_EQUAL_HEX32(0x3FF, oc_vox_br_get(&r, 10));
    TEST_ASSERT_FALSE(r.overrun);
    TEST_ASSERT_EQUAL_HEX32(0, oc_vox_br_get(&r, 4));
    TEST_ASSERT_TRUE(r.overrun);
}

static uint32_t xs = 0x9E3779B9u;
static uint32_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

static void test_pack_unpack_round_trip_masks_wide_values(void)
{
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        const oc_vox_shape_t *sh = oc_vox_shape((oc_vox_mode_t)m);
        oc_vox_qblock_t in, out;
        uint8_t pstr[OC_VOX_PARAM_MAX_BYTES];
        memset(&in, 0, sizeof(in));
        for (unsigned i = 0; i < sh->n; i++) in.v[i] = (uint16_t)rnd(); /* wider than the field */
        oc_vox_bits_pack((oc_vox_mode_t)m, &in, pstr);
        oc_vox_bits_unpack((oc_vox_mode_t)m, pstr, &out);
        for (unsigned i = 0; i < sh->n; i++) TEST_ASSERT_EQUAL_UINT16(in.v[i] & ((1u << sh->f[i].bits) - 1u), out.v[i]);
        for (unsigned i = sh->n; i < OC_VOX_MAX_FIELDS; i++) TEST_ASSERT_EQUAL_UINT16(0, out.v[i]);
        /* the tail past param_bits is zero */
        for (unsigned b = sh->param_bits; b < 8u * ((sh->param_bits + 7u) / 8u); b++) TEST_ASSERT_EQUAL_UINT(0, oc_vox_bit_get(pstr, b));
    }
}

/* Field i = (7 i + 3) masked to its width, written in layout order: the
 * bytes come from tools/vox/gen_layout.py --golden, not from this code. */
static void golden(oc_vox_mode_t m, const uint8_t *want, size_t n)
{
    const oc_vox_shape_t *sh = oc_vox_shape(m);
    oc_vox_qblock_t q;
    uint8_t pstr[OC_VOX_PARAM_MAX_BYTES];
    memset(&q, 0, sizeof(q));
    for (unsigned i = 0; i < sh->n; i++) q.v[i] = (uint16_t)((7u * i + 3u) & ((1u << sh->f[i].bits) - 1u));
    oc_vox_bits_pack(m, &q, pstr);
    TEST_ASSERT_EQUAL_size_t(n, (sh->param_bits + 7u) / 8u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, pstr, n);
}

static void test_golden_parameter_strings(void)
{
    static const uint8_t g600[] = { 0x94, 0x98, 0x07, 0xe6, 0x39, 0x14, 0x05, 0x70 };
    static const uint8_t g1200[] = { 0x94, 0x26, 0x01, 0xfd, 0x63, 0x52, 0x0f, 0xcb, 0x87, 0x10, 0x24, 0x41, 0xe5, 0x9d };
    static const uint8_t g3200[] = { 0x8a, 0x11, 0x80, 0x7d, 0xb5, 0x2c, 0x93, 0x0b, 0xf8, 0xb8, 0x74, 0x31, 0xe4, 0xe2,
                                     0xe4, 0xc0, 0x31, 0xcc, 0xe3, 0x57, 0x73, 0x8f, 0xab, 0xc7, 0xe3, 0xfc, 0x18, 0x34,
                                     0x50, 0x6c, 0x88, 0xa4, 0x7d, 0x63, 0x5a, 0x61, 0x68, 0x6f, 0x76, 0x7d };
    golden(OC_VOX_600, g600, sizeof(g600));
    golden(OC_VOX_1200, g1200, sizeof(g1200));
    golden(OC_VOX_3200, g3200, sizeof(g3200));
    TEST_ASSERT_EQUAL_UINT8(13, oc_vox_shape(OC_VOX_600)->n);
    TEST_ASSERT_EQUAL_UINT8(23, oc_vox_shape(OC_VOX_1200)->n);
    TEST_ASSERT_EQUAL_UINT8(55, oc_vox_shape(OC_VOX_3200)->n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_mode_sums_to_its_block);
    RUN_TEST(test_bit_writer_and_reader_round_trip_and_bound);
    RUN_TEST(test_pack_unpack_round_trip_masks_wide_values);
    RUN_TEST(test_golden_parameter_strings);
    return UNITY_END();
}
