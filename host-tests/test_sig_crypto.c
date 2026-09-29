#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "oc_sig_crypto.h"

void setUp(void) {}
void tearDown(void) {}

static void hex(const char *s, uint8_t *out)
{
    for (size_t i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(&s[2 * i], "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_aes128_fips197(void)
{
    uint8_t k[16], p[16], c[16], want[16];
    hex("000102030405060708090a0b0c0d0e0f", k);
    hex("00112233445566778899aabbccddeeff", p);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", want);
    TEST_ASSERT_EQUAL_INT(0, oc_sig_aes128_block(k, p, c));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, c, 16);
}

/* FIPS 180-2 appendix B.1 ("abc"); the host backend's plain SHA-256 behind
 * HXRES (network-core spec §19). */
static void test_sha256_fips180_abc(void)
{
    uint8_t d[32], want[32];
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
    TEST_ASSERT_EQUAL_INT(0, oc_sig_sha256((const uint8_t *)"abc", 3, d));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 32);
}

static void test_hmac_sha256_rfc4231_case2(void)
{
    uint8_t mac[32], want[32];
    hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want);
    const char *data = "what do ya want for nothing?";
    TEST_ASSERT_EQUAL_INT(0, oc_sig_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)data, strlen(data), mac));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, mac, 32);
}

static void test_hkdf_rfc5869_case1(void)
{
    uint8_t ikm[22], salt[13], info[10], okm[42], want[42];
    memset(ikm, 0x0b, sizeof(ikm));
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", want);
    TEST_ASSERT_EQUAL_INT(0, oc_sig_hkdf(salt, 13, ikm, 22, info, 10, okm, 42));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, okm, 42);
}

static void test_x25519_rfc7748(void)
{
    uint8_t a[32], apub[32], bpub[32], ss[32], want_pub[32], want_ss[32];
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bpub);
    hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want_pub);
    hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", want_ss);
    TEST_ASSERT_EQUAL_INT(0, oc_sig_x25519_public(a, apub));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_pub, apub, 32);
    TEST_ASSERT_EQUAL_INT(0, oc_sig_x25519(a, bpub, ss));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_ss, ss, 32);
}

/* CTR: keystream block i = AES(key, nonce || i as 2 bytes BE); XOR is its own inverse. */
static void test_ctr_matches_block_cipher_and_roundtrips(void)
{
    uint8_t key[16], nonce[14], data[40], orig[40], blk[16], ks[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 7);
    for (int i = 0; i < 14; i++) nonce[i] = (uint8_t)(0x40 + i);
    for (int i = 0; i < 40; i++) orig[i] = data[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL_INT(0, oc_sig_aes128_ctr(key, nonce, data, sizeof(data)));
    memcpy(blk, nonce, 14);
    blk[14] = 0;
    blk[15] = 2; /* third block covers bytes 32..39 */
    oc_sig_aes128_block(key, blk, ks);
    for (int i = 0; i < 8; i++) TEST_ASSERT_EQUAL_HEX8(orig[32 + i] ^ ks[i], data[32 + i]);
    TEST_ASSERT_EQUAL_INT(0, oc_sig_aes128_ctr(key, nonce, data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig, data, sizeof(data));
}

static void test_selftest_passes(void)
{
    TEST_ASSERT_EQUAL_INT(0, oc_sig_selftest());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_aes128_fips197);
    RUN_TEST(test_sha256_fips180_abc);
    RUN_TEST(test_hmac_sha256_rfc4231_case2);
    RUN_TEST(test_hkdf_rfc5869_case1);
    RUN_TEST(test_x25519_rfc7748);
    RUN_TEST(test_ctr_matches_block_cipher_and_roundtrips);
    RUN_TEST(test_selftest_passes);
    return UNITY_END();
}
