/* The per-mode field layout of a block's parameter string, generated into
 * oc_vox_layout.c by tools/vox/gen_layout.py (PLAN.md §1.3, BITSTREAM.md).
 * Fields are written MSB-first in this order; the string's first
 * 12 x golay bits are class A, the next 8 x hamming bits class B, the
 * rest class C. A field that straddles a boundary has its MSBs in the
 * better-protected class. */
#ifndef OC_VOX_LAYOUT_H
#define OC_VOX_LAYOUT_H

#include <stdint.h>

#include "oc_vox_modes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OC_VOX_F_T = 0,   /* the block toggle, 1 bit */
    OC_VOX_F_E_ABS,   /* energy, absolute: e >> (7 - bits) */
    OC_VOX_F_E_DEL,   /* energy, a delta-table index against the previous transmitted frame */
    OC_VOX_F_E_MID,   /* half-frame energy step (OVX-3200), 3 bits */
    OC_VOX_F_P_ABS,   /* pitch, absolute: p >> (8 - bits) */
    OC_VOX_F_P_DEL,   /* pitch, a delta-table index */
    OC_VOX_F_V_CLS,   /* voicing class, 2 bits (0 unvoiced, 1 band 0, 2 bands 0-1, 3 all) */
    OC_VOX_F_V_MSK,   /* voicing mask, 4 bits (bit b: band b voiced) */
    OC_VOX_F_V_MIX,   /* aperiodicity: vmix >> (3 - bits) */
    OC_VOX_F_S1,      /* envelope stage 1, 10 bits */
    OC_VOX_F_S2,      /* envelope stage 2, 3-8 bits */
    OC_VOX_F_S3,      /* envelope stage 3, 5-8 bits */
    OC_VOX_F_S4,      /* envelope stage 4, 8 bits (OVX-3200) */
    OC_VOX_F_COUNT
} oc_vox_fkind_t;

typedef struct {
    uint8_t kind;  /* oc_vox_fkind_t */
    uint8_t frame; /* 0..5 */
    uint8_t bits;
} oc_vox_field_t;

#define OC_VOX_MAX_FIELDS 64u

typedef struct {
    const oc_vox_field_t *f;
    uint8_t  n;           /* fields */
    uint16_t param_bits;  /* their total: the parameter string's length */
    uint8_t  golay;       /* class A words of 12 bits (24 coded) */
    uint8_t  hamming;     /* class B words of 8 bits (12 coded) */
    uint8_t  crc_bits;    /* 0, 8 or 12, over classes B and C */
    uint8_t  ilv_width;   /* interleaver width: block_bits is a multiple */
    uint8_t  block_bytes;
} oc_vox_shape_t;

/* NULL for a value that is not a mode. */
const oc_vox_shape_t *oc_vox_shape(oc_vox_mode_t m);

#ifdef __cplusplus
}
#endif

#endif
