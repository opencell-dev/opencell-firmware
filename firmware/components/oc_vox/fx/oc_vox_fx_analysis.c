/* The fixed-point analysis: the reference's (ref/oc_vox_ref_analysis.c)
 * in integers. Energies are int64 sums; spectra are the FFT's power per
 * bin scaled to 32 bits by a per-frame shift (only ratios within a frame
 * are used, and the envelope is mean-removed); costs are Q15; logs are
 * Q8 log2 and come out in 0.75 dB units through oc_vox_fx_units_of_log2_q8. */
#include <string.h>

#include "oc_vox_fx_int.h"
#include "oc_vox_quant.h"

#define Q15(x) ((int32_t)((x) * 32768.0 + 0.5))

void oc_vox_fx_ana_init(oc_vox_fx_ana_t *a)
{
    memset(a, 0, sizeof(*a));
    a->prev_p = 84;
}

typedef struct {
    uint8_t  p[OC_VOX_FX_CANDS];
    int32_t  cost[OC_VOX_FX_CANDS]; /* Q15 */
    unsigned n;
    int32_t  ncc_max;              /* Q15 */
} cands_t;

/* The reference's harmonic sum, 0.5 ln(power) at the harmonics against
 * the valleys: 0.5 ln 2 = 0.34657 per Q8 log2 unit. Returned in Q8 "ln"
 * units, clamped to 0..3 (0..768). */
static int32_t harmonic_sum(const uint32_t *pow, int32_t f0_x16)
{
    int64_t sum = 0;
    unsigned n = 0, h;
    for (h = 1; (int32_t)h * f0_x16 < 2000 * 16 && h <= OC_VOX_FX_MAX_HARM; h++) {
        /* bins: f / 31.25 = f_x16 / 500 */
        unsigned kp = (unsigned)(((int32_t)h * f0_x16 + 250) / 500);
        unsigned kv1 = (unsigned)(((2 * (int32_t)h - 1) * f0_x16 / 2 + 250) / 500);
        unsigned kv2 = (unsigned)(((2 * (int32_t)h + 1) * f0_x16 / 2 + 250) / 500);
        if (kv2 >= OC_VOX_FX_BINS) break;
        sum += oc_vox_fx_log2_q8(pow[kp]) - (oc_vox_fx_log2_q8(pow[kv1]) + oc_vox_fx_log2_q8(pow[kv2])) / 2;
        n++;
    }
    if (n == 0) return 0;
    {
        int32_t v = (int32_t)((sum * 22713) / (int64_t)n >> 16); /* x 0.34657 */
        return v < 0 ? 0 : v > 768 ? 768 : v;
    }
}

static void add_cand(cands_t *c, uint8_t p, int32_t cost)
{
    for (unsigned i = 0; i < c->n; i++)
        if (c->p[i] == p) { if (cost < c->cost[i]) c->cost[i] = cost; return; }
    if (c->n < OC_VOX_FX_CANDS) { c->p[c->n] = p; c->cost[c->n] = cost; c->n++; }
}

