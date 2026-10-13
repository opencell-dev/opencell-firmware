/* Unequal error protection of a block (PLAN.md §1.3). The parameter string
 * (oc_vox_bits.h) is cut into classes: A, the first 12 x golay bits, each
 * 12 coded as an extended Golay(24,12) word (corrects 3 bit errors,
 * detects 4); B, the next 8 x hamming bits, each 8 coded as a shortened
 * Hamming(12,8) word (corrects 1); C, the rest, sent as is; then, in
 * OVX-2400 and OVX-3200, a CRC over the B and C bits. The coded block is
 * written row by row into a matrix ilv_width wide and read column by
 * column, so a burst of up to ilv_width bits touches distinct words.
 *
 * Codes are the textbook ones, implemented here from their definitions:
 * the Golay code from its generator polynomial x^11+x^10+x^6+x^5+x^4+x^2+1
 * in systematic form, decoded with the self-dual two-syndrome method; the
 * Hamming code from a parity-check matrix whose columns are 1..12. */
#ifndef OC_VOX_FEC_H
#define OC_VOX_FEC_H

#include <stdint.h>

#include "oc_vox_modes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* data in the low 12 bits -> the 24-bit word, data in bits 23..12. */
uint32_t oc_vox_golay_encode(uint32_t data12);
/* Corrected errors (0..3) with *data12 the decoded data; -1 when more than
 * 3 bits were wrong (detected: *data12 is then the word's data bits as
 * received). */
int      oc_vox_golay_decode(uint32_t word24, uint32_t *data12);

/* data in the low 8 bits -> the 12-bit word, data in bits 11..4. */
uint16_t oc_vox_hamming_encode(uint8_t data8);
/* 0 or 1 corrected error with *data8 the data; -1 for a detected double
 * error (some are; the rest decode wrongly, which the CRC above or the
 * parameter trajectory may catch). */
int      oc_vox_hamming_decode(uint16_t word12, uint8_t *data8);

/* CRC-8 (poly 0x2F) or CRC-12 (poly 0x80F) over bits [from, to) of a bit
 * string, MSB-first, init 0. crc_bits is 8 or 12. */
uint16_t oc_vox_crc_bits(const uint8_t *bits, unsigned from, unsigned to, unsigned crc_bits);

typedef struct {
    uint8_t golay_fixed, golay_failed, hamming_fixed, hamming_failed, crc_failed;
} oc_vox_fec_report_t;

/* The parameter string (ceil(param_bits / 8) bytes) -> the block
 * (block_bytes). */
void oc_vox_fec_protect(oc_vox_mode_t m, const uint8_t *pstr, uint8_t *block);
/* The block -> the parameter string as best it can be recovered, and what
 * happened. golay_failed > 0 means class A is not to be trusted: the
 * caller treats the block as lost. hamming_failed and crc_failed say the
 * B or C bits are suspect. */
void oc_vox_fec_recover(oc_vox_mode_t m, const uint8_t *block, uint8_t *pstr, oc_vox_fec_report_t *rep);

#ifdef __cplusplus
}
#endif

#endif
