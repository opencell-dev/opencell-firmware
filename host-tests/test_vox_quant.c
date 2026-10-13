#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "oc_vox.h"
#include "oc_vox_bits.h"
#include "oc_vox_conceal.h"
#include "oc_vox_quant.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t xs = 0x1234ABCDu;
static uint32_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

static void test_pitch_table_spans_50_to_410_hz_in_84_steps_an_octave(void)
{
    TEST_ASSERT_EQUAL_INT32(800, oc_vox_pitch_hz_x16(0));
    TEST_ASSERT_EQUAL_INT32(1600, oc_vox_pitch_hz_x16(84));
    TEST_ASSERT_EQUAL_INT32(3200, oc_vox_pitch_hz_x16(168));
    TEST_ASSERT_INT32_WITHIN(20, 410 * 16, oc_vox_pitch_hz_x16(255));
    for (int p = 1; p < 256; p++) {
        TEST_ASSERT_TRUE(oc_vox_pitch_hz_x16((uint8_t)p) > oc_vox_pitch_hz_x16((uint8_t)(p - 1)));
        TEST_ASSERT_EQUAL_UINT8(p, oc_vox_pitch_index(oc_vox_pitch_hz_x16((uint8_t)p)));
    }
    TEST_ASSERT_EQUAL_UINT8(0, oc_vox_pitch_index(100));
    TEST_ASSERT_EQUAL_UINT8(255, oc_vox_pitch_index(100000));
    /* 100 Hz is index 84; 99 Hz rounds to 83 or 84 */
    TEST_ASSERT_TRUE(oc_vox_pitch_index(99 * 16) >= 82 && oc_vox_pitch_index(99 * 16) <= 84);
}

static void test_voicing_class_and_mask(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, oc_vox_vclass_of_mask(0x0));
    TEST_ASSERT_EQUAL_UINT8(1, oc_vox_vclass_of_mask(0x1));
    TEST_ASSERT_EQUAL_UINT8(2, oc_vox_vclass_of_mask(0x3));
    TEST_ASSERT_EQUAL_UINT8(2, oc_vox_vclass_of_mask(0x2));
    TEST_ASSERT_EQUAL_UINT8(3, oc_vox_vclass_of_mask(0xF));
    TEST_ASSERT_EQUAL_UINT8(3, oc_vox_vclass_of_mask(0x4));
    for (unsigned c = 0; c < 4; c++) TEST_ASSERT_EQUAL_UINT8(c, oc_vox_vclass_of_mask(oc_vox_vmask_of_class((uint8_t)c)));
}

static void test_dct_round_trip_is_within_one_unit(void)
{
    for (int n = 0; n < 200; n++) {
        int8_t s[12], back[12];
        int32_t c[12], sum = 0;
        for (int b = 0; b < 12; b++) { s[b] = (int8_t)((int)(rnd() % 81) - 40); sum += s[b]; }
        for (int b = 0; b < 12; b++) s[b] = (int8_t)(s[b] - sum / 12); /* mean removed, as the encoder does */
        oc_vox_env_dct(s, c);
        TEST_ASSERT_EQUAL_INT32(0, c[0]);
        oc_vox_env_idct(c, back);
        int mean = 0;
        for (int b = 0; b < 12; b++) mean += s[b];
        for (int b = 0; b < 12; b++) TEST_ASSERT_INT_WITHIN(1 + abs(mean), s[b], back[b]); /* the residual mean is not coded */
    }
}

static void test_delta_tables_are_sorted_and_hold_zero(void)
{
    for (unsigned bits = 2; bits <= 4; bits++) {
        const int16_t *t = oc_vox_e_delta_table(bits);
        for (unsigned i = 1; i < (1u << bits); i++) TEST_ASSERT_TRUE(t[i] > t[i - 1]);
        if (bits >= 3) TEST_ASSERT_EQUAL_UINT(bits == 3 ? 4 : 7, oc_vox_delta_index(t, bits, 0));
        TEST_ASSERT_EQUAL_UINT(0, oc_vox_delta_index(t, bits, -1000));
        TEST_ASSERT_EQUAL_UINT((1u << bits) - 1u, oc_vox_delta_index(t, bits, 1000));
    }
    for (unsigned bits = 3; bits <= 5; bits++) {
        const int16_t *t = oc_vox_p_delta_table(bits);
        for (unsigned i = 1; i < (1u << bits); i++) TEST_ASSERT_TRUE(t[i] > t[i - 1]);
        TEST_ASSERT_EQUAL_INT16(0, t[oc_vox_delta_index(t, bits, 0)]);
        TEST_ASSERT_TRUE(t[(1u << bits) - 1u] >= 84); /* an octave up is reachable in one step */
    }
}