static void pitch_candidates(const int16_t *seg, const uint32_t *pow, cands_t *c, uint8_t anchor)
{
    int32_t ncc[OC_VOX_FX_LAG_MAX + 1];
    unsigned lag;
    c->n = 0;
    c->ncc_max = 0;
    for (lag = OC_VOX_FX_LAG_MIN; lag <= OC_VOX_FX_LAG_MAX; lag++) {
        ncc[lag] = oc_vox_fx_ncc_q15(seg + 160, 160, lag);
        if (ncc[lag] > c->ncc_max) c->ncc_max = ncc[lag];
    }
    for (unsigned pass = 0; pass < 3; pass++) {
        unsigned best = 0;
        int32_t bestv = Q15(0.25);
        for (lag = OC_VOX_FX_LAG_MIN + 1; lag < OC_VOX_FX_LAG_MAX; lag++) {
            if (ncc[lag] > bestv && ncc[lag] >= ncc[lag - 1] && ncc[lag] >= ncc[lag + 1]) {
                unsigned taken = 0;
                uint8_t pl = oc_vox_pitch_index((128000 + (int32_t)lag / 2) / (int32_t)lag);
                for (unsigned i = 0; i < c->n; i++)
                    if (c->p[i] == pl) taken = 1;
                if (!taken) { best = lag; bestv = ncc[lag]; }
            }
        }
        if (best == 0) break;
        for (int oct = -1; oct <= 1; oct++) {
            /* the lag, doubled or halved: a halved odd lag is fractional, so
             * the frequency is computed from the lag itself (16000 / best) and
             * the correlation is read at the rounded lag, as the reference does */
            unsigned li = oct == 0 ? best : oct < 0 ? best * 2u : (best + 1u) / 2u;
            int32_t f0_x16, v, hs, cost;
            uint8_t p;
            if (oct > 0 && best < 2u * OC_VOX_FX_LAG_MIN) continue;  /* half lag under 20 */
            if (oct < 0 && best > OC_VOX_FX_LAG_MAX / 2u) continue;   /* double lag over 160 */
            f0_x16 = oct == 0 ? (128000 + (int32_t)best / 2) / (int32_t)best
                   : oct < 0  ? (64000 + (int32_t)best / 2) / (int32_t)best
                              : (256000 + (int32_t)best / 2) / (int32_t)best;
            p = oc_vox_pitch_index(f0_x16);
            v = ncc[li];
            hs = harmonic_sum(pow, f0_x16);
            cost = (32768 - v) + (int32_t)(((int64_t)Q15(0.3) * (768 - hs)) / 768) + (oct ? Q15(0.05) : 0);
            add_cand(c, p, cost);
        }
    }
    if (c->ncc_max < Q15(0.3) || c->n == 0) {
        c->n = 0;
        add_cand(c, anchor, Q15(0.6));
    }
}

static int32_t absdiff(int a, int b) { return a > b ? a - b : b - a; }

static void track(const cands_t c[OC_VOX_FRAMES], uint8_t anchor, int have_anchor, uint8_t out[OC_VOX_FRAMES])
{
    int32_t acc[OC_VOX_FRAMES][OC_VOX_FX_CANDS];
    uint8_t from[OC_VOX_FRAMES][OC_VOX_FX_CANDS];
    unsigned f, i, j;
    for (f = 0; f < OC_VOX_FRAMES; f++) {
        for (i = 0; i < c[f].n; i++) {
            int32_t best = 0x7FFFFFFF;
            uint8_t bj = 0;
            if (f == 0) {
                best = c[0].cost[i] + (have_anchor ? (int32_t)((int64_t)Q15(0.6) * absdiff(c[0].p[i], anchor) / 84) : 0);
            } else {
                for (j = 0; j < c[f - 1].n; j++) {
                    int32_t t = acc[f - 1][j] + c[f].cost[i] + (int32_t)(32768 * absdiff(c[f].p[i], c[f - 1].p[j]) / 84);
                    if (t < best) { best = t; bj = (uint8_t)j; }
                }
            }
            acc[f][i] = best;
            from[f][i] = bj;
        }
    }
    {
        unsigned bi = 0;
        for (i = 1; i < c[OC_VOX_FRAMES - 1].n; i++)
            if (acc[OC_VOX_FRAMES - 1][i] < acc[OC_VOX_FRAMES - 1][bi]) bi = i;
        for (f = OC_VOX_FRAMES; f-- > 0;) {
            out[f] = c[f].p[bi];
            bi = from[f][bi];
        }
    }
}

