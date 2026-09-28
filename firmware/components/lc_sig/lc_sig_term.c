#include "lc_sig_term.h"

#include <string.h>

#include "lc_sig_crypto.h"
#include "lc_sig_keys.h"
#include "lc_sig_milenage.h"

#define US(s) ((uint64_t)(s) * 1000000ull)

static const uint8_t k_amf[2] = { 0x80, 0x00 };
static const uint8_t k_amf_resync[2] = { 0x00, 0x00 };
static const uint8_t k_sw[3] = { 0, 6, 0 }; /* 0.6.0: numbering v2 */

/* ------------------------------------------------------------ identity */

size_t lc_sig_ident_pack(const lc_sig_ident_t *id, uint8_t out[LC_SIG_IDENT_BLOB])
{
    out[0] = 2;
    out[1] = (uint8_t)(id->activated != 0);
    out[2] = (uint8_t)id->key_id;
    out[3] = (uint8_t)(id->key_id >> 8);
    memcpy(out + 4, id->sk, 32);
    memcpy(out + 36, id->pk, 32);
    memcpy(out + 68, id->k, 16);
    memcpy(out + 84, id->opc, 16);
    memcpy(out + 100, id->sqn, 6);
    memcpy(out + 106, id->number, LC_SIG_NUMBER_LEN);
    return LC_SIG_IDENT_BLOB;
}

int lc_sig_ident_unpack(const uint8_t *in, size_t len, lc_sig_ident_t *id)
{
    if (len == LC_SIG_IDENT_BLOB_V1 && in[0] == 1) return LC_SIG_IDENT_OLD;
    if (len != LC_SIG_IDENT_BLOB || in[0] != 2) return -1;
    memset(id, 0, sizeof(*id));
    id->activated = in[1] != 0;
    id->key_id = (uint16_t)(in[2] | (in[3] << 8));
    memcpy(id->sk, in + 4, 32);
    memcpy(id->pk, in + 36, 32);
    memcpy(id->k, in + 68, 16);
    memcpy(id->opc, in + 84, 16);
    memcpy(id->sqn, in + 100, 6);
    memcpy(id->number, in + 106, LC_SIG_NUMBER_LEN);
    return 0;
}

int lc_sig_ident_new(lc_sig_ident_t *id, const uint8_t random32[32])
{
    memset(id, 0, sizeof(*id));
    memcpy(id->sk, random32, 32);
    return lc_sig_x25519_public(id->sk, id->pk);
}

/* ------------------------------------------------------------- helpers */

static int in_call(uint8_t s)
{
    return s >= LC_SIG_ST_CALLING && s <= LC_SIG_ST_RELEASING;
}

static void emit(lc_sig_term_t *t, const uint8_t *ev, uint8_t n)
{
    if (t->io.event != NULL) t->io.event(t->io.ctx, ev, n);
}

static void save(lc_sig_term_t *t)
{
    if (t->io.save != NULL) t->io.save(t->io.ctx, t->id);
}

static void queue(lc_sig_term_t *t, const lc_sig_msg_t *m)
{
    if (t->out_count < LC_SIG_OUTQ) t->outq[t->out_count++] = *m;
}

static void queue_call(lc_sig_term_t *t, uint8_t type, uint32_t call_id)
{
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.u.call.call_id = call_id;
    queue(t, &m);
}

static void queue_release(lc_sig_term_t *t, uint32_t call_id, uint8_t cause)
{
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_RELEASE;
    m.u.release.call_id = call_id;
    m.u.release.cause = cause;
    queue(t, &m);
}

static void reg_start(lc_sig_term_t *t, uint64_t at)
{
    t->state = LC_SIG_ST_REGISTERING;
    t->reg_sent = 0;
    t->auth_sent = 0; /* from here, only this attempt's AUTH_RSP lets REG_ACK in (fix round 3) */
    t->reg_retry_at = at;
    /* A fresh registration's own REG_ACK brings a CHAN_LIST push anyway, so
     * any config ask outstanding from the old session is moot - carrying it
     * over would leave a stale cfg_retry_at that fires the moment we're
     * REGISTERED again (Task 9 fix). */
    t->cfg_asked = 0;
    t->cfg_answered = 0;
}

static void reg_failed(lc_sig_term_t *t, uint8_t reason, uint64_t now)
{
    uint8_t ev[2] = { LC_SIG_EV_REG_FAILED, reason };
    emit(t, ev, 2);
    if (reason == LC_SIG_REG_TIMEOUT) {
        /* no answer (coverage lost): not a refusal, so no escalation */
        reg_start(t, now + US(30));
        return;
    }
    t->backoff_s = t->backoff_s == 0 ? 30u : (t->backoff_s >= 300u ? 600u : t->backoff_s * 2u);
    reg_start(t, now + US(t->backoff_s));
}

