/* The fixed-point synthesis: the reference's (ref/oc_vox_ref_synth.c) in
 * integers. Amplitudes are worked out in Q8 log2 (envelope densities,
 * per-band renormalisation, the frame's gain) and come out of exp2 in Q12
 * sample units; phases are turns in Q32 with harmonics at h x the
 * fundamental's phase plus a fixed dispersion; the random sequence is
 * the reference's, drawn in the same order, so the two decoders put the
 * same deviations on the same oscillators. */
#include <string.h>

#include "oc_vox_fx_int.h"
#include "oc_vox_quant.h"

static uint32_t next_rng(oc_vox_fx_syn_t *s)
{
    s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
    return s->rng;
}

/* r in 0..9999 -> turns Q32 of r / 10000. */
static uint32_t turns_of_10000(uint32_t r) { return (uint32_t)(((uint64_t)r << 32) / 10000u); }

void oc_vox_fx_syn_init(oc_vox_fx_syn_t *s)
{
    memset(s, 0, sizeof(*s));
    s->rng = 0x3C6EF372u;
    for (unsigned h = 0; h < OC_VOX_FX_MAX_HARM; h++)
        s->disp[h] = turns_of_10000(next_rng(s) % 10000u) - 0x80000000u; /* (r / 10000 - 0.5) turns */
    for (unsigned g = 0; g < OC_VOX_FX_NOISE; g++)
        s->phase_n[g] = turns_of_10000(next_rng(s) % 10000u);
    s->f0_prev_x16 = 100 * 16;
}

/* Band centres sqrt(edge_b edge_b+1), Hz x 16. */
static const int32_t CENTRE_X16[OC_VOX_BANDS] = {
    2771, 6197, 9466, 12700, 16288, 20660, 25851, 31835, 38647, 46229, 54613, 61548
};

/* The envelope (0.75 dB units, Q8) at hz x 16: linear between band
 * centres, flat beyond the outer ones. The units are signed, so Q8 is
 * "* 256", not "<< 8": a left shift of a negative value is undefined (C11 6.5.7p4). */
static int32_t env_at_q8(const int8_t s[OC_VOX_BANDS], int32_t hz_x16)
{
    unsigned b;
    if (hz_x16 <= CENTRE_X16[0]) return (int32_t)s[0] * 256;
    for (b = 0; b + 1 < OC_VOX_BANDS; b++) {
        int32_t c0 = CENTRE_X16[b], c1 = CENTRE_X16[b + 1];
        if (hz_x16 <= c1) return ((int32_t)s[b] * 256) + (((int32_t)s[b + 1] - (int32_t)s[b]) * 256 * (hz_x16 - c0)) / (c1 - c0);
    }
    return (int32_t)s[OC_VOX_BANDS - 1] * 256;
}

static unsigned vband_of(int32_t hz_x16) { return hz_x16 < 600 * 16 ? 0 : hz_x16 < 1200 * 16 ? 1 : hz_x16 < 2400 * 16 ? 2 : 3; }

static unsigned band_of(int32_t hz_x16)
{
    unsigned b;
    for (b = 0; b + 1 < OC_VOX_BANDS; b++)
        if (hz_x16 < (int32_t)oc_vox_band_edge_hz[b + 1] * 16) return b;
    return OC_VOX_BANDS - 1u;
}

/* 0.75 dB units (Q8) to Q8 log2 of power: x 0.249145. */
static int32_t lpow_of_units_q8(int32_t u_q8) { return (int32_t)(((int64_t)u_q8 * 16329) >> 16); }

/* Hz x 16 to a Q32 phase increment per sample at 8 kHz. */
static uint32_t inc_of_hz_x16(int32_t hz_x16) { return (uint32_t)(((uint64_t)(uint32_t)hz_x16 << 32) / 128000u); }