/* pk / (pk + vl) in Q15 for a voicing band. */
static int32_t band_periodicity(const uint32_t *pow, int32_t f0_x16, unsigned vb, uint64_t floor_p)
{
    static const int32_t LO[4] = { 0, 600 * 16, 1200 * 16, 2400 * 16 }, HI[4] = { 600 * 16, 1200 * 16, 2400 * 16, 4000 * 16 };
    uint64_t pk = 0, vl = 0;
    for (unsigned h = 1; (int32_t)h * f0_x16 < HI[vb]; h++) {
        int32_t fh = (int32_t)h * f0_x16, fv = fh + f0_x16 / 2;
        unsigned kp, kv;
        if (fh < LO[vb]) continue;
        kp = (unsigned)((fh + 250) / 500);
        kv = (unsigned)((fv + 250) / 500);
        if (kv >= OC_VOX_FX_BINS) break;
        pk += pow[kp];
        vl += pow[kv];
    }
    if (pk + vl == 0 || pk + vl < floor_p) return Q15(0.5); /* nothing there: noise, not a decision */
    return (int32_t)((pk << 15) / (pk + vl));
}

void oc_vox_fx_analyse(oc_vox_fx_ana_t *a, const int16_t pcm[960], oc_vox_pframe_t out[OC_VOX_FRAMES])
{
    cands_t cands[OC_VOX_FRAMES];
    uint8_t pitch[OC_VOX_FRAMES];
    unsigned f, i;

    memmove(a->buf, a->buf + 960, OC_VOX_FX_HIST * sizeof(int16_t));
    for (i = 0; i < 960; i++) {
        int32_t x = pcm[i];
        /* 0.985 y1 by a division (toward zero), not a shift: a floor would bias
         * the filter by -0.5 a sample, a -33 offset through the feedback, and
         * hold -66 forever on silence */
        int32_t y = x - a->hp_x1 + (int32_t)(((int64_t)a->hp_y1 * 32276) / 32768);
        a->hp_x1 = x;
        a->hp_y1 = y;
        a->buf[OC_VOX_FX_HIST + i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    memset(out, 0, sizeof(oc_vox_pframe_t) * OC_VOX_FRAMES);

    for (f = 0; f < OC_VOX_FRAMES; f++) {
        unsigned off = OC_VOX_FX_DELAY + 160u * f;
        const int16_t *frame = a->buf + off;
        int32_t *xr = a->fft_re, *xi = a->fft_im;
        int64_t acc = 0, acc1 = 0, acc2 = 0;
        uint64_t pmax = 0;
        for (i = 0; i < 160; i++) {
            int64_t sq = (int64_t)frame[i] * frame[i];
            acc += sq;
            if (i < 80) acc1 += sq; else acc2 += sq;
        }
        if (acc == 0) out[f].e = 0;
        else {
            /* e = 4.0137 log2(mean square) + 7.59 (-96 dBFS .. -0.75 dBFS in 0.75 dB) */
            int64_t num = (int64_t)oc_vox_fx_log2_q8_64((uint64_t)(acc / 160)) * 1028 + 497254 + (1 << 15);
            int32_t e = (int32_t)(num >> 16);
            out[f].e = (uint8_t)(e < 0 ? 0 : e > 127 ? 127 : e);
        }
        {
            int32_t d = oc_vox_fx_units_of_log2_q8(oc_vox_fx_log2_q8_64((uint64_t)acc2 + 1u) - oc_vox_fx_log2_q8_64((uint64_t)acc1 + 1u));
            out[f].emid = (int8_t)(d > 48 ? 48 : d < -48 ? -48 : d);
        }
        for (i = 0; i < 256; i++) {
            xr[i] = ((int32_t)frame[(int)i - 48] * oc_vox_fx_hann256[i]) >> 7;
            xi[i] = 0;
        }
        oc_vox_fx_fft256(xr, xi);
        for (i = 0; i < OC_VOX_FX_BINS; i++) {
            uint64_t p = (uint64_t)((int64_t)xr[i] * xr[i] + (int64_t)xi[i] * xi[i]);
            if (p > pmax) pmax = p;
        }
        a->shift[f] = 0;
        while ((pmax >> a->shift[f]) > 0x7FFFFFFFu) a->shift[f]++;
        for (i = 0; i < OC_VOX_FX_BINS; i++) {
            uint64_t p = (uint64_t)((int64_t)xr[i] * xr[i] + (int64_t)xi[i] * xi[i]);
            a->pow[f][i] = (uint32_t)(p >> a->shift[f]);
        }
        pitch_candidates(frame - 80, a->pow[f], &cands[f], a->prev_p);
    }
    track(cands, a->prev_p, a->have_prev, pitch);

    for (f = 0; f < OC_VOX_FRAMES; f++) {
        int32_t f0_x16 = oc_vox_pitch_hz_x16(pitch[f]), ncc = cands[f].ncc_max;
        static const int32_t THR[4] = { Q15(0.58), Q15(0.60), Q15(0.64), Q15(0.68) };
        uint8_t v = 0;
        int32_t lb[OC_VOX_BANDS], mean = 0;
        out[f].p = pitch[f];
        uint32_t pmax = 0;
        for (i = 0; i < OC_VOX_FX_BINS; i++) if (a->pow[f][i] > pmax) pmax = a->pow[f][i];
        for (unsigned vb = 0; vb < OC_VOX_VBANDS; vb++) {
            int32_t per = band_periodicity(a->pow[f], f0_x16, vb, (uint64_t)pmax >> 20); /* -60 dB of the peak bin */
            int32_t thr = THR[vb] - (((a->prev_v >> vb) & 1u) ? Q15(0.05) : 0);
            if ((ncc > Q15(0.45) && per > thr) || (vb == 0 && ncc > Q15(0.65))) v |= (uint8_t)(1u << vb);
        }
        if (out[f].e < 8) v = 0;
        out[f].v = v;
        a->prev_v = v;
        out[f].vmix = ncc >= Q15(0.85) ? 0 : ncc >= Q15(0.75) ? 1 : ncc >= Q15(0.65) ? 2 : ncc >= Q15(0.55) ? 3 : ncc >= Q15(0.45) ? 4 : 6;
        for (unsigned b = 0; b < OC_VOX_BANDS; b++) {
            unsigned k0 = (oc_vox_band_edge_hz[b] * 16u + 250u) / 500u, k1 = (oc_vox_band_edge_hz[b + 1] * 16u + 250u) / 500u, k, n = 0;
            uint64_t p = 0;
            if (k1 > 128) k1 = 128;
            for (k = k0; k < k1; k++) { p += a->pow[f][k]; n++; }
            /* log2 of the mean power per bin, Q8 (the frame's shift cancels in the mean removal) */
            lb[b] = n ? oc_vox_fx_log2_q8_64(p / n) : 0;
            mean += lb[b];
        }
        {
            /* floored 60 dB (19.93 octaves of power, 5102 in Q8 log2) under the loudest band */
            int32_t lmax = -0x40000000;
            for (unsigned b = 0; b < OC_VOX_BANDS; b++) if (lb[b] > lmax) lmax = lb[b];
            mean = 0;
            for (unsigned b = 0; b < OC_VOX_BANDS; b++) { if (lb[b] < lmax - 5102) lb[b] = lmax - 5102; mean += lb[b]; }
        }
        mean = (mean + (mean >= 0 ? (int32_t)OC_VOX_BANDS / 2 : -(int32_t)OC_VOX_BANDS / 2)) / (int32_t)OC_VOX_BANDS;
        for (unsigned b = 0; b < OC_VOX_BANDS; b++) {
            int32_t u = oc_vox_fx_units_of_log2_q8(lb[b] - mean);
            out[f].s[b] = (int8_t)(u > 127 ? 127 : u < -127 ? -127 : u);
        }
    }
    a->prev_p = pitch[OC_VOX_FRAMES - 1];
    a->have_prev = 1;
}
