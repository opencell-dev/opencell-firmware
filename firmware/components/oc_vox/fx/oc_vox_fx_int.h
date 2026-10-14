/* Internals of the fixed-point production codec (fx/): integer kernels,
 * analysis and synthesis. No float anywhere (test_vox_nofloat.sh); 32x32
 * -> 64 products where the precision needs them, int64 sums for energies.
 * The algorithms are the reference's (ref/), step for step, so the two
 * are compared on the shared vectors (test_vox_equiv). */
#ifndef OC_VOX_FX_INT_H
#define OC_VOX_FX_INT_H

#include <stdint.h>

#include "oc_vox_state.h"

#define OC_VOX_FX_LAG_MIN  20u
#define OC_VOX_FX_LAG_MAX  160u
#define OC_VOX_FX_DELAY    80u
#define OC_VOX_FX_CANDS    8u

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
void oc_vox_fx_ana_init(oc_vox_fx_ana_t *a);
void oc_vox_fx_analyse(oc_vox_fx_ana_t *a, const int16_t pcm[960], oc_vox_pframe_t out[OC_VOX_FRAMES]);

/* Synthesis (oc_vox_fx_synth.c) */
void oc_vox_fx_syn_init(oc_vox_fx_syn_t *s);
void oc_vox_fx_synth_frame(oc_vox_fx_syn_t *s, const oc_vox_pframe_t *pf, int16_t out[160]);

#endif