void oc_vox_fx_synth_frame(oc_vox_fx_syn_t *s, const oc_vox_pframe_t *pf, int16_t out[160])
{
    int32_t f0_x16 = oc_vox_pitch_hz_x16(pf->p);
    int32_t *la2_h = s->la2_h, *la2_n = s->la2_n, *fdev = s->fdev; /* log2 of amplitude^2 (Q8) and deviations: the state's scratch */
    int32_t amp_h[OC_VOX_FX_MAX_HARM], amp_n[OC_VOX_FX_NOISE];
    int32_t lmax_b[OC_VOX_BANDS], lpsd[OC_VOX_BANDS], lbins[OC_VOX_BANDS];
    uint32_t wsum_b[OC_VOX_BANDS];
    unsigned cnt[OC_VOX_BANDS];
    int32_t mix_q15 = (int32_t)pf->vmix * 32768 / 14;                         /* vmix / 7 x 0.5 */
    int32_t lharm2 = oc_vox_fx_log2_q8((uint32_t)((1 << 30) - mix_q15 * mix_q15)) - 30 * 256; /* log2(1 - mix^2) */
    int32_t lmix2 = mix_q15 ? 2 * (oc_vox_fx_log2_q8((uint32_t)mix_q15) - 15 * 256) : -30 * 256;
    int32_t lref = -0x40000000, lrms_q8, ltot_q8, lgain_q8;
    uint64_t total = 0;
    unsigned nh = (unsigned)(3900 * 16 / f0_x16), h, g, n, b;
    uint32_t inc_from, inc_to;
    int32_t dinc;
    if (nh > OC_VOX_FX_MAX_HARM) nh = OC_VOX_FX_MAX_HARM;

    for (b = 0; b < OC_VOX_BANDS; b++) {
        lmax_b[b] = -0x40000000; wsum_b[b] = 0; cnt[b] = 0;
        lpsd[b] = lpow_of_units_q8((int32_t)pf->s[b] * 256);
        lbins[b] = oc_vox_fx_log2_q8((uint32_t)(oc_vox_band_edge_hz[b + 1] - oc_vox_band_edge_hz[b]) * 16u) - oc_vox_fx_log2_q8(500); /* bins = width / 31.25 */
    }
    for (h = 0; h < nh; h++) {
        int32_t fh = (int32_t)(h + 1) * f0_x16;
        b = band_of(fh);
        la2_h[h] = lpow_of_units_q8(env_at_q8(pf->s, fh)); /* the interpolated weight, for now */
        if (la2_h[h] > lmax_b[b]) lmax_b[b] = la2_h[h];
        cnt[b]++;
    }
    for (h = 0; h < nh; h++) {
        b = band_of((int32_t)(h + 1) * f0_x16);
        wsum_b[b] += oc_vox_fx_exp2_q15(la2_h[h] - lmax_b[b]); /* <= 32768 each */
    }
    for (h = 0; h < OC_VOX_FX_MAX_HARM; h++) {
        int32_t fh = (int32_t)(h + 1) * f0_x16;
        if (h < nh && ((pf->v >> vband_of(fh)) & 1u)) {
            int32_t lmean;
            b = band_of(fh);
            lmean = lmax_b[b] + oc_vox_fx_log2_q8(wsum_b[b]) - oc_vox_fx_log2_q8((uint32_t)cnt[b]) - 15 * 256;
            la2_h[h] = la2_h[h] - lmean + lpsd[b] + lbins[b] - oc_vox_fx_log2_q8((uint32_t)cnt[b]) + lharm2;
            if (la2_h[h] > lref) lref = la2_h[h];
        } else {
            la2_h[h] = -0x40000000; /* silent */
        }
    }
    for (g = 0; g < OC_VOX_FX_NOISE; g++) {
        int32_t fg = 1000 * (int32_t)g + 500; /* 62.5 (g + 0.5) Hz x 16 */
        b = band_of(fg);
        la2_n[g] = lpsd[b] + 256; /* x 2 bins */
        if ((pf->v >> vband_of(fg)) & 1u) la2_n[g] += lmix2;
        if (la2_n[g] > lref) lref = la2_n[g];
        fdev[g] = (int32_t)((next_rng(s) % 1000u) * 800u / 1000u) - 400; /* +-25 Hz x 16 */
    }
    /* total relative power, Q30 per unit amplitude^2 of 2^lref */
    for (h = 0; h < nh; h++)
        if (la2_h[h] > -0x40000000) total += oc_vox_fx_exp2_q15(la2_h[h] - lref + 15 * 256) >> 1; /* amplitude^2 / 2 in Q30 */
    for (g = 0; g < OC_VOX_FX_NOISE; g++) total += oc_vox_fx_exp2_q15(la2_n[g] - lref + 15 * 256) >> 1;
    /* rms target: log2(rms^2) = 2 (e x 0.124572 - 0.94525) */
    if (pf->e == 0 || total == 0) {
        for (h = 0; h < OC_VOX_FX_MAX_HARM; h++) amp_h[h] = 0;
        for (g = 0; g < OC_VOX_FX_NOISE; g++) amp_n[g] = 0;
    } else {
        lrms_q8 = 2 * (((int32_t)pf->e * 31890 + 500) / 1000 - 242);
        ltot_q8 = oc_vox_fx_log2_q8_64(total) - 30 * 256; /* log2 of the total in units of 2^lref */
        lgain_q8 = lrms_q8 - ltot_q8 - lref;              /* log2(gain^2): amplitude^2 = 2^(la2 + lgain) */
        for (h = 0; h < OC_VOX_FX_MAX_HARM; h++) {
            /* Q12 amplitude: 2^((la2 + lgain) / 2 + 12) = exp2_q15(x - 15 x 256) */
            amp_h[h] = la2_h[h] > -0x40000000 ? (int32_t)oc_vox_fx_exp2_q15((la2_h[h] + lgain_q8) / 2 + 12 * 256 - 15 * 256) : 0;
        }
        for (g = 0; g < OC_VOX_FX_NOISE; g++) amp_n[g] = (int32_t)oc_vox_fx_exp2_q15((la2_n[g] + lgain_q8) / 2 + 12 * 256 - 15 * 256);
    }

    /* the fundamental glides from the previous frame's unless that was far away */
    inc_to = inc_of_hz_x16(f0_x16);
    {
        int32_t from = s->f0_prev_x16, dl = oc_vox_fx_log2_q8((uint32_t)f0_x16) - oc_vox_fx_log2_q8((uint32_t)from);
        if (!s->started || dl > 222 || dl < -222) from = f0_x16; /* |ln| > 0.6 */
        inc_from = inc_of_hz_x16(from);
    }
    dinc = (int32_t)(inc_to - inc_from) / 160;

    for (n = 0; n < 160; n++) {
        int32_t acc = 0;
        uint32_t ph1;
        s->phase1 += inc_from + (uint32_t)(dinc * (int32_t)n);
        ph1 = s->phase1;
        for (h = 0; h < nh; h++) {
            int32_t a = s->amp_h[h] + (int32_t)(((int64_t)(amp_h[h] - s->amp_h[h]) * (int32_t)n) / 160);
            if (a > 0) acc += (int32_t)(((int64_t)a * oc_vox_fx_sin_q15(ph1 * (h + 1u) + s->disp[h])) >> 15);
        }
        for (g = 0; g < OC_VOX_FX_NOISE; g++) {
            int32_t a = s->amp_n[g] + (int32_t)(((int64_t)(amp_n[g] - s->amp_n[g]) * (int32_t)n) / 160);
            int32_t fg = 1000 * (int32_t)g + 500 + s->fdev_n[g] + ((fdev[g] - s->fdev_n[g]) * (int32_t)n) / 160;
            s->phase_n[g] += inc_of_hz_x16(fg);
            if (a > 0) acc += (int32_t)(((int64_t)a * oc_vox_fx_sin_q15(s->phase_n[g])) >> 15);
        }
        if (pf->emid) {
            /* 10^(emid 0.75 / 20 (t - 0.5)): log2 = emid x 31.89 / 256 x (n - 80) / 160 */
            int32_t lg = (int32_t)(((int64_t)pf->emid * 31890 * ((int32_t)n - 80)) / (1000 * 160));
            acc = (int32_t)(((int64_t)acc * (int64_t)oc_vox_fx_exp2_q15(lg)) >> 15);
        }
        acc = (acc + (1 << 11)) >> 12;
        out[n] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }
    memcpy(s->amp_h, amp_h, sizeof(amp_h));
    memcpy(s->amp_n, amp_n, sizeof(amp_n));
    memcpy(s->fdev_n, fdev, sizeof(s->fdev));
    s->f0_prev_x16 = f0_x16;
    s->started = 1;
}
