#include "oc_sig_term.h"

#include <string.h>

#include "oc_sig_crypto.h"
#include "oc_sig_keys.h"
#include "oc_sig_milenage.h"

#define US(s) ((uint64_t)(s) * 1000000ull)

static const uint8_t k_amf[2] = { 0x80, 0x00 };
static const uint8_t k_amf_resync[2] = { 0x00, 0x00 };
static const uint8_t k_sw[3] = { 0, 6, 0 }; /* 0.6.0: numbering v2 */

/* ------------------------------------------------------------ identity */

size_t oc_sig_ident_pack(const oc_sig_ident_t *id, uint8_t out[OC_SIG_IDENT_BLOB])
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
    memcpy(out + 106, id->number, OC_SIG_NUMBER_LEN);
    return OC_SIG_IDENT_BLOB;
}

int oc_sig_ident_unpack(const uint8_t *in, size_t len, oc_sig_ident_t *id)
{
    if (len == OC_SIG_IDENT_BLOB_V1 && in[0] == 1) return OC_SIG_IDENT_OLD;
    if (len != OC_SIG_IDENT_BLOB || in[0] != 2) return -1;
    memset(id, 0, sizeof(*id));
    id->activated = in[1] != 0;
    id->key_id = (uint16_t)(in[2] | (in[3] << 8));
    memcpy(id->sk, in + 4, 32);
    memcpy(id->pk, in + 36, 32);
    memcpy(id->k, in + 68, 16);
    memcpy(id->opc, in + 84, 16);
    memcpy(id->sqn, in + 100, 6);
    memcpy(id->number, in + 106, OC_SIG_NUMBER_LEN);
    return 0;
}

int oc_sig_ident_new(oc_sig_ident_t *id, const uint8_t random32[32])
{
    memset(id, 0, sizeof(*id));
    memcpy(id->sk, random32, 32);
    return oc_sig_x25519_public(id->sk, id->pk);
}

/* ------------------------------------------------------------- helpers */

static int in_call(uint8_t s)
{
    return s >= OC_SIG_ST_CALLING && s <= OC_SIG_ST_RELEASING;
}

static void emit(oc_sig_term_t *t, const uint8_t *ev, uint8_t n)
{
    if (t->io.event != NULL) t->io.event(t->io.ctx, ev, n);
}

static void save(oc_sig_term_t *t)
{
    if (t->io.save != NULL) t->io.save(t->io.ctx, t->id);
}

static void queue(oc_sig_term_t *t, const oc_sig_msg_t *m)
{
    if (t->out_count < OC_SIG_OUTQ) t->outq[t->out_count++] = *m;
}

static void queue_call(oc_sig_term_t *t, uint8_t type, uint32_t call_id)
{
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.u.call.call_id = call_id;
    queue(t, &m);
}

static void queue_release(oc_sig_term_t *t, uint32_t call_id, uint8_t cause)
{
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_SIG_RELEASE;
    m.u.release.call_id = call_id;
    m.u.release.cause = cause;
    queue(t, &m);
}

static void reg_start(oc_sig_term_t *t, uint64_t at)
{
    t->state = OC_SIG_ST_REGISTERING;
    t->reg_sent = 0;
    t->auth_sent = 0; /* from here, only this attempt's AUTH_RSP lets REG_ACK in (fix round 3) */
    t->reg_retry_at = at;
    /* An UNANSWERED config ask is moot once a fresh registration starts (its
     * own REG_ACK brings a CHAN_LIST push anyway) - drop it so a stale
     * cfg_retry_at can't fire the moment we're REGISTERED again. An ALREADY
     * ANSWERED one is left alone: its M1 record is still valid as long as
     * list_ver doesn't move (review fix: M1 is scoped to list_ver, so this is
     * now safe even across a registration that changes nothing else). */
    if (t->cfg_asked && !t->cfg_answered) t->cfg_asked = 0;
}

static void reg_failed(oc_sig_term_t *t, uint8_t reason, uint64_t now)
{
    uint8_t ev[2] = { OC_SIG_EV_REG_FAILED, reason };
    emit(t, ev, 2);
    if (reason == OC_SIG_REG_TIMEOUT) {
        /* no answer (coverage lost): not a refusal, so no escalation */
        reg_start(t, now + US(30));
        return;
    }
    t->backoff_s = t->backoff_s == 0 ? 30u : (t->backoff_s >= 300u ? 600u : t->backoff_s * 2u);
    reg_start(t, now + US(t->backoff_s));
}

static void act_failed(oc_sig_term_t *t, uint8_t reason, uint64_t now)
{
    uint8_t ev[2] = { OC_SIG_EV_ACT_FAILED, reason };
    emit(t, ev, 2);
    if (t->id->activated) {
        reg_start(t, now);
    } else {
        t->state = OC_SIG_ST_NOT_ACTIVATED;
    }
}

