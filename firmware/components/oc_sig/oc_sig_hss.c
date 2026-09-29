#include "oc_sig_hss.h"

#include <string.h>

#include "oc_sig_crypto.h"
#include "oc_sig_keys.h"
#include "oc_sig_milenage.h"

static const uint8_t k_amf[2] = { 0x80, 0x00 };
static const uint8_t k_amf_resync[2] = { 0x00, 0x00 };

int oc_sig_av_make(const uint8_t k[16], const uint8_t opc[16], const uint8_t sqn[6], const uint8_t rand[16],
                   oc_sig_av_t *av)
{
    oc_milenage_t o;
    if (oc_milenage(k, opc, rand, sqn, k_amf, &o) != 0) {
        oc_sig_wipe(&o, sizeof(o));
        return -1;
    }
    memcpy(av->rand, rand, 16);
    for (int i = 0; i < 6; i++) av->autn[i] = (uint8_t)(sqn[i] ^ o.ak[i]);
    memcpy(av->autn + 6, k_amf, 2);
    memcpy(av->autn + 8, o.mac_a, 8);
    memcpy(av->xres, o.res, 8);
    memcpy(av->ck, o.ck, 16);
    memcpy(av->ik, o.ik, 16);
    oc_sig_wipe(&o, sizeof(o));
    return 0;
}

int oc_sig_hxres(const uint8_t rand[16], const uint8_t res[8], uint8_t hxres[16])
{
    uint8_t in[24], d[32];
    memcpy(in, rand, 16);
    memcpy(in + 16, res, 8);
    int r = oc_sig_sha256(in, sizeof(in), d);
    if (r == 0) memcpy(hxres, d, 16);
    oc_sig_wipe(in, sizeof(in));
    oc_sig_wipe(d, sizeof(d));
    return r == 0 ? 0 : -1;
}

int oc_sig_av_for_cell(const oc_sig_av_t *av, oc_sig_cell_av_t *out)
{
    memset(out, 0, sizeof(*out));
    if (oc_sig_hxres(av->rand, av->xres, out->hxres) != 0) return -1;
    memcpy(out->rand, av->rand, 16);
    memcpy(out->autn, av->autn, 16);
    memcpy(out->ck, av->ck, 16);
    memcpy(out->ik, av->ik, 16);
    return 0;
}

int oc_sig_av_auts(const uint8_t k[16], const uint8_t opc[16], const uint8_t rand[16], const uint8_t auts[14],
                   uint8_t sqn_ms[6])
{
    static const uint8_t zero[6] = { 0 };
    oc_milenage_t o;
    uint8_t ms[6];
    if (oc_milenage(k, opc, rand, zero, k_amf_resync, &o) != 0) { /* AK* */
        oc_sig_wipe(&o, sizeof(o));
        return -1;
    }
    for (int i = 0; i < 6; i++) ms[i] = (uint8_t)(auts[i] ^ o.ak_s[i]);
    if (oc_milenage(k, opc, rand, ms, k_amf_resync, &o) != 0) {
        oc_sig_wipe(&o, sizeof(o));
        return -1;
    }
    if (!oc_sig_ct_equal(o.mac_s, auts + 6, 8)) {
        oc_sig_wipe(&o, sizeof(o));
        return -1;
    }
    memcpy(sqn_ms, ms, 6);
    oc_sig_wipe(&o, sizeof(o));
    return 0;
}

int oc_sig_act_answer(const oc_sig_act_token_t *tok, const uint8_t sk[32], uint32_t unix_now, uint32_t tmid,
                      const uint8_t token_id[8], const uint8_t pkt[32], const uint8_t tag[8],
                      const uint8_t number[OC_SIG_NUMBER_LEN], oc_sig_msg_t *out, uint8_t k[16], uint8_t opc[16])
{
    uint8_t reason = 0, t[8];
    memset(out, 0, sizeof(*out));
    /* A used token is refused unless it is still bound to this very terminal
     * (TMID, tag and key pair all the same): then its ACT_ACK was lost and the
     * terminal gave up, so it is answered again. */
    if (tmid == 0) {
        reason = OC_SIG_ACT_BAD_TAG; /* no terminal has TMID 0: the nearest existing "invalid input" status */
    } else if (!tok->known) {
        reason = OC_SIG_ACT_UNKNOWN;
    } else if (tok->used && tok->bound_tmid != tmid) {
        reason = OC_SIG_ACT_USED;
    } else if (!tok->used && unix_now > tok->expiry) {
        reason = OC_SIG_ACT_EXPIRED;
    } else if (oc_sig_act_tag(tok->secret, tmid, pkt, token_id, t) != 0 || !oc_sig_ct_equal(t, tag, 8)) {
        reason = tok->used ? OC_SIG_ACT_USED : OC_SIG_ACT_BAD_TAG;
    }
    oc_sig_wipe(t, sizeof(t));
    if (reason == 0 && oc_sig_act_keys(sk, pkt, tmid, token_id, k, opc) != 0) reason = OC_SIG_ACT_BAD_TAG;
    if (reason == 0 && tok->used && !oc_sig_ct_equal(k, tok->bound_k, 16)) reason = OC_SIG_ACT_USED; /* another key pair */
    if (reason != 0) {
        memset(k, 0, 16);
        memset(opc, 0, 16);
        out->type = OC_SIG_ACT_NAK;
        out->u.act_nak.reason = reason;
        if (tok->known) oc_sig_act_nak_tag(tok->secret, tmid, token_id, reason, out->u.act_nak.tag);
        return OC_SIG_ACT_REFUSED; /* no secret for an unknown token: zero tag (ruling in plan 5) */
    }
    out->type = OC_SIG_ACT_ACK;
    memcpy(out->u.act_ack.number, number, OC_SIG_NUMBER_LEN);
    oc_sig_act_confirm(k, tmid, token_id, out->u.act_ack.confirm);
    return tok->used ? OC_SIG_ACT_AGAIN : OC_SIG_ACT_FRESH;
}

