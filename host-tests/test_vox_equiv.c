/* The production (fixed-point) codec against the reference on the shared
 * vectors (PLAN.md §1.6): the same bitstream layers by construction, the
 * encoders' indices in agreement, the decoders' outputs alike, and the
 * production decoder's output pinned by golden hashes. */
#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oc_vox.h"
#include "oc_vox_quant.h"
#include "oc_vox_ref.h"
#include "vox_vectors.h"

void setUp(void) {}
void tearDown(void) {}

static vox_vector_t vec, rean;
static oc_vox_pframe_t fr_ref[VOX_MAX_FRAMES], fr_fx[VOX_MAX_FRAMES];
static int16_t out_ref[VOX_MAX_SAMPLES], out_fx[VOX_MAX_SAMPLES];
static uint8_t blocks_ref[VOX_MAX_SAMPLES / 960][OC_VOX_BLOCK_MAX_BYTES];

static const char *VECTORS[3] = { "synth_vowels", "synth_mixed", "chirp" };

static uint64_t fnv1a(const int16_t *x, unsigned n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < n; i++) {
        h ^= (uint8_t)x[i]; h *= 0x100000001b3ull;
        h ^= (uint8_t)((uint16_t)x[i] >> 8); h *= 0x100000001b3ull;
    }
    return h;
}

static double frame_rms(const int16_t *x) { double a = 0; for (int i = 0; i < 160; i++) a += (double)x[i] * x[i]; return sqrt(a / 160); }

/* Both analyses on the same input, compared in the parameter domain on
 * frames above -81 dBFS (e >= 20 in both): the rest is noise-floor
 * arithmetic nobody hears. Pitch is compared where both hear band 0 voiced. */
static void test_encoders_agree_on_the_parameters(void)
{
    unsigned n = 0, e_same = 0, e_1 = 0, p_n = 0, p_same = 0, v_same = 0, v0_same = 0, mix_1 = 0, mid_3 = 0;
    unsigned s_n = 0, s_same = 0, s_1 = 0;
    for (int vi = 0; vi < 3; vi++) {
        oc_vox_ref_enc_t er;
        oc_vox_enc_t ex;
        unsigned nb, b, i;
        TEST_ASSERT_EQUAL_INT(0, vox_vector_load(VECTORS[vi], &vec));
        nb = vec.n / 960;
        oc_vox_ref_enc_init(&er, OC_VOX_1200);
        oc_vox_enc_init(&ex, OC_VOX_1200);
        for (b = 0; b < nb; b++) {
            oc_vox_ref_analyse_block(&er, vec.pcm + 960 * b, &fr_ref[6 * b]);
            oc_vox_analyse_block(&ex, vec.pcm + 960 * b, &fr_fx[6 * b]);
        }
        for (i = 6; i < nb * 6; i++) {
            const oc_vox_pframe_t *r = &fr_ref[i], *x = &fr_fx[i];
            int de = abs((int)r->e - (int)x->e);
            if (r->e < 20 || x->e < 20) continue;
            n++;
            e_same += de == 0; e_1 += de <= 1;
            if ((r->v & 1u) && (x->v & 1u)) { p_n++; p_same += r->p == x->p; }
            v_same += r->v == x->v;
            v0_same += (r->v & 1u) == (x->v & 1u);
            mix_1 += abs((int)r->vmix - (int)x->vmix) <= 2; /* the levels are 0, 1, 2, 3, 4, 6: a threshold flip is two */
            /* within the 3-bit table's finest step, except where a half-frame of digital
             * silence puts one side at the clamp: the float filter's tail against integer zero */
            if (abs((int)r->emid) == 48 || abs((int)x->emid) == 48) mid_3++;
            else mid_3 += abs((int)r->emid - (int)x->emid) <= 4;
            for (int k = 0; k < 12; k++) {
                int ds = abs((int)r->s[k] - (int)x->s[k]);
                s_n++; s_same += ds == 0; s_1 += ds <= 1;
            }
        }
    }
    {
        char msg[300];
        snprintf(msg, sizeof(msg), "%u frames: e same %.1f%% within 1 %.1f%% | p same %.1f%% of %u | v same %.1f%% band0 %.1f%% | vmix within 2 %.1f%% | emid within 4 %.1f%% | s same %.1f%% within 1 %.2f%%",
                 n, 100.0 * e_same / n, 100.0 * e_1 / n, 100.0 * p_same / p_n, p_n, 100.0 * v_same / n, 100.0 * v0_same / n, 100.0 * mix_1 / n, 100.0 * mid_3 / n, 100.0 * s_same / s_n, 100.0 * s_1 / s_n);
        TEST_MESSAGE(msg);
    }
    TEST_ASSERT_TRUE(n > 400 && p_n > 250);
    TEST_ASSERT_TRUE_MESSAGE(e_same * 100 >= n * 97, "energy index identical on 97 % of frames");
    TEST_ASSERT_TRUE_MESSAGE(e_1 * 1000 >= n * 995, "energy index within one step on 99.5 %");
    TEST_ASSERT_TRUE_MESSAGE(p_same * 100 >= p_n * 97, "pitch index identical on 97 % of voiced frames");
    TEST_ASSERT_TRUE_MESSAGE(v_same * 100 >= n * 95, "voicing mask identical on 95 %");
    TEST_ASSERT_TRUE_MESSAGE(v0_same * 100 >= n * 98, "band 0 voicing identical on 98 %");
    TEST_ASSERT_TRUE_MESSAGE(mix_1 * 100 >= n * 99, "vmix within two levels on 99 %");
    TEST_ASSERT_TRUE_MESSAGE(mid_3 * 100 >= n * 99, "emid within 4 units on 99 %");
    TEST_ASSERT_TRUE_MESSAGE(s_same * 100 >= s_n * 90, "envelope bands identical on 90 %");
    TEST_ASSERT_TRUE_MESSAGE(s_1 * 1000 >= s_n * 990, "envelope bands within one unit on 99 %");
}

