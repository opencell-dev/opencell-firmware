#include "oc_vox_conceal.h"

#include <string.h>

void oc_vox_conceal_init(oc_vox_conceal_t *c)
{
    memset(c, 0, sizeof(*c));
    c->rng = 0x6A09E667u;
}

static uint32_t next_rng(oc_vox_conceal_t *c)
{
    c->rng ^= c->rng << 13; c->rng ^= c->rng >> 17; c->rng ^= c->rng << 5;
    return c->rng;
}

void oc_vox_conceal_block(oc_vox_conceal_t *c, oc_vox_pframe_t out[OC_VOX_FRAMES])
{
    for (unsigned f = 0; f < OC_VOX_FRAMES; f++) {
        oc_vox_pframe_t n = c->last;
        if (c->lost_frames < 255) c->lost_frames++;
        if (!c->have_last) {
            memset(&n, 0, sizeof(n));
            n.p = 84; /* 100 Hz, for want of anything */
        } else {
            int e = n.e;
            if (c->lost_frames >= 12) e = 0;
            else if (c->lost_frames > 6) e -= 8;
            else if (c->lost_frames > 1) e -= 4;
            n.e = (uint8_t)(e < 0 ? 0 : e);
            int p = (int)n.p + (int)(next_rng(c) % 3u) - 1;
            n.p = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p);
            if (c->lost_frames > 3 && n.v) {
                unsigned top = 3;
                while (top > 0 && !(n.v & (1u << top))) top--;
                n.v &= (uint8_t)~(1u << top);
            }
            for (unsigned b = 8; b < OC_VOX_BANDS; b++)
                if (n.s[b] > -127) n.s[b]--;
            if (c->lost_frames > 1 && n.vmix < 7) n.vmix++;
            n.emid = 0;
        }
        out[f] = n;
        c->last = n;
    }
}

void oc_vox_conceal_recover(oc_vox_conceal_t *c, oc_vox_pframe_t frames[OC_VOX_FRAMES])
{
    for (unsigned f = 0; f < OC_VOX_FRAMES; f++) {
        if (c->lost_frames > 0 && c->have_last && f < 2) {
            int cap = (int)c->last.e + 8;
            if (frames[f].e > cap) frames[f].e = (uint8_t)cap;
        }
        c->last = frames[f];
        c->have_last = 1;
    }
    c->lost_frames = 0;
}