static oc_sig_sub_t *bound(oc_sig_sub_t *subs, unsigned n, uint32_t tmid)
{
    for (unsigned i = 0; i < n; i++) {
        if (subs[i].activated && subs[i].tmid == tmid) return &subs[i];
    }
    return NULL;
}

int oc_sig_flat_act(oc_sig_sub_t *subs, unsigned n, const uint8_t sk[32], uint32_t unix_now, uint32_t tmid,
                    const uint8_t token_id[8], const uint8_t pkt[32], const uint8_t tag[8], oc_sig_msg_t *out,
                    uint32_t drop[2], unsigned *ndrop)
{
    static const uint8_t none[OC_SIG_NUMBER_LEN] = { 0 };
    static const uint8_t zero_token[8] = { 0 };
    oc_sig_sub_t *sub = NULL;
    oc_sig_act_token_t tok;
    uint8_t k[16], opc[16];
    *ndrop = 0;
    memset(&tok, 0, sizeof(tok));
    for (unsigned i = 0; i < n && sub == NULL; i++) {
        /* an all-zero token_id (never provisioned) can never match, even
         * against a request that also presents an all-zero token_id */
        if (memcmp(subs[i].token_id, zero_token, 8) == 0) continue;
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
    int r = oc_sig_act_answer(&tok, sk, unix_now, tmid, token_id, pkt, tag, sub != NULL ? sub->number : none, out,
                              k, opc);
    oc_sig_wipe(tok.secret, sizeof(tok.secret));
    oc_sig_wipe(tok.bound_k, sizeof(tok.bound_k));
    if (r != OC_SIG_ACT_FRESH) {
        oc_sig_wipe(k, sizeof(k));
        oc_sig_wipe(opc, sizeof(opc));
        return r;
    }
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
    oc_sig_wipe(k, sizeof(k));
    oc_sig_wipe(opc, sizeof(opc));
    memset(sub->sqn, 0, 6);
    sub->tmid = tmid;
    sub->activated = 1;
    sub->token_used = 1;
    return r;
}

uint8_t oc_sig_flat_av(oc_sig_sub_t *subs, unsigned n, uint32_t tmid, const uint8_t rand[16],
                       uint8_t number[OC_SIG_NUMBER_LEN], oc_sig_cell_av_t *av)
{
    oc_sig_sub_t *sub = bound(subs, n, tmid);
    if (sub == NULL) return OC_SIG_AV_NOT_ACTIVATED;
    uint8_t sqn[6];
    oc_sig_av_t full;
    /* SQN is 48 bits: at one authentication per second this wraps only
     * after about 8.9 million years, so the wrap is not handled. */
    oc_sig_sqn_put(sqn, oc_sig_sqn_get(sub->sqn) + 1u);
    int r = oc_sig_av_make(sub->k, sub->opc, sqn, rand, &full);
    if (r == 0) r = oc_sig_av_for_cell(&full, av);
    oc_sig_wipe(&full, sizeof(full));
    if (r != 0) return OC_SIG_AV_UNAVAILABLE;
    memcpy(sub->sqn, sqn, 6);
    memcpy(number, sub->number, OC_SIG_NUMBER_LEN);
    return OC_SIG_AV_OK;
}

uint8_t oc_sig_flat_resync(oc_sig_sub_t *subs, unsigned n, uint32_t tmid, const uint8_t rand[16],
                           const uint8_t auts[14], const uint8_t fresh_rand[16], uint8_t number[OC_SIG_NUMBER_LEN],
                           oc_sig_cell_av_t *av)
{
    oc_sig_sub_t *sub = bound(subs, n, tmid);
    uint8_t ms[6];
    if (sub == NULL) return OC_SIG_AV_NOT_ACTIVATED;
    if (oc_sig_av_auts(sub->k, sub->opc, rand, auts, ms) != 0) return OC_SIG_AV_AUTH_FAILED;
    memcpy(sub->sqn, ms, 6);
    return oc_sig_flat_av(subs, n, tmid, fresh_rand, number, av);
}
