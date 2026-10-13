#include "oc_vox_bits.h"

#include <string.h>

#include "oc_vox_int.h"

const uint16_t oc_vox_band_edge_hz[OC_VOX_BANDS + 1u] = { 100, 300, 500, 700, 900, 1150, 1450, 1800, 2200, 2650, 3150, 3700, 4000 };
const uint8_t  oc_vox_band_vband[OC_VOX_BANDS] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3 };

void oc_vox_bw_init(oc_vox_bw_t *w, uint8_t *buf, uint16_t cap_bits)
{
    w->buf = buf;
    w->pos = 0;
    w->cap = cap_bits;
    memset(buf, 0, (cap_bits + 7u) / 8u);
}

void oc_vox_bw_put(oc_vox_bw_t *w, uint32_t v, unsigned bits)
{
    for (unsigned i = bits; i > 0; i--) {
        if (w->pos < w->cap && ((v >> (i - 1u)) & 1u)) w->buf[w->pos >> 3] |= (uint8_t)(0x80u >> (w->pos & 7u));
        w->pos++;
    }
}

int oc_vox_bw_overrun(const oc_vox_bw_t *w) { return w->pos > w->cap; }

void oc_vox_br_init(oc_vox_br_t *r, const uint8_t *buf, uint16_t len_bits)
{
    r->buf = buf;
    r->pos = 0;
    r->len = len_bits;
    r->overrun = 0;
}

uint32_t oc_vox_br_get(oc_vox_br_t *r, unsigned bits)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < bits; i++) {
        unsigned b = 0;
        if (r->pos < r->len) b = oc_vox_bit_get(r->buf, r->pos); else r->overrun = 1;
        v = (v << 1) | b;
        r->pos++;
    }
    return v;
}

void oc_vox_bits_pack(oc_vox_mode_t m, const oc_vox_qblock_t *q, uint8_t *pstr)
{
    const oc_vox_shape_t *sh = oc_vox_shape(m);
    oc_vox_bw_t w;
    if (sh == NULL) return;
    oc_vox_bw_init(&w, pstr, sh->param_bits);
    for (unsigned i = 0; i < sh->n; i++) oc_vox_bw_put(&w, q->v[i] & ((1u << sh->f[i].bits) - 1u), sh->f[i].bits);
}

void oc_vox_bits_unpack(oc_vox_mode_t m, const uint8_t *pstr, oc_vox_qblock_t *q)
{
    const oc_vox_shape_t *sh = oc_vox_shape(m);
    oc_vox_br_t r;
    memset(q, 0, sizeof(*q));
    if (sh == NULL) return;
    oc_vox_br_init(&r, pstr, sh->param_bits);
    for (unsigned i = 0; i < sh->n; i++) q->v[i] = (uint16_t)oc_vox_br_get(&r, sh->f[i].bits);
}