static void act_failed(lc_sig_term_t *t, uint8_t reason, uint64_t now)
{
    uint8_t ev[2] = { LC_SIG_EV_ACT_FAILED, reason };
    emit(t, ev, 2);
    if (t->id->activated) {
        reg_start(t, now);
    } else {
        t->state = LC_SIG_ST_NOT_ACTIVATED;
    }
}

static void call_end(lc_sig_term_t *t, uint8_t cause)
{
    uint8_t ev[6] = { LC_SIG_EV_ENDED };
    lc_sig_put32(ev + 1, t->call_id);
    ev[5] = cause;
    emit(t, ev, 6);
    t->state = LC_SIG_ST_REGISTERED;
    t->call_id = 0;
    t->answered = 0;
    t->hangup_pending = 0;
}

static void call_up(lc_sig_term_t *t, uint8_t codec)
{
    lc_sig_voice_key(t->ck, t->ik, t->rand, t->tmid, t->call_id, t->k_voice);
    t->d_tx = 0;
    t->d_rx_next = 0;
    t->state = LC_SIG_ST_IN_CALL;
    uint8_t ev[6] = { LC_SIG_EV_CONNECTED };
    lc_sig_put32(ev + 1, t->call_id);
    ev[5] = codec;
    emit(t, ev, 6);
}

/* Send what can go now; ask for a channel if a message is waiting. */
static void flush(lc_sig_term_t *t, uint64_t now)
{
    while (t->out_count > 0 && t->granted) {
        if (lc_sig_chan_send(&t->ch, &t->outq[0], now) != 0) break; /* a request in flight: later */
        t->out_count--;
        memmove(t->outq, t->outq + 1, t->out_count * sizeof(t->outq[0]));
    }
    /* Not granted: ask for a channel (at most every 2 s) whenever something
     * still needs one to go out or complete - queued locally, waiting on the
     * channel's own request/response, or already fragmented into its txq. A
     * message handed to the channel has left out_count, so out_count alone
     * missed this once the request was in flight and the grant went away. */
    if (!t->granted && t->attached && now >= t->svc_due && t->io.service_req != NULL &&
        (t->out_count > 0 || lc_sig_chan_busy(&t->ch) || t->ch.txq_count > 0)) {
        uint8_t cause = in_call(t->state) ? LC_SIG_SVC_CALL : LC_SIG_SVC_REGISTER;
        if (t->io.service_req(t->io.ctx, cause) == 0) t->svc_due = now + US(2);
    }
    const uint8_t *p;
    uint8_t n;
    while (t->granted && lc_sig_chan_peek(&t->ch, &p, &n) == 0 && t->io.send(t->io.ctx, p, n) == 0) {
        lc_sig_chan_pop(&t->ch);
    }
}

/* -------------------------------------------------------------- public */

void lc_sig_term_init(lc_sig_term_t *t, const lc_sig_term_io_t *io, lc_sig_ident_t *id, uint32_t tmid,
                      uint64_t now_us)
{
    memset(t, 0, sizeof(*t));
    t->io = *io;
    t->id = id;
    t->tmid = tmid;
    lc_sig_chan_init(&t->ch, 0);
    t->state = id->activated ? LC_SIG_ST_REGISTERING : LC_SIG_ST_NOT_ACTIVATED;
    t->reg_retry_at = now_us;
}

uint8_t lc_sig_term_state(const lc_sig_term_t *t)
{
    return t->state;
}

void lc_sig_term_link(lc_sig_term_t *t, int attached, int granted, uint64_t now_us)
{
    if (t->granted && !granted) t->grant_lost_at = now_us;
    if (granted) t->grant_lost_at = 0;
    if (!attached) t->svc_due = 0; /* ask again as soon as the link is back */
    /* Back on a cell after losing it: its network may have restarted (and lost
     * our session) or be another one, so register again (ruling in plan 5). */
    if (attached && !t->attached && t->state == LC_SIG_ST_REGISTERED) reg_start(t, now_us);
    /* Back in coverage while waiting to retry a registration: try now. */
    if (attached && !t->attached && t->state == LC_SIG_ST_REGISTERING && !t->reg_sent) t->reg_retry_at = now_us;
    t->attached = attached;
    t->granted = granted;
}

