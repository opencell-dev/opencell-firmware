#include "oc_vox_quant.h"

#include <string.h>

#include "oc_vox_cb.h"

/* 2^(k/84) in Q14, k = 0..83 (one octave in 84 steps). */
static const uint16_t OCT_Q14[84] = {
    16384, 16520, 16657, 16795, 16934, 17074, 17216, 17358, 17502, 17647, 17793, 17941, 18089, 18239, 18390, 18543,
    18696, 18851, 19008, 19165, 19324, 19484, 19645, 19808, 19972, 20138, 20305, 20473, 20643, 20814, 20986, 21160,
    21335, 21512, 21690, 21870, 22051, 22234, 22418, 22604, 22791, 22980, 23170, 23362, 23556, 23751, 23948, 24146,
    24347, 24548, 24752, 24957, 25164, 25372, 25582, 25794, 26008, 26223, 26441, 26660, 26881, 27103, 27328, 27554,
    27783, 28013, 28245, 28479, 28715, 28953, 29193, 29435, 29679, 29925, 30173, 30423, 30675, 30929, 31185, 31444,
    31704, 31967, 32232, 32499,
};

int32_t oc_vox_pitch_hz_x16(uint8_t p)
{
    unsigned oct = p / 84u, k = p % 84u;
    /* 50 Hz x 16 = 800; 800 x 2^(k/84) x 2^oct */
    return (int32_t)(((800u * (uint32_t)OCT_Q14[k]) >> 14) << oct);
}

uint8_t oc_vox_pitch_index(int32_t hz_x16)
{
    /* binary search on the monotonic table */
    unsigned lo = 0, hi = 255;
    if (hz_x16 <= oc_vox_pitch_hz_x16(0)) return 0;
    if (hz_x16 >= oc_vox_pitch_hz_x16(255)) return 255;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (oc_vox_pitch_hz_x16((uint8_t)mid) <= hz_x16) lo = mid; else hi = mid;
    }
    return (hz_x16 - oc_vox_pitch_hz_x16((uint8_t)lo) < oc_vox_pitch_hz_x16((uint8_t)hi) - hz_x16) ? (uint8_t)lo : (uint8_t)hi;
}

uint8_t oc_vox_vclass_of_mask(uint8_t v)
{
    if (v & 0x0Cu) return 3;
    if (v & 0x02u) return 2;
    if (v & 0x01u) return 1;
    return 0;
}

uint8_t oc_vox_vmask_of_class(uint8_t cls)
{
    static const uint8_t M[4] = { 0x0, 0x1, 0x3, 0xF };
    return M[cls & 3u];
}

/* cos(pi (2n + 1) k / 24) in Q14, k = 0..11 rows, n = 0..11. */
static const int16_t DCT_Q14[12][12] = {
    { 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384 },
    { 16244, 15137, 12998, 9974, 6270, 2139, -2139, -6270, -9974, -12998, -15137, -16244 },
    { 15826, 11585, 4240, -4240, -11585, -15826, -15826, -11585, -4240, 4240, 11585, 15826 },
    { 15137, 6270, -6270, -15137, -15137, -6270, 6270, 15137, 15137, 6270, -6270, -15137 },
    { 14189, 0, -14189, -14189, 0, 14189, 14189, 0, -14189, -14189, 0, 14189 },
    { 12998, -6270, -16244, -2139, 15137, 9974, -9974, -15137, 2139, 16244, 6270, -12998 },
    { 11585, -11585, -11585, 11585, 11585, -11585, -11585, 11585, 11585, -11585, -11585, 11585 },
    { 9974, -15137, -2139, 16244, -6270, -12998, 12998, 6270, -16244, 2139, 15137, -9974 },
    { 8192, -16384, 8192, 8192, -16384, 8192, 8192, -16384, 8192, 8192, -16384, 8192 },
    { 6270, -15137, 15137, -6270, -6270, 15137, -15137, 6270, 6270, -15137, 15137, -6270 },
    { 4240, -11585, 15826, -15826, 11585, -4240, -4240, 11585, -15826, 15826, -11585, 4240 },
    { 2139, -6270, 9974, -12998, 15137, -16244, 16244, -15137, 12998, -9974, 6270, -2139 },
};

