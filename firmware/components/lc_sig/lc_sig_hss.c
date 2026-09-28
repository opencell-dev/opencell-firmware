#include "lc_sig_hss.h"

#include <string.h>

#include "lc_sig_keys.h"
#include "lc_sig_milenage.h"

static const uint8_t k_amf[2] = { 0x80, 0x00 };
static const uint8_t k_amf_resync[2] = { 0x00, 0x00 };

int lc_sig_av_make(const uint8_t k[16], const uint8_t opc[16], const uint8_t sqn[6], const uint8_t rand[16],
                   lc_sig_av_t *av)
{
    lc_milenage_t o;
    if (lc_milenage(k, opc, rand, sqn, k_amf, &o) != 0) return -1;
    memcpy(av->rand, rand, 16);
    for (int i = 0; i < 6; i++) av->autn[i] = (uint8_t)(sqn[i] ^ o.ak[i]);
    memcpy(av->autn + 6, k_amf, 2);
    memcpy(av->autn + 8, o.mac_a, 8);
    memcpy(av->xres, o.res, 8);
    memcpy(av->ck, o.ck, 16);
    memcpy(av->ik, o.ik, 16);
    return 0;
}

int lc_sig_av_auts(const uint8_t k[16], const uint8_t opc[16], const uint8_t rand[16], const uint8_t auts[14],
                   uint8_t sqn_ms[6])
{
    static const uint8_t zero[6] = { 0 };
    lc_milenage_t o;
    uint8_t ms[6];
    if (lc_milenage(k, opc, rand, zero, k_amf_resync, &o) != 0) return -1; /* AK* */
    for (int i = 0; i < 6; i++) ms[i] = (uint8_t)(auts[i] ^ o.ak_s[i]);
    if (lc_milenage(k, opc, rand, ms, k_amf_resync, &o) != 0) return -1;
    if (!lc_sig_ct_equal(o.mac_s, auts + 6, 8)) return -1;
    memcpy(sqn_ms, ms, 6);
    return 0;
}

int lc_sig_act_answer(const lc_sig_act_token_t *tok, const uint8_t sk[32], uint32_t unix_now, uint32_t tmid,
                      const uint8_t token_id[8], const uint8_t pkt[32], const uint8_t tag[8],
                      const uint8_t number[LC_SIG_NUMBER_LEN], lc_sig_msg_t *out, uint8_t k[16], uint8_t opc[16])
{
    uint8_t reason = 0, t[8];
    memset(out, 0, sizeof(*out));
    /* A used token is refused unless it is still bound to this very terminal
     * (TMID, tag and key pair all the same): then its ACT_ACK was lost and the
     * terminal gave up, so it is answered again. */
    if (!tok->known) {
        reason = LC_SIG_ACT_UNKNOWN;
    } else if (tok->used && tok->bound_tmid != tmid) {
        reason = LC_SIG_ACT_USED;
    } else if (!tok->used && unix_now > tok->expiry) {
        reason = LC_SIG_ACT_EXPIRED;
    } else if (lc_sig_act_tag(tok->secret, tmid, pkt, token_id, t) != 0 || !lc_sig_ct_equal(t, tag, 8)) {
        reason = tok->used ? LC_SIG_ACT_USED : LC_SIG_ACT_BAD_TAG;
    }
    if (reason == 0 && lc_sig_act_keys(sk, pkt, tmid, token_id, k, opc) != 0) reason = LC_SIG_ACT_BAD_TAG;
    if (reason == 0 && tok->used && !lc_sig_ct_equal(k, tok->bound_k, 16)) reason = LC_SIG_ACT_USED; /* another key pair */
    if (reason != 0) {
        memset(k, 0, 16);
        memset(opc, 0, 16);
        out->type = LC_SIG_ACT_NAK;
        out->u.act_nak.reason = reason;
        if (tok->known) lc_sig_act_nak_tag(tok->secret, tmid, token_id, reason, out->u.act_nak.tag);
        return LC_SIG_ACT_REFUSED; /* no secret for an unknown token: zero tag (ruling in plan 5) */
    }
    out->type = LC_SIG_ACT_ACK;
    memcpy(out->u.act_ack.number, number, LC_SIG_NUMBER_LEN);
    lc_sig_act_confirm(k, tmid, token_id, out->u.act_ack.confirm);
    return tok->used ? LC_SIG_ACT_AGAIN : LC_SIG_ACT_FRESH;
}

