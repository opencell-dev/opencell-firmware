/* The production codec: the fixed-point analysis and synthesis (fx/)
 * around the shared block pipeline (oc_vox_pipe.h). */
#include "oc_vox.h"

#include <string.h>

#include "oc_vox_fx_int.h"

_Static_assert(sizeof(oc_vox_enc_t) <= 8192u, "the encoder's state fits 8 KB (PLAN.md §1.7)");
_Static_assert(sizeof(oc_vox_dec_t) <= 4096u, "the decoder's state fits 4 KB (PLAN.md §1.7)");

int oc_vox_enc_init(oc_vox_enc_t *e, oc_vox_mode_t mode)
{
    oc_vox_fx_ana_init(&e->ana);
    return oc_vox_pipe_enc_init(&e->pipe, mode);
}

int oc_vox_dec_init(oc_vox_dec_t *d, oc_vox_mode_t mode)
{
    oc_vox_fx_syn_init(&d->syn);
    return oc_vox_pipe_dec_init(&d->pipe, mode);
}

void oc_vox_encode(oc_vox_enc_t *e, const int16_t pcm[OC_VOX_BLOCK_SAMPLES], uint8_t *out)
{
    oc_vox_pframe_t frames[OC_VOX_FRAMES];
    oc_vox_fx_analyse(&e->ana, pcm, frames);
    oc_vox_pipe_encode(&e->pipe, frames, out);
}

void oc_vox_decode(oc_vox_dec_t *d, const uint8_t *block, int16_t pcm[OC_VOX_BLOCK_SAMPLES])
{
    oc_vox_pframe_t frames[OC_VOX_FRAMES];
    oc_vox_pipe_decode(&d->pipe, block, frames);
    for (unsigned f = 0; f < OC_VOX_FRAMES; f++) oc_vox_fx_synth_frame(&d->syn, &frames[f], pcm + 160u * f);
}

const oc_vox_dec_stats_t *oc_vox_dec_stats(const oc_vox_dec_t *d) { return &d->pipe.stats; }

/* For tools and tests: the analysed frames of a block, and synthesis of
 * given frames, with the production implementation. */
void oc_vox_analyse_block(oc_vox_enc_t *e, const int16_t pcm[OC_VOX_BLOCK_SAMPLES], void *frames6)
{
    oc_vox_fx_analyse(&e->ana, pcm, (oc_vox_pframe_t *)frames6);
}

void oc_vox_synth_block(oc_vox_dec_t *d, const void *frames6, int16_t pcm[OC_VOX_BLOCK_SAMPLES])
{
    const oc_vox_pframe_t *fr = (const oc_vox_pframe_t *)frames6;
    for (unsigned f = 0; f < OC_VOX_FRAMES; f++) oc_vox_fx_synth_frame(&d->syn, &fr[f], pcm + 160u * f);
}
