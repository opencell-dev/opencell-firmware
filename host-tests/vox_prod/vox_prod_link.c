/* Links oc_vox's production library alone (no reference, no -lm) and runs
 * one block through it, as opencell-core's oc-core uses it. */
#include <stdio.h>
#include <string.h>

#include "oc_vox.h"

int main(void)
{
    static oc_vox_enc_t e;
    static oc_vox_dec_t d;
    int16_t pcm[OC_VOX_BLOCK_SAMPLES];
    uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
    memset(pcm, 0, sizeof(pcm));
    if (oc_vox_enc_init(&e, OC_VOX_1200) != 0 || oc_vox_dec_init(&d, OC_VOX_1200) != 0) return 1;
    oc_vox_encode(&e, pcm, block);
    oc_vox_decode(&d, block, pcm);
    printf("vox_prod_link: one OVX-1200 block of %u bytes\n", (unsigned)oc_vox_block_bytes(OC_VOX_1200));
    return 0;
}
