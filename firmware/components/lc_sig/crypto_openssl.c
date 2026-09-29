/* Host backend: OpenSSL 3 libcrypto. */
#include <openssl/evp.h>

#include "lc_sig_crypto.h"

/* Host tests only: non-zero makes lc_sig_aes128_block fail (fault injection). */
int lc_sig_test_fail_aes;

int lc_sig_aes128_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    if (lc_sig_test_fail_aes) return -1;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0;
    int ok = c != NULL && EVP_EncryptInit_ex(c, EVP_aes_128_ecb(), NULL, key, NULL) == 1 &&
             EVP_CIPHER_CTX_set_padding(c, 0) == 1 && EVP_EncryptUpdate(c, out, &n, in, 16) == 1 && n == 16;
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}

int lc_sig_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    unsigned n = 0;
    return EVP_Digest(data, len, out, &n, EVP_sha256(), NULL) == 1 && n == 32 ? 0 : -1;
}

int lc_sig_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32])
{
    size_t n = 0;
    return EVP_Q_mac(NULL, "HMAC", NULL, "SHA256", NULL, key, key_len, data, len, out, 32, &n) != NULL && n == 32
               ? 0
               : -1;
}

int lc_sig_x25519_public(const uint8_t priv[32], uint8_t pub[32])
{
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
    size_t n = 32;
    int ok = k != NULL && EVP_PKEY_get_raw_public_key(k, pub, &n) == 1 && n == 32;
    EVP_PKEY_free(k);
    return ok ? 0 : -1;
}

int lc_sig_x25519(const uint8_t priv[32], const uint8_t peer[32], uint8_t shared[32])
{
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
    EVP_PKEY *p = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, 32);
    EVP_PKEY_CTX *c = k != NULL ? EVP_PKEY_CTX_new(k, NULL) : NULL;
    size_t n = 32;
    int ok = c != NULL && p != NULL && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, p) == 1 &&
             EVP_PKEY_derive(c, shared, &n) == 1 && n == 32;
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(p);
    EVP_PKEY_free(k);
    return ok ? 0 : -1;
}