static void smooth_env(int8_t s[12])
{
    /* a two-formant-ish log envelope plus tilt, mean removed */
    int tilt = (int)(rnd() % 41) - 20, f1 = (int)(rnd() % 5) + 1, f2 = (int)(rnd() % 5) + 6, sum = 0;
    for (int b = 0; b < 12; b++) {
        int v = -tilt * (b - 6) / 6;
        v += 24 - 4 * abs(b - f1) * abs(b - f1);
        v += 16 - 3 * abs(b - f2) * abs(b - f2);
        if (v < -60) v = -60;
        s[b] = (int8_t)v; sum += v;
    }
    for (int b = 0; b < 12; b++) s[b] = (int8_t)(s[b] - sum / 12);
}

static oc_vox_pframe_t random_frame(void)
{
    oc_vox_pframe_t p;
    memset(&p, 0, sizeof(p));
    p.e = (uint8_t)(40 + rnd() % 80);
    p.p = (uint8_t)(rnd() % 256);
    p.v = (uint8_t)(rnd() % 16);
    p.vmix = (uint8_t)(rnd() % 8);
    p.emid = (int8_t)((int)(rnd() % 33) - 16);
    smooth_env(p.s);
    return p;
}

static double env_rms_err(const int8_t *a, const int8_t *b)
{
    double acc = 0;
    for (int i = 0; i < 12; i++) acc += (double)(a[i] - b[i]) * (a[i] - b[i]);
    return acc / 12.0;
}

/* The envelope error falls as the modes spend more bits on it (stage 1 only
 * in OVX-600, four stages in OVX-3200), and is within reason at both ends. */
static void test_envelope_error_falls_with_the_stages(void)
{
    double err[OC_VOX_MODE_COUNT] = { 0 };
    for (int n = 0; n < 100; n++) {
        oc_vox_pframe_t in[6], recon[6];
        oc_vox_qblock_t q;
        for (int f = 0; f < 6; f++) in[f] = random_frame();
        for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
            oc_vox_quant_block((oc_vox_mode_t)m, in, NULL, 0, &q, recon);
            /* frame 1 is transmitted in every mode */
            err[m] += env_rms_err(in[1].s, recon[1].s);
        }
    }
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) err[m] /= 100.0;
    TEST_ASSERT_TRUE_MESSAGE(err[OC_VOX_600] < 7.0 * 7.0, "stage 1 alone: under 7 units (5 dB) rms");
    TEST_ASSERT_TRUE_MESSAGE(err[OC_VOX_3200] < 3.5 * 3.5, "four stages: under 3.5 units rms");
    TEST_ASSERT_TRUE(err[OC_VOX_1200] < err[OC_VOX_600]);
    TEST_ASSERT_TRUE(err[OC_VOX_2400] < err[OC_VOX_1200]);
    TEST_ASSERT_TRUE(err[OC_VOX_3200] < err[OC_VOX_2400]);
}

/* What the encoder reconstructs is what the decoder reconstructs from the
 * packed bits, in every mode, with and without a previous block. */
static void test_encoder_and_decoder_reconstruct_the_same_frames(void)
{
    oc_vox_pframe_t prev = random_frame();
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        for (int n = 0; n < 30; n++) {
            oc_vox_pframe_t in[6], recon[6], out[6];
            oc_vox_qblock_t q, q2;
            uint8_t pstr[OC_VOX_PARAM_MAX_BYTES], toggle = 0;
            const oc_vox_pframe_t *pp = (n & 1) ? &prev : NULL;
            for (int f = 0; f < 6; f++) in[f] = random_frame();
            oc_vox_quant_block((oc_vox_mode_t)m, in, pp, (uint8_t)(n & 1), &q, recon);
            oc_vox_bits_pack((oc_vox_mode_t)m, &q, pstr);
            oc_vox_bits_unpack((oc_vox_mode_t)m, pstr, &q2);
            oc_vox_dequant_block((oc_vox_mode_t)m, &q2, pp, 0, out, &toggle);
            TEST_ASSERT_EQUAL_UINT8(n & 1, toggle);
            TEST_ASSERT_EQUAL_MEMORY(recon, out, sizeof(recon));
        }
    }
}

