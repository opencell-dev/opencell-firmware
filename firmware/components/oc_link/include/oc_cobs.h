#ifndef OC_COBS_H
#define OC_COBS_H

#include <stddef.h>
#include <stdint.h>

/* Worst-case encoded size for n input bytes (excluding the 0x00 delimiter). */
#define OC_COBS_MAX_ENCODED(n) ((n) + (n) / 254 + 1)

/* Encode len bytes into out (no trailing delimiter). out must hold
 * OC_COBS_MAX_ENCODED(len). Returns the encoded length. */
size_t oc_cobs_encode(const uint8_t *in, size_t len, uint8_t *out);

/* Decode len bytes (no delimiter) into out. Returns the decoded length, or -1
 * if the input is malformed or the output would exceed out_cap. */
int oc_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

#endif
