/* lc_sig_crypto: the four primitives a backend provides (crypto_openssl.c on
 * the host, crypto_psa.c on the ESP32-S3), and the portable pieces built on
 * them. Every function returns 0 on success. */
#ifndef LC_SIG_CRYPTO_H
#define LC_SIG_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

int lc_sig_aes128_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
int lc_sig_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]);
int lc_sig_x25519_public(const uint8_t priv[32], uint8_t pub[32]);
int lc_sig_x25519(const uint8_t priv[32], const uint8_t peer[32], uint8_t shared[32]);

/* HKDF-SHA256 (RFC 5869). info_len <= 64, out_len <= 255 * 32. */
int lc_sig_hkdf(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, const uint8_t *info,
                size_t info_len, uint8_t *out, size_t out_len);

/* AES-128-CTR in place. Counter block i = nonce (14 B) || i (2 B, big-endian), i from 0. */
int lc_sig_aes128_ctr(const uint8_t key[16], const uint8_t nonce[14], uint8_t *data, size_t len);

/* Known-answer test of every primitive (and MILENAGE); 0 when all pass,
 * otherwise the number of the first failing check. Run by the host tests and
 * at terminal boot. */
int lc_sig_selftest(void);

#endif