static void test_transmitted_values_track_the_input_within_their_steps(void)
{
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        const oc_vox_shape_t *sh = oc_vox_shape((oc_vox_mode_t)m);
        oc_vox_pframe_t in[6], recon[6];
        oc_vox_qblock_t q;
        for (int f = 0; f < 6; f++) {
            in[f] = random_frame();
            in[f].e = (uint8_t)(60 + 2 * f);  /* gentle contours the delta tables can follow */
            in[f].p = (uint8_t)(120 + 3 * f);
        }
        oc_vox_quant_block((oc_vox_mode_t)m, in, NULL, 0, &q, recon);
        for (unsigned i = 0; i < sh->n; i++) {
            unsigned f = sh->f[i].frame;
            switch (sh->f[i].kind) {
            case OC_VOX_F_E_ABS: TEST_ASSERT_INT_WITHIN(1 << (7 - sh->f[i].bits), in[f].e, recon[f].e); break;
            case OC_VOX_F_E_DEL: TEST_ASSERT_INT_WITHIN(sh->f[i].bits == 2 ? 4 : 2, in[f].e, recon[f].e); break;
            case OC_VOX_F_P_ABS: TEST_ASSERT_INT_WITHIN(1 << (8 - sh->f[i].bits), in[f].p, recon[f].p); break;
            case OC_VOX_F_P_DEL: TEST_ASSERT_INT_WITHIN(sh->f[i].bits == 3 ? 7 : 2, in[f].p, recon[f].p); break;
            case OC_VOX_F_V_MSK: TEST_ASSERT_EQUAL_UINT8(in[f].v, recon[f].v); break;
            case OC_VOX_F_V_CLS: TEST_ASSERT_EQUAL_UINT8(oc_vox_vmask_of_class(oc_vox_vclass_of_mask(in[f].v)), recon[f].v); break;
            case OC_VOX_F_E_MID: TEST_ASSERT_INT_WITHIN(8, in[f].emid, recon[f].emid); break;
            default: break;
            }
        }
    }
}

/* OVX-600 transmits frames 1, 3, 5: frame 0 is the mean of the previous
 * block's last frame and frame 1, frames 2 and 4 the means of their
 * neighbours; voicing takes the next transmitted frame's. */
static void test_untransmitted_frames_are_interpolated(void)
{
    oc_vox_pframe_t in[6], recon[6], prev;
    oc_vox_qblock_t q;
    memset(in, 0, sizeof(in));
    memset(&prev, 0, sizeof(prev));
    prev.e = 40; prev.p = 100; prev.v = 0; prev.vmix = 3;
    for (int f = 0; f < 6; f++) { in[f].e = (uint8_t)(60 + 10 * f); in[f].p = (uint8_t)(100 + 10 * f); in[f].v = f >= 3 ? 0xF : 0; in[f].vmix = 5; }
    oc_vox_quant_block(OC_VOX_600, in, &prev, 1, &q, recon);
    TEST_ASSERT_INT_WITHIN(1, (prev.e + recon[1].e) / 2, recon[0].e);
    TEST_ASSERT_INT_WITHIN(1, (recon[1].e + recon[3].e) / 2, recon[2].e);
    TEST_ASSERT_INT_WITHIN(1, (recon[3].e + recon[5].e) / 2, recon[4].e);
    TEST_ASSERT_INT_WITHIN(1, (prev.p + recon[1].p) / 2, recon[0].p);
    TEST_ASSERT_EQUAL_UINT8(recon[1].v, recon[0].v);
    TEST_ASSERT_EQUAL_UINT8(0xF, recon[2].v); /* the onset at frame 3 starts a frame early */
    TEST_ASSERT_EQUAL_UINT8(0xF, recon[4].v);
    TEST_ASSERT_EQUAL_UINT8(3, recon[0].vmix); /* OVX-600 sends no vmix: the previous block's holds */
    /* without a previous block, frame 0 takes frame 1's */
    oc_vox_quant_block(OC_VOX_600, in, NULL, 1, &q, recon);
    TEST_ASSERT_EQUAL_UINT8(recon[1].e, recon[0].e);
    TEST_ASSERT_EQUAL_UINT8(recon[1].p, recon[0].p);
}

