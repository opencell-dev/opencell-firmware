/* The production codec's state, complete so callers can hold it (no heap
 * anywhere). The fields are the codec's own: read nothing here, use
 * oc_vox.h. Sizes are pinned by _Static_assert in oc_vox.c. */
#ifndef OC_VOX_STATE_H
#define OC_VOX_STATE_H

#include <stdint.h>

#include "oc_vox_int.h"
#include "oc_vox_pipe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OC_VOX_FX_FFT      256u
#define OC_VOX_FX_BINS     129u
#define OC_VOX_FX_HIST     160u  /* samples kept from the previous block */
#define OC_VOX_FX_BUF      (OC_VOX_FX_HIST + 960u)
#define OC_VOX_FX_MAX_HARM 80u
#define OC_VOX_FX_NOISE    64u

/* The fixed-point analysis. */
typedef struct {
    int16_t  buf[OC_VOX_FX_BUF];
    int32_t  hp_x1, hp_y1;
    uint32_t pow[OC_VOX_FRAMES][OC_VOX_FX_BINS]; /* the block's spectra, each scaled by 2^-shift[f] */
    int32_t  fft_re[OC_VOX_FX_FFT], fft_im[OC_VOX_FX_FFT]; /* scratch: kept here, not on the stack (PLAN.md §1.7) */
    uint8_t  shift[OC_VOX_FRAMES];
    uint8_t  prev_v, prev_p, have_prev;
} oc_vox_fx_ana_t;

/* The fixed-point synthesis. */
typedef struct {
    uint32_t phase1;                     /* the fundamental, turns Q32 */
    uint32_t disp[OC_VOX_FX_MAX_HARM];   /* per-harmonic dispersion, turns Q32 */
    uint32_t phase_n[OC_VOX_FX_NOISE];
    int32_t  amp_h[OC_VOX_FX_MAX_HARM];  /* the previous frame's amplitudes, Q12 samples */
    int32_t  amp_n[OC_VOX_FX_NOISE];
    int32_t  fdev_n[OC_VOX_FX_NOISE];    /* the previous frame's deviations, Hz x 16 */
    int32_t  la2_h[OC_VOX_FX_MAX_HARM], la2_n[OC_VOX_FX_NOISE], fdev[OC_VOX_FX_NOISE]; /* a frame's scratch */
    int32_t  f0_prev_x16;
    uint8_t  started;
    uint32_t rng;
} oc_vox_fx_syn_t;

typedef struct oc_vox_enc {
    oc_vox_pipe_enc_t pipe;
    oc_vox_fx_ana_t   ana;
} oc_vox_enc_t;

typedef struct oc_vox_dec {
    oc_vox_pipe_dec_t pipe;
    oc_vox_fx_syn_t   syn;
} oc_vox_dec_t;

#ifdef __cplusplus
}
#endif

#endif