void lc_sig_term_cell_mode(lc_sig_term_t *t, uint8_t mode, uint64_t now_us)
{
    if (t->state == LC_SIG_ST_REGISTERED && t->reg_mode != 0 && mode != t->reg_mode) reg_start(t, now_us);
}

void lc_sig_term_cell_cfg(lc_sig_term_t *t, uint8_t cfg_ver, uint64_t now_us)
{
    /* Granted, a service request can't go out (RACH UPPER is IDLE only); the
     * network pushes CHAN_LIST after every REG_ACK anyway. */
    if (t->state != LC_SIG_ST_REGISTERED || !t->attached || t->granted || t->io.service_req == NULL) return;
    if (((t->list_ver ^ cfg_ver) & 3u) == 0 || now_us < t->cfg_retry_at) return;
    /* Already asked for this exact cfg_ver and a CHAN_LIST answered it: a
     * persistent mismatch (the beacon's cfg_ver stuck against a list version
     * that doesn't clear it) is not worth asking again every 30 s forever -
     * only a DIFFERENT cfg_ver reopens the question (M1). */
    if (t->cfg_answered && t->cfg_asked && t->cfg_asked_ver == cfg_ver) return;
    /* An ask still unanswered when its own 30 s retry comes due means no
     * CHAN_LIST ever came back at all - not even a mismatched one. Bench
     * case: the cell was restarted within ~1 s, so we never lost sync and
     * still believe we're REGISTERED, but the new network has no session for
     * us - only a REGISTERED session holds the keys CHAN_LIST needs, so our
     * cause-4 ask goes unanswered forever (Task 9 fix). Re-register instead
     * of asking again: a REG_ACK is followed by the network's CHAN_LIST push
     * anyway, so the list still arrives - just through that path. */
    if (t->cfg_asked && !t->cfg_answered) {
        reg_start(t, now_us);
        return;
    }
    if (t->io.service_req(t->io.ctx, LC_SIG_SVC_CONFIG) == 0) {
        t->cfg_retry_at = now_us + US(30);
        t->cfg_asked_ver = cfg_ver;
        t->cfg_asked = 1;
        t->cfg_answered = 0; /* waiting on the answer to this ask now */
    }
}

int lc_sig_term_chan_list(lc_sig_term_t *t, lc_sig_chan_list_t *out)
{
    if (!t->list_new) return 0;
    *out = t->list_in;
    t->list_new = 0;
    return 1;
}

int lc_sig_term_act_prepare(const lc_sig_ident_t *id, uint32_t tmid, const uint8_t *text, size_t len,
                            lc_sig_act_prep_t *p)
{
    if (len == 0 || len > 120) return LC_SIG_ATT_BAD_LEN;
    if (lc_sig_qr_parse((const char *)text, len, &p->qr) != 0) return LC_SIG_ATT_BAD_ARG;
    /* A low-order network key gives an all-zero secret: refused here. */
    if (lc_sig_act_keys(id->sk, p->qr.pkn, tmid, p->qr.token_id, p->k, p->opc) != 0) return LC_SIG_ATT_BAD_ARG;
    return 0;
}

int lc_sig_term_activate(lc_sig_term_t *t, const lc_sig_act_prep_t *p, uint64_t now_us)
{
    if (t->state == LC_SIG_ST_ACTIVATING || in_call(t->state)) return LC_SIG_ATT_NOT_NOW;
    t->qr = p->qr;
    memcpy(t->act_k, p->k, 16);
    memcpy(t->act_opc, p->opc, 16);
    t->state = LC_SIG_ST_ACTIVATING;
    t->act_sent = 0;
    t->out_count = 0;
    lc_sig_chan_reset(&t->ch);
    flush(t, now_us);
    return 0;
}