static void test_dropping_the_fine_stages_keeps_the_coarse_envelope(void)
{
    oc_vox_pframe_t in[6], full[6], coarse[6];
    oc_vox_qblock_t q;
    uint8_t t;
    for (int f = 0; f < 6; f++) in[f] = random_frame();
    oc_vox_quant_block(OC_VOX_3200, in, NULL, 0, &q, full);
    oc_vox_dequant_block(OC_VOX_3200, &q, NULL, OC_VOX_DQ_DROP_FINE, coarse, &t);
    for (int f = 0; f < 6; f++) {
        TEST_ASSERT_EQUAL_UINT8(full[f].e, coarse[f].e);
        TEST_ASSERT_EQUAL_UINT8(full[f].p, coarse[f].p);
        TEST_ASSERT_EQUAL_UINT8(full[f].v, coarse[f].v);
        TEST_ASSERT_EQUAL_INT8(0, coarse[f].emid);
        TEST_ASSERT_TRUE(env_rms_err(in[f].s, coarse[f].s) < 8.0 * 8.0);
    }
}

static void test_concealment_decays_and_mutes_then_recovers_with_a_ramp(void)
{
    oc_vox_conceal_t c;
    oc_vox_pframe_t good[6], out[6];
    oc_vox_conceal_init(&c);
    /* nothing heard yet: silence */
    oc_vox_conceal_block(&c, out);
    for (int f = 0; f < 6; f++) TEST_ASSERT_EQUAL_UINT8(0, out[f].e);
    oc_vox_conceal_init(&c);
    for (int f = 0; f < 6; f++) { good[f] = random_frame(); good[f].e = 100; good[f].p = 120; good[f].v = 0xF; good[f].vmix = 1; }
    oc_vox_conceal_recover(&c, good);
    TEST_ASSERT_EQUAL_UINT8(100, good[0].e); /* no loss before: untouched */
    oc_vox_conceal_block(&c, out);
    TEST_ASSERT_EQUAL_UINT8(100, out[0].e);           /* held */
    TEST_ASSERT_EQUAL_UINT8(96, out[1].e);            /* -3 dB a frame */
    TEST_ASSERT_EQUAL_UINT8(80, out[5].e);
    TEST_ASSERT_UINT8_WITHIN(6, 120, out[5].p);       /* pitch held, give or take the walk */
    TEST_ASSERT_EQUAL_UINT8(0xF, out[2].v);
    TEST_ASSERT_EQUAL_UINT8(0x7, out[3].v);           /* a band off from the top after three frames */
    TEST_ASSERT_EQUAL_UINT8(0x1, out[5].v);
    TEST_ASSERT_TRUE(out[5].vmix > 1);
    TEST_ASSERT_EQUAL_INT8(good[5].s[11] - 6, out[5].s[11]); /* the top tilts down a unit a frame */
    TEST_ASSERT_EQUAL_INT8(good[5].s[0], out[5].s[0]);
    oc_vox_conceal_block(&c, out);                    /* frames 7..12: -6 dB a frame, then silence at 12 */
    TEST_ASSERT_EQUAL_UINT8(72, out[0].e);
    TEST_ASSERT_EQUAL_UINT8(0, out[5].e);
    TEST_ASSERT_EQUAL_UINT8(0, out[5].v);
    /* recovery: a loud return is capped to +6 dB a frame for two frames */
    for (int f = 0; f < 6; f++) good[f].e = 110;
    oc_vox_conceal_recover(&c, good);
    TEST_ASSERT_EQUAL_UINT8(8, good[0].e);
    TEST_ASSERT_EQUAL_UINT8(16, good[1].e);
    TEST_ASSERT_EQUAL_UINT8(110, good[2].e);
    TEST_ASSERT_EQUAL_UINT8(0, c.lost_frames);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pitch_table_spans_50_to_410_hz_in_84_steps_an_octave);
    RUN_TEST(test_voicing_class_and_mask);
    RUN_TEST(test_dct_round_trip_is_within_one_unit);
    RUN_TEST(test_delta_tables_are_sorted_and_hold_zero);
    RUN_TEST(test_envelope_error_falls_with_the_stages);
    RUN_TEST(test_encoder_and_decoder_reconstruct_the_same_frames);
    RUN_TEST(test_transmitted_values_track_the_input_within_their_steps);
    RUN_TEST(test_untransmitted_frames_are_interpolated);
    RUN_TEST(test_dropping_the_fine_stages_keeps_the_coarse_envelope);
    RUN_TEST(test_concealment_decays_and_mutes_then_recovers_with_a_ramp);
    return UNITY_END();
}
