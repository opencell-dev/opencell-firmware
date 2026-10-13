#include "unity.h"

#include <math.h>
#include <string.h>

#include "oc_vox_quant.h"
#include "oc_vox_ref.h"
#include "vox_vectors.h"

void setUp(void) {}
void tearDown(void) {}

static vox_vector_t vec;
static oc_vox_pframe_t frames[VOX_MAX_FRAMES];

/* Analyse a whole vector; frame i of the result is the generator's frame
 * i shifted by the analysis delay (80 samples, half a frame). */
static unsigned analyse_all(const vox_vector_t *v)
{
    oc_vox_ref_enc_t e;
    unsigned blocks = v->n / 960, b;
    oc_vox_ref_enc_init(&e, OC_VOX_1200);
    for (b = 0; b < blocks; b++) oc_vox_ref_analyse_block(&e, v->pcm + 960 * b, &frames[6 * b]);
    return blocks * 6;
}

/* The generator's f0 at analysed frame i: it covers samples 160 i - 80 ..
 * 160 i + 80, so the nearer of label frames i - 1 and i. */
static float label_f0(const vox_vector_t *v, unsigned i, uint8_t p)
{
    float a = i > 0 ? v->f0[i - 1] : 0.0f, b = i < v->frames ? v->f0[i] : 0.0f, hz = (float)oc_vox_pitch_hz_x16(p) / 16.0f;
    return fabsf(a - hz) < fabsf(b - hz) ? a : b;
}

static void test_pitch_tracks_the_vowels_without_octave_errors(void)
{
    unsigned n, i, voiced = 0, good = 0, octave = 0;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    n = analyse_all(&vec);
    for (i = 6; i < n; i++) { /* skip the first block: the DC blocker and history settle */
        float f0;
        if (i >= vec.frames || vec.kind[i] != 1 || (i > 0 && vec.kind[i - 1] != 1)) continue;
        f0 = label_f0(&vec, i, frames[i].p);
        if (f0 <= 0.0f) continue;
        voiced++;
        {
            float hz = (float)oc_vox_pitch_hz_x16(frames[i].p) / 16.0f, ratio = hz / f0;
            if (fabsf(ratio - 1.0f) < 0.03f) good++;
            if (fabsf(ratio - 2.0f) < 0.1f || fabsf(ratio - 0.5f) < 0.05f) octave++;
        }
    }
    TEST_ASSERT_TRUE(voiced > 100);
    TEST_ASSERT_TRUE_MESSAGE(good * 100 >= voiced * 90, "pitch within 3 % on at least 90 % of voiced frames");
    TEST_ASSERT_TRUE_MESSAGE(octave * 100 <= voiced * 2, "octave errors on at most 2 % of voiced frames");
}

static void test_voicing_follows_vowels_noise_and_silence(void)
{
    unsigned n, i, vow = 0, vow_v = 0, noi = 0, noi_u = 0, sil = 0, sil_q = 0;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_mixed", &vec));
    n = analyse_all(&vec);
    for (i = 6; i + 1 < n && i + 1 < vec.frames; i++) {
        /* only frames whose two label neighbours agree (the half-frame shift) */
        if (vec.kind[i] != vec.kind[i + 1] || (i > 0 && vec.kind[i - 1] != vec.kind[i])) continue;
        if (vec.kind[i] == 1) { vow++; if (frames[i].v & 1u) vow_v++; }
        if (vec.kind[i] == 2) { noi++; if (!(frames[i].v & 1u) && frames[i].e > 20) noi_u++; }
        if (vec.kind[i] == 0) { sil++; if (frames[i].e <= 8) sil_q++; }
    }
    TEST_ASSERT_TRUE(vow > 60 && noi > 8 && sil > 8);
    TEST_ASSERT_TRUE_MESSAGE(vow_v * 100 >= vow * 90, "band 0 voiced on at least 90 % of vowel frames");
    TEST_ASSERT_TRUE_MESSAGE(noi_u * 100 >= noi * 80, "band 0 unvoiced on at least 80 % of noise frames");
    TEST_ASSERT_TRUE_MESSAGE(sil_q * 100 >= sil * 90, "silence is near the floor");
}