static lc_sig_sub_t *bound(lc_sig_sub_t *subs, unsigned n, uint32_t tmid)
{
    for (unsigned i = 0; i < n; i++) {
        if (subs[i].activated && subs[i].tmid == tmid) return &subs[i];
    }
    return NULL;
}

int lc_sig_flat_act(lc_sig_sub_t *subs, unsigned n, const uint8_t sk[32], uint32_t unix_now, uint32_t tmid,
                    const uint8_t token_id[8], const uint8_t pkt[32], const uint8_t tag[8], lc_sig_msg_t *out,
                    uint32_t drop[2], unsigned *ndrop)
{
    static const uint8_t none[LC_SIG_NUMBER_LEN] = { 0 };
    lc_sig_sub_t *sub = NULL;
    lc_sig_act_token_t tok;
    uint8_t k[16], opc[16];
    *ndrop = 0;
    memset(&tok, 0, sizeof(tok));
    for (unsigned i = 0; i < n && sub == NULL; i++) {
        if (memcmp(subs[i].token_id, token_id, 8) == 0) sub = &subs[i];
    }
    if (sub != NULL) {
        tok.known = 1;
        tok.used = sub->token_used;
        tok.expiry = sub->token_expiry;
        memcpy(tok.secret, sub->token_secret, 16);
        tok.bound_tmid = sub->activated ? sub->tmid : 0;
        memcpy(tok.bound_k, sub->k, 16);
    }
    int r = lc_sig_act_answer(&tok, sk, unix_now, tmid, token_id, pkt, tag, sub != NULL ? sub->number : none, out,
                              k, opc);
    if (r != LC_SIG_ACT_FRESH) return r;
    /* the subscriber is moving to a new terminal: the old one must not keep
     * serving calls or look registered (fix round 1, Review Focus 2) */
    if (sub->activated && sub->tmid != 0 && sub->tmid != tmid) drop[(*ndrop)++] = sub->tmid;
    /* ...and whatever tmid itself was bound to, its keys predate this activation */
    if (bound(subs, n, tmid) != NULL) drop[(*ndrop)++] = tmid;
    for (unsigned i = 0; i < n; i++) {
        if (subs[i].tmid == tmid) {
            subs[i].tmid = 0;
            subs[i].activated = 0;
        }
    }
    memcpy(sub->k, k, 16);
    memcpy(sub->opc, opc, 16);
    memset(sub->sqn, 0, 6);
    sub->tmid = tmid;
    sub->activated = 1;
    sub->token_used = 1;
    return r;
}

uint8_t lc_sig_flat_av(lc_sig_sub_t *subs, unsigned n, uint32_t tmid, const uint8_t rand[16],
                       uint8_t number[LC_SIG_NUMBER_LEN], lc_sig_av_t *av)
{
    lc_sig_sub_t *sub = bound(subs, n, tmid);
    if (sub == NULL) return LC_SIG_AV_NOT_ACTIVATED;
    uint8_t sqn[6];
    lc_sig_sqn_put(sqn, lc_sig_sqn_get(sub->sqn) + 1u);
    if (lc_sig_av_make(sub->k, sub->opc, sqn, rand, av) != 0) return LC_SIG_AV_UNAVAILABLE;
    memcpy(sub->sqn, sqn, 6);
    memcpy(number, sub->number, LC_SIG_NUMBER_LEN);
    return LC_SIG_AV_OK;
}

uint8_t lc_sig_flat_resync(lc_sig_sub_t *subs, unsigned n, uint32_t tmid, const uint8_t rand[16],
                           const uint8_t auts[14], const uint8_t fresh_rand[16], uint8_t number[LC_SIG_NUMBER_LEN],
                           lc_sig_av_t *av)
{
    lc_sig_sub_t *sub = bound(subs, n, tmid);
    uint8_t ms[6];
    if (sub == NULL) return LC_SIG_AV_NOT_ACTIVATED;
    if (lc_sig_av_auts(sub->k, sub->opc, rand, auts, ms) != 0) return LC_SIG_AV_AUTH_FAILED;
    memcpy(sub->sqn, ms, 6);
    return lc_sig_flat_av(subs, n, tmid, fresh_rand, number, av);
}
