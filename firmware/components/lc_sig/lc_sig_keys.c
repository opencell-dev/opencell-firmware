#include "lc_sig_keys.h"

#include <string.h>

#include "lc_sig.h"
#include "lc_sig_crypto.h"

int lc_sig_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

void lc_sig_wipe(void *p, size_t n)
{
    volatile uint8_t *vp = (volatile uint8_t *)p;
    while (n--) *vp++ = 0;
}

static int hmac8(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[8])
{
    uint8_t mac[32];
    if (lc_sig_hmac_sha256(key, key_len, data, len, mac) != 0) return -1;
    memcpy(out, mac, 8);
    return 0;
}

int lc_sig_act_tag(const uint8_t secret[16], uint32_t tmid, const uint8_t pkt[32], const uint8_t token_id[8],
                   uint8_t tag[8])
{
    uint8_t b[6 + 4 + 32 + 8];
    memcpy(b, "oc-act", 6);
    lc_sig_put32(b + 6, tmid);
    memcpy(b + 10, pkt, 32);
    memcpy(b + 42, token_id, 8);
    return hmac8(secret, 16, b, sizeof(b), tag);
}

int lc_sig_act_nak_tag(const uint8_t secret[16], uint32_t tmid, const uint8_t token_id[8], uint8_t reason,
                       uint8_t tag[8])
{
    uint8_t b[10 + 4 + 8 + 1];
    memcpy(b, "oc-act-nak", 10);
    lc_sig_put32(b + 10, tmid);
    memcpy(b + 14, token_id, 8);
    b[22] = reason;
    return hmac8(secret, 16, b, sizeof(b), tag);
}

int lc_sig_act_keys(const uint8_t priv[32], const uint8_t peer[32], uint32_t tmid, const uint8_t token_id[8],
                    uint8_t k[16], uint8_t opc[16])
{
    static const uint8_t zero[32];
    uint8_t ss[32], info[15 + 4];
    if (lc_sig_x25519(priv, peer, ss) != 0 || lc_sig_ct_equal(ss, zero, 32)) return -1;
    memcpy(info, "opencell-K-v1", 13);
    lc_sig_put32(info + 13, tmid);
    if (lc_sig_hkdf(token_id, 8, ss, 32, info, 17, k, 16) != 0) return -1;
    memcpy(info, "opencell-OPc-v1", 15);
    lc_sig_put32(info + 15, tmid);
    return lc_sig_hkdf(token_id, 8, ss, 32, info, 19, opc, 16);
}

int lc_sig_act_confirm(const uint8_t k[16], uint32_t tmid, const uint8_t token_id[8], uint8_t out[8])
{
    uint8_t b[9 + 4 + 8];
    memcpy(b, "oc-act-ok", 9);
    lc_sig_put32(b + 9, tmid);
    memcpy(b + 13, token_id, 8);
    return hmac8(k, 16, b, sizeof(b), out);
}

int lc_sig_session_keys(const uint8_t ck[16], const uint8_t ik[16], const uint8_t rand[16], uint32_t tmid,
                        uint8_t k_int[16], uint8_t k_enc[16])
{
    uint8_t ikm[32], info[19 + 4];
    memcpy(ikm, ck, 16);
    memcpy(ikm + 16, ik, 16);
    memcpy(info, "opencell-sig-int-v1", 19);
    lc_sig_put32(info + 19, tmid);
    if (lc_sig_hkdf(rand, 16, ikm, 32, info, 23, k_int, 16) != 0) return -1;
    memcpy(info, "opencell-sig-enc-v1", 19);
    return lc_sig_hkdf(rand, 16, ikm, 32, info, 23, k_enc, 16);
}

int lc_sig_voice_key(const uint8_t ck[16], const uint8_t ik[16], const uint8_t rand[16], uint32_t tmid,
                     uint32_t call_id, uint8_t out[16])
{
    uint8_t ikm[32], info[17 + 8];
    memcpy(ikm, ck, 16);
    memcpy(ikm + 16, ik, 16);
    memcpy(info, "opencell-voice-v1", 17);
    lc_sig_put32(info + 17, tmid);
    lc_sig_put32(info + 21, call_id);
    return lc_sig_hkdf(rand, 16, ikm, 32, info, 25, out, 16);
}