static void test_envelope_is_mean_removed_and_follows_the_spectrum(void)
{
    unsigned n, i, hi_a = 0, hi_i = 0, cnt_a = 0, cnt_i = 0;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    n = analyse_all(&vec);
    /* vowels in order a e i o u a i u e o, 300 ms = 15 frames each: /a/ has F1 at 730 Hz (band 3),
     * /i/ has F2 at 2290 Hz (band 8): band 8 is louder than band 3 in /i/, the reverse in /a/ */
    for (i = 6; i < n; i++) {
        int sum = 0, b;
        unsigned vowel = i / 15;
        for (b = 0; b < 12; b++) sum += frames[i].s[b];
        TEST_ASSERT_INT_WITHIN(12, 0, sum); /* mean within a unit per band */
        if (i % 15 < 3 || i % 15 > 12) continue; /* the vowel's middle */
        if (vowel == 0 || vowel == 5) { cnt_a++; if (frames[i].s[3] > frames[i].s[8]) hi_a++; }
        if (vowel == 2 || vowel == 6) { cnt_i++; if (frames[i].s[8] > frames[i].s[3]) hi_i++; }
    }
    TEST_ASSERT_TRUE(cnt_a > 10 && cnt_i > 10);
    TEST_ASSERT_EQUAL_UINT(cnt_a, hi_a);
    TEST_ASSERT_EQUAL_UINT(cnt_i, hi_i);
}

static void test_chirp_is_voiced_and_tracks_until_the_pitch_range_ends(void)
{
    unsigned n, i, tracked = 0, total = 0;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("chirp", &vec));
    n = analyse_all(&vec);
    for (i = 6; i < n; i++) {
        /* f = 100 x 30^(t/2): within 100..400 Hz for t < 0.815 s (frames up to ~40) */
        float t = ((float)i * 160.0f) / 8000.0f, f = 100.0f * powf(30.0f, t / 2.0f);
        if (f > 380.0f) break;
        total++;
        if (fabsf((float)oc_vox_pitch_hz_x16(frames[i].p) / 16.0f / f - 1.0f) < 0.05f) tracked++;
    }
    TEST_ASSERT_TRUE(total > 25);
    TEST_ASSERT_TRUE_MESSAGE(tracked * 100 >= total * 85, "a sine's pitch is tracked across the range");
}

static double block_rms(const int16_t *x, unsigned n)
{
    double acc = 0;
    for (unsigned i = 0; i < n; i++) acc += (double)x[i] * x[i];
    return sqrt(acc / n);
}

static int16_t out[VOX_MAX_SAMPLES];

static void roundtrip(oc_vox_mode_t m, const vox_vector_t *v, int16_t *dst)
{
    oc_vox_ref_enc_t e;
    oc_vox_ref_dec_t d;
    uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
    unsigned blocks = v->n / 960, b;
    oc_vox_ref_enc_init(&e, m);
    oc_vox_ref_dec_init(&d, m);
    for (b = 0; b < blocks; b++) {
        oc_vox_ref_encode(&e, v->pcm + 960 * b, block);
        oc_vox_ref_decode(&d, block, dst + 960 * b);
    }
}

static void test_round_trip_keeps_the_loudness_in_every_mode(void)
{
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        unsigned blocks = vec.n / 960, b, bad = 0;
        roundtrip((oc_vox_mode_t)m, &vec, out);
        for (b = 2; b < blocks; b++) {
            double in = block_rms(vec.pcm + 960 * b, 960), o = block_rms(out + 960 * b, 960);
            if (in < 100.0) continue;
            if (fabs(20.0 * log10((o + 1.0) / (in + 1.0))) > 2.5) bad++;
        }
        /* the 40 ms modes update energy three times a block and interpolate, so onsets inside a block cost more */
        unsigned allow = m <= OC_VOX_800 ? 15 : 10;
        TEST_ASSERT_TRUE_MESSAGE(bad * 100 <= (blocks - 2) * allow, "block loudness within 2.5 dB on 90 % of blocks (85 % at 600 and 800)");
    }
}