static void call_end(oc_sig_term_t *t, uint8_t cause)
{
    uint8_t ev[6] = { OC_SIG_EV_ENDED };
    oc_sig_put32(ev + 1, t->call_id);
    ev[5] = cause;
    emit(t, ev, 6);
    t->state = OC_SIG_ST_REGISTERED;
    t->call_id = 0;
    t->answered = 0;
    t->hangup_pending = 0;
}

static void call_up(oc_sig_term_t *t, uint8_t codec)
{
    oc_sig_voice_key(t->ck, t->ik, t->rand, t->tmid, t->call_id, t->k_voice);
    t->d_tx = 0;
    t->d_rx_next = 0;
    t->state = OC_SIG_ST_IN_CALL;
    uint8_t ev[6] = { OC_SIG_EV_CONNECTED };
    oc_sig_put32(ev + 1, t->call_id);
    ev[5] = codec;
    emit(t, ev, 6);
}

/* Send what can go now; ask for a channel if a message is waiting. */
static void flush(oc_sig_term_t *t, uint64_t now)
{
    while (t->out_count > 0 && t->granted) {
        if (oc_sig_chan_send(&t->ch, &t->outq[0], now) != 0) break; /* a request in flight: later */
        t->out_count--;
        memmove(t->outq, t->outq + 1, t->out_count * sizeof(t->outq[0]));
    }
    /* Not granted: ask for a channel (at most every 2 s) whenever something
     * still needs one to go out or complete - queued locally, waiting on the
     * channel's own request/response, or already fragmented into its txq. A
     * message handed to the channel has left out_count, so out_count alone
     * missed this once the request was in flight and the grant went away. */
    if (!t->granted && t->attached && now >= t->svc_due && t->io.service_req != NULL &&
        (t->out_count > 0 || oc_sig_chan_busy(&t->ch) || t->ch.txq_count > 0)) {
        uint8_t cause = in_call(t->state) ? OC_SIG_SVC_CALL : OC_SIG_SVC_REGISTER;
        if (t->io.service_req(t->io.ctx, cause) == 0) t->svc_due = now + US(2);
    }
    const uint8_t *p;
    uint8_t n;
    while (t->granted && oc_sig_chan_peek(&t->ch, &p, &n) == 0 && t->io.send(t->io.ctx, p, n) == 0) {
        oc_sig_chan_pop(&t->ch);
    }
}

/* -------------------------------------------------------------- public */

void oc_sig_term_init(oc_sig_term_t *t, const oc_sig_term_io_t *io, oc_sig_ident_t *id, uint32_t tmid,
                      uint64_t now_us)
{
    memset(t, 0, sizeof(*t));
    t->io = *io;
    t->id = id;
    t->tmid = tmid;
    oc_sig_chan_init(&t->ch, 0);
    t->state = id->activated ? OC_SIG_ST_REGISTERING : OC_SIG_ST_NOT_ACTIVATED;
    t->reg_retry_at = now_us;
}

uint8_t oc_sig_term_state(const oc_sig_term_t *t)
{
    return t->state;
}

void oc_sig_term_link(oc_sig_term_t *t, int attached, int granted, uint64_t now_us)
{
    if (t->granted && !granted) t->grant_lost_at = now_us;
    if (granted) t->grant_lost_at = 0;
    if (!attached) t->svc_due = 0; /* ask again as soon as the link is back */
    /* Back on a cell after losing it: its network may have restarted (and lost
     * our session) or be another one, so register again (ruling in plan 5). */
    if (attached && !t->attached && t->state == OC_SIG_ST_REGISTERED) reg_start(t, now_us);
    /* Back in coverage while waiting to retry a registration: try now. */
    if (attached && !t->attached && t->state == OC_SIG_ST_REGISTERING && !t->reg_sent) t->reg_retry_at = now_us;
    t->attached = attached;
    t->granted = granted;
}

void oc_sig_term_cell_mode(oc_sig_term_t *t, uint8_t mode, uint64_t now_us)
{
    if (t->state == OC_SIG_ST_REGISTERED && t->reg_mode != 0 && mode != t->reg_mode) reg_start(t, now_us);
}

