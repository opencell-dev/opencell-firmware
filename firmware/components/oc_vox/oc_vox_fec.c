#include "oc_vox_fec.h"

#include <string.h>

#include "oc_vox_bits.h"
#include "oc_vox_layout.h"

/* Row i: the 12 check bits (11 from the polynomial, then the overall
 * parity) of the data word with only bit i (MSB first) set. The
 * polynomial is x^11+x^10+x^6+x^5+x^4+x^2+1 (0xC75); test_vox_fec
 * re-derives these rows from it. For this systematic form of the
 * (self-dual) extended Golay code A x A^T = I, which the decoder uses. */
static const uint16_t GOLAY_A[12] = { 0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB };

static unsigned weight(uint32_t v)
{
    unsigned n = 0;
    while (v) { n += v & 1u; v >>= 1; }
    return n;
}

/* d (12 bits) x A: the check bits of d. */
static uint32_t golay_check(uint32_t d)
{
    uint32_t c = 0;
    for (int i = 0; i < 12; i++)
        if (d & (0x800u >> i)) c ^= GOLAY_A[i];
    return c;
}

/* c (12 bits) x A^T. */
static uint32_t golay_check_t(uint32_t c)
{
    uint32_t d = 0;
    for (int i = 0; i < 12; i++)
        if (weight(GOLAY_A[i] & c) & 1u) d |= 0x800u >> i;
    return d;
}

uint32_t oc_vox_golay_encode(uint32_t data12)
{
    data12 &= 0xFFFu;
    return (data12 << 12) | golay_check(data12);
}

int oc_vox_golay_decode(uint32_t word24, uint32_t *data12)
{
    uint32_t r1 = (word24 >> 12) & 0xFFFu, r2 = word24 & 0xFFFu;
    uint32_t s = golay_check(r1) ^ r2; /* = e1 A + e2 */
    uint32_t e1 = 0, e2 = 0;
    int found = 0;
    if (weight(s) <= 3) { e2 = s; found = 1; }
    for (int i = 0; !found && i < 12; i++) {
        if (weight(s ^ GOLAY_A[i]) <= 2) { e1 = 0x800u >> i; e2 = s ^ GOLAY_A[i]; found = 1; }
    }
    if (!found) {
        uint32_t s2 = r1 ^ golay_check_t(r2); /* = e1 + e2 A^T */
        if (weight(s2) <= 3) { e1 = s2; found = 1; }
        for (int j = 0; !found && j < 12; j++) {
            uint32_t col = golay_check_t(0x800u >> j);
            if (weight(s2 ^ col) <= 2) { e2 = 0x800u >> j; e1 = s2 ^ col; found = 1; }
        }
    }
    if (!found) { *data12 = r1; return -1; }
    *data12 = r1 ^ e1;
    return (int)(weight(e1) + weight(e2));
}

/* Hamming(12,8): the word is d7..d0 (bits 11..4) then p (bits 3..0); the
 * parity-check column of data bit k (k = 0 for d7) is DCOL[k], of parity
 * bit j (j = 0 for bit 3) is 8 >> j. */
static const uint8_t DCOL[8] = { 3, 5, 6, 7, 9, 10, 11, 12 };

uint16_t oc_vox_hamming_encode(uint8_t data8)
{
    unsigned p = 0;
    for (int k = 0; k < 8; k++)
        if (data8 & (0x80u >> k)) p ^= DCOL[k];
    return (uint16_t)(((unsigned)data8 << 4) | p);
}

int oc_vox_hamming_decode(uint16_t word12, uint8_t *data8)
{
    unsigned s = word12 & 0xFu;
    uint8_t d = (uint8_t)(word12 >> 4);
    for (int k = 0; k < 8; k++)
        if (d & (0x80u >> k)) s ^= DCOL[k];
    if (s == 0) { *data8 = d; return 0; }
    for (int k = 0; k < 8; k++) {
        if (s == DCOL[k]) { *data8 = (uint8_t)(d ^ (0x80u >> k)); return 1; }
    }
    *data8 = d;
    if (s == 1 || s == 2 || s == 4 || s == 8) return 1; /* a parity bit was hit */
    return -1;
}

