#include <string.h>

#include "lc_sig_crypto.h"

int lc_sig_hkdf(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, const uint8_t *info,
                size_t info_len, uint8_t *out, size_t out_len)
{
    static const uint8_t zero_salt[32];
    uint8_t prk[32], t[32], buf[32 + 64 + 1];
    if (info_len > 64 || out_len > 255u * 32u) {
        return -1;
    }
    if (salt == NULL || salt_len == 0) {
        salt = zero_salt;
        salt_len = sizeof(zero_salt);
    }
    if (lc_sig_hmac_sha256(salt, salt_len, ikm, ikm_len, prk) != 0) {
        return -1;
    }
    size_t done = 0, tlen = 0;
    for (uint8_t i = 1; done < out_len; i++) {
        memcpy(buf, t, tlen);
        memcpy(buf + tlen, info, info_len);
        buf[tlen + info_len] = i;
        if (lc_sig_hmac_sha256(prk, sizeof(prk), buf, tlen + info_len + 1, t) != 0) {
            return -1;
        }
        tlen = 32;
        size_t n = out_len - done < 32 ? out_len - done : 32;
        memcpy(out + done, t, n);
        done += n;
    }
    return 0;
}

int lc_sig_aes128_ctr(const uint8_t key[16], const uint8_t nonce[14], uint8_t *data, size_t len)
{
    uint8_t blk[16], ks[16];
    memcpy(blk, nonce, 14);
    for (size_t off = 0, i = 0; off < len; off += 16, i++) {
        blk[14] = (uint8_t)(i >> 8);
        blk[15] = (uint8_t)i;
        if (lc_sig_aes128_block(key, blk, ks) != 0) {
            return -1;
        }
        size_t n = len - off < 16 ? len - off : 16;
        for (size_t j = 0; j < n; j++) {
            data[off + j] ^= ks[j];
        }
    }
    return 0;
}
