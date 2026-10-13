/* Parameter-domain concealment (PLAN.md §1.4), shared by both decoders:
 * what to synthesise for a block that was lost (or whose class A failed),
 * and how to come back when good frames return. */
#ifndef OC_VOX_CONCEAL_H
#define OC_VOX_CONCEAL_H

#include "oc_vox_int.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    oc_vox_pframe_t last;        /* the last frame synthesised (good or concealed) */
    uint8_t         have_last;
    uint8_t         lost_frames; /* concealed frames in a row, saturating */
    uint32_t        rng;
} oc_vox_conceal_t;

void oc_vox_conceal_init(oc_vox_conceal_t *c);

/* Six concealed frames from the trajectory so far: energy held one frame
 * then -3 dB a frame (-6 dB after a whole block), silence from the 12th
 * lost frame; pitch held with a slight random walk; voicing loses a band
 * a frame from the top after three frames; the envelope tilts down above
 * 2 kHz; vmix drifts toward dispersed. Nothing yet heard: silence. */
void oc_vox_conceal_block(oc_vox_conceal_t *c, oc_vox_pframe_t out[OC_VOX_FRAMES]);

/* Good frames arrive: the first two after a loss are capped to +6 dB a
 * frame above the trajectory so a late burst does not click; then the
 * frames are recorded as the trajectory. */
void oc_vox_conceal_recover(oc_vox_conceal_t *c, oc_vox_pframe_t frames[OC_VOX_FRAMES]);

#ifdef __cplusplus
}
#endif

#endif
