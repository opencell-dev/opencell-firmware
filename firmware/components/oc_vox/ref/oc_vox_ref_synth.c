/* The reference synthesis (PLAN.md Task 6): a bank of sinusoids with
 * continuous phase. Voiced bands are the harmonics of the interpolated
 * fundamental, coherent (phase h x the fundamental's plus a fixed
 * dispersion); unvoiced bands are a 62.5 Hz grid of oscillators whose
 * frequencies wander a little every frame, which sounds like noise and
 * never clicks. Amplitudes come from the envelope at each oscillator's
 * frequency, mixed between the two sets by vmix, and interpolate linearly
 * across the frame; the whole frame is scaled to the energy e. */
#include <math.h>
#include <string.h>

#include "oc_vox_quant.h"
#include "oc_vox_ref_int.h"

static uint32_t next_rng(oc_vox_ref_syn_t *s)
{
    s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
    return s->rng;
}

void oc_vox_ref_syn_init(oc_vox_ref_syn_t *s)
{
    memset(s, 0, sizeof(*s));
    s->rng = 0x3C6EF372u;
    for (unsigned h = 0; h < OC_VOX_MAX_HARM; h++)
        s->disp[h] = ((float)(next_rng(s) % 10000u) / 10000.0f - 0.5f) * 2.0f * OC_VOX_PI;
    for (unsigned g = 0; g < OC_VOX_NOISE_OSC; g++)
        s->phase_n[g] = ((float)(next_rng(s) % 10000u) / 10000.0f) * 2.0f * OC_VOX_PI;
    s->f0_prev = 100.0f;
}

/* The envelope (0.75 dB units, mean removed) at frequency hz: linear
 * between band centres, flat beyond the outer centres, with the
 * post-filter's contrast (1.0 until tuned). */
static float env_at(const int8_t s[OC_VOX_BANDS], float hz)
{
    const float contrast = 1.0f; /* a post-filter (contrast > 1) is a Task 13 tuning option */
    unsigned b;
    if (hz <= oc_vox_ref_band_centre_hz(0)) return contrast * (float)s[0];
    for (b = 0; b + 1 < OC_VOX_BANDS; b++) {
        float c0 = oc_vox_ref_band_centre_hz(b), c1 = oc_vox_ref_band_centre_hz(b + 1);
        if (hz <= c1) return contrast * ((float)s[b] + ((float)s[b + 1] - (float)s[b]) * (hz - c0) / (c1 - c0));
    }
    return contrast * (float)s[OC_VOX_BANDS - 1];
}

static unsigned vband_of(float hz) { return hz < 600.0f ? 0 : hz < 1200.0f ? 1 : hz < 2400.0f ? 2 : 3; }

static unsigned band_of(float hz)
{
    unsigned b;
    for (b = 0; b + 1 < OC_VOX_BANDS; b++)
        if (hz < (float)oc_vox_band_edge_hz[b + 1]) return b;
    return OC_VOX_BANDS - 1u;
}