/* The same (reference-encoded) bitstream through both decoders. */
static void test_decoders_produce_alike_audio(void)
{
    static const oc_vox_mode_t MODES[3] = { OC_VOX_600, OC_VOX_1200, OC_VOX_3200 };
    for (int mi = 0; mi < 3; mi++) {
        oc_vox_mode_t m = MODES[mi];
        oc_vox_ref_enc_t er;
        oc_vox_ref_dec_t dr;
        oc_vox_dec_t dx;
        oc_vox_ref_enc_t ar, ax;
        unsigned nb, b, i, n_loud = 0, rms_bad = 0, corr_bad = 0, n_voiced = 0;
        double lsd = 0, lsd99[VOX_MAX_FRAMES];
        TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
        nb = vec.n / 960;
        oc_vox_ref_enc_init(&er, m);
        oc_vox_ref_dec_init(&dr, m);
        oc_vox_dec_init(&dx, m);
        for (b = 0; b < nb; b++) {
            oc_vox_ref_encode(&er, vec.pcm + 960 * b, blocks_ref[b]);
            oc_vox_ref_decode(&dr, blocks_ref[b], out_ref + 960 * b);
            oc_vox_decode(&dx, blocks_ref[b], out_fx + 960 * b);
        }
        /* re-analyse both outputs with the reference analysis for the envelope distance */
        oc_vox_ref_enc_init(&ar, m);
        oc_vox_ref_enc_init(&ax, m);
        memcpy(rean.pcm, out_ref, vec.n * 2);
        for (b = 0; b < nb; b++) oc_vox_ref_analyse_block(&ar, rean.pcm + 960 * b, &fr_ref[6 * b]);
        memcpy(rean.pcm, out_fx, vec.n * 2);
        for (b = 0; b < nb; b++) oc_vox_ref_analyse_block(&ax, rean.pcm + 960 * b, &fr_fx[6 * b]);
        for (i = 12; i < nb * 6; i++) {
            double rr = frame_rms(out_ref + 160 * i), rx = frame_rms(out_fx + 160 * i), acc = 0, xy = 0, xx = 0, yy = 0;
            if (rr < 200.0) continue;
            n_loud++;
            if (fabs(20.0 * log10((rx + 1.0) / (rr + 1.0))) > 1.0) rms_bad++;
            for (int k = 0; k < 12; k++) acc += (double)(fr_ref[i].s[k] - fr_fx[i].s[k]) * (fr_ref[i].s[k] - fr_fx[i].s[k]);
            lsd99[n_loud - 1] = sqrt(acc / 12.0) * 0.75;
            lsd += lsd99[n_loud - 1];
            if (fr_ref[i].v & 1u) {
                n_voiced++;
                for (int k = 0; k < 160; k++) {
                    double a = out_ref[160 * i + k], c = out_fx[160 * i + k];
                    xy += a * c; xx += a * a; yy += c * c;
                }
                if (xy / sqrt(xx * yy + 1.0) < 0.8) corr_bad++;
            }
        }
        lsd /= n_loud;
        /* the 99th percentile */
        {
            unsigned over3 = 0;
            for (i = 0; i < n_loud; i++) if (lsd99[i] > 3.0) over3++;
            char msg[200];
            snprintf(msg, sizeof(msg), "%s: LSD mean %.2f dB, over 3 dB %u/%u, rms off >1 dB %u, corr<0.8 %u/%u", oc_vox_mode_name(m), lsd, over3, n_loud, rms_bad, corr_bad, n_voiced);
            TEST_MESSAGE(msg);
            TEST_ASSERT_TRUE_MESSAGE(lsd <= 1.0, "mean log-spectral distance between the decoders within 1 dB");
            TEST_ASSERT_TRUE_MESSAGE(over3 * 100 <= n_loud, "99 % of frames within 3 dB");
            TEST_ASSERT_TRUE_MESSAGE(rms_bad * 100 <= n_loud * 2, "frame loudness within 1 dB");
            TEST_ASSERT_TRUE_MESSAGE(corr_bad * 100 <= n_voiced * 5, "voiced frames correlate at 0.8 or better");
        }
    }
}

