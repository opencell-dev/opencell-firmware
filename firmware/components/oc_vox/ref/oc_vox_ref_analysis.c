/* The reference analysis (PLAN.md §1.2). Per frame: energy; a 256-point
 * spectrum; pitch candidates from the normalized cross-correlation over
 * a 320-sample segment with a harmonic-sum check; then, over the block,
 * dynamic programming picks the pitch track; voicing per band from the
 * spectrum's peak-to-valley ratio at the tracked harmonics; the envelope
 * as 12 warped-band log energies. */
#include <math.h>
#include <string.h>

#include "oc_vox_quant.h"
#include "oc_vox_ref_int.h"

void oc_vox_ref_ana_init(oc_vox_ref_ana_t *a)
{
    memset(a, 0, sizeof(*a));
    a->prev_p = 84;
}

typedef struct {
    uint8_t p[OC_VOX_REF_CANDS];
    float   cost[OC_VOX_REF_CANDS];
    unsigned n;
    float   ncc_max;
} cands_t;

/* Harmonic-sum score of f0 on a log-magnitude spectrum: peaks at the
 * harmonics against the valleys between them, in natural-log units,
 * averaged over the harmonics below 2 kHz; ~0 for noise and for an octave
 * error either way, 2-3 for a clean harmonic spectrum. */
static float harmonic_sum(const float *logmag, float f0)
{
    float sum = 0.0f;
    unsigned h, n = 0;
    for (h = 1; h * f0 < 2000.0f && h <= OC_VOX_MAX_HARM; h++) {
        float bp = h * f0 / 31.25f, bv1 = (h - 0.5f) * f0 / 31.25f, bv2 = (h + 0.5f) * f0 / 31.25f;
        unsigned kp = (unsigned)(bp + 0.5f), kv1 = (unsigned)(bv1 + 0.5f), kv2 = (unsigned)(bv2 + 0.5f);
        if (kv2 >= OC_VOX_REF_BINS) break;
        sum += logmag[kp] - 0.5f * (logmag[kv1] + logmag[kv2]);
        n++;
    }
    return n ? sum / (float)n : 0.0f;
}

static void add_cand(cands_t *c, uint8_t p, float cost)
{
    for (unsigned i = 0; i < c->n; i++)
        if (c->p[i] == p) { if (cost < c->cost[i]) c->cost[i] = cost; return; }
    if (c->n < OC_VOX_REF_CANDS) { c->p[c->n] = p; c->cost[c->n] = cost; c->n++; }
}

/* Candidates of one frame from its pitch segment seg[0..320) (the frame
 * is seg[80..240)) and its log-magnitude spectrum. */
static void pitch_candidates(const float *seg, const float *logmag, cands_t *c, uint8_t anchor)
{
    float ncc[OC_VOX_REF_LAG_MAX + 1];
    unsigned lag;
    c->n = 0;
    c->ncc_max = 0.0f;
    for (lag = OC_VOX_REF_LAG_MIN; lag <= OC_VOX_REF_LAG_MAX; lag++) {
        ncc[lag] = oc_vox_ref_ncc(seg + 160, 160, lag);
        if (ncc[lag] > c->ncc_max) c->ncc_max = ncc[lag];
    }
    /* the local maxima, best first, with their octave neighbours */
    for (unsigned pass = 0; pass < 3; pass++) {
        unsigned best = 0;
        float bestv = 0.25f;
        for (lag = OC_VOX_REF_LAG_MIN + 1; lag < OC_VOX_REF_LAG_MAX; lag++) {
            if (ncc[lag] > bestv && ncc[lag] >= ncc[lag - 1] && ncc[lag] >= ncc[lag + 1]) {
                unsigned taken = 0;
                uint8_t pl = oc_vox_pitch_index((int32_t)(8000.0f * 16.0f / (float)lag + 0.5f));
                for (unsigned i = 0; i < c->n; i++)
                    if (c->p[i] == pl) taken = 1;
                if (!taken) { best = lag; bestv = ncc[lag]; }
            }
        }
        if (best == 0) break;
        for (int oct = -1; oct <= 1; oct++) {
            float l = (float)best * (oct == 0 ? 1.0f : oct < 0 ? 2.0f : 0.5f);
            float f0 = 8000.0f / l;
            uint8_t p;
            float v, hs, cost;
            if (l < (float)OC_VOX_REF_LAG_MIN || l > (float)OC_VOX_REF_LAG_MAX) continue;
            p = oc_vox_pitch_index((int32_t)(f0 * 16.0f + 0.5f));
            v = ncc[(unsigned)(l + 0.5f)];
            hs = harmonic_sum(logmag, f0);
            if (hs < 0.0f) hs = 0.0f;
            if (hs > 3.0f) hs = 3.0f;
            cost = (1.0f - v) + 0.3f * (1.0f - hs / 3.0f) + (oct ? 0.05f : 0.0f);
            add_cand(c, p, cost);
        }
    }
    /* a frame that is not periodic enough holds the track: one flat candidate */
    if (c->ncc_max < 0.3f || c->n == 0) {
        c->n = 0;
        add_cand(c, anchor, 0.6f);
    }
}