uint16_t oc_vox_crc_bits(const uint8_t *bits, unsigned from, unsigned to, unsigned crc_bits)
{
    uint32_t poly = crc_bits == 8 ? 0x2Fu : 0x80Fu, top = 1u << (crc_bits - 1u), mask = (1u << crc_bits) - 1u;
    uint32_t crc = 0;
    for (unsigned i = from; i < to; i++) {
        unsigned in = oc_vox_bit_get(bits, i) ^ ((crc & top) ? 1u : 0u);
        crc = (crc << 1) & mask;
        if (in) crc ^= poly;
    }
    return (uint16_t)crc;
}

/* Position in the interleaved block of coded bit i: written row-wise into
 * a matrix width wide, read column-wise. */
static unsigned ilv_pos(unsigned i, unsigned width, unsigned nbits)
{
    unsigned depth = nbits / width;
    return (i % width) * depth + i / width;
}

void oc_vox_fec_protect(oc_vox_mode_t m, const uint8_t *pstr, uint8_t *block)
{
    const oc_vox_shape_t *sh = oc_vox_shape(m);
    uint8_t coded[OC_VOX_BLOCK_MAX_BYTES];
    oc_vox_bw_t w;
    oc_vox_br_t r;
    unsigned nbits, abits, bbits;
    if (sh == NULL) return;
    nbits = (unsigned)sh->block_bytes * 8u;
    abits = 12u * sh->golay;
    bbits = 8u * sh->hamming;
    oc_vox_bw_init(&w, coded, (uint16_t)nbits);
    oc_vox_br_init(&r, pstr, sh->param_bits);
    for (unsigned i = 0; i < sh->golay; i++) oc_vox_bw_put(&w, oc_vox_golay_encode(oc_vox_br_get(&r, 12)), 24);
    for (unsigned i = 0; i < sh->hamming; i++) oc_vox_bw_put(&w, oc_vox_hamming_encode((uint8_t)oc_vox_br_get(&r, 8)), 12);
    for (unsigned i = abits + bbits; i < sh->param_bits; i++) oc_vox_bw_put(&w, oc_vox_br_get(&r, 1), 1);
    if (sh->crc_bits) oc_vox_bw_put(&w, oc_vox_crc_bits(pstr, abits, sh->param_bits, sh->crc_bits), sh->crc_bits);
    memset(block, 0, sh->block_bytes);
    for (unsigned i = 0; i < nbits; i++)
        if (oc_vox_bit_get(coded, i)) oc_vox_bit_set(block, ilv_pos(i, sh->ilv_width, nbits), 1);
}

void oc_vox_fec_recover(oc_vox_mode_t m, const uint8_t *block, uint8_t *pstr, oc_vox_fec_report_t *rep)
{
    const oc_vox_shape_t *sh = oc_vox_shape(m);
    uint8_t coded[OC_VOX_BLOCK_MAX_BYTES];
    oc_vox_bw_t w;
    oc_vox_br_t r;
    unsigned nbits, abits, bbits;
    memset(rep, 0, sizeof(*rep));
    if (sh == NULL) return;
    nbits = (unsigned)sh->block_bytes * 8u;
    abits = 12u * sh->golay;
    bbits = 8u * sh->hamming;
    memset(coded, 0, sizeof(coded));
    for (unsigned i = 0; i < nbits; i++)
        if (oc_vox_bit_get(block, ilv_pos(i, sh->ilv_width, nbits))) oc_vox_bit_set(coded, i, 1);
    oc_vox_br_init(&r, coded, (uint16_t)nbits);
    oc_vox_bw_init(&w, pstr, sh->param_bits);
    for (unsigned i = 0; i < sh->golay; i++) {
        uint32_t d;
        int n = oc_vox_golay_decode(oc_vox_br_get(&r, 24), &d);
        if (n < 0) rep->golay_failed++; else if (n > 0) rep->golay_fixed++;
        oc_vox_bw_put(&w, d, 12);
    }
    for (unsigned i = 0; i < sh->hamming; i++) {
        uint8_t d;
        int n = oc_vox_hamming_decode((uint16_t)oc_vox_br_get(&r, 12), &d);
        if (n < 0) rep->hamming_failed++; else if (n > 0) rep->hamming_fixed++;
        oc_vox_bw_put(&w, d, 8);
    }
    for (unsigned i = abits + bbits; i < sh->param_bits; i++) oc_vox_bw_put(&w, oc_vox_br_get(&r, 1), 1);
    if (sh->crc_bits) {
        uint16_t got = (uint16_t)oc_vox_br_get(&r, sh->crc_bits);
        if (got != oc_vox_crc_bits(pstr, abits, sh->param_bits, sh->crc_bits)) rep->crc_failed = 1;
    }
}