/* The production decoder's output is deterministic across hosts: pinned. */
static void test_golden_hashes_of_the_production_decoder(void)
{
    /* FNV-1a 64 of the production decoder's output, per vector and mode (OC_VOX_VERSION 0):
     * a change here is a deliberate change of the codec, reviewed and regenerated. */
    static const uint64_t GOLDEN[3][OC_VOX_MODE_COUNT] = {
        /* synth_vowels */ { 0x3cc6fe9af8f2b890ull, 0xcddf874a2a55b544ull, 0x93e1c728d4afc3b7ull, 0xa1a16b6508c0ded2ull, 0x1cd50601a9c07833ull, 0xc3bce82f286772acull, 0x4f25dbdbedfcfc6bull, 0x94bfec0a8d252d87ull, 0xec97f1cf5f442c52ull },
        /* synth_mixed */ { 0xfdb59c5ecba62412ull, 0xf7e0be869a61eee8ull, 0xda63df1214396cd3ull, 0x9eaff264d411de09ull, 0x8278a0bf08f0a49cull, 0xbbb56dd3b9a138b8ull, 0xd906dc761605e911ull, 0xc7875e9d5a566b81ull, 0xcca909f29162386bull },
        /* chirp */ { 0x01a29fa0753292a5ull, 0xb57bf2df249b1937ull, 0x256763c7ef876b55ull, 0xbb2df26da2c522ffull, 0x922522e330b4a4e4ull, 0xa63187d872491348ull, 0xbfa1a4377c88ffaaull, 0x7156b07464269507ull, 0x3b6204e8335a4663ull },
    };
    for (int vi = 0; vi < 3; vi++) {
        unsigned nb, b;
        TEST_ASSERT_EQUAL_INT(0, vox_vector_load(VECTORS[vi], &vec));
        nb = vec.n / 960;
        for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
            oc_vox_enc_t e;
            oc_vox_dec_t d;
            uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
            uint64_t h;
            char msg[120];
            oc_vox_enc_init(&e, (oc_vox_mode_t)m);
            oc_vox_dec_init(&d, (oc_vox_mode_t)m);
            for (b = 0; b < nb; b++) {
                oc_vox_encode(&e, vec.pcm + 960 * b, block);
                oc_vox_decode(&d, block, out_fx + 960 * b);
            }
            h = fnv1a(out_fx, nb * 960);
            snprintf(msg, sizeof(msg), "%s %s %016llx", VECTORS[vi], oc_vox_mode_name((oc_vox_mode_t)m), (unsigned long long)h);
            TEST_MESSAGE(msg);
            TEST_ASSERT_EQUAL_HEX64(GOLDEN[vi][m], h);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encoders_agree_on_the_parameters);
    RUN_TEST(test_decoders_produce_alike_audio);
    RUN_TEST(test_golden_hashes_of_the_production_decoder);
    return UNITY_END();
}
