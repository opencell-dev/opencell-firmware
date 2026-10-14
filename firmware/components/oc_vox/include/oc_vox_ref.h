/* The floating-point reference codec (PLAN.md §1.6): the same API as
 * oc_vox.h with the oc_vox_ref_ prefix, the same bitstream, host only.
 * It defines what the parameters mean; the production codec (oc_vox.h)
 * is checked against it on the shared vectors (test_vox_equiv). */
#ifndef OC_VOX_REF_H
#define OC_VOX_REF_H

#include "oc_vox.h"
#include "oc_vox_ref_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int  oc_vox_ref_enc_init(oc_vox_ref_enc_t *e, oc_vox_mode_t mode);
int  oc_vox_ref_dec_init(oc_vox_ref_dec_t *d, oc_vox_mode_t mode);
void oc_vox_ref_encode(oc_vox_ref_enc_t *e, const int16_t pcm[OC_VOX_BLOCK_SAMPLES], uint8_t *out);
void oc_vox_ref_decode(oc_vox_ref_dec_t *d, const uint8_t *block, int16_t pcm[OC_VOX_BLOCK_SAMPLES]);
const oc_vox_dec_stats_t *oc_vox_ref_dec_stats(const oc_vox_ref_dec_t *d);

/* For tools and tests: the analysed (unquantized) frames of a block, and
 * synthesis of given frames. */
void oc_vox_ref_analyse_block(oc_vox_ref_enc_t *e, const int16_t pcm[OC_VOX_BLOCK_SAMPLES], void *frames6 /* oc_vox_pframe_t[6] */);
void oc_vox_ref_synth_block(oc_vox_ref_dec_t *d, const void *frames6, int16_t pcm[OC_VOX_BLOCK_SAMPLES]);

#ifdef __cplusplus
}
#endif

#endif