/* The track: minimum total cost over the block, the previous block's last
 * pitch anchoring frame -1; transitions cost their size in octaves. */
static void track(const cands_t c[OC_VOX_FRAMES], uint8_t anchor, int have_anchor, uint8_t out[OC_VOX_FRAMES])
{
    float acc[OC_VOX_FRAMES][OC_VOX_REF_CANDS];
    uint8_t from[OC_VOX_FRAMES][OC_VOX_REF_CANDS];
    unsigned f, i, j;
    for (f = 0; f < OC_VOX_FRAMES; f++) {
        for (i = 0; i < c[f].n; i++) {
            float best = 1e9f;
            uint8_t bj = 0;
            if (f == 0) {
                best = c[0].cost[i] + (have_anchor ? 0.6f * fabsf((float)c[0].p[i] - (float)anchor) / 84.0f : 0.0f);
            } else {
                for (j = 0; j < c[f - 1].n; j++) {
                    float t = acc[f - 1][j] + c[f].cost[i] + 1.0f * fabsf((float)c[f].p[i] - (float)c[f - 1].p[j]) / 84.0f;
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

/* Per voicing band: the power at the harmonics against the power between
 * them, 0.5 for noise, toward 1 for a line spectrum. */
static float band_periodicity(const float *pow, float f0, unsigned vb, float floor_p)
{
    static const float LO[4] = { 0.0f, 600.0f, 1200.0f, 2400.0f }, HI[4] = { 600.0f, 1200.0f, 2400.0f, 4000.0f };
    float pk = 0.0f, vl = 0.0f;
    for (unsigned h = 1; h * f0 < HI[vb]; h++) {
        float fh = h * f0, fv = (h + 0.5f) * f0;
        unsigned kp, kv;
        if (fh < LO[vb]) continue;
        kp = (unsigned)(fh / 31.25f + 0.5f);
        kv = (unsigned)(fv / 31.25f + 0.5f);
        if (kv >= OC_VOX_REF_BINS) break;
        pk += pow[kp];
        vl += pow[kv];
    }
    if (pk + vl <= 0.0f || pk + vl < floor_p) return 0.5f; /* nothing there: noise, not a decision */
    return pk / (pk + vl);
}

void oc_vox_ref_analyse(oc_vox_ref_ana_t *a, const int16_t pcm[960], oc_vox_pframe_t out[OC_VOX_FRAMES])
{
    static float hann[256];
    static int hann_ready = 0;
    float pow[OC_VOX_FRAMES][OC_VOX_REF_BINS], logmag[OC_VOX_FRAMES][OC_VOX_REF_BINS];
    cands_t cands[OC_VOX_FRAMES];
    uint8_t pitch[OC_VOX_FRAMES];
    unsigned f, i;
    if (!hann_ready) { oc_vox_ref_hann256(hann); hann_ready = 1; }

    /* the history, then the new block through the DC blocker */
    memmove(a->buf, a->buf + 960, OC_VOX_REF_HIST * sizeof(float));
    for (i = 0; i < 960; i++) {
        float x = (float)pcm[i];
        float y = x - a->hp_x1 + 0.985f * a->hp_y1;
        a->hp_x1 = x;
        a->hp_y1 = y;
        a->buf[OC_VOX_REF_HIST + i] = y;
    }
    memset(out, 0, sizeof(oc_vox_pframe_t) * OC_VOX_FRAMES);

    for (f = 0; f < OC_VOX_FRAMES; f++) {
        unsigned off = OC_VOX_REF_DELAY + 160u * f; /* the frame's first sample in buf */
        const float *frame = a->buf + off;
        float win[256], re[129], im[129];
        double acc = 0, acc1 = 0, acc2 = 0;
        for (i = 0; i < 160; i++) {
            acc += (double)frame[i] * frame[i];
            if (i < 80) acc1 += (double)frame[i] * frame[i]; else acc2 += (double)frame[i] * frame[i];
        }
        out[f].e = oc_vox_ref_rms_to_e((float)sqrt(acc / 160.0));
        {
            float d = (float)(10.0 * log10((acc2 + 1.0) / (acc1 + 1.0))) / 0.75f;
            out[f].emid = (int8_t)(d > 48.0f ? 48 : d < -48.0f ? -48 : d);
        }
        for (i = 0; i < 256; i++) win[i] = frame[(int)i - 48] * hann[i];
        oc_vox_ref_fft256(win, re, im);
        for (i = 0; i < OC_VOX_REF_BINS; i++) {
            pow[f][i] = re[i] * re[i] + im[i] * im[i];
            logmag[f][i] = 0.5f * logf(pow[f][i] + 1e-3f);
        }
        pitch_candidates(frame - 80, logmag[f], &cands[f], a->prev_p);
    }
    track(cands, a->prev_p, a->have_prev, pitch);

    for (f = 0; f < OC_VOX_FRAMES; f++) {
        float f0 = (float)oc_vox_pitch_hz_x16(pitch[f]) / 16.0f, ncc = cands[f].ncc_max;
        static const float THR[4] = { 0.58f, 0.60f, 0.64f, 0.68f };
        uint8_t v = 0;
        float sdb[OC_VOX_BANDS], mean = 0.0f;
        out[f].p = pitch[f];
        float pmax = 0.0f;
        for (i = 0; i < OC_VOX_REF_BINS; i++) if (pow[f][i] > pmax) pmax = pow[f][i];
        for (unsigned vb = 0; vb < OC_VOX_VBANDS; vb++) {
            float per = band_periodicity(pow[f], f0, vb, pmax * (1.0f / 1048576.0f)); /* -60 dB of the peak bin */
            float thr = THR[vb] - ((a->prev_v >> vb) & 1u ? 0.05f : 0.0f);
            if ((ncc > 0.45f && per > thr) || (vb == 0 && ncc > 0.65f)) v |= (uint8_t)(1u << vb);
        }
        if (out[f].e < 8) v = 0; /* below -90 dBFS nothing is voiced */
        out[f].v = v;
        a->prev_v = v;
        out[f].vmix = ncc >= 0.85f ? 0 : ncc >= 0.75f ? 1 : ncc >= 0.65f ? 2 : ncc >= 0.55f ? 3 : ncc >= 0.45f ? 4 : 6;
        for (unsigned b = 0; b < OC_VOX_BANDS; b++) {
            unsigned k0 = (unsigned)((float)oc_vox_band_edge_hz[b] / 31.25f + 0.5f), k1 = (unsigned)((float)oc_vox_band_edge_hz[b + 1] / 31.25f + 0.5f), k, n = 0;
            float p = 0.0f;
            if (k1 > 128) k1 = 128;
            for (k = k0; k < k1; k++) { p += pow[f][k]; n++; }
            sdb[b] = oc_vox_ref_db_of_power(n ? p / (float)n : 0.0f);
            mean += sdb[b];
        }
        /* a band 60 dB under the loudest is as good as empty: floored there, so
         * the two implementations agree where their noise floors differ */
        {
            float smax = -1000.0f;
            for (unsigned b = 0; b < OC_VOX_BANDS; b++) if (sdb[b] > smax) smax = sdb[b];
            mean = 0.0f;
            for (unsigned b = 0; b < OC_VOX_BANDS; b++) { if (sdb[b] < smax - 60.0f) sdb[b] = smax - 60.0f; mean += sdb[b]; }
        }
        mean /= (float)OC_VOX_BANDS;
        for (unsigned b = 0; b < OC_VOX_BANDS; b++) {
            float u = (sdb[b] - mean) / 0.75f;
            out[f].s[b] = (int8_t)(u > 127.0f ? 127 : u < -127.0f ? -127 : (u >= 0 ? u + 0.5f : u - 0.5f));
        }
    }
    a->prev_p = pitch[OC_VOX_FRAMES - 1];
    a->have_prev = 1;
}