int lc_sig_term_command(lc_sig_term_t *t, const uint8_t *cmd, size_t len, uint64_t now_us)
{
    if (len == 0) return LC_SIG_ATT_BAD_LEN;
    const uint8_t *a = cmd + 1;
    size_t al = len - 1;
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    switch (cmd[0]) {
    case LC_SIG_CMD_ACTIVATE: {
        if (t->state == LC_SIG_ST_ACTIVATING || in_call(t->state)) return LC_SIG_ATT_NOT_NOW;
        lc_sig_act_prep_t p;
        int err = lc_sig_term_act_prepare(t->id, t->tmid, a, al, &p);
        return err != 0 ? err : lc_sig_term_activate(t, &p, now_us);
    }
    case LC_SIG_CMD_DIAL:
        /* Any dialled form (numbering v2 §5.3), completed from the terminal's
         * own number; CALL_SETUP always carries the full form. */
        if (al == 0 || al > LC_SIG_DIAL_MAX) return LC_SIG_ATT_BAD_LEN;
        if (t->state != LC_SIG_ST_REGISTERED) return LC_SIG_ATT_NOT_NOW;
        m.type = LC_SIG_CALL_SETUP;
        if (lc_sig_number_normalize((const char *)a, al, t->id->number, m.u.call_setup.called) != 0) {
            return LC_SIG_ATT_BAD_ARG;
        }
        m.u.call_setup.ref = ++t->call_ref;
        m.u.call_setup.codec_caps = 1;
        t->call_id = 0;
        queue(t, &m);
        t->state = LC_SIG_ST_CALLING;
        t->call_timer_at = now_us + US(10);
        break;
    case LC_SIG_CMD_ANSWER:
        if (al != 0) return LC_SIG_ATT_BAD_LEN;
        if (t->state != LC_SIG_ST_RINGING_IN || t->answered) return LC_SIG_ATT_NOT_NOW;
        m.type = LC_SIG_CONNECT;
        m.u.connect.call_id = t->call_id;
        m.u.connect.codec = 1;
        queue(t, &m);
        t->answered = 1;
        t->call_timer_at = now_us + US(10);
        break;
    case LC_SIG_CMD_REJECT:
    case LC_SIG_CMD_HANGUP: {
        if (al != 0) return LC_SIG_ATT_BAD_LEN;
        int ringing_in = t->state == LC_SIG_ST_RINGING_IN;
        if (cmd[0] == LC_SIG_CMD_REJECT ? !ringing_in
                                        : !(ringing_in || t->state == LC_SIG_ST_CALLING ||
                                            t->state == LC_SIG_ST_RINGING_OUT || t->state == LC_SIG_ST_IN_CALL)) {
            return LC_SIG_ATT_NOT_NOW;
        }
        t->end_cause = ringing_in ? LC_SIG_CAUSE_REJECTED : LC_SIG_CAUSE_NORMAL;
        if (t->state == LC_SIG_ST_CALLING && t->call_id == 0) {
            if (t->out_count > 0 && t->outq[t->out_count - 1].type == LC_SIG_CALL_SETUP) {
                t->out_count--; /* never sent: just drop it */
                call_end(t, LC_SIG_CAUSE_NORMAL);
                break;
            }
            t->hangup_pending = 1; /* RELEASE once CALL_PROC gives the call id */
        } else {
            queue_release(t, t->call_id, t->end_cause);
        }
        t->state = LC_SIG_ST_RELEASING;
        t->call_timer_at = now_us + US(10);
        break;
    }
    case LC_SIG_CMD_DEACTIVATE:
        if (al != 1) return LC_SIG_ATT_BAD_LEN;
        if (a[0] != 0xA5) return LC_SIG_ATT_BAD_ARG;
        if (in_call(t->state)) return LC_SIG_ATT_NOT_NOW; /* as ACTIVATE: hang up first */
        t->id->activated = 0;
        t->id->key_id = 0;
        memset(t->id->k, 0, 16);
        memset(t->id->opc, 0, 16);
        memset(t->id->sqn, 0, 6);
        memset(t->id->number, 0, sizeof(t->id->number));
        save(t);
        /* ...and every RAM copy (spec §3.2 "wipes the keys"): a pending
         * activation's keys and QR (token secret), the registration's vector,
         * the last call's voice key, the session keys (sec_init below) */
        memset(t->act_k, 0, sizeof(t->act_k));
        memset(t->act_opc, 0, sizeof(t->act_opc));
        memset(&t->qr, 0, sizeof(t->qr));
        memset(t->ck, 0, sizeof(t->ck));
        memset(t->ik, 0, sizeof(t->ik));
        memset(t->rand, 0, sizeof(t->rand));
        memset(t->k_voice, 0, sizeof(t->k_voice));
        t->reg_mode = 0;
        t->list_ver = 0; /* the network's entries go too (lc_term_scan_deactivate) */
        t->list_new = 0;
        t->cfg_asked = 0;
        t->cfg_answered = 0;
        lc_sig_sec_init(&t->ch.sec, 0);
        lc_sig_chan_reset(&t->ch);
        t->out_count = 0;
        t->state = LC_SIG_ST_NOT_ACTIVATED;
        {
            uint8_t ev = LC_SIG_EV_DEACTIVATED;
            emit(t, &ev, 1);
        }
        break;
    default:
        return LC_SIG_ATT_BAD_ARG;
    }
    flush(t, now_us);
    return 0;
}

