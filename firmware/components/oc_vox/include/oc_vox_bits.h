/* Bit I/O (MSB-first) and the packing of a block's quantized fields into
 * its parameter string (oc_vox_layout.h). The parameter string is what
 * the FEC protects (oc_vox_fec.h); a block on the wire is the FEC's
 * output. Public so tools and tests can take a block apart. */
#ifndef OC_VOX_BITS_H
#define OC_VOX_BITS_H

#include <stddef.h>
#include <stdint.h>

#include "oc_vox_modes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    uint16_t pos, cap; /* bits */
} oc_vox_bw_t;

/* Zeroes cap bits of buf (rounded up to bytes) and writes from bit 0. */
void oc_vox_bw_init(oc_vox_bw_t *w, uint8_t *buf, uint16_t cap_bits);
/* The low `bits` (1..32) of v, MSB-first. Past cap nothing is written and
 * pos still advances (oc_vox_bw_overrun says so). */
void oc_vox_bw_put(oc_vox_bw_t *w, uint32_t v, unsigned bits);
int  oc_vox_bw_overrun(const oc_vox_bw_t *w);

typedef struct {
    const uint8_t *buf;
    uint16_t pos, len; /* bits */
    uint8_t  overrun;  /* a read went past len: zeros were returned */
} oc_vox_br_t;

void     oc_vox_br_init(oc_vox_br_t *r, const uint8_t *buf, uint16_t len_bits);
uint32_t oc_vox_br_get(oc_vox_br_t *r, unsigned bits);

static inline unsigned oc_vox_bit_get(const uint8_t *buf, unsigned i) { return (buf[i >> 3] >> (7u - (i & 7u))) & 1u; }
static inline void oc_vox_bit_set(uint8_t *buf, unsigned i, unsigned v)
{
    uint8_t m = (uint8_t)(0x80u >> (i & 7u));
    if (v) buf[i >> 3] |= m; else buf[i >> 3] &= (uint8_t)~m;
}

/* The parameter string: ceil(shape->param_bits / 8) bytes, the tail zero. */
#define OC_VOX_PARAM_MAX_BYTES 40u /* OVX-3200: 320 bits */

struct oc_vox_qblock; /* oc_vox_int.h */
/* Field values beyond their width are masked. */
void oc_vox_bits_pack(oc_vox_mode_t m, const struct oc_vox_qblock *q, uint8_t *pstr);
void oc_vox_bits_unpack(oc_vox_mode_t m, const uint8_t *pstr, struct oc_vox_qblock *q);

#ifdef __cplusplus
}
#endif

#endif
