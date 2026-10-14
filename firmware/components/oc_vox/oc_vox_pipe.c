#include "oc_vox_pipe.h"

#include <string.h>

#include "oc_vox_bits.h"
#include "oc_vox_fec.h"
#include "oc_vox_quant.h"

int oc_vox_pipe_enc_init(oc_vox_pipe_enc_t *p, oc_vox_mode_t mode)
{
    memset(p, 0, sizeof(*p));
    p->mode = mode;
    return oc_vox_shape(mode) ? 0 : -1;
}

void oc_vox_pipe_encode(oc_vox_pipe_enc_t *p, const oc_vox_pframe_t frames[OC_VOX_FRAMES], uint8_t *block)
{
    oc_vox_qblock_t q;
    oc_vox_pframe_t recon[OC_VOX_FRAMES];
    uint8_t pstr[OC_VOX_PARAM_MAX_BYTES];
    oc_vox_quant_block(p->mode, frames, p->have_last ? &p->last : NULL, p->toggle, &q, recon);
    oc_vox_bits_pack(p->mode, &q, pstr);
    oc_vox_fec_protect(p->mode, pstr, block);
    p->last = recon[OC_VOX_FRAMES - 1];
    p->have_last = 1;
    p->toggle ^= 1u;
}

int oc_vox_pipe_dec_init(oc_vox_pipe_dec_t *p, oc_vox_mode_t mode)
{
    memset(p, 0, sizeof(*p));
    p->mode = mode;
    oc_vox_conceal_init(&p->conceal);
    return oc_vox_shape(mode) ? 0 : -1;
}

static void lost(oc_vox_pipe_dec_t *p, oc_vox_pframe_t frames[OC_VOX_FRAMES])
{
    p->stats.lost_blocks++;
    p->stats.concealed_frames += OC_VOX_FRAMES;
    oc_vox_conceal_block(&p->conceal, frames);
    p->have_toggle = 0; /* the next block's toggle says nothing about what was missed */
}

void oc_vox_pipe_decode(oc_vox_pipe_dec_t *p, const uint8_t *block, oc_vox_pframe_t frames[OC_VOX_FRAMES])
{
    oc_vox_fec_report_t rep;
    oc_vox_qblock_t q;
    uint8_t pstr[OC_VOX_PARAM_MAX_BYTES], toggle;
    p->stats.blocks++;
    if (block == NULL) { lost(p, frames); return; }
    oc_vox_fec_recover(p->mode, block, pstr, &rep);
    p->stats.golay_fixed += rep.golay_fixed;
    p->stats.golay_failed += rep.golay_failed;
    p->stats.hamming_fixed += rep.hamming_fixed;
    p->stats.crc_failed += rep.crc_failed;
    if (rep.golay_failed) { lost(p, frames); return; }
    oc_vox_bits_unpack(p->mode, pstr, &q);
    oc_vox_dequant_block(p->mode, &q, p->conceal.have_last ? &p->conceal.last : NULL,
                         rep.crc_failed ? OC_VOX_DQ_DROP_FINE : 0u, frames, &toggle);
    if (p->have_toggle && toggle == p->toggle) {
        /* the same toggle twice: a block went missing between (the
         * transport told us nothing); the recovery ramp applies */
        p->stats.toggle_gaps++;
        if (p->conceal.lost_frames == 0) p->conceal.lost_frames = OC_VOX_FRAMES;
    }
    p->toggle = toggle;
    p->have_toggle = 1;
    oc_vox_conceal_recover(&p->conceal, frames);
}