static void on_auth_req(lc_sig_term_t *t, const lc_sig_msg_t *m, uint64_t now)
{
    static const uint8_t zero[6] = { 0 };
    const uint8_t *rand = m->u.auth_req.rand, *autn = m->u.auth_req.autn;
    lc_sig_ident_t *id = t->id;
    lc_milenage_t o;
    lc_sig_msg_t r;
    uint8_t sqn[6];
    memset(&r, 0, sizeof(r));
    if (lc_milenage(id->k, id->opc, rand, zero, k_amf, &o) != 0) return; /* AK */
    for (int i = 0; i < 6; i++) sqn[i] = (uint8_t)(autn[i] ^ o.ak[i]);
    if (lc_milenage(id->k, id->opc, rand, sqn, autn + 6, &o) != 0) return;
    if (!lc_sig_ct_equal(o.mac_a, autn + 8, 8)) {
        r.type = LC_SIG_AUTH_FAIL;
        r.u.auth_fail.cause = 1;
        queue(t, &r);
        reg_failed(t, LC_SIG_REG_NET_AUTH, now); /* retries after the backoff */
        return;
    }
    uint64_t s = lc_sig_sqn_get(sqn), ms = lc_sig_sqn_get(id->sqn);
    if (!(s > ms && s - ms <= (1ull << 28))) {
        lc_milenage_t o2;
        if (lc_milenage(id->k, id->opc, rand, id->sqn, k_amf_resync, &o2) != 0) return;
        r.type = LC_SIG_AUTH_FAIL;
        r.u.auth_fail.cause = 2;
        for (int i = 0; i < 6; i++) r.u.auth_fail.auts[i] = (uint8_t)(id->sqn[i] ^ o2.ak_s[i]);
        memcpy(r.u.auth_fail.auts + 6, o2.mac_s, 8);
        queue(t, &r);
        return;
    }
    memcpy(id->sqn, sqn, 6);
    save(t);
    memcpy(t->ck, o.ck, 16);
    memcpy(t->ik, o.ik, 16);
    memcpy(t->rand, rand, 16);
    uint8_t ki[16], ke[16];
    lc_sig_session_keys(o.ck, o.ik, rand, t->tmid, ki, ke);
    lc_sig_sec_key(&t->ch.sec, ki, ke, -1); /* the mode comes in REG_ACK */
    r.type = LC_SIG_AUTH_RSP;
    memcpy(r.u.auth_rsp.res, o.res, 8);
    queue(t, &r);
    t->auth_sent = 1;
}

