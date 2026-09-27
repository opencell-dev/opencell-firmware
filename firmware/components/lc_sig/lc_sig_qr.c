#include "lc_sig_qr.h"

#include <string.h>

#include "lc_crc.h"

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char k_prefix[] = "opencell:1:";

static int b64_val(char c)
{
    const char *p = c != '\0' ? strchr(k_b64, c) : NULL;
    return p != NULL ? (int)(p - k_b64) : -1;
}

static void pack(const lc_sig_qr_t *q, uint8_t b[LC_SIG_QR_BLOB])
{
    b[0] = 1;
    b[1] = (uint8_t)q->key_id;
    b[2] = (uint8_t)(q->key_id >> 8);
    memcpy(b + 3, q->pkn, 32);
    memcpy(b + 35, q->token_id, 8);
    memcpy(b + 43, q->token_secret, 16);
    memcpy(b + 59, q->number, 7);
    for (int i = 0; i < 4; i++) b[66 + i] = (uint8_t)(q->expiry >> (8 * i));
    uint16_t crc = lc_crc16(b, 70);
    b[70] = (uint8_t)crc;
    b[71] = (uint8_t)(crc >> 8);
}

size_t lc_sig_qr_format(const lc_sig_qr_t *q, char *out, size_t cap)
{
    uint8_t b[LC_SIG_QR_BLOB];
    if (cap < LC_SIG_QR_TEXT + 1) return 0;
    pack(q, b);
    size_t n = strlen(k_prefix);
    memcpy(out, k_prefix, n);
    for (size_t i = 0; i < LC_SIG_QR_BLOB; i += 3) { /* 72 is a multiple of 3: no padding */
        uint32_t v = ((uint32_t)b[i] << 16) | ((uint32_t)b[i + 1] << 8) | b[i + 2];
        out[n++] = k_b64[(v >> 18) & 63];
        out[n++] = k_b64[(v >> 12) & 63];
        out[n++] = k_b64[(v >> 6) & 63];
        out[n++] = k_b64[v & 63];
    }
    out[n] = '\0';
    return n;
}

int lc_sig_qr_parse(const char *text, size_t len, lc_sig_qr_t *q)
{
    while (len > 0 && (text[0] == ' ' || text[0] == '\t' || text[0] == '\r' || text[0] == '\n')) {
        text++;
        len--;
    }
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' ||
                       text[len - 1] == '\n')) {
        len--;
    }
    size_t pl = strlen(k_prefix);
    if (len != LC_SIG_QR_TEXT || memcmp(text, k_prefix, pl) != 0) return -1;
    uint8_t b[LC_SIG_QR_BLOB];
    for (size_t i = 0, o = 0; i < 96; i += 4, o += 3) {
        int v0 = b64_val(text[pl + i]), v1 = b64_val(text[pl + i + 1]);
        int v2 = b64_val(text[pl + i + 2]), v3 = b64_val(text[pl + i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return -1;
        uint32_t v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        b[o] = (uint8_t)(v >> 16);
        b[o + 1] = (uint8_t)(v >> 8);
        b[o + 2] = (uint8_t)v;
    }
    if (b[0] != 1 || lc_crc16(b, 70) != (uint16_t)(b[70] | (b[71] << 8))) return -1;
    memset(q, 0, sizeof(*q));
    q->key_id = (uint16_t)(b[1] | (b[2] << 8));
    memcpy(q->pkn, b + 3, 32);
    memcpy(q->token_id, b + 35, 8);
    memcpy(q->token_secret, b + 43, 16);
    memcpy(q->number, b + 59, 7);
    q->expiry = (uint32_t)b[66] | ((uint32_t)b[67] << 8) | ((uint32_t)b[68] << 16) | ((uint32_t)b[69] << 24);
    return 0;
}
