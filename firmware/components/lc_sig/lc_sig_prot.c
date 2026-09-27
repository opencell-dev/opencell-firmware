#include "lc_sig_prot.h"

#include <string.h>

#include "lc_sig_crypto.h"
#include "lc_sig_keys.h"

void lc_sig_sec_init(lc_sig_sec_t *s, uint8_t tx_dir)
{
    memset(s, 0, sizeof(*s));
    s->tx_dir = tx_dir;
    s->encrypt = -1;
}

void lc_sig_sec_key(lc_sig_sec_t *s, const uint8_t k_int[16], const uint8_t k_enc[16], int encrypt)
{
    memcpy(s->k_int, k_int, 16);
    memcpy(s->k_enc, k_enc, 16);
    s->encrypt = encrypt;
    s->keyed = 1;
    s->tx_ctr = 0;
    s->rx_next = 0;
}

int lc_sig_preauth_type(uint8_t type)
{
    switch (type) {
    case LC_SIG_ACT_REQ: case LC_SIG_ACT_ACK: case LC_SIG_ACT_NAK: case LC_SIG_REG_REQ:
    case LC_SIG_AUTH_REQ: case LC_SIG_AUTH_RSP: case LC_SIG_AUTH_FAIL: case LC_SIG_REG_REJ:
        return 1;
    default:
        return 0;
    }
}

/* HMAC-SHA256(K_int, dir || ctr || type || prot || body)[0:4] over msg = type|prot|ctr_lo|body. */
static int mac4(const uint8_t k[16], uint8_t dir, uint32_t ctr, const uint8_t *msg, size_t body_len, uint8_t out[4])
{
    uint8_t buf[7 + LC_SIG_MAX_MSG], mac[32];
    buf[0] = dir;
    lc_sig_put32(buf + 1, ctr);
    buf[5] = msg[0];
    buf[6] = msg[1];
    memcpy(buf + 7, msg + 3, body_len);
    if (lc_sig_hmac_sha256(k, 16, buf, 7 + body_len, mac) != 0) return -1;
    memcpy(out, mac, 4);
    return 0;
}

static int prot_crypt(const uint8_t k[16], uint8_t dir, uint32_t ctr, uint8_t *body, size_t n)
{
    uint8_t nonce[14] = { 0 };
    nonce[0] = dir;
    lc_sig_put32(nonce + 1, ctr);
    return lc_sig_aes128_ctr(k, nonce, body, n);
}

size_t lc_sig_seal(lc_sig_sec_t *s, const lc_sig_msg_t *m, uint8_t *out, size_t cap)
{
    uint8_t body[64];
    size_t bl = lc_sig_body_encode(m, body, sizeof(body));
    if (bl == 0) return 0;
    uint8_t prot = 0;
    if (!lc_sig_preauth_type(m->type)) {
        if (!s->keyed) return 0;
        prot = s->encrypt == 1 ? 2 : 1;
    }
    size_t need = 3 + bl + (prot ? 4u : 0u);
    if (need > cap || need > LC_SIG_MAX_MSG) return 0;
    uint32_t ctr = prot ? s->tx_ctr : 0;
    out[0] = m->type;
    out[1] = prot;
    out[2] = (uint8_t)ctr;
    memcpy(out + 3, body, bl);
    if (prot == 2 && prot_crypt(s->k_enc, s->tx_dir, ctr, out + 3, bl) != 0) return 0;
    if (prot) {
        if (mac4(s->k_int, s->tx_dir, ctr, out, bl, out + 3 + bl) != 0) return 0;
        s->tx_ctr++;
    }
    return need;
}

int lc_sig_open(lc_sig_sec_t *s, const uint8_t *in, size_t len, lc_sig_msg_t *m)
{
    if (len < 3) return -1;
    uint8_t type = in[0], prot = in[1];
    if (lc_sig_preauth_type(type)) {
        return prot == 0 ? lc_sig_body_decode(type, in + 3, len - 3, m) : -1;
    }
    if (!s->keyed || prot < 1 || prot > 2 || len < 7) return -1;
    if ((s->encrypt == 1 && prot != 2) || (s->encrypt == 0 && prot != 1)) return -1;
    uint32_t cand = (s->rx_next & ~0xFFu) | in[2];
    if (cand < s->rx_next) cand += 256u;
    if (cand - s->rx_next >= 128u) return -1;
    size_t bl = len - 7;
    uint8_t dir = (uint8_t)(s->tx_dir ^ 1u), mac[4], body[64];
    if (bl > sizeof(body) || mac4(s->k_int, dir, cand, in, bl, mac) != 0 || !lc_sig_ct_equal(mac, in + 3 + bl, 4)) {
        return -1;
    }
    memcpy(body, in + 3, bl);
    if (prot == 2 && prot_crypt(s->k_enc, dir, cand, body, bl) != 0) return -1;
    if (lc_sig_body_decode(type, body, bl, m) != 0) return -1;
    s->rx_next = cand + 1u;
    return 0;
}