static void handle(lc_sig_term_t *t, const lc_sig_msg_t *m, uint64_t now)
{
    lc_sig_ident_t *id = t->id;
    switch (m->type) {
    case LC_SIG_ACT_ACK: {
        if (t->state != LC_SIG_ST_ACTIVATING) return;
        uint8_t conf[8];
        if (lc_sig_act_confirm(t->act_k, t->tmid, t->qr.token_id, conf) != 0 ||
            !lc_sig_ct_equal(conf, m->u.act_ack.confirm, 8)) {
            act_failed(t, LC_SIG_ACT_BAD_CONFIRM, now);
            return;
        }
        memcpy(id->k, t->act_k, 16);
        memcpy(id->opc, t->act_opc, 16);
        memset(id->sqn, 0, 6);
        memcpy(id->number, m->u.act_ack.number, LC_SIG_NUMBER_LEN);
        id->key_id = t->qr.key_id;
        id->activated = 1;
        save(t);
        lc_sig_sec_init(&t->ch.sec, 0);
        uint8_t ev[1 + LC_SIG_NUMBER_LEN] = { LC_SIG_EV_ACTIVATED };
        memcpy(ev + 1, id->number, LC_SIG_NUMBER_LEN);
        emit(t, ev, sizeof(ev));
        t->backoff_s = 0;
        reg_start(t, now);
        return;
    }
    case LC_SIG_ACT_NAK:
        /* Reported whether or not its tag verifies: a terminal holding a forged
         * QR can't check the network's tag, and a spoofed NAK can only fail one
         * attempt, as a jammer could (ruling in plan 5). */
        if (t->state == LC_SIG_ST_ACTIVATING) act_failed(t, m->u.act_nak.reason, now);
        return;
    case LC_SIG_AUTH_REQ:
        if (t->state == LC_SIG_ST_REGISTERING) on_auth_req(t, m, now);
        return;
    case LC_SIG_REG_ACK: {
        /* Only once this attempt's AUTH_RSP went out: until then the chan
         * still holds the previous session's keys, and a REG_ACK they open -
         * the network's cached reply, drawn out by a recorded AUTH_RSP played
         * back (lc_sig_chan_rx can't tell it from a retransmission) - would
         * complete the registration without an AKA. A lost REG_ACK's resend
         * (answering this attempt's retransmitted AUTH_RSP) still passes. */
        if (t->state != LC_SIG_ST_REGISTERING || !t->auth_sent) return;
        t->ch.sec.encrypt = m->u.reg_ack.mode == LC_SIG_MODE_PART15 ? 1 : 0;
        t->reg_mode = m->u.reg_ack.mode;
        t->rereg_at = now + US(m->u.reg_ack.period_s != 0 ? m->u.reg_ack.period_s : 1800u);
        t->state = LC_SIG_ST_REGISTERED;
        t->backoff_s = 0;
        uint8_t ev[2 + LC_SIG_NUMBER_LEN] = { LC_SIG_EV_REGISTERED };
        memcpy(ev + 1, m->u.reg_ack.number, LC_SIG_NUMBER_LEN);
        ev[1 + LC_SIG_NUMBER_LEN] = m->u.reg_ack.mode;
        emit(t, ev, sizeof(ev));
        return;
    }
    case LC_SIG_REG_REJ:
        /* only while a registration is in flight: a REJ answering our AUTH_FAIL
         * after a bad network MAC was already reported. REG_REJ travels
         * unauthenticated (prot 0), so a forged one must not be able to clear
         * the terminal's activation on its own word: every cause, including
         * "not activated", is just reported and backed off from. Only a
         * DEACTIVATE command from the phone drops the activation (the BLE
         * link is not paired or encrypted yet: see security-model.md). */
        if (t->state != LC_SIG_ST_REGISTERING || !t->reg_sent) return;
        reg_failed(t, m->u.reg_rej.cause, now);
        return;
    case LC_SIG_CALL_PROC:
        if (m->u.call_proc.ref != t->call_ref) return;
        if (t->state == LC_SIG_ST_CALLING) {
            t->call_id = m->u.call_proc.call_id;
            t->call_timer_at = now + US(60); /* setup done: now the far end has 60 s to answer */
        } else if (t->state == LC_SIG_ST_RELEASING && t->hangup_pending) {
            t->call_id = m->u.call_proc.call_id;
            t->hangup_pending = 0;
            queue_release(t, t->call_id, LC_SIG_CAUSE_NORMAL);
            t->call_timer_at = now + US(5);
        }
        return;
    case LC_SIG_ALERTING:
        if (t->state == LC_SIG_ST_CALLING && m->u.call.call_id == t->call_id) {
            t->state = LC_SIG_ST_RINGING_OUT;
            t->call_timer_at = now + US(60);
            uint8_t ev[5] = { LC_SIG_EV_RINGING };
            lc_sig_put32(ev + 1, t->call_id);
            emit(t, ev, 5);
        }
        return;
    case LC_SIG_CONNECT:
        if ((t->state == LC_SIG_ST_CALLING || t->state == LC_SIG_ST_RINGING_OUT) &&
            m->u.connect.call_id == t->call_id) {
            queue_call(t, LC_SIG_CONNECT_ACK, t->call_id);
            call_up(t, m->u.connect.codec);
        }
        return;
    case LC_SIG_SETUP_IND:
        if (t->state == LC_SIG_ST_REGISTERED) {
            t->call_id = m->u.setup_ind.call_id;
            t->answered = 0;
            t->state = LC_SIG_ST_RINGING_IN;
            t->call_timer_at = now + US(60);
            queue_call(t, LC_SIG_ALERTING, t->call_id);
            uint8_t ev[5 + LC_SIG_NUMBER_LEN] = { LC_SIG_EV_INCOMING };
            lc_sig_put32(ev + 1, t->call_id);
            memcpy(ev + 5, m->u.setup_ind.caller, LC_SIG_NUMBER_LEN);
            emit(t, ev, sizeof(ev));
        } else {
            queue_release(t, m->u.setup_ind.call_id,
                          in_call(t->state) ? LC_SIG_CAUSE_BUSY : LC_SIG_CAUSE_UNREACHABLE);
        }
        return;
    case LC_SIG_CONNECT_ACK:
        if (t->state == LC_SIG_ST_RINGING_IN && t->answered && m->u.call.call_id == t->call_id) call_up(t, 1);
        return;
    case LC_SIG_RELEASE:
        queue_call(t, LC_SIG_RELEASE_COMPLETE, m->u.release.call_id);
        if (in_call(t->state) && m->u.release.call_id == t->call_id) call_end(t, m->u.release.cause);
        return;
    case LC_SIG_RELEASE_COMPLETE:
        if (t->state == LC_SIG_ST_RELEASING && m->u.call.call_id == t->call_id) call_end(t, t->end_cause);
        return;
    case LC_SIG_CHAN_LIST: {
        /* It opened, so it is MAC-protected by this registration's keys
         * (lc_sig_open drops a clear one). A repeat is answered by the channel
         * with the same ACK and never reaches here. Never while REGISTERING,
         * though (controller ruling B): applying it before REG_ACK has set
         * reg_mode (and the terminal's own state) risks a list from the vector
         * that is about to be superseded or rejected. Ignored outright - no
         * ACK, no hand-over; the network's own chan retries it, and once
         * registered a fresh push (after REG_ACK, or a cause-4 request) picks
         * it up normally. */
        if (t->state != LC_SIG_ST_REGISTERED && !in_call(t->state)) return;
        lc_sig_msg_t r;
        memset(&r, 0, sizeof(r));
        r.type = LC_SIG_CHAN_LIST_ACK;
        r.u.chan_list_ack.ver = m->u.chan_list.ver;
        queue(t, &r);
        t->list_in = m->u.chan_list;
        t->list_new = 1;
        t->list_ver = m->u.chan_list.ver;
        /* if this list is the answer to an outstanding cell_cfg ask, remember
         * it so cell_cfg stops repeating that exact ask (M1) - only while one
         * is actually outstanding: an automatic post-REG_ACK push must not be
         * mistaken for an answer to a cfg_ver we never asked about. */
        if (t->cfg_asked) t->cfg_answered = 1;
        return;
    }
    default:
        return;
    }
}

