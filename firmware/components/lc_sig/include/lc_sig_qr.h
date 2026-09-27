/* Activation QR code (spec §3.1): "opencell:1:" + base64url (no padding) of a
 * 72-byte blob; key id, expiry and CRC little-endian. */
#ifndef LC_SIG_QR_H
#define LC_SIG_QR_H

#include "lc_sig.h"

#define LC_SIG_QR_BLOB 72u
#define LC_SIG_QR_TEXT 107u /* prefix 11 + 96 characters */

typedef struct {
    uint16_t key_id;
    uint8_t  pkn[32];
    uint8_t  token_id[8];
    uint8_t  token_secret[16];
    uint8_t  number[LC_SIG_NUMBER_LEN];
    uint32_t expiry; /* unix seconds; display only */
} lc_sig_qr_t;

/* Leading/trailing whitespace is ignored. 0 or -1 (prefix, length, version or CRC). */
int lc_sig_qr_parse(const char *text, size_t len, lc_sig_qr_t *q);

/* NUL-terminated text; returns its length (without NUL), 0 if cap is too small. */
size_t lc_sig_qr_format(const lc_sig_qr_t *q, char *out, size_t cap);

#endif