void oc_sig_term_cell_cfg(oc_sig_term_t *t, uint8_t cfg_ver, uint64_t now_us)
{
    /* Granted, a service request can't go out (RACH UPPER is IDLE only); the
     * network pushes CHAN_LIST after every REG_ACK anyway. */
    if (t->state != OC_SIG_ST_REGISTERED || !t->attached || t->granted || t->io.service_req == NULL) return;
    if (((t->list_ver ^ cfg_ver) & 3u) == 0) {
        /* Caught up: any ask still outstanding for what's now a stale
         * mismatch is moot - drop it (review fix) so a LATER mismatch asks
         * fresh instead of walking straight into the one-re-registration-
         * per-cfg_ver rule below for what would look like the same ask. The
         * one-re-registration record itself is moot too (review fix, second
         * spot alongside the CHAN_LIST handler): a LATER session loss at
         * this same 2-bit cfg_ver value deserves its own fresh
         * re-registration, not a leftover "already tried that". */
        if (t->cfg_asked && !t->cfg_answered) t->cfg_asked = 0;
        t->cfg_reregistered = 0;
        t->cfg_rereg_ver = 0;
        t->cfg_backoff_s = 0;
        return;
    }
    if (now_us < t->cfg_retry_at) return;
    /* Already asked for this exact cfg_ver and a CHAN_LIST answered it while
     * the list version we hold hasn't moved since (review fix: M1 scoped to
     * list_ver): a persistent mismatch (the beacon's cfg_ver stuck against a
     * list version that doesn't clear it) is not worth asking again every
     * 30 s forever. Only a DIFFERENT cfg_ver, or the held list itself moving
     * on from under that old answer (e.g. a restart handing us back an older
     * list), reopens the question (M1). */
    if (t->cfg_answered && t->cfg_asked && t->cfg_asked_ver == cfg_ver && t->cfg_answered_list_ver == t->list_ver) {
        return;
    }
    if (t->cfg_asked && !t->cfg_answered && !(t->cfg_reregistered && t->cfg_rereg_ver == cfg_ver)) {
        /* Still no CHAN_LIST for this ask, and we haven't yet tried
         * re-registering for this exact cfg_ver. Bench case: the cell was
         * restarted within ~1 s, so we never lost sync and still believe
         * we're REGISTERED, but the new network has no session for us - only
         * a REGISTERED session holds the keys CHAN_LIST needs, so our
         * cause-4 ask goes unanswered forever. Re-register once instead: a
         * REG_ACK is followed by the network's own CHAN_LIST push, so the
         * list still arrives, just through that path - but only once per
         * cfg_ver (review fix): if this exact cfg_ver is still unanswered
         * afterwards, the network genuinely has nothing for it (not a lost
         * session), and re-registering again on a loop would never stop. */
        t->cfg_rereg_ver = cfg_ver;
        t->cfg_reregistered = 1;
        t->cfg_backoff_s = 0;
        reg_start(t, now_us);
        t->cfg_retry_at = now_us + US(30); /* give the post-REG_ACK push time to arrive */
        return;
    }
    /* A fresh ask (never tried for this cfg_ver, or cfg_asked was just
     * cleared by the caught-up gate or reg_start above) goes out at the
     * usual flat 30 s; once this cfg_ver has already had its one
     * re-registration and is still unanswered, back off instead (review
     * fix): 30 s, 60, 120 ... capped at 600 s. Computed here but only
     * COMMITTED to cfg_backoff_s once the ask actually goes out (review fix,
     * second minor): io.service_req can refuse (e.g. RACH busy) while
     * cell_cfg itself is re-run every ~100 ms, and doubling the backoff on
     * every refusal would race it to 600 s before a single ask ever left. */
    int fallback = t->cfg_reregistered && t->cfg_rereg_ver == cfg_ver;
    uint32_t wait_s = 30u;
    if (fallback) wait_s = t->cfg_backoff_s == 0 ? 30u : (t->cfg_backoff_s >= 300u ? 600u : t->cfg_backoff_s * 2u);
    if (t->io.service_req(t->io.ctx, OC_SIG_SVC_CONFIG) == 0) {
        if (fallback) t->cfg_backoff_s = wait_s;
        t->cfg_retry_at = now_us + US(wait_s);
        t->cfg_asked_ver = cfg_ver;
        t->cfg_asked = 1;
        t->cfg_answered = 0; /* waiting on the answer to this ask now */
    }
}

int oc_sig_term_chan_list(oc_sig_term_t *t, oc_sig_chan_list_t *out)
{
    if (!t->list_new) return 0;
    *out = t->list_in;
    t->list_new = 0;
    return 1;
}

int oc_sig_term_act_prepare(const oc_sig_ident_t *id, uint32_t tmid, const uint8_t *text, size_t len,
                            oc_sig_act_prep_t *p)
{
    if (len == 0 || len > 120) return OC_SIG_ATT_BAD_LEN;
    if (oc_sig_qr_parse((const char *)text, len, &p->qr) != 0) return OC_SIG_ATT_BAD_ARG;
    /* A low-order network key gives an all-zero secret: refused here. */
    if (oc_sig_act_keys(id->sk, p->qr.pkn, tmid, p->qr.token_id, p->k, p->opc) != 0) return OC_SIG_ATT_BAD_ARG;
    return 0;
}