void oc_vox_env_dct(const int8_t s[OC_VOX_BANDS], int32_t c[OC_VOX_BANDS])
{
    c[0] = 0;
    for (unsigned k = 1; k < OC_VOX_BANDS; k++) {
        int32_t acc = 0;
        for (unsigned n = 0; n < OC_VOX_BANDS; n++) acc += (int32_t)s[n] * DCT_Q14[k][n];
        c[k] = (acc + (1 << 13)) >> 14;
    }
}

void oc_vox_env_idct(const int32_t c[OC_VOX_BANDS], int8_t s[OC_VOX_BANDS])
{
    for (unsigned n = 0; n < OC_VOX_BANDS; n++) {
        int32_t acc = 0;
        for (unsigned k = 1; k < OC_VOX_BANDS; k++) acc += c[k] * DCT_Q14[k][n];
        /* x 2/12, then out of Q14 */
        int32_t v = (acc / 6 + (1 << 13)) >> 14;
        s[n] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
}

/* Energy deltas in 0.75 dB units by field width (2, 3, 4 bits). */
static const int16_t E_D2[4] = { -8, -2, 0, 6 };
static const int16_t E_D3[8] = { -16, -8, -4, -2, 0, 4, 8, 16 };
static const int16_t E_D4[16] = { -24, -16, -12, -8, -6, -4, -2, 0, 1, 2, 4, 6, 8, 12, 16, 24 };
/* Half-frame energy step (OVX-3200), 3 bits. */
static const int16_t E_MID3[8] = { -16, -8, -4, 0, 4, 8, 16, 32 };
/* Pitch deltas in 1/84 octave by width (3, 4, 5 bits). */
static const int16_t P_D3[8] = { -42, -14, -4, 0, 4, 14, 42, 84 };
static const int16_t P_D4[16] = { -84, -42, -24, -14, -8, -4, -2, 0, 2, 4, 8, 14, 24, 42, 84, 126 };
static const int16_t P_D5[32] = { -168, -126, -84, -60, -42, -32, -24, -18, -14, -10, -7, -5, -3, -2, -1, 0,
                                  1, 2, 3, 5, 7, 10, 14, 18, 24, 32, 42, 60, 84, 126, 168, 252 };

const int16_t *oc_vox_e_delta_table(unsigned bits) { return bits == 2 ? E_D2 : bits == 3 ? E_D3 : E_D4; }
const int16_t *oc_vox_p_delta_table(unsigned bits) { return bits == 3 ? P_D3 : bits == 4 ? P_D4 : P_D5; }

unsigned oc_vox_delta_index(const int16_t *table, unsigned bits, int32_t d)
{
    unsigned n = 1u << bits, best = 0;
    int32_t bestdist = 0x7FFFFFFF;
    for (unsigned i = 0; i < n; i++) {
        int32_t dist = d - table[i];
        if (dist < 0) dist = -dist;
        if (dist < bestdist) { bestdist = dist; best = i; }
    }
    return best;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The envelope stages. Each stage codes some DCT coefficients with a
 * uniform quantizer: mid-rise for 1 and 2 bits (levels at odd multiples
 * of step/2, no zero), mid-tread for 3+ bits (levels -2^(b-1) .. 2^(b-1)-1
 * times the step). The base step is the 2-bit step of each coefficient,
 * sized to its spread over speech-like envelopes; it halves for each bit
 * beyond two and doubles at one, so every width covers the same range.
 * Stage 4 adds one bit to each of c1..c8: a quarter-step correction. */
typedef struct { uint8_t coef, bits; } sub_t;
#define SUBS 4
static const sub_t S1_SUBS[SUBS] = { { 1, 3 }, { 2, 3 }, { 3, 2 }, { 4, 2 } };
static const uint16_t STEP_AT_2BITS[12] = { 0, 128, 192, 96, 48, 16, 16, 16, 12, 12, 12, 12 }; /* 0.75 dB-sum units */

static unsigned s2_subs(unsigned bits, sub_t out[SUBS])
{
    static const uint8_t T[9][3] = { { 0 }, { 0 }, { 0 }, { 2, 1, 0 }, { 2, 2, 0 }, { 2, 2, 1 }, { 2, 2, 2 }, { 3, 2, 2 }, { 3, 3, 2 } };
    unsigned n = 0;
    for (unsigned i = 0; i < 3; i++)
        if (T[bits][i]) { out[n].coef = (uint8_t)(5 + i); out[n].bits = T[bits][i]; n++; }
    return n;
}

static unsigned s3_subs(unsigned bits, sub_t out[SUBS])
{
    static const uint8_t T[9][4] = { { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 2, 1, 1, 1 }, { 2, 2, 1, 1 }, { 2, 2, 2, 1 }, { 2, 2, 2, 2 } };
    unsigned n = 0;
    for (unsigned i = 0; i < 4; i++)
        if (T[bits][i]) { out[n].coef = (uint8_t)(8 + i); out[n].bits = T[bits][i]; n++; }
    return n;
}

static int32_t sub_step(const sub_t *s)
{
    int32_t step = STEP_AT_2BITS[s->coef];
    if (s->bits > 2) step >>= (s->bits - 2); else if (s->bits == 1) step <<= 1;
    return step;
}

/* Quantize residual r of a coefficient with sub-quantizer s: the index and
 * the reconstructed value. */
static unsigned sub_quant(const sub_t *s, int32_t r, int32_t *recon)
{
    int32_t step = sub_step(s);
    unsigned idx;
    if (s->bits <= 2) {
        /* mid-rise: levels (2 i + 1 - 2^b) step / 2 */
        int32_t n = 1 << s->bits, q = (r >= 0 ? r : r - step + 1) / step + n / 2; /* floor(r / step) + n/2 */
        q = clamp(q, 0, n - 1);
        idx = (unsigned)q;
        *recon = (2 * q + 1 - n) * step / 2;
    } else {
        int32_t half = 1 << (s->bits - 1);
        int32_t q = (r >= 0 ? (r + step / 2) : (r - step / 2)) / step;
        q = clamp(q, -half, half - 1);
        idx = (unsigned)(q + half);
        *recon = q * step;
    }
    return idx;
}

static int32_t sub_dequant(const sub_t *s, unsigned idx)
{
    int32_t step = sub_step(s);
    if (s->bits <= 2) return (2 * (int32_t)idx + 1 - (1 << s->bits)) * step / 2;
    return ((int32_t)idx - (1 << (s->bits - 1))) * step;
}

#if OC_VOX_CB_TRAINED
/* Stage 1 as a vector quantizer: the codeword nearest to s (squared
 * error over the 12 bands), and its DCT, which the later stages refine. */
static unsigned cb_nearest(const int8_t s[OC_VOX_BANDS])
{
    unsigned best = 0;
    int32_t bestd = 0x7FFFFFFF;
    for (unsigned i = 0; i < OC_VOX_CB_S1_SIZE; i++) {
        int32_t d = 0;
        for (unsigned b = 0; b < OC_VOX_BANDS; b++) {
            int32_t x = (int32_t)s[b] - oc_vox_cb_s1[i][b];
            d += x * x;
        }
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static void cb_add(int32_t c[OC_VOX_BANDS], unsigned idx)
{
    int32_t cc[OC_VOX_BANDS];
    oc_vox_env_dct(oc_vox_cb_s1[idx], cc);
    for (unsigned k = 1; k < OC_VOX_BANDS; k++) c[k] += cc[k];
}
#endif

static unsigned stage_subs(unsigned kind, unsigned bits, sub_t out[SUBS])
{
    if (kind == OC_VOX_F_S1) { memcpy(out, S1_SUBS, sizeof(S1_SUBS)); return SUBS; }
    if (kind == OC_VOX_F_S2) return s2_subs(bits, out);
    if (kind == OC_VOX_F_S3) return s3_subs(bits, out);
    return 0;
}

/* Stage 4: one sign bit per coefficient c1..c8, a quarter of its stage's step. */
static int32_t s4_step(unsigned coef)
{
    sub_t subs[SUBS];
    unsigned n = coef <= 4 ? stage_subs(OC_VOX_F_S1, 10, subs) : s2_subs(8, subs), i;
    if (coef > 7) { n = s3_subs(8, subs); }
    for (i = 0; i < n; i++)
        if (subs[i].coef == coef) return sub_step(&subs[i]) / 4;
    return 8;
}

/* The per-block walk shared by quantization and reconstruction: which
 * frames a parameter is transmitted on is implicit in the layout. */
typedef struct {
    int32_t c[OC_VOX_FRAMES][OC_VOX_BANDS]; /* reconstructed DCT coefficients so far */
    uint8_t s_sent[OC_VOX_FRAMES], e_sent[OC_VOX_FRAMES], p_sent[OC_VOX_FRAMES], v_sent[OC_VOX_FRAMES];
    uint8_t mix_sent[OC_VOX_FRAMES], mid_sent[OC_VOX_FRAMES];
    int32_t e_ref, p_ref; /* the last reconstructed transmitted value */
} walk_t;

static void walk_init(walk_t *w)
{
    memset(w, 0, sizeof(*w));
    w->e_ref = -1;
    w->p_ref = -1;
}

/* Interpolate parameter x over the frames where sent[f] is 0: linear
 * between the nearest sent frames, the previous block's last frame
 * standing in before the first; hold after the last; the first sent
 * frame's value before it when there is no prev. */
static void interp_u8(uint8_t *x, const uint8_t *sent, int prev /* -1: none */)
{
    for (unsigned f = 0; f < OC_VOX_FRAMES; f++) {
        if (sent[f]) continue;
        int a = -1, b = -1, fa = -1, fb = -1;
        for (int g = (int)f - 1; g >= 0; g--) if (sent[g]) { a = x[g]; fa = g; break; }
        if (a < 0 && prev >= 0) { a = prev; fa = -1; }
        for (int g = (int)f + 1; g < (int)OC_VOX_FRAMES; g++) if (sent[g]) { b = x[g]; fb = g; break; }
        if (a >= 0 && b >= 0) x[f] = (uint8_t)((a * (fb - (int)f) + b * ((int)f - fa) + (fb - fa) / 2) / (fb - fa));
        else if (a >= 0) x[f] = (uint8_t)a;
        else if (b >= 0) x[f] = (uint8_t)b;
    }
}

static void interp_s8(int8_t *x, const uint8_t *sent, int prev_ok, int8_t prev)
{
    for (unsigned f = 0; f < OC_VOX_FRAMES; f++) {
        if (sent[f]) continue;
        int a = 0, b = 0, fa = -1, fb = -1, have_a = 0, have_b = 0;
        for (int g = (int)f - 1; g >= 0; g--) if (sent[g]) { a = x[g]; fa = g; have_a = 1; break; }
        if (!have_a && prev_ok) { a = prev; fa = -1; have_a = 1; }
        for (int g = (int)f + 1; g < (int)OC_VOX_FRAMES; g++) if (sent[g]) { b = x[g]; fb = g; have_b = 1; break; }
        if (have_a && have_b) x[f] = (int8_t)((a * (fb - (int)f) + b * ((int)f - fa)) / (fb - fa));
        else if (have_a) x[f] = (int8_t)a;
        else if (have_b) x[f] = (int8_t)b;
    }
}

/* Voicing takes the next transmitted frame's (an onset starts a frame
 * early rather than late), else the previous; vmix holds the previous. */
static void fill_nearest(uint8_t *x, const uint8_t *sent, int prev, int take_next)
{
    for (unsigned f = 0; f < OC_VOX_FRAMES; f++) {
        if (sent[f]) continue;
        int a = -1, b = -1;
        for (int g = (int)f - 1; g >= 0; g--) if (sent[g]) { a = x[g]; break; }
        if (a < 0) a = prev;
        for (int g = (int)f + 1; g < (int)OC_VOX_FRAMES; g++) if (sent[g]) { b = x[g]; break; }
        if (take_next && b >= 0) x[f] = (uint8_t)b;
        else if (a >= 0) x[f] = (uint8_t)a;
        else if (b >= 0) x[f] = (uint8_t)b;
    }
}

static void finish(walk_t *w, const oc_vox_pframe_t *prev, oc_vox_pframe_t out[OC_VOX_FRAMES])
{
    uint8_t tmp[OC_VOX_FRAMES];
    unsigned f, b;
    for (f = 0; f < OC_VOX_FRAMES; f++) tmp[f] = out[f].e;
    interp_u8(tmp, w->e_sent, prev ? prev->e : -1);
    for (f = 0; f < OC_VOX_FRAMES; f++) out[f].e = tmp[f];
    for (f = 0; f < OC_VOX_FRAMES; f++) tmp[f] = out[f].p;
    interp_u8(tmp, w->p_sent, prev ? prev->p : -1);
    for (f = 0; f < OC_VOX_FRAMES; f++) out[f].p = tmp[f];
    for (f = 0; f < OC_VOX_FRAMES; f++) tmp[f] = out[f].v;
    fill_nearest(tmp, w->v_sent, prev ? prev->v : -1, 1);
    for (f = 0; f < OC_VOX_FRAMES; f++) out[f].v = tmp[f];
    for (f = 0; f < OC_VOX_FRAMES; f++) tmp[f] = out[f].vmix;
    fill_nearest(tmp, w->mix_sent, prev ? prev->vmix : 2, 0);
    for (f = 0; f < OC_VOX_FRAMES; f++) out[f].vmix = tmp[f];
    for (f = 0; f < OC_VOX_FRAMES; f++)
        if (w->s_sent[f]) oc_vox_env_idct(w->c[f], out[f].s);
    for (b = 0; b < OC_VOX_BANDS; b++) {
        int8_t col[OC_VOX_FRAMES];
        for (f = 0; f < OC_VOX_FRAMES; f++) col[f] = out[f].s[b];
        interp_s8(col, w->s_sent, prev != NULL, prev ? prev->s[b] : 0);
        for (f = 0; f < OC_VOX_FRAMES; f++) out[f].s[b] = col[f];
    }
    for (f = 0; f < OC_VOX_FRAMES; f++)
        if (!w->mid_sent[f]) out[f].emid = 0;
}

/* The index widened to the full domain, at the centre of its step; index
 * 0 stays 0 (an energy of 0 is silence, and must come back as silence). */
static uint8_t center(unsigned val, unsigned shift)
{
    unsigned v = val ? (val << shift) | (shift ? (1u << (shift - 1u)) : 0u) : 0u;
    return (uint8_t)(v > 255u ? 255u : v);
}

void oc_vox_quant_block(oc_vox_mode_t m, const oc_vox_pframe_t in[OC_VOX_FRAMES], const oc_vox_pframe_t *prev,
                        uint8_t toggle, oc_vox_qblock_t *q, oc_vox_pframe_t *recon)
{
    const oc_vox_shape_t *sh = oc_vox_shape(m);
    walk_t w;
    oc_vox_pframe_t out[OC_VOX_FRAMES];
    int32_t cin[OC_VOX_FRAMES][OC_VOX_BANDS];
    unsigned i, f;
    memset(q, 0, sizeof(*q));
    if (sh == NULL) return;
    walk_init(&w);
    memcpy(out, in, sizeof(out));
    for (f = 0; f < OC_VOX_FRAMES; f++) oc_vox_env_dct(in[f].s, cin[f]);
    for (i = 0; i < sh->n; i++) {
        const oc_vox_field_t *fd = &sh->f[i];
        f = fd->frame;
        switch (fd->kind) {
        case OC_VOX_F_T: q->v[i] = toggle & 1u; break;
        case OC_VOX_F_E_ABS:
            q->v[i] = (uint16_t)(in[f].e >> (7u - fd->bits));
            out[f].e = (uint8_t)clamp(center(q->v[i], 7u - fd->bits), 0, 127);
            w.e_ref = out[f].e; w.e_sent[f] = 1;
            break;
        case OC_VOX_F_E_DEL: {
            const int16_t *t = oc_vox_e_delta_table(fd->bits);
            q->v[i] = (uint16_t)oc_vox_delta_index(t, fd->bits, (int32_t)in[f].e - w.e_ref);
            out[f].e = (uint8_t)clamp(w.e_ref + t[q->v[i]], 0, 127);
            w.e_ref = out[f].e; w.e_sent[f] = 1;
            break;
        }
        case OC_VOX_F_E_MID:
            q->v[i] = (uint16_t)oc_vox_delta_index(E_MID3, 3, in[f].emid);
            out[f].emid = (int8_t)E_MID3[q->v[i]];
            w.mid_sent[f] = 1;
            break;
        case OC_VOX_F_P_ABS:
            q->v[i] = (uint16_t)(in[f].p >> (8u - fd->bits));
            out[f].p = center(q->v[i], 8u - fd->bits);
            w.p_ref = out[f].p; w.p_sent[f] = 1;
            break;
        case OC_VOX_F_P_DEL: {
            const int16_t *t = oc_vox_p_delta_table(fd->bits);
            q->v[i] = (uint16_t)oc_vox_delta_index(t, fd->bits, (int32_t)in[f].p - w.p_ref);
            out[f].p = (uint8_t)clamp(w.p_ref + t[q->v[i]], 0, 255);
            w.p_ref = out[f].p; w.p_sent[f] = 1;
            break;
        }
        case OC_VOX_F_V_CLS:
            q->v[i] = oc_vox_vclass_of_mask(in[f].v);
            out[f].v = oc_vox_vmask_of_class((uint8_t)q->v[i]);
            w.v_sent[f] = 1;
            break;
        case OC_VOX_F_V_MSK:
            q->v[i] = in[f].v & 0xFu;
            out[f].v = (uint8_t)q->v[i];
            w.v_sent[f] = 1;
            break;
        case OC_VOX_F_V_MIX:
            q->v[i] = (uint16_t)((in[f].vmix & 7u) >> (3u - fd->bits));
            out[f].vmix = (uint8_t)clamp(center(q->v[i], 3u - fd->bits), 0, 7);
            w.mix_sent[f] = 1;
            break;
        case OC_VOX_F_S1: case OC_VOX_F_S2: case OC_VOX_F_S3: {
            sub_t subs[SUBS];
            unsigned n, k, val = 0;
#if OC_VOX_CB_TRAINED
            if (fd->kind == OC_VOX_F_S1) {
                val = cb_nearest(in[f].s);
                cb_add(w.c[f], val);
                q->v[i] = (uint16_t)val;
                w.s_sent[f] = 1;
                break;
            }
#endif
            n = stage_subs(fd->kind, fd->bits, subs);
            for (k = 0; k < n; k++) {
                int32_t r;
                unsigned idx = sub_quant(&subs[k], cin[f][subs[k].coef] - w.c[f][subs[k].coef], &r);
                w.c[f][subs[k].coef] += r;
                val = (val << subs[k].bits) | idx;
            }
            q->v[i] = (uint16_t)val;
            w.s_sent[f] = 1;
            break;
        }
        case OC_VOX_F_S4: {
            unsigned val = 0, k;
            for (k = 1; k <= 8; k++) {
                int32_t step = s4_step(k), r = cin[f][k] - w.c[f][k];
                unsigned bit = r >= 0 ? 1u : 0u;
                w.c[f][k] += bit ? step : -step;
                val = (val << 1) | bit;
            }
            q->v[i] = (uint16_t)val;
            break;
        }
        default: break;
        }
    }
    if (recon != NULL) {
        finish(&w, prev, out);
        memcpy(recon, out, sizeof(out));
    }
}

void oc_vox_dequant_block(oc_vox_mode_t m, const oc_vox_qblock_t *q, const oc_vox_pframe_t *prev, unsigned flags,
                          oc_vox_pframe_t out[OC_VOX_FRAMES], uint8_t *toggle)
{
    const oc_vox_shape_t *sh = oc_vox_shape(m);
    walk_t w;
    unsigned i, f;
    memset(out, 0, sizeof(oc_vox_pframe_t) * OC_VOX_FRAMES);
    if (toggle) *toggle = 0;
    if (sh == NULL) return;
    walk_init(&w);
    for (i = 0; i < sh->n; i++) {
        const oc_vox_field_t *fd = &sh->f[i];
        unsigned val = q->v[i] & ((1u << fd->bits) - 1u);
        f = fd->frame;
        switch (fd->kind) {
        case OC_VOX_F_T: if (toggle) *toggle = (uint8_t)val; break;
        case OC_VOX_F_E_ABS:
            out[f].e = (uint8_t)clamp(center(val, 7u - fd->bits), 0, 127);
            w.e_ref = out[f].e; w.e_sent[f] = 1;
            break;
        case OC_VOX_F_E_DEL:
            out[f].e = (uint8_t)clamp(w.e_ref + oc_vox_e_delta_table(fd->bits)[val], 0, 127);
            w.e_ref = out[f].e; w.e_sent[f] = 1;
            break;
        case OC_VOX_F_E_MID:
            if (!(flags & OC_VOX_DQ_DROP_FINE)) { out[f].emid = (int8_t)E_MID3[val]; w.mid_sent[f] = 1; }
            break;
        case OC_VOX_F_P_ABS:
            out[f].p = center(val, 8u - fd->bits);
            w.p_ref = out[f].p; w.p_sent[f] = 1;
            break;
        case OC_VOX_F_P_DEL:
            out[f].p = (uint8_t)clamp(w.p_ref + oc_vox_p_delta_table(fd->bits)[val], 0, 255);
            w.p_ref = out[f].p; w.p_sent[f] = 1;
            break;
        case OC_VOX_F_V_CLS: out[f].v = oc_vox_vmask_of_class((uint8_t)val); w.v_sent[f] = 1; break;
        case OC_VOX_F_V_MSK: out[f].v = (uint8_t)val; w.v_sent[f] = 1; break;
        case OC_VOX_F_V_MIX: out[f].vmix = (uint8_t)clamp(center(val, 3u - fd->bits), 0, 7); w.mix_sent[f] = 1; break;
        case OC_VOX_F_S1: case OC_VOX_F_S2: case OC_VOX_F_S3: {
            sub_t subs[SUBS];
            unsigned n, k, shift = fd->bits;
            if (fd->kind == OC_VOX_F_S3 && (flags & OC_VOX_DQ_DROP_FINE)) break;
#if OC_VOX_CB_TRAINED
            if (fd->kind == OC_VOX_F_S1) {
                cb_add(w.c[f], val);
                w.s_sent[f] = 1;
                break;
            }
#endif
            n = stage_subs(fd->kind, fd->bits, subs);
            for (k = 0; k < n; k++) {
                shift -= subs[k].bits;
                w.c[f][subs[k].coef] += sub_dequant(&subs[k], (val >> shift) & ((1u << subs[k].bits) - 1u));
            }
            w.s_sent[f] = 1;
            break;
        }
        case OC_VOX_F_S4:
            if (flags & OC_VOX_DQ_DROP_FINE) break;
            for (unsigned k = 1; k <= 8; k++) {
                int32_t step = s4_step(k);
                w.c[f][k] += ((val >> (8u - k)) & 1u) ? step : -step;
            }
            break;
        default: break;
        }
    }
    finish(&w, prev, out);
}
