/* Internals of the floating-point reference (ref/): the DSP kernels, the
 * analysis and the synthesis. Host only; never built for the board. */
#ifndef OC_VOX_REF_INT_H
#define OC_VOX_REF_INT_H

#include <stdint.h>

#include "oc_vox_int.h"

#define OC_VOX_PI           3.14159265358979323846f /* M_PI is not C11 */
#define OC_VOX_REF_FFT      256u  /* analysis window and FFT length: 31.25 Hz bins */
#define OC_VOX_REF_BINS     129u
#define OC_VOX_REF_PITCHSEG 320u  /* the pitch segment around a frame */
#define OC_VOX_REF_LAG_MIN  20u   /* 400 Hz */
#define OC_VOX_REF_LAG_MAX  160u  /* 50 Hz */
#define OC_VOX_REF_HIST     160u  /* samples kept from the previous block */
#define OC_VOX_REF_BUF      (OC_VOX_REF_HIST + 960u)
#define OC_VOX_REF_DELAY    80u   /* the coded block starts this many samples before the input block */
#define OC_VOX_REF_CANDS    8u    /* pitch candidates per frame */
#define OC_VOX_MAX_HARM     80u   /* 4000 / 50 */
#define OC_VOX_NOISE_OSC    64u   /* the 62.5 Hz noise grid */

/* DSP kernels (oc_vox_ref_dsp.c) */
void  oc_vox_ref_fft256(const float in[256], float re[129], float im[129]);
void  oc_vox_ref_hann256(float w[256]);
/* Normalized cross-correlation of x[0..n) with x[-lag..n-lag): the caller
 * guarantees x[-lag] exists. 0 when either segment is silent. */
float oc_vox_ref_ncc(const float *x, unsigned n, unsigned lag);
float oc_vox_ref_db_of_power(float p); /* 10 log10, floored at -120 */

/* Analysis (oc_vox_ref_analysis.c) */
typedef struct {
    float   buf[OC_VOX_REF_BUF];
    float   hp_x1, hp_y1;
    uint8_t prev_v;
    uint8_t prev_p;
    int     have_prev;
} oc_vox_ref_ana_t;

void oc_vox_ref_ana_init(oc_vox_ref_ana_t *a);
void oc_vox_ref_analyse(oc_vox_ref_ana_t *a, const int16_t pcm[960], oc_vox_pframe_t out[OC_VOX_FRAMES]);

/* Synthesis (oc_vox_ref_synth.c) */
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

void oc_vox_ref_syn_init(oc_vox_ref_syn_t *s);
void oc_vox_ref_synth_frame(oc_vox_ref_syn_t *s, const oc_vox_pframe_t *pf, int16_t out[160]);

/* Shared by analysis and synthesis: a band's centre and a parameter's
 * physical value. */
float oc_vox_ref_band_centre_hz(unsigned b);
float oc_vox_ref_e_to_rms(uint8_t e); /* against 32768 */
uint8_t oc_vox_ref_rms_to_e(float rms);

#endif
