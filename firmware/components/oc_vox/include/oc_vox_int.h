/* Shared internals of both implementations (PLAN.md §1.2): the integer
 * parameter domain one 20 ms frame is described in, and a block of them.
 * Everything the quantizers, the bitstream, the FEC and the concealment do
 * happens here, so the reference and the fixed-point codec differ only in
 * how they estimate these from audio and how they turn them back into it. */
#ifndef OC_VOX_INT_H
#define OC_VOX_INT_H

#include <stdint.h>

#include "oc_vox_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OC_VOX_BANDS   12u /* envelope bands */
#define OC_VOX_VBANDS  4u  /* voicing bands: 0-600, 600-1200, 1200-2400, 2400-4000 Hz */
#define OC_VOX_FRAMES  6u

/* e: energy, 0.75 dB steps, 0 = -96 dBFS ... 127 = -0.75 dBFS (RMS against 32768).
 * p: pitch, f0 = 50 Hz x 2^(p / 84), 0 ... 255 (50 ... 410 Hz).
 * v: voicing mask, bit b = band b voiced. vmix: 0 periodic ... 7 dispersed.
 * emid: energy of the frame's second half minus the first, 0.75 dB steps (OVX-3200; else 0).
 * s[b]: band log energy minus the frame's mean over bands, 0.75 dB steps. */
typedef struct {
    uint8_t e, p, v, vmix;
    int8_t  emid;
    int8_t  s[OC_VOX_BANDS];
} oc_vox_pframe_t;

/* A block's quantized field values, one per layout field, in layout order. */
typedef struct oc_vox_qblock {
    uint16_t v[OC_VOX_MAX_FIELDS];
} oc_vox_qblock_t;

/* Band edges in Hz (13 edges, 12 bands) and the voicing band of each. */
extern const uint16_t oc_vox_band_edge_hz[OC_VOX_BANDS + 1u];
extern const uint8_t  oc_vox_band_vband[OC_VOX_BANDS];

#ifdef __cplusplus
}
#endif

#endif
