/* oc_vox: OpenVoxNB, OpenCell's narrowband speech codec (docs/openvox/
 * PLAN.md). 8 kHz mono int16 in 20 ms frames, coded six frames at a time
 * as one 120 ms block, the radio frame (OC_FRAME_US). Nine modes, 600 to
 * 3200 bit/s (oc_vox_modes.h); unequal error protection inside the block
 * (oc_vox_fec.h); the decoder conceals lost and damaged blocks itself.
 *
 * A portable C11 library like oc_sig: no OS calls, no heap, no float in
 * this (the production, fixed-point) implementation, state in caller-owned
 * structs, const tables only. One encoder and one decoder are each used
 * from one thread. The floating-point reference is oc_vox_ref.h, host only. */
#ifndef OC_VOX_H
#define OC_VOX_H

#include <stddef.h>
#include <stdint.h>

#include "oc_vox_modes.h"
#include "oc_vox_state.h"

#define OC_VOX_VERSION       0u   /* 1 at the bitstream freeze (PLAN.md Task 13) */
#define OC_VOX_RATE_HZ       8000u
#define OC_VOX_FRAME_SAMPLES 160u /* 20 ms */
#define OC_VOX_BLOCK_FRAMES  6u   /* 120 ms: one radio frame */
#define OC_VOX_BLOCK_SAMPLES 960u

/* 0, or -1 for a value that is not a mode (the state is then unusable). */
int  oc_vox_enc_init(oc_vox_enc_t *e, oc_vox_mode_t mode);
int  oc_vox_dec_init(oc_vox_dec_t *d, oc_vox_mode_t mode);

/* One block: 960 samples in, oc_vox_block_bytes(mode) bytes out. The
 * coded block covers the input delayed by 80 samples (10 ms: the analysis
 * lookahead), a constant offset the decoder's output keeps. */
void oc_vox_encode(oc_vox_enc_t *e, const int16_t pcm[OC_VOX_BLOCK_SAMPLES], uint8_t *out);

/* One block in, 960 samples out. block NULL: the block was lost, conceal.
 * Damaged blocks (the FEC could not repair the important bits) are
 * concealed the same way; the rest is repaired or interpolated. Always
 * produces audio. */
void oc_vox_decode(oc_vox_dec_t *d, const uint8_t *block, int16_t pcm[OC_VOX_BLOCK_SAMPLES]);

/* Counters since init: what the decoder saw and did. */
typedef struct {
    uint32_t blocks;           /* decode calls */
    uint32_t lost_blocks;      /* NULL blocks, plus blocks whose class A failed */
    uint32_t concealed_frames; /* frames synthesised from extrapolated parameters */
    uint32_t toggle_gaps;      /* the toggle bit repeated: a block went missing unannounced */
    uint32_t golay_fixed;      /* Golay words with 1-3 bit errors corrected */
    uint32_t golay_failed;     /* Golay words with more (the block counts as lost) */
    uint32_t hamming_fixed;    /* Hamming words with a bit error corrected */
    uint32_t crc_failed;       /* class C discarded (OVX-2400, OVX-3200) */
} oc_vox_dec_stats_t;
const oc_vox_dec_stats_t *oc_vox_dec_stats(const oc_vox_dec_t *d);

#endif