void oc_vox_ref_synth_frame(oc_vox_ref_syn_t *s, const oc_vox_pframe_t *pf, int16_t out[160])
{
    float f0 = (float)oc_vox_pitch_hz_x16(pf->p) / 16.0f;
    float rms = oc_vox_ref_e_to_rms(pf->e);
    float amp_h[OC_VOX_MAX_HARM], amp_n[OC_VOX_NOISE_OSC], fdev[OC_VOX_NOISE_OSC];
    float wsum[OC_VOX_BANDS], psd[OC_VOX_BANDS], bins[OC_VOX_BANDS];
    unsigned cnt[OC_VOX_BANDS];
    float mix = (float)pf->vmix / 7.0f * 0.5f; /* noise share in voiced bands */
    float harm_scale = sqrtf(1.0f - mix * mix);
    float total = 0.0f, gain, f0_from, emid_db = (float)pf->emid * 0.75f;
    unsigned nh = (unsigned)(3900.0f / f0), h, g, n, b;
    if (nh > OC_VOX_MAX_HARM) nh = OC_VOX_MAX_HARM;

    /* The envelope value of a band is the power per 31.25 Hz bin there
     * (what the analysis measures). Harmonics in a band take the shape of
     * the interpolated envelope but are renormalised so their mean power
     * is that density times the band's bins per harmonic; a noise
     * oscillator stands for its 2 bins. So the decoded spectrum has the
     * analysed density in every band whatever the pitch and voicing. */
    for (b = 0; b < OC_VOX_BANDS; b++) {
        wsum[b] = 0.0f; cnt[b] = 0;
        psd[b] = powf(10.0f, (float)pf->s[b] * 0.75f / 10.0f);
        bins[b] = ((float)oc_vox_band_edge_hz[b + 1] - (float)oc_vox_band_edge_hz[b]) / 31.25f;
    }
    for (h = 0; h < nh; h++) {
        float fh = (float)(h + 1) * f0;
        b = band_of(fh);
        amp_h[h] = powf(10.0f, env_at(pf->s, fh) * 0.75f / 10.0f); /* a power weight for now */
        wsum[b] += amp_h[h];
        cnt[b]++;
    }
    for (h = 0; h < OC_VOX_MAX_HARM; h++) {
        float fh = (float)(h + 1) * f0, a = 0.0f;
        if (h < nh && ((pf->v >> vband_of(fh)) & 1u)) {
            b = band_of(fh);
            a = sqrtf(amp_h[h] / (wsum[b] / (float)cnt[b]) * psd[b] * bins[b] / (float)cnt[b]) * harm_scale;
        }
        amp_h[h] = a;
        total += a * a * 0.5f;
    }
    for (g = 0; g < OC_VOX_NOISE_OSC; g++) {
        float fg = 62.5f * ((float)g + 0.5f), a;
        b = band_of(fg);
        a = sqrtf(psd[b] * 2.0f);
        if ((pf->v >> vband_of(fg)) & 1u) a *= mix;
        amp_n[g] = a;
        total += a * a * 0.5f;
        fdev[g] = ((float)(next_rng(s) % 1000u) / 1000.0f - 0.5f) * 50.0f;
    }
    gain = total > 1e-12f ? rms / sqrtf(total) : 0.0f;
    for (h = 0; h < OC_VOX_MAX_HARM; h++) amp_h[h] *= gain;
    for (g = 0; g < OC_VOX_NOISE_OSC; g++) amp_n[g] *= gain;

    /* glide the fundamental from the previous frame's unless that was far away (an onset) */
    f0_from = s->f0_prev;
    if (!s->started || fabsf(logf(f0 / f0_from)) > 0.6f) f0_from = f0;

    for (n = 0; n < 160; n++) {
        float t = (float)n / 160.0f, fn = f0_from + (f0 - f0_from) * t, x = 0.0f;
        float emid_gain = powf(10.0f, emid_db * (t - 0.5f) / 20.0f);
        s->phase1 += 2.0f * OC_VOX_PI * fn / 8000.0f;
        if (s->phase1 > 2.0f * OC_VOX_PI) s->phase1 -= 2.0f * OC_VOX_PI;
        for (h = 0; h < nh; h++) {
            float a = s->amp_h[h] + (amp_h[h] - s->amp_h[h]) * t;
            if (a > 0.0f) x += a * sinf((float)(h + 1) * s->phase1 + s->disp[h]);
        }
        for (g = 0; g < OC_VOX_NOISE_OSC; g++) {
            float a = s->amp_n[g] + (amp_n[g] - s->amp_n[g]) * t;
            float fg = 62.5f * ((float)g + 0.5f) + s->fdev_n[g] + (fdev[g] - s->fdev_n[g]) * t;
            s->phase_n[g] += 2.0f * OC_VOX_PI * fg / 8000.0f;
            if (s->phase_n[g] > 2.0f * OC_VOX_PI) s->phase_n[g] -= 2.0f * OC_VOX_PI;
            if (a > 0.0f) x += a * sinf(s->phase_n[g]);
        }
        x *= emid_gain;
        out[n] = (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (x >= 0 ? x + 0.5f : x - 0.5f));
    }
    memcpy(s->amp_h, amp_h, sizeof(amp_h));
    memcpy(s->amp_n, amp_n, sizeof(amp_n));
    memcpy(s->fdev_n, fdev, sizeof(fdev));
    s->f0_prev = f0;
    s->started = 1;
}
