/* Activation QR code v2 (numbering-v2 spec §6.1): "opencell:2:" + base64url
 * (no padding) of a 75-byte blob; key id, expiry and CRC little-endian.
 *   0 version 2 | 1 key id (2) | 3 PKn (32) | 35 token id (8) | 43 token secret (16)
 *   59 number (8, BCD) | 67 expiry (4) | 71 reserved (2, zero) | 73 CRC-16 over 0..72 (2)
 * A v1 code ("opencell:1:", 72 bytes, 7-byte number) is refused. */
#ifndef LC_SIG_QR_H
#define LC_SIG_QR_H

#include "lc_sig.h"

#define LC_SIG_QR_BLOB 75u
#define LC_SIG_QR_TEXT 111u /* prefix 11 + 100 characters */

typedef struct {
    uint16_t key_id;
    uint8_t  pkn[32];
    uint8_t  token_id[8];
    uint8_t  token_secret[16];
    uint8_t  number[LC_SIG_NUMBER_LEN];
    uint32_t expiry; /* unix seconds; display only */
} lc_sig_qr_t;

/* Leading/trailing whitespace is ignored. 0 or -1 (prefix, length, version,
 * reserved bytes, CRC or number). */
int lc_sig_qr_parse(const char *text, size_t len, lc_sig_qr_t *q);

/* NUL-terminated text; returns its length (without NUL), 0 if cap is too small. */
size_t lc_sig_qr_format(const lc_sig_qr_t *q, char *out, size_t cap);

#endif
