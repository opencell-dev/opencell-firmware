#include <math.h>
#include <string.h>

#include "oc_vox_ref_int.h"

/* A plain iterative radix-2 complex FFT of 256 points on real input, from
 * the definition; the reference is for clarity, not speed. */
void oc_vox_ref_fft256(const float in[256], float re[129], float im[129])
{
    float xr[256], xi[256];
    unsigned n = 256, i, j, len;
    for (i = 0; i < n; i++) {
        /* bit reversal of 8 bits */
        unsigned r = 0, k;
        for (k = 0; k < 8; k++) r |= ((i >> k) & 1u) << (7u - k);
        xr[r] = in[i];
        xi[r] = 0.0f;
    }
    for (len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * OC_VOX_PI / (float)len;
        for (i = 0; i < n; i += len) {
            for (j = 0; j < len / 2; j++) {
                float wr = cosf(ang * (float)j), wi = sinf(ang * (float)j);
                unsigned a = i + j, b = i + j + len / 2;
                float tr = xr[b] * wr - xi[b] * wi, ti = xr[b] * wi + xi[b] * wr;
                xr[b] = xr[a] - tr; xi[b] = xi[a] - ti;
                xr[a] += tr;        xi[a] += ti;
            }
        }
    }
    memcpy(re, xr, 129 * sizeof(float));
    memcpy(im, xi, 129 * sizeof(float));
}

void oc_vox_ref_hann256(float w[256])
{
    for (unsigned i = 0; i < 256; i++) w[i] = 0.5f - 0.5f * cosf(2.0f * OC_VOX_PI * ((float)i + 0.5f) / 256.0f);
}

float oc_vox_ref_ncc(const float *x, unsigned n, unsigned lag)
{
    double xy = 0, xx = 0, yy = 0;
    for (unsigned i = 0; i < n; i++) {
        double a = x[i], b = x[(int)i - (int)lag];
        xy += a * b; xx += a * a; yy += b * b;
    }
    if (xx < 1e-3 || yy < 1e-3) return 0.0f;
    return (float)(xy / sqrt(xx * yy));
}

float oc_vox_ref_db_of_power(float p)
{
    if (p < 1e-12f) return -120.0f;
    return 10.0f * log10f(p);
}

float oc_vox_ref_band_centre_hz(unsigned b)
{
    return sqrtf((float)oc_vox_band_edge_hz[b] * (float)oc_vox_band_edge_hz[b + 1]);
}

float oc_vox_ref_e_to_rms(uint8_t e)
{
    if (e == 0) return 0.0f;
    return 32768.0f * powf(10.0f, ((float)e * 0.75f - 96.0f) / 20.0f);
}

uint8_t oc_vox_ref_rms_to_e(float rms)
{
    float db, v;
    if (rms < 1e-3f) return 0;
    db = 20.0f * log10f(rms / 32768.0f);
    v = (db + 96.0f) / 0.75f;
    if (v < 0.0f) return 0;
    if (v > 127.0f) return 127;
    return (uint8_t)(v + 0.5f);
}
