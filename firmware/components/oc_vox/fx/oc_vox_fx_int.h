/* Internals of the fixed-point production codec (fx/): integer kernels,
 * analysis and synthesis. No float anywhere (test_vox_nofloat.sh); 32x32
 * -> 64 products where the precision needs them, int64 sums for energies.
 * The algorithms are the reference's (ref/), step for step, so the two
 * are compared on the shared vectors (test_vox_equiv). */
#ifndef OC_VOX_FX_INT_H
#define OC_VOX_FX_INT_H

#include <stdint.h>

#include "oc_vox_int.h"

#define OC_VOX_FX_FFT      256u
#define OC_VOX_FX_BINS     129u
#define OC_VOX_FX_LAG_MIN  20u
#define OC_VOX_FX_LAG_MAX  160u
#define OC_VOX_FX_HIST     160u
#define OC_VOX_FX_BUF      (OC_VOX_FX_HIST + 960u)
#define OC_VOX_FX_DELAY    80u
#define OC_VOX_FX_CANDS    8u
#define OC_VOX_FX_MAX_HARM 80u
#define OC_VOX_FX_NOISE    64u

/* Kernels (oc_vox_fx_dsp.c) */

/* In-place complex FFT of 256 points; the output is the DFT / 256 (one
 * halving per stage). Inputs within +-2^23 keep every stage in range. */
void     oc_vox_fx_fft256(int32_t re[256], int32_t im[256]);
extern const int16_t oc_vox_fx_hann256[256]; /* Q15 */

/* log2(v) in Q8 (0 for v = 0); 2^(x / 256) in Q15 for x in [-15, 15] x 256
 * (saturating); integer square roots. */
int32_t  oc_vox_fx_log2_q8(uint32_t v);
int32_t  oc_vox_fx_log2_q8_64(uint64_t v);
uint32_t oc_vox_fx_exp2_q15(int32_t x_q8);
uint32_t oc_vox_fx_sqrt32(uint32_t v);
uint32_t oc_vox_fx_sqrt64(uint64_t v);

/* sin of a phase in turns Q32, in Q15. */
int16_t  oc_vox_fx_sin_q15(uint32_t phase_q32);

/* Normalized cross-correlation of x[0..n) with x[-lag..n-lag), in Q15
 * (0 when either side is silent). */
int32_t  oc_vox_fx_ncc_q15(const int16_t *x, unsigned n, unsigned lag);

/* 10 log10 of a ratio, in 0.75 dB units: 4.0137 x (log2 a - log2 b). */
int32_t  oc_vox_fx_units_of_log2_q8(int32_t dlog2_q8);

/* Analysis (oc_vox_fx_analysis.c) */
typedef struct {
    int16_t  buf[OC_VOX_FX_BUF];
    int32_t  hp_x1, hp_y1;
    uint32_t pow[OC_VOX_FRAMES][OC_VOX_FX_BINS]; /* the block's spectra, each scaled by 2^-shift[f] */
    uint8_t  shift[OC_VOX_FRAMES];
    uint8_t  prev_v, prev_p, have_prev;
} oc_vox_fx_ana_t;

void oc_vox_fx_ana_init(oc_vox_fx_ana_t *a);
void oc_vox_fx_analyse(oc_vox_fx_ana_t *a, const int16_t pcm[960], oc_vox_pframe_t out[OC_VOX_FRAMES]);

/* Synthesis (oc_vox_fx_synth.c) */
typedef struct {
    uint32_t phase1;                     /* the fundamental, turns Q32 */
    uint32_t disp[OC_VOX_FX_MAX_HARM];   /* per-harmonic dispersion, turns Q32 */
    uint32_t phase_n[OC_VOX_FX_NOISE];
    int32_t  amp_h[OC_VOX_FX_MAX_HARM];  /* the previous frame's amplitudes, Q12 samples */
    int32_t  amp_n[OC_VOX_FX_NOISE];
    int32_t  fdev_n[OC_VOX_FX_NOISE];    /* the previous frame's deviations, Hz x 16 */
    int32_t  f0_prev_x16;
    uint8_t  started;
    uint32_t rng;
} oc_vox_fx_syn_t;

void oc_vox_fx_syn_init(oc_vox_fx_syn_t *s);
void oc_vox_fx_synth_frame(oc_vox_fx_syn_t *s, const oc_vox_pframe_t *pf, int16_t out[160]);

#endif