void lc_sig_term_rx(lc_sig_term_t *t, const uint8_t *p, uint8_t n, uint64_t now_us)
{
    lc_sig_msg_t m;
    if (lc_sig_chan_rx(&t->ch, p, n, &m, now_us) == 1) handle(t, &m, now_us);
    flush(t, now_us);
}

static void expired(lc_sig_term_t *t, uint8_t type, uint64_t now)
{
    switch (type) {
    case LC_SIG_ACT_REQ:
        act_failed(t, LC_SIG_ACT_TIMEOUT, now);
        break;
    case LC_SIG_REG_REQ:
    case LC_SIG_AUTH_RSP:
    case LC_SIG_AUTH_FAIL:
        if (t->state == LC_SIG_ST_REGISTERING && t->reg_sent) {
            reg_failed(t, LC_SIG_REG_TIMEOUT, now);
        }
        break;
    case LC_SIG_CALL_SETUP:
    case LC_SIG_CONNECT:
        if (in_call(t->state)) call_end(t, LC_SIG_CAUSE_NET_FAILURE);
        break;
    case LC_SIG_RELEASE:
        if (t->state == LC_SIG_ST_RELEASING) call_end(t, t->end_cause);
        break;
    default:
        break;
    }
}

void lc_sig_term_tick(lc_sig_term_t *t, uint64_t now_us)
{
    uint8_t exp;
    if (lc_sig_chan_tick(&t->ch, now_us, t->granted, &exp) == 1) expired(t, exp, now_us);
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    switch (t->state) {
    case LC_SIG_ST_ACTIVATING:
        if (!t->act_sent && t->attached) {
            m.type = LC_SIG_ACT_REQ;
            memcpy(m.u.act_req.token_id, t->qr.token_id, 8);
            memcpy(m.u.act_req.pkt, t->id->pk, 32);
            lc_sig_act_tag(t->qr.token_secret, t->tmid, t->id->pk, t->qr.token_id, m.u.act_req.tag);
            queue(t, &m);
            t->act_sent = 1;
            t->proc_deadline = now_us + US(30);
        } else if (t->act_sent && now_us >= t->proc_deadline) {
            /* No answer even with repeated service requests (the channel's own
             * retransmit clock froze while ungranted): give up. */
            lc_sig_chan_reset(&t->ch);
            t->out_count = 0;
            act_failed(t, LC_SIG_ACT_TIMEOUT, now_us);
        }
        break;
    case LC_SIG_ST_REGISTERING:
        if (t->id->activated && !t->reg_sent && t->attached && now_us >= t->reg_retry_at) {
            m.type = LC_SIG_REG_REQ;
            memcpy(m.u.reg_req.sw_version, k_sw, 3);
            m.u.reg_req.caps = 1;
            /* A new registration: the network may have restarted (its
             * session numbering from 0 again) or be another one, so its next
             * message is no plain repeat of the last one heard, whatever its
             * seq. (A new network's AUTH_REQ can't match the old cached one
             * either: a repeat must be the same bytes.) */
            lc_sig_chan_forget_rx(&t->ch);
            queue(t, &m);
            t->reg_sent = 1;
            t->auth_sent = 0;
            t->proc_deadline = now_us + US(30);
        } else if (t->reg_sent && now_us >= t->proc_deadline) {
            lc_sig_chan_reset(&t->ch);
            t->out_count = 0;
            reg_failed(t, LC_SIG_REG_TIMEOUT, now_us);
        }
        break;
    case LC_SIG_ST_REGISTERED:
        if (now_us >= t->rereg_at) reg_start(t, now_us);
        break;
    case LC_SIG_ST_CALLING:
    case LC_SIG_ST_RINGING_OUT:
    case LC_SIG_ST_RINGING_IN:
        if (now_us >= t->call_timer_at) {
            if (t->state == LC_SIG_ST_CALLING && t->call_id == 0) {
                call_end(t, LC_SIG_CAUSE_NET_FAILURE);
            } else {
                t->end_cause = LC_SIG_CAUSE_NO_ANSWER;
                queue_release(t, t->call_id, LC_SIG_CAUSE_NO_ANSWER);
                t->state = LC_SIG_ST_RELEASING;
                t->call_timer_at = now_us + US(5);
            }
        }
        break;
    case LC_SIG_ST_IN_CALL:
        if (!t->granted && t->grant_lost_at != 0 && now_us - t->grant_lost_at >= US(5)) {
            call_end(t, LC_SIG_CAUSE_LINK_LOST);
        }
        break;
    case LC_SIG_ST_RELEASING:
        if (now_us >= t->call_timer_at) call_end(t, t->end_cause);
        break;
    default:
        break;
    }
    flush(t, now_us);
}