int oc_sig_term_activate(oc_sig_term_t *t, const oc_sig_act_prep_t *p, uint64_t now_us)
{
    if (t->state == OC_SIG_ST_ACTIVATING || in_call(t->state)) return OC_SIG_ATT_NOT_NOW;
    t->qr = p->qr;
    memcpy(t->act_k, p->k, 16);
    memcpy(t->act_opc, p->opc, 16);
    t->state = OC_SIG_ST_ACTIVATING;
    t->act_sent = 0;
    t->out_count = 0;
    oc_sig_chan_reset(&t->ch);
    flush(t, now_us);
    return 0;
}

int oc_sig_term_command(oc_sig_term_t *t, const uint8_t *cmd, size_t len, uint64_t now_us)
{
    if (len == 0) return OC_SIG_ATT_BAD_LEN;
    const uint8_t *a = cmd + 1;
    size_t al = len - 1;
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    switch (cmd[0]) {
    case OC_SIG_CMD_ACTIVATE: {
        if (t->state == OC_SIG_ST_ACTIVATING || in_call(t->state)) return OC_SIG_ATT_NOT_NOW;
        oc_sig_act_prep_t p;
        int err = oc_sig_term_act_prepare(t->id, t->tmid, a, al, &p);
        return err != 0 ? err : oc_sig_term_activate(t, &p, now_us);
    }
    case OC_SIG_CMD_DIAL:
        /* Any dialled form (numbering v2 §5.3), completed from the terminal's
         * own number; CALL_SETUP always carries the full form. */
        if (al == 0 || al > OC_SIG_DIAL_MAX) return OC_SIG_ATT_BAD_LEN;
        if (t->state != OC_SIG_ST_REGISTERED) return OC_SIG_ATT_NOT_NOW;
        m.type = OC_SIG_CALL_SETUP;
        if (oc_sig_number_normalize((const char *)a, al, t->id->number, m.u.call_setup.called) != 0) {
            return OC_SIG_ATT_BAD_ARG;
        }
        m.u.call_setup.ref = ++t->call_ref;
        m.u.call_setup.codec_caps = 1;
        t->call_id = 0;
        queue(t, &m);
        t->state = OC_SIG_ST_CALLING;
        t->call_timer_at = now_us + US(10);
        break;
    case OC_SIG_CMD_ANSWER:
        if (al != 0) return OC_SIG_ATT_BAD_LEN;
        if (t->state != OC_SIG_ST_RINGING_IN || t->answered) return OC_SIG_ATT_NOT_NOW;
        m.type = OC_SIG_CONNECT;
        m.u.connect.call_id = t->call_id;
        m.u.connect.codec = 1;
        queue(t, &m);
        t->answered = 1;
        t->call_timer_at = now_us + US(10);
        break;
    case OC_SIG_CMD_REJECT:
    case OC_SIG_CMD_HANGUP: {
        if (al != 0) return OC_SIG_ATT_BAD_LEN;
        int ringing_in = t->state == OC_SIG_ST_RINGING_IN;
        if (cmd[0] == OC_SIG_CMD_REJECT ? !ringing_in
                                        : !(ringing_in || t->state == OC_SIG_ST_CALLING ||
                                            t->state == OC_SIG_ST_RINGING_OUT || t->state == OC_SIG_ST_IN_CALL)) {
            return OC_SIG_ATT_NOT_NOW;
        }
        t->end_cause = ringing_in ? OC_SIG_CAUSE_REJECTED : OC_SIG_CAUSE_NORMAL;
        if (t->state == OC_SIG_ST_CALLING && t->call_id == 0) {
            if (t->out_count > 0 && t->outq[t->out_count - 1].type == OC_SIG_CALL_SETUP) {
                t->out_count--; /* never sent: just drop it */
                call_end(t, OC_SIG_CAUSE_NORMAL);
                break;
            }
            t->hangup_pending = 1; /* RELEASE once CALL_PROC gives the call id */
        } else {
            queue_release(t, t->call_id, t->end_cause);
        }
        t->state = OC_SIG_ST_RELEASING;
        t->call_timer_at = now_us + US(10);
        break;
    }
    case OC_SIG_CMD_DEACTIVATE:
        if (al != 1) return OC_SIG_ATT_BAD_LEN;
        if (a[0] != 0xA5) return OC_SIG_ATT_BAD_ARG;
        if (in_call(t->state)) return OC_SIG_ATT_NOT_NOW; /* as ACTIVATE: hang up first */
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
        t->list_ver = 0; /* the network's entries go too (oc_term_scan_deactivate) */
        t->list_new = 0;
        t->cfg_asked = 0;
        t->cfg_answered = 0;
        t->cfg_answered_list_ver = 0;
        t->cfg_rereg_ver = 0;
        t->cfg_reregistered = 0;
        t->cfg_backoff_s = 0;
        oc_sig_sec_init(&t->ch.sec, 0);
        oc_sig_chan_reset(&t->ch);
        t->out_count = 0;
        t->state = OC_SIG_ST_NOT_ACTIVATED;
        {
            uint8_t ev = OC_SIG_EV_DEACTIVATED;
            emit(t, &ev, 1);
        }
        break;
    default:
        return OC_SIG_ATT_BAD_ARG;
    }
    flush(t, now_us);
    return 0;
}