static void test_decoded_pitch_and_envelope_follow_the_input(void)
{
    static oc_vox_pframe_t in_frames[VOX_MAX_FRAMES];
    static vox_vector_t dec;
    unsigned n, i;
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_vowels", &vec));
    n = analyse_all(&vec);
    memcpy(in_frames, frames, sizeof(oc_vox_pframe_t) * n);
    static const oc_vox_mode_t MODES[3] = { OC_VOX_600, OC_VOX_1200, OC_VOX_2400 };
    for (int mi = 0; mi < 3; mi++) {
        int m = MODES[mi];
        unsigned voiced = 0, pgood = 0;
        double lsd = 0;
        roundtrip((oc_vox_mode_t)m, &vec, out);
        memcpy(dec.pcm, out, sizeof(int16_t) * vec.n);
        dec.n = vec.n;
        analyse_all(&dec);
        for (i = 12; i < n; i++) {
            double acc = 0;
            if (!(in_frames[i].v & 1u) || in_frames[i].e < 60) continue;
            voiced++;
            /* the decoder's output is the input delayed 80 samples: compare with frame i-1 and i */
            if (abs((int)frames[i].p - (int)in_frames[i].p) <= 4 || abs((int)frames[i].p - (int)in_frames[i - 1].p) <= 4) pgood++;
            {
                double acc1 = 0;
                for (int b = 0; b < 12; b++) {
                    acc += (double)(frames[i].s[b] - in_frames[i].s[b]) * (frames[i].s[b] - in_frames[i].s[b]);
                    acc1 += (double)(frames[i].s[b] - in_frames[i - 1].s[b]) * (frames[i].s[b] - in_frames[i - 1].s[b]);
                }
                if (acc1 < acc) acc = acc1;
            }
            lsd += sqrt(acc / 12.0) * 0.75;
        }
        TEST_ASSERT_TRUE(voiced > 100);
        /* 600 sends pitch every 40 ms with 3-bit deltas: a looser bound there */
        TEST_ASSERT_TRUE_MESSAGE(pgood * 100 >= voiced * (m == OC_VOX_600 ? 75 : 85), "decoded pitch within 4 steps on 85 % of voiced frames (75 % at 600)");
        lsd /= voiced;
        /* Measured 5.4 / 4.8 / 4.2 dB on these synthetic vowels (deep high-band valleys the DCT
         * stages smooth over): regression bounds until Task 13 trains the codebooks. */
        TEST_ASSERT_TRUE_MESSAGE(lsd < (m == OC_VOX_600 ? 6.0 : m == OC_VOX_1200 ? 5.5 : 5.0), "log-spectral distance of the decoded envelope");
    }
}

static void test_silence_decodes_to_silence_and_encoding_is_deterministic(void)
{
    oc_vox_ref_enc_t e1, e2;
    uint8_t b1[OC_VOX_BLOCK_MAX_BYTES], b2[OC_VOX_BLOCK_MAX_BYTES];
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("silence", &vec));
    roundtrip(OC_VOX_1200, &vec, out);
    for (unsigned i = 0; i < (vec.n / 960) * 960; i++) TEST_ASSERT_EQUAL_INT16(0, out[i]); /* the whole blocks */
    TEST_ASSERT_EQUAL_INT(0, vox_vector_load("synth_mixed", &vec));
    oc_vox_ref_enc_init(&e1, OC_VOX_2400);
    oc_vox_ref_enc_init(&e2, OC_VOX_2400);
    for (unsigned b = 0; b < vec.n / 960; b++) {
        oc_vox_ref_encode(&e1, vec.pcm + 960 * b, b1);
        oc_vox_ref_encode(&e2, vec.pcm + 960 * b, b2);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(b1, b2, 36);
    }
    TEST_ASSERT_EQUAL_INT(-1, oc_vox_ref_enc_init(&e1, (oc_vox_mode_t)9));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pitch_tracks_the_vowels_without_octave_errors);
    RUN_TEST(test_voicing_follows_vowels_noise_and_silence);
    RUN_TEST(test_envelope_is_mean_removed_and_follows_the_spectrum);
    RUN_TEST(test_chirp_is_voiced_and_tracks_until_the_pitch_range_ends);
    RUN_TEST(test_round_trip_keeps_the_loudness_in_every_mode);
    RUN_TEST(test_decoded_pitch_and_envelope_follow_the_input);
    RUN_TEST(test_silence_decodes_to_silence_and_encoding_is_deterministic);
    return UNITY_END();
}
