/* ESP32-S3 crypto backend: PSA Crypto (mbedTLS 4.0 in ESP-IDF v6), which
 * uses the hardware AES and SHA engines. Keys are imported as volatile
 * keys and destroyed after each call. */
#include <string.h>

#include "lc_sig_crypto.h"
#include "psa/crypto.h"

static int ready(void)
{
    return psa_crypto_init() == PSA_SUCCESS; /* cheap once initialised */
}

static psa_key_id_t import(psa_key_type_t type, size_t bits, psa_key_usage_t usage, psa_algorithm_t alg,
                           const uint8_t *key, size_t len)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, type);
    psa_set_key_bits(&a, bits);
    psa_set_key_usage_flags(&a, usage);
    psa_set_key_algorithm(&a, alg);
    psa_key_id_t id = 0;
    psa_status_t st = psa_import_key(&a, key, len, &id);
    psa_reset_key_attributes(&a);
    return st == PSA_SUCCESS ? id : 0;
}

int lc_sig_aes128_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    if (!ready()) return -1;
    psa_key_id_t id = import(PSA_KEY_TYPE_AES, 128, PSA_KEY_USAGE_ENCRYPT, PSA_ALG_ECB_NO_PADDING, key, 16);
    if (id == 0) return -1;
    size_t n = 0;
    psa_status_t st = psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS && n == 16 ? 0 : -1;
}

int lc_sig_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32])
{
    if (!ready()) return -1;
    psa_key_id_t id = import(PSA_KEY_TYPE_HMAC, key_len * 8u, PSA_KEY_USAGE_SIGN_MESSAGE,
                             PSA_ALG_HMAC(PSA_ALG_SHA_256), key, key_len);
    if (id == 0) return -1;
    size_t n = 0;
    psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, len, out, 32, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS && n == 32 ? 0 : -1;
}

static psa_key_id_t x25519_key(const uint8_t priv[32], psa_key_usage_t usage)
{
    return import(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY), 255, usage, PSA_ALG_ECDH, priv, 32);
}

int lc_sig_x25519_public(const uint8_t priv[32], uint8_t pub[32])
{
    if (!ready()) return -1;
    psa_key_id_t id = x25519_key(priv, 0);
    if (id == 0) return -1;
    size_t n = 0;
    psa_status_t st = psa_export_public_key(id, pub, 32, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS && n == 32 ? 0 : -1;
}

int lc_sig_x25519(const uint8_t priv[32], const uint8_t peer[32], uint8_t shared[32])
{
    if (!ready()) return -1;
    psa_key_id_t id = x25519_key(priv, PSA_KEY_USAGE_DERIVE);
    if (id == 0) return -1;
    size_t n = 0;
    psa_status_t st = psa_raw_key_agreement(PSA_ALG_ECDH, id, peer, 32, shared, 32, &n);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || n != 32) return -1;
    uint8_t acc = 0; /* RFC 7748 §6.1: reject the all-zero shared secret */
    for (int i = 0; i < 32; i++) acc |= shared[i];
    return acc != 0 ? 0 : -1;
}
