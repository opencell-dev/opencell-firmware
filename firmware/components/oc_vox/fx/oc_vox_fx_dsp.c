#include "oc_vox_fx_int.h"

extern const int16_t oc_vox_fx_sin1024[1024];
extern const int16_t oc_vox_fx_tw256[256];
extern const uint16_t oc_vox_fx_log2m65[65];
extern const uint32_t oc_vox_fx_exp2f65[65];

static int32_t mul_q15(int32_t a, int32_t w) { return (int32_t)(((int64_t)a * w) >> 15); }

void oc_vox_fx_fft256(int32_t re[256], int32_t im[256])
{
    unsigned i, j, len;
    for (i = 1; i < 256; i++) {
        unsigned r = 0, k;
        for (k = 0; k < 8; k++) r |= ((i >> k) & 1u) << (7u - k);
        if (r > i) {
            int32_t t = re[i]; re[i] = re[r]; re[r] = t;
            t = im[i]; im[i] = im[r]; im[r] = t;
        }
    }
    for (len = 2; len <= 256; len <<= 1) {
        unsigned step = 256 / len; /* twiddle index step: e^{-2 pi i j / len} = table[j * step] */
        for (i = 0; i < 256; i += len) {
            for (j = 0; j < len / 2; j++) {
                unsigned a = i + j, b = i + j + len / 2, t = j * step;
                int32_t wr = oc_vox_fx_tw256[(t + 64u) & 255u], wi = -oc_vox_fx_tw256[t & 255u]; /* cos, -sin */
                int32_t tr = mul_q15(re[b], wr) - mul_q15(im[b], wi);
                int32_t ti = mul_q15(re[b], wi) + mul_q15(im[b], wr);
                re[b] = (re[a] - tr) >> 1; im[b] = (im[a] - ti) >> 1;
                re[a] = (re[a] + tr) >> 1; im[a] = (im[a] + ti) >> 1;
            }
        }
    }
}

int32_t oc_vox_fx_log2_q8(uint32_t v)
{
    unsigned e = 31, frac;
    uint32_t m;
    if (v == 0) return 0;
    while (!(v & (1u << e))) e--;
    /* the mantissa's top 6 bits select the table entry, the next 8 interpolate */
    m = e >= 14 ? v >> (e - 14) : v << (14 - e); /* 1.xxx in Q14 */
    frac = (m >> 8) & 63u;
    {
        uint32_t lo = oc_vox_fx_log2m65[frac], hi = oc_vox_fx_log2m65[frac + 1], r = m & 255u;
        return (int32_t)(e * 256u + lo + ((hi - lo) * r >> 8));
    }
}

int32_t oc_vox_fx_log2_q8_64(uint64_t v)
{
    unsigned sh = 0;
    while (v >> 32) { v >>= 1; sh++; }
    if (v == 0) return 0;
    return oc_vox_fx_log2_q8((uint32_t)v) + (int32_t)(sh * 256u);
}

uint32_t oc_vox_fx_exp2_q15(int32_t x_q8)
{
    int32_t ip;
    unsigned fi, fr;
    uint32_t lo, hi, v;
    if (x_q8 > 15 * 256) x_q8 = 15 * 256;
    if (x_q8 < -15 * 256) return 0;
    ip = x_q8 >> 8; /* floor */
    fi = (unsigned)(x_q8 & 255) >> 2; /* 6 bits */
    fr = (unsigned)(x_q8 & 3);
    lo = oc_vox_fx_exp2f65[fi]; hi = oc_vox_fx_exp2f65[fi + 1];
    v = lo + (((hi - lo) * fr) >> 2);
    return ip >= 0 ? v << ip : v >> (-ip);
}

uint32_t oc_vox_fx_sqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

uint32_t oc_vox_fx_sqrt64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

int16_t oc_vox_fx_sin_q15(uint32_t phase_q32)
{
    unsigned i = phase_q32 >> 22, f = (phase_q32 >> 14) & 255u; /* 10-bit index, 8-bit fraction */
    int32_t a = oc_vox_fx_sin1024[i], b = oc_vox_fx_sin1024[(i + 1u) & 1023u];
    return (int16_t)(a + (((b - a) * (int32_t)f) >> 8));
}

int32_t oc_vox_fx_ncc_q15(const int16_t *x, unsigned n, unsigned lag)
{
    int64_t xy = 0, xx = 0, yy = 0;
    uint32_t sx, sy;
    for (unsigned i = 0; i < n; i++) {
        int32_t a = x[i], b = x[(int)i - (int)lag];
        xy += (int64_t)a * b; xx += (int64_t)a * a; yy += (int64_t)b * b;
    }
    if (xx < 1 || yy < 1) return 0;
    sx = oc_vox_fx_sqrt64((uint64_t)xx);
    sy = oc_vox_fx_sqrt64((uint64_t)yy);
    if (sx == 0 || sy == 0) return 0;
    /* xy / (sx sy) in Q15: divide in two steps to stay in 64 bits */
    return (int32_t)(((xy * 32768) / sx) / (int64_t)sy); /* a multiply: shifting a negative value is undefined */
}

int32_t oc_vox_fx_units_of_log2_q8(int32_t dlog2_q8)
{
    /* 10 log10(2) / 0.75 = 4.01373 -> x 1028 / 256 (0.3 % high) ... use 1027.5: (d x 2055) >> 9 */
    int64_t v = (int64_t)dlog2_q8 * 2055;
    return (int32_t)((v + (v >= 0 ? (1 << 16) : -(1 << 16))) / (1 << 17)); /* round half away from zero (a shift would floor) */
}