static int voice_crypt(lc_sig_term_t *t, uint8_t dir, uint32_t fctr, uint8_t *d, size_t n)
{
    uint8_t nonce[14] = { 0 };
    nonce[0] = dir;
    lc_sig_put32(nonce + 1, t->call_id);
    lc_sig_put32(nonce + 5, fctr);
    return lc_sig_aes128_ctr(t->k_voice, nonce, d, n);
}

int lc_sig_term_data_out(lc_sig_term_t *t, const uint8_t *d, uint8_t n, uint8_t out[LC_SIG_LINK_MAX], uint8_t *out_n)
{
    if (n > LC_SIG_APP_MAX) return LC_SIG_ATT_BAD_LEN;
    out[0] = LC_SIG_KIND_DATA;
    out[1] = (uint8_t)t->d_tx;
    memcpy(out + 2, d, n);
    if (t->state == LC_SIG_ST_IN_CALL && t->ch.sec.encrypt == 1 && voice_crypt(t, 0, t->d_tx, out + 2, n) != 0) {
        memset(out + 2, 0, n); /* fail closed: never the plaintext, and the counter doesn't move */
        return LC_SIG_ATT_NOT_NOW;
    }
    t->d_tx++;
    *out_n = (uint8_t)(n + 2u);
    return 0;
}

int lc_sig_term_data_in(lc_sig_term_t *t, const uint8_t *p, uint8_t n, uint8_t out[LC_SIG_APP_MAX], uint8_t *out_n)
{
    if (n < 2 || p[0] != LC_SIG_KIND_DATA || n - 2u > LC_SIG_APP_MAX) return -1;
    uint8_t dn = (uint8_t)(n - 2u);
    memcpy(out, p + 2, dn);
    if (t->state == LC_SIG_ST_IN_CALL && t->ch.sec.encrypt == 1) {
        uint32_t cand = (t->d_rx_next & ~0xFFu) | p[1];
        if (cand < t->d_rx_next) cand += 256u;
        if (cand - t->d_rx_next >= 128u) return -1; /* a replay/duplicate: same window as lc_sig_open */
        if (voice_crypt(t, 1, cand, out, dn) != 0) return -1; /* can't decrypt: dropped */
        t->d_rx_next = cand + 1u;
    }
    *out_n = dn;
    return 0;
}