static void on_auth_req(oc_sig_term_t *t, const oc_sig_msg_t *m, uint64_t now)
{
    static const uint8_t zero[6] = { 0 };
    const uint8_t *rand = m->u.auth_req.rand, *autn = m->u.auth_req.autn;
    oc_sig_ident_t *id = t->id;
    oc_milenage_t o;
    oc_sig_msg_t r;
    uint8_t sqn[6];
    memset(&r, 0, sizeof(r));
    if (oc_milenage(id->k, id->opc, rand, zero, k_amf, &o) != 0) return; /* AK */
    for (int i = 0; i < 6; i++) sqn[i] = (uint8_t)(autn[i] ^ o.ak[i]);
    if (oc_milenage(id->k, id->opc, rand, sqn, autn + 6, &o) != 0) return;
    if (!oc_sig_ct_equal(o.mac_a, autn + 8, 8)) {
        r.type = OC_SIG_AUTH_FAIL;
        r.u.auth_fail.cause = 1;
        queue(t, &r);
        reg_failed(t, OC_SIG_REG_NET_AUTH, now); /* retries after the backoff */
        return;
    }
    uint64_t s = oc_sig_sqn_get(sqn), ms = oc_sig_sqn_get(id->sqn);
    if (!(s > ms && s - ms <= (1ull << 28))) {
        oc_milenage_t o2;
        if (oc_milenage(id->k, id->opc, rand, id->sqn, k_amf_resync, &o2) != 0) return;
        r.type = OC_SIG_AUTH_FAIL;
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
    oc_sig_session_keys(o.ck, o.ik, rand, t->tmid, ki, ke);
    oc_sig_sec_key(&t->ch.sec, ki, ke, -1); /* the mode comes in REG_ACK */
    r.type = OC_SIG_AUTH_RSP;
    memcpy(r.u.auth_rsp.res, o.res, 8);
    queue(t, &r);
    t->auth_sent = 1;
}

static void handle(oc_sig_term_t *t, const oc_sig_msg_t *m, uint64_t now)
{
    oc_sig_ident_t *id = t->id;
    switch (m->type) {
    case OC_SIG_ACT_ACK: {
        if (t->state != OC_SIG_ST_ACTIVATING) return;
        uint8_t conf[8];
        if (oc_sig_act_confirm(t->act_k, t->tmid, t->qr.token_id, conf) != 0 ||
            !oc_sig_ct_equal(conf, m->u.act_ack.confirm, 8)) {
            act_failed(t, OC_SIG_ACT_BAD_CONFIRM, now);
            return;
        }
        memcpy(id->k, t->act_k, 16);
        memcpy(id->opc, t->act_opc, 16);
        memset(id->sqn, 0, 6);
        memcpy(id->number, m->u.act_ack.number, OC_SIG_NUMBER_LEN);
        id->key_id = t->qr.key_id;
        id->activated = 1;
        save(t);
        oc_sig_sec_init(&t->ch.sec, 0);
        uint8_t ev[1 + OC_SIG_NUMBER_LEN] = { OC_SIG_EV_ACTIVATED };
        memcpy(ev + 1, id->number, OC_SIG_NUMBER_LEN);
        emit(t, ev, sizeof(ev));
        t->backoff_s = 0;
        reg_start(t, now);
        return;
    }
    case OC_SIG_ACT_NAK:
        /* Reported whether or not its tag verifies: a terminal holding a forged
         * QR can't check the network's tag, and a spoofed NAK can only fail one
         * attempt, as a jammer could (ruling in plan 5). */
        if (t->state == OC_SIG_ST_ACTIVATING) act_failed(t, m->u.act_nak.reason, now);
        return;
    case OC_SIG_AUTH_REQ:
        if (t->state == OC_SIG_ST_REGISTERING) on_auth_req(t, m, now);
        return;
    case OC_SIG_REG_ACK: {
        /* Only once this attempt's AUTH_RSP went out: until then the chan
         * still holds the previous session's keys, and a REG_ACK they open -
         * the network's cached reply, drawn out by a recorded AUTH_RSP played
         * back (oc_sig_chan_rx can't tell it from a retransmission) - would
         * complete the registration without an AKA. A lost REG_ACK's resend
         * (answering this attempt's retransmitted AUTH_RSP) still passes. */
        if (t->state != OC_SIG_ST_REGISTERING || !t->auth_sent) return;
        t->ch.sec.encrypt = m->u.reg_ack.mode == OC_SIG_MODE_PART15 ? 1 : 0;
        t->reg_mode = m->u.reg_ack.mode;
        t->rereg_at = now + US(m->u.reg_ack.period_s != 0 ? m->u.reg_ack.period_s : 1800u);
        t->state = OC_SIG_ST_REGISTERED;
        t->backoff_s = 0;
        uint8_t ev[2 + OC_SIG_NUMBER_LEN] = { OC_SIG_EV_REGISTERED };
        memcpy(ev + 1, m->u.reg_ack.number, OC_SIG_NUMBER_LEN);
        ev[1 + OC_SIG_NUMBER_LEN] = m->u.reg_ack.mode;
        emit(t, ev, sizeof(ev));
        return;
    }
    case OC_SIG_REG_REJ:
        /* only while a registration is in flight: a REJ answering our AUTH_FAIL
         * after a bad network MAC was already reported. REG_REJ travels
         * unauthenticated (prot 0), so a forged one must not be able to clear
         * the terminal's activation on its own word: every cause, including
         * "not activated", is just reported and backed off from. Only a
         * DEACTIVATE command from the phone drops the activation (the BLE
         * link is not paired or encrypted yet: see security-model.md). */
        if (t->state != OC_SIG_ST_REGISTERING || !t->reg_sent) return;
        reg_failed(t, m->u.reg_rej.cause, now);
        return;
    case OC_SIG_CALL_PROC:
        if (m->u.call_proc.ref != t->call_ref) return;
        if (t->state == OC_SIG_ST_CALLING) {
            t->call_id = m->u.call_proc.call_id;
            t->call_timer_at = now + US(60); /* setup done: now the far end has 60 s to answer */
        } else if (t->state == OC_SIG_ST_RELEASING && t->hangup_pending) {
            t->call_id = m->u.call_proc.call_id;
            t->hangup_pending = 0;
            queue_release(t, t->call_id, OC_SIG_CAUSE_NORMAL);
            t->call_timer_at = now + US(5);
        }
        return;
    case OC_SIG_ALERTING:
        if (t->state == OC_SIG_ST_CALLING && m->u.call.call_id == t->call_id) {
            t->state = OC_SIG_ST_RINGING_OUT;
            t->call_timer_at = now + US(60);
            uint8_t ev[5] = { OC_SIG_EV_RINGING };
            oc_sig_put32(ev + 1, t->call_id);
            emit(t, ev, 5);
        }
        return;
    case OC_SIG_CONNECT:
        if ((t->state == OC_SIG_ST_CALLING || t->state == OC_SIG_ST_RINGING_OUT) &&
            m->u.connect.call_id == t->call_id) {
            queue_call(t, OC_SIG_CONNECT_ACK, t->call_id);
            call_up(t, m->u.connect.codec);
        }
        return;
    case OC_SIG_SETUP_IND:
        if (t->state == OC_SIG_ST_REGISTERED) {
            t->call_id = m->u.setup_ind.call_id;
            t->answered = 0;
            t->state = OC_SIG_ST_RINGING_IN;
            t->call_timer_at = now + US(60);
            queue_call(t, OC_SIG_ALERTING, t->call_id);
            uint8_t ev[5 + OC_SIG_NUMBER_LEN] = { OC_SIG_EV_INCOMING };
            oc_sig_put32(ev + 1, t->call_id);
            memcpy(ev + 5, m->u.setup_ind.caller, OC_SIG_NUMBER_LEN);
            emit(t, ev, sizeof(ev));
        } else {
            queue_release(t, m->u.setup_ind.call_id,
                          in_call(t->state) ? OC_SIG_CAUSE_BUSY : OC_SIG_CAUSE_UNREACHABLE);
        }
        return;
    case OC_SIG_CONNECT_ACK:
        if (t->state == OC_SIG_ST_RINGING_IN && t->answered && m->u.call.call_id == t->call_id) call_up(t, 1);
        return;
    case OC_SIG_RELEASE:
        queue_call(t, OC_SIG_RELEASE_COMPLETE, m->u.release.call_id);
        if (in_call(t->state) && m->u.release.call_id == t->call_id) call_end(t, m->u.release.cause);
        return;
    case OC_SIG_RELEASE_COMPLETE:
        if (t->state == OC_SIG_ST_RELEASING && m->u.call.call_id == t->call_id) call_end(t, t->end_cause);
        return;
    case OC_SIG_CHAN_LIST: {
        /* It opened, so it is MAC-protected by this registration's keys
         * (oc_sig_open drops a clear one). A repeat is answered by the channel
         * with the same ACK and never reaches here. Never while REGISTERING,
         * though (controller ruling B): applying it before REG_ACK has set
         * reg_mode (and the terminal's own state) risks a list from the vector
         * that is about to be superseded or rejected. Ignored outright - no
         * ACK, no hand-over; the network's own chan retries it, and once
         * registered a fresh push (after REG_ACK, or a cause-4 request) picks
         * it up normally. */
        if (t->state != OC_SIG_ST_REGISTERED && !in_call(t->state)) return;
        oc_sig_msg_t r;
        memset(&r, 0, sizeof(r));
        r.type = OC_SIG_CHAN_LIST_ACK;
        r.u.chan_list_ack.ver = m->u.chan_list.ver;
        queue(t, &r);
        t->list_in = m->u.chan_list;
        t->list_new = 1;
        t->list_ver = m->u.chan_list.ver;
        /* Any list taken resolves whatever mismatch a past re-registration
         * (and its fallback backoff) was tracking - forget that record
         * (review fix) so a LATER, unrelated session loss that happens to
         * show the same 2-bit cfg_ver value again gets its own fresh
         * re-registration, instead of being mistaken for the same
         * already-tried question and left asking uselessly (a lost session
         * can't be fixed by a plain ask) until the next periodic
         * re-registration, up to 30 min away. */
        t->cfg_reregistered = 0;
        t->cfg_rereg_ver = 0;
        t->cfg_backoff_s = 0;
        /* if this list is the answer to an outstanding, still-unanswered
         * cell_cfg ask, remember it (and the list_ver it arrived at) so
         * cell_cfg stops repeating that exact ask (M1) - only while one is
         * genuinely outstanding: an unsolicited push (no ask outstanding, or
         * one already answered before) must not be mistaken for a fresh
         * answer, or mark a stale ask answered again at a list_ver it never
         * actually settled (review fix). */
        if (t->cfg_asked && !t->cfg_answered) {
            t->cfg_answered = 1;
            t->cfg_answered_list_ver = t->list_ver;
        }
        return;
    }
    default:
        return;
    }
}

void oc_sig_term_rx(oc_sig_term_t *t, const uint8_t *p, uint8_t n, uint64_t now_us)
{
    oc_sig_msg_t m;
    if (oc_sig_chan_rx(&t->ch, p, n, &m, now_us) == 1) handle(t, &m, now_us);
    flush(t, now_us);
}

static void expired(oc_sig_term_t *t, uint8_t type, uint64_t now)
{
    switch (type) {
    case OC_SIG_ACT_REQ:
        act_failed(t, OC_SIG_ACT_TIMEOUT, now);
        break;
    case OC_SIG_REG_REQ:
    case OC_SIG_AUTH_RSP:
    case OC_SIG_AUTH_FAIL:
        if (t->state == OC_SIG_ST_REGISTERING && t->reg_sent) {
            reg_failed(t, OC_SIG_REG_TIMEOUT, now);
        }
        break;
    case OC_SIG_CALL_SETUP:
    case OC_SIG_CONNECT:
        if (in_call(t->state)) call_end(t, OC_SIG_CAUSE_NET_FAILURE);
        break;
    case OC_SIG_RELEASE:
        if (t->state == OC_SIG_ST_RELEASING) call_end(t, t->end_cause);
        break;
    default:
        break;
    }
}

void oc_sig_term_tick(oc_sig_term_t *t, uint64_t now_us)
{
    uint8_t exp;
    if (oc_sig_chan_tick(&t->ch, now_us, t->granted, &exp) == 1) expired(t, exp, now_us);
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    switch (t->state) {
    case OC_SIG_ST_ACTIVATING:
        if (!t->act_sent && t->attached) {
            m.type = OC_SIG_ACT_REQ;
            memcpy(m.u.act_req.token_id, t->qr.token_id, 8);
            memcpy(m.u.act_req.pkt, t->id->pk, 32);
            oc_sig_act_tag(t->qr.token_secret, t->tmid, t->id->pk, t->qr.token_id, m.u.act_req.tag);
            queue(t, &m);
            t->act_sent = 1;
            t->proc_deadline = now_us + US(30);
        } else if (t->act_sent && now_us >= t->proc_deadline) {
            /* No answer even with repeated service requests (the channel's own
             * retransmit clock froze while ungranted): give up. */
            oc_sig_chan_reset(&t->ch);
            t->out_count = 0;
            act_failed(t, OC_SIG_ACT_TIMEOUT, now_us);
        }
        break;
    case OC_SIG_ST_REGISTERING:
        if (t->id->activated && !t->reg_sent && t->attached && now_us >= t->reg_retry_at) {
            m.type = OC_SIG_REG_REQ;
            memcpy(m.u.reg_req.sw_version, k_sw, 3);
            m.u.reg_req.caps = 1;
            /* A new registration: the network may have restarted (its
             * session numbering from 0 again) or be another one, so its next
             * message is no plain repeat of the last one heard, whatever its
             * seq. (A new network's AUTH_REQ can't match the old cached one
             * either: a repeat must be the same bytes.) */
            oc_sig_chan_forget_rx(&t->ch);
            queue(t, &m);
            t->reg_sent = 1;
            t->auth_sent = 0;
            t->proc_deadline = now_us + US(30);
        } else if (t->reg_sent && now_us >= t->proc_deadline) {
            oc_sig_chan_reset(&t->ch);
            t->out_count = 0;
            reg_failed(t, OC_SIG_REG_TIMEOUT, now_us);
        }
        break;
    case OC_SIG_ST_REGISTERED:
        if (now_us >= t->rereg_at) reg_start(t, now_us);
        break;
    case OC_SIG_ST_CALLING:
    case OC_SIG_ST_RINGING_OUT:
    case OC_SIG_ST_RINGING_IN:
        if (now_us >= t->call_timer_at) {
            if (t->state == OC_SIG_ST_CALLING && t->call_id == 0) {
                call_end(t, OC_SIG_CAUSE_NET_FAILURE);
            } else {
                t->end_cause = OC_SIG_CAUSE_NO_ANSWER;
                queue_release(t, t->call_id, OC_SIG_CAUSE_NO_ANSWER);
                t->state = OC_SIG_ST_RELEASING;
                t->call_timer_at = now_us + US(5);
            }
        }
        break;
    case OC_SIG_ST_IN_CALL:
        if (!t->granted && t->grant_lost_at != 0 && now_us - t->grant_lost_at >= US(5)) {
            call_end(t, OC_SIG_CAUSE_LINK_LOST);
        }
        break;
    case OC_SIG_ST_RELEASING:
        if (now_us >= t->call_timer_at) call_end(t, t->end_cause);
        break;
    default:
        break;
    }
    flush(t, now_us);
}

static int voice_crypt(oc_sig_term_t *t, uint8_t dir, uint32_t fctr, uint8_t *d, size_t n)
{
    uint8_t nonce[14] = { 0 };
    nonce[0] = dir;
    oc_sig_put32(nonce + 1, t->call_id);
    oc_sig_put32(nonce + 5, fctr);
    return oc_sig_aes128_ctr(t->k_voice, nonce, d, n);
}

int oc_sig_term_data_out(oc_sig_term_t *t, const uint8_t *d, uint8_t n, uint8_t out[OC_SIG_LINK_MAX], uint8_t *out_n)
{
    if (n > OC_SIG_APP_MAX) return OC_SIG_ATT_BAD_LEN;
    out[0] = OC_SIG_KIND_DATA;
    out[1] = (uint8_t)t->d_tx;
    memcpy(out + 2, d, n);
    if (t->state == OC_SIG_ST_IN_CALL && t->ch.sec.encrypt == 1 && voice_crypt(t, 0, t->d_tx, out + 2, n) != 0) {
        memset(out + 2, 0, n); /* fail closed: never the plaintext, and the counter doesn't move */
        return OC_SIG_ATT_NOT_NOW;
    }
    t->d_tx++;
    *out_n = (uint8_t)(n + 2u);
    return 0;
}

int oc_sig_term_data_in(oc_sig_term_t *t, const uint8_t *p, uint8_t n, uint8_t out[OC_SIG_APP_MAX], uint8_t *out_n)
{
    if (n < 2 || p[0] != OC_SIG_KIND_DATA || n - 2u > OC_SIG_APP_MAX) return -1;
    uint8_t dn = (uint8_t)(n - 2u);
    memcpy(out, p + 2, dn);
    if (t->state == OC_SIG_ST_IN_CALL && t->ch.sec.encrypt == 1) {
        uint32_t cand = (t->d_rx_next & ~0xFFu) | p[1];
        if (cand < t->d_rx_next) cand += 256u;
        if (cand - t->d_rx_next >= 128u) return -1; /* a replay/duplicate: same window as oc_sig_open */
        if (voice_crypt(t, 1, cand, out, dn) != 0) return -1; /* can't decrypt: dropped */
        t->d_rx_next = cand + 1u;
    }
    *out_n = dn;
    return 0;
}
