/* The production codec: fixed-point analysis and synthesis (fx/) around the
 * shared bitstream, FEC, quantizers and concealment. Until Task 9 of
 * docs/openvox/PLAN.md this file is the API shell: it codes nothing yet. */
#include "oc_vox.h"

#include <string.h>

static int valid(oc_vox_mode_t m) { return oc_vox_block_bytes(m) != 0; }

int oc_vox_enc_init(oc_vox_enc_t *e, oc_vox_mode_t mode)
{
    memset(e, 0, sizeof(*e));
    e->mode = mode;
    return valid(mode) ? 0 : -1;
}

int oc_vox_dec_init(oc_vox_dec_t *d, oc_vox_mode_t mode)
{
    memset(d, 0, sizeof(*d));
    d->mode = mode;
    return valid(mode) ? 0 : -1;
}

void oc_vox_encode(oc_vox_enc_t *e, const int16_t pcm[OC_VOX_BLOCK_SAMPLES], uint8_t *out)
{
    (void)pcm;
    memset(out, 0, oc_vox_block_bytes(e->mode));
    e->toggle ^= 1u;
}

void oc_vox_decode(oc_vox_dec_t *d, const uint8_t *block, int16_t pcm[OC_VOX_BLOCK_SAMPLES])
{
    d->stats.blocks++;
    if (block == NULL) d->stats.lost_blocks++;
    memset(pcm, 0, OC_VOX_BLOCK_SAMPLES * sizeof(pcm[0]));
}

const oc_vox_dec_stats_t *oc_vox_dec_stats(const oc_vox_dec_t *d)
{
    return (const oc_vox_dec_stats_t *)&d->stats;
}
