/* The production decoder on garbage: random bytes of random lengths in
 * every mode never crash, never read out of bounds (run under the
 * sanitizers too) and keep the counters consistent; the encoder on
 * hostile audio (full-scale noise, DC, square waves, impulses) always
 * produces a block. */
#include "unity.h"

#include <string.h>

#include "oc_vox.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t xs = 0xDEADBEEFu;
static uint32_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

static void test_random_blocks_of_random_lengths_decode(void)
{
    static int16_t pcm[OC_VOX_BLOCK_SAMPLES];
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        oc_vox_dec_t d;
        uint8_t block[64];
        unsigned nb = oc_vox_block_bytes((oc_vox_mode_t)m), given = 0;
        oc_vox_dec_init(&d, (oc_vox_mode_t)m);
        for (unsigned n = 0; n < 400; n++) {
            unsigned len = rnd() % 65u;
            for (unsigned i = 0; i < len; i++) block[i] = (uint8_t)rnd();
            /* a transport hands the codec the bytes it got; a wrong length is a loss */
            if (len == nb) { oc_vox_decode(&d, block, pcm); given++; }
            else oc_vox_decode(&d, NULL, pcm);
            for (unsigned i = 0; i < OC_VOX_BLOCK_SAMPLES; i += 97) TEST_ASSERT_TRUE(pcm[i] >= -32768 && pcm[i] <= 32767);
        }
        {
            const oc_vox_dec_stats_t *st = oc_vox_dec_stats(&d);
            TEST_ASSERT_EQUAL_UINT32(400, st->blocks);
            TEST_ASSERT_TRUE(st->lost_blocks >= 400 - given);
            TEST_ASSERT_TRUE(st->lost_blocks <= 400);
            TEST_ASSERT_EQUAL_UINT32(6 * st->lost_blocks, st->concealed_frames);
            TEST_ASSERT_TRUE(st->golay_failed <= 3 * given);
            TEST_ASSERT_TRUE(st->toggle_gaps <= given);
        }
    }
}

static void test_right_sized_garbage_in_every_mode(void)
{
    static int16_t pcm[OC_VOX_BLOCK_SAMPLES];
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        oc_vox_dec_t d;
        uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
        unsigned nb = oc_vox_block_bytes((oc_vox_mode_t)m);
        oc_vox_dec_init(&d, (oc_vox_mode_t)m);
        for (unsigned n = 0; n < 400; n++) {
            unsigned kind = n % 4;
            for (unsigned i = 0; i < nb; i++) block[i] = kind == 0 ? (uint8_t)rnd() : kind == 1 ? 0xFF : kind == 2 ? 0x00 : (uint8_t)(i * 0x55u);
            oc_vox_decode(&d, block, pcm);
        }
        TEST_ASSERT_EQUAL_UINT32(400, oc_vox_dec_stats(&d)->blocks);
    }
}

static void test_hostile_audio_always_encodes(void)
{
    static int16_t pcm[OC_VOX_BLOCK_SAMPLES];
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m += 2) {
        oc_vox_enc_t e;
        oc_vox_dec_t d;
        uint8_t block[OC_VOX_BLOCK_MAX_BYTES + 1];
        unsigned nb = oc_vox_block_bytes((oc_vox_mode_t)m);
        oc_vox_enc_init(&e, (oc_vox_mode_t)m);
        oc_vox_dec_init(&d, (oc_vox_mode_t)m);
        for (unsigned n = 0; n < 40; n++) {
            for (unsigned i = 0; i < OC_VOX_BLOCK_SAMPLES; i++) {
                switch (n % 8) {
                case 0: pcm[i] = (int16_t)rnd(); break;                              /* full-scale noise */
                case 1: pcm[i] = 32767; break;                                       /* DC */
                case 2: pcm[i] = -32768; break;
                case 3: pcm[i] = (i / 20) & 1u ? 32767 : -32768; break;              /* a 200 Hz square */
                case 4: pcm[i] = (i % 960) == 0 ? 32767 : 0; break;                  /* an impulse */
                case 5: pcm[i] = (int16_t)(i & 1u ? 32767 : -32768); break;          /* Nyquist */
                case 6: pcm[i] = 0; break;
                default: pcm[i] = (int16_t)((rnd() % 7u) - 3u); break;               /* near-silence */
                }
            }
            block[nb] = 0xA5;
            oc_vox_encode(&e, pcm, block);
            TEST_ASSERT_EQUAL_HEX8(0xA5, block[nb]); /* exactly the block's bytes written */
            oc_vox_decode(&d, block, pcm);
        }
        TEST_ASSERT_EQUAL_UINT32(0, oc_vox_dec_stats(&d)->golay_failed); /* a well-formed block always decodes */
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_random_blocks_of_random_lengths_decode);
    RUN_TEST(test_right_sized_garbage_in_every_mode);
    RUN_TEST(test_hostile_audio_always_encodes);
    return UNITY_END();
}
