/* The reference codec's state, complete for callers to hold (no heap).
 * Host only. */
#ifndef OC_VOX_REF_STATE_H
#define OC_VOX_REF_STATE_H

#include "oc_vox_int.h"
#include "oc_vox_pipe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OC_VOX_REF_HIST     160u  /* samples kept from the previous block */
#define OC_VOX_REF_BUF      (OC_VOX_REF_HIST + 960u)
#define OC_VOX_MAX_HARM     80u   /* 4000 / 50 */
#define OC_VOX_NOISE_OSC    64u   /* the 62.5 Hz noise grid */

typedef struct {
    float   buf[OC_VOX_REF_BUF];
    float   hp_x1, hp_y1;
    uint8_t prev_v;
    uint8_t prev_p;
    int     have_prev;
} oc_vox_ref_ana_t;

typedef struct {
    float    phase1;                      /* the fundamental's phase */
    float    disp[OC_VOX_MAX_HARM];       /* fixed per-harmonic phase dispersion */
    float    phase_n[OC_VOX_NOISE_OSC];
    float    amp_h[OC_VOX_MAX_HARM];      /* the previous frame's amplitudes, interpolated from */
    float    amp_n[OC_VOX_NOISE_OSC];
    float    fdev_n[OC_VOX_NOISE_OSC];    /* the previous frame's noise-grid frequency deviations */
    float    f0_prev;
    int      started;
    uint32_t rng;
} oc_vox_ref_syn_t;

typedef struct oc_vox_ref_enc {
    oc_vox_pipe_enc_t pipe;
    oc_vox_ref_ana_t  ana;
} oc_vox_ref_enc_t;

typedef struct oc_vox_ref_dec {
    oc_vox_pipe_dec_t pipe;
    oc_vox_ref_syn_t  syn;
} oc_vox_ref_dec_t;

#ifdef __cplusplus
}
#endif

#endif
