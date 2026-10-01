#include "oc_sig_net.h"

#include <stdio.h>
#include <string.h>

#include "oc_sig_crypto.h"
#include "oc_sig_keys.h"

#define US(s) ((uint64_t)(s) * 1000000ull)

enum { C_NONE = 0, C_MO_PROC, C_MO_ALERT, C_MO_CONNECTING, C_MT_SETUP, C_MT_ALERT, C_ACTIVE, C_RELEASING };

static void logs(oc_sig_net_t *n, const char *s)
{
    if (n->io.log != NULL) n->io.log(n->io.ctx, s);
}

static oc_sig_net_sess_t *sess(oc_sig_net_t *n, uint32_t tmid, int create)
{
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        if (n->s[i].used && n->s[i].tmid == tmid) return &n->s[i];
    }
    if (!create) return NULL;
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        if (!n->s[i].used) {
            oc_sig_net_sess_t *s = &n->s[i];
            memset(s, 0, sizeof(*s));
            s->used = 1;
            s->tmid = tmid;
            oc_sig_chan_init(&s->ch, 1);
            return s;
        }
    }
    /* the table is full: reclaim the least recently active slot that isn't
     * registered and isn't mid-call, so terminals merely heard once (or
     * forged traffic for a bogus TMID) can't permanently starve the table
     * (fix round 1, Review Focus 3). If every slot is live, refuse as before. */
    oc_sig_net_sess_t *victim = NULL;
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        oc_sig_net_sess_t *cand = &n->s[i];
        if (cand->registered || cand->call != C_NONE) continue;
        if (victim == NULL || cand->last_sig < victim->last_sig) victim = cand;
    }
    if (victim == NULL) return NULL;
    memset(victim, 0, sizeof(*victim));
    victim->used = 1;
    victim->tmid = tmid;
    oc_sig_chan_init(&victim->ch, 1);
    return victim;
}

static oc_sig_net_sess_t *by_call(oc_sig_net_t *n, uint32_t call_id)
{
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        if (n->s[i].used && n->s[i].call != C_NONE && n->s[i].call_id == call_id) return &n->s[i];
    }
    return NULL;
}

static void queue(oc_sig_net_sess_t *s, const oc_sig_msg_t *m)
{
    if (s->out_count < OC_SIG_OUTQ) s->outq[s->out_count++] = *m;
}

static int outq_has(const oc_sig_net_sess_t *s, uint8_t type)
{
    for (uint8_t i = 0; i < s->out_count; i++) {
        if (s->outq[i].type == type) return 1;
    }
    return 0;
}

static void queue_call(oc_sig_net_sess_t *s, uint8_t type, uint32_t call_id)
{
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.u.call.call_id = call_id;
    queue(s, &m);
}

static void queue_release(oc_sig_net_sess_t *s, uint32_t call_id, uint8_t cause)
{
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_SIG_RELEASE;
    m.u.release.call_id = call_id;
    m.u.release.cause = cause;
    queue(s, &m);
}

static void rej(oc_sig_net_sess_t *s, uint8_t cause)
{
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_SIG_REG_REJ;
    m.u.reg_rej.cause = cause;
    queue(s, &m);
}

static void flush(oc_sig_net_t *n, oc_sig_net_sess_t *s, uint64_t now);
static void channel(oc_sig_net_t *n, oc_sig_net_sess_t *s, uint64_t now);

/* CHAN_LIST to s, unless one is already waiting (its body is kept current by
 * oc_sig_net_set_chan_list) or the current version is in flight. A stale one
 * in flight is followed by the current list: it may land after the terminal
 * asked for the new cfg_ver and answer that ask (M1: not asked again), so
 * the new list would otherwise never come (fix round 2). */
static void queue_chan_list(oc_sig_net_t *n, oc_sig_net_sess_t *s)
{
    uint8_t ver = n->have_list ? n->list.ver : 0;
    if (outq_has(s, OC_SIG_CHAN_LIST)) return;
    if (s->ch.pend && s->ch.pend_type == OC_SIG_CHAN_LIST && s->cl_ver == ver) return;
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_SIG_CHAN_LIST;
    if (n->have_list) m.u.chan_list = n->list;
    queue(s, &m);
}

/* The terminal is registering (a REG_REQ came): it ignores CHAN_LIST until
 * REG_ACK, which pushes it again, so a queued or in-flight one only holds
 * the session's single request slot - AUTH_REQ waited behind it and the
 * terminal's REG_REQ retransmissions were dropped as repeats (fix round 2). */
static void drop_chan_list(oc_sig_net_sess_t *s)
{
    uint8_t j = 0;
    for (uint8_t i = 0; i < s->out_count; i++) {
        if (s->outq[i].type != OC_SIG_CHAN_LIST) s->outq[j++] = s->outq[i];
    }
    s->out_count = j;
    oc_sig_chan_cancel(&s->ch, OC_SIG_CHAN_LIST);
}

static void call_ev(oc_sig_net_t *n, oc_sig_net_sess_t *s, uint8_t what, uint8_t cause)
{
    if (n->io.call == NULL) return;
    oc_sig_net_call_ev_t e = { what, s->tmid, s->call_id, { 0 }, cause, s->other };
    memcpy(e.number, s->peer, OC_SIG_NUMBER_LEN);
    n->io.call(n->io.ctx, &e);
}

/* The other leg of a local call, if it still points back at s. */
static oc_sig_net_sess_t *other_leg(oc_sig_net_t *n, const oc_sig_net_sess_t *s)
{
    if (s->other == 0) return NULL;
    oc_sig_net_sess_t *o = sess(n, s->other, 0);
    return o != NULL && o->other == s->tmid && o->call != C_NONE ? o : NULL;
}

/* The network ends one leg: RELEASE, then RELEASE_COMPLETE (or 5 s). */
static void release_leg(oc_sig_net_t *n, oc_sig_net_sess_t *s, uint8_t cause, uint64_t now)
{
    queue_release(s, s->call_id, cause);
    s->end_cause = cause;
    s->call = C_RELEASING;
    s->call_at = now + US(5);
    flush(n, s, now);
}

static void call_end(oc_sig_net_t *n, oc_sig_net_sess_t *s, uint8_t cause, uint64_t now)
{
    call_ev(n, s, OC_SIG_NET_ENDED, cause);
    oc_sig_net_sess_t *o = other_leg(n, s);
    s->other = 0;
    s->call = C_NONE;
    s->call_id = 0;
    if (o != NULL) { /* a local call: end the other leg with the same cause */
        o->other = 0;
        if (o->call != C_RELEASING) release_leg(n, o, cause, now);
    }
}

static void call_up(oc_sig_net_sess_t *s, uint64_t now)
{
    oc_sig_voice_key(s->ck, s->ik, s->rand, s->tmid, s->call_id, s->k_voice);
    s->d_tx = 0;
    s->d_rx_next = 0;
    s->call = C_ACTIVE;
    s->heard = now;
}

/* Ask the core for a vector (network-core spec §7.2). Its answer goes into
 * the pending fields (oc_sig_net_av_done): the session's confirmed
 * rand/ck/ik (and "registered") stay untouched until AUTH_RSP actually
 * matches, so an unauthenticated REG_REQ (forged or repeated) can never
 * deregister a session or overwrite live session keys on its own say-so
 * (fix round 1, Review Focus 1). One question at a time: while it stands,
 * another REG_REQ waits for the same answer. */
static void ask_av(oc_sig_net_t *n, oc_sig_net_sess_t *s, uint64_t now)
{
    if (s->av_wait && now - s->av_at < OC_SIG_NET_ASK_US) return;
    s->av_wait = 1;
    s->av_at = now;
    n->io.av_req(n->io.ctx, s->tmid); /* may answer from inside the call */
}

/* The core runs the checks and holds the keys (network-core spec §7.1); the
 * terminal's retransmits of this same ACT_REQ never reach here (the channel
 * drops a repeat until the answer exists, then repeats the answer). */
static void on_act_req(oc_sig_net_t *n, oc_sig_net_sess_t *s, const oc_sig_msg_t *m, uint64_t now)
{
    if (s->act_wait && now - s->act_at < OC_SIG_NET_ASK_US) return;
    s->act_wait = 1;
    s->act_at = now;
    n->io.act_req(n->io.ctx, s->tmid, m->u.act_req.token_id, m->u.act_req.pkt, m->u.act_req.tag);
}

/* The registered session for number, if any. */
static oc_sig_net_sess_t *by_number(oc_sig_net_t *n, const uint8_t number[OC_SIG_NUMBER_LEN])
{
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        oc_sig_net_sess_t *s = &n->s[i];
        if (s->used && s->registered && memcmp(s->number, number, OC_SIG_NUMBER_LEN) == 0) return s;
    }
    return NULL;
}

/* A call to a local subscriber (spec §5): the network rings the callee's
 * terminal and relays between the two legs. Each leg keeps its own call id
 * and voice key; app data is decrypted and re-encrypted in the network. */
static void local_setup(oc_sig_net_t *n, oc_sig_net_sess_t *a, oc_sig_net_sess_t *b, uint64_t now)
{
    if (b == a || b->call != C_NONE) {
        release_leg(n, a, OC_SIG_CAUSE_BUSY, now);
        return;
    }
    b->call_id = ++n->next_call_id;
    memcpy(b->peer, a->number, OC_SIG_NUMBER_LEN);
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_SIG_SETUP_IND;
    m.u.setup_ind.call_id = b->call_id;
    memcpy(m.u.setup_ind.caller, a->number, OC_SIG_NUMBER_LEN);
    m.u.setup_ind.codec_caps = 1;
    queue(b, &m);
    b->call = C_MT_SETUP;
    b->call_at = now + US(60);
    a->other = b->tmid;
    b->other = a->tmid;
    call_ev(n, a, OC_SIG_NET_LOCAL, 0);
    flush(n, b, now);
    channel(n, b, now);
}

static void handle(oc_sig_net_t *n, oc_sig_net_sess_t *s, const oc_sig_msg_t *m, uint64_t now)
{
    switch (m->type) {
    case OC_SIG_ACT_REQ:
        on_act_req(n, s, m, now);
        return;
    case OC_SIG_REG_REQ:
        drop_chan_list(s);
        if (s->auth_pending) {
            if (outq_has(s, OC_SIG_AUTH_REQ)) {
                /* the pending AUTH_REQ hasn't even reached the channel yet
                 * (something else - e.g. a call's own SETUP_IND/CONNECT -
                 * has the one request-in-flight slot): nothing of ours is
                 * actually in flight to accelerate (fix round 2, Review
                 * Focus 1a: a forged REG_REQ must not burn another
                 * message's retries) */
                return;
            }
            if (s->ch.pend && s->ch.pend_type == OC_SIG_AUTH_REQ && s->ch.pend_tries < OC_SIG_RETX_MAX) {
                /* genuinely our own AUTH_REQ, in flight, with retries left:
                 * prompt an immediate resend of the very same sealed
                 * message (same RAND/AUTN) - no SQN advance, no save, per a
                 * repeated or forged REG_REQ */
                s->ch.pend_due = now;
                return;
            }
            /* the vector is dead: its retries are exhausted, or nothing of
             * ours is pending in the channel any more (e.g. an AUTH_FAIL
             * already answered it for a subscriber that has since gone
             * unbound - fix round 2, Review Focus 1b/1c). Give up on it and
             * draw a fresh one exactly as for a first REG_REQ. */
            s->auth_pending = 0;
        }
        ask_av(n, s, now);
        return;
    case OC_SIG_AUTH_RSP: {
        if (!s->auth_pending) return;
        s->auth_pending = 0;
        /* the cell holds HXRES, not XRES (network-core spec §19.1): the
         * RES must hash to it; the core checks RES itself on LOC_UPDATE */
        uint8_t h[16];
        int ok = oc_sig_hxres(s->p_rand, m->u.auth_rsp.res, h) == 0 && oc_sig_ct_equal(h, s->p_hxres, 16);
        oc_sig_wipe(h, sizeof(h));
        if (!ok) { /* the vector is spent: its keys and HXRES go */
            oc_sig_wipe(s->p_hxres, sizeof(s->p_hxres));
            oc_sig_wipe(s->p_ck, sizeof(s->p_ck));
            oc_sig_wipe(s->p_ik, sizeof(s->p_ik));
            rej(s, OC_SIG_REG_AUTH_FAILED);
            return;
        }
        /* the vector is confirmed: only now does it replace the session's
         * live keys (fix round 1, Review Focus 1) */
        memcpy(s->rand, s->p_rand, 16);
        memcpy(s->ck, s->p_ck, 16);
        memcpy(s->ik, s->p_ik, 16);
        memcpy(s->number, s->p_number, OC_SIG_NUMBER_LEN);
        /* the pending copies are spent: the confirmed fields hold the keys */
        oc_sig_wipe(s->p_ck, sizeof(s->p_ck));
        oc_sig_wipe(s->p_ik, sizeof(s->p_ik));
        oc_sig_wipe(s->p_hxres, sizeof(s->p_hxres));
        uint8_t ki[16], ke[16];
        oc_sig_session_keys(s->ck, s->ik, s->rand, s->tmid, ki, ke);
        oc_sig_sec_key(&s->ch.sec, ki, ke, n->cfg.mode == OC_SIG_MODE_PART15 ? 1 : 0);
        oc_sig_wipe(ki, sizeof(ki));
        oc_sig_wipe(ke, sizeof(ke));
        s->registered = 1;
        s->reg_until = now + US(2u * n->cfg.period_s);
        /* a terminal only registers outside a call: a leg still up here is
         * left over from before a reboot (heard stays fresh on its empty UL
         * frames, so nothing else would ever end it) */
        if (s->call != C_NONE) call_end(n, s, OC_SIG_CAUSE_LINK_LOST, now);
        oc_sig_msg_t r;
        memset(&r, 0, sizeof(r));
        r.type = OC_SIG_REG_ACK;
        r.u.reg_ack.mode = n->cfg.mode;
        r.u.reg_ack.period_s = n->cfg.period_s;
        memcpy(r.u.reg_ack.number, s->number, OC_SIG_NUMBER_LEN);
        queue(s, &r);
        if (n->have_list) { /* spec §7: after every REG_ACK */
            s->cl_again = 0;
            queue_chan_list(n, s);
        }
        char line[64], num[OC_SIG_NUMBER_TEXT];
        oc_sig_number_to_text(s->number, num);
        snprintf(line, sizeof(line), "registered %s (terminal %08x)", num, (unsigned)s->tmid);
        logs(n, line);
        if (n->io.registered != NULL) n->io.registered(n->io.ctx, s->tmid, s->number, s->rand, m->u.auth_rsp.res);
        return;
    }
    case OC_SIG_AUTH_FAIL: {
        int had_vector = s->auth_pending;
        /* cleared unconditionally, before any early return: the channel's
         * own pending AUTH_REQ already cleared as this reply arrived (chan_rx
         * matches AUTH_FAIL as AUTH_REQ's reply before handle() ever runs),
         * so this flag must never outlive it - even when the subscriber has
         * since gone unbound (fix round 2, Review Focus 1c: otherwise every
         * later REG_REQ only pokes an idle channel and registration wedges) */
        s->auth_pending = 0;
        if (!had_vector) return;
        if (m->u.auth_fail.cause == 2) { /* the core checks AUTS and answers with a fresh vector */
            s->av_wait = 1;
            s->av_at = now;
            n->io.resync_req(n->io.ctx, s->tmid, s->p_rand, m->u.auth_fail.auts);
            return;
        }
        rej(s, OC_SIG_REG_AUTH_FAILED);
        return;
    }
    case OC_SIG_CALL_SETUP: {
        oc_sig_msg_t r;
        memset(&r, 0, sizeof(r));
        if (!s->registered) {
            /* not registered, or (fix round 1, Review Focus 2) this TMID's
             * subscriber moved to another terminal since (oc_sig_net_drop) */
            queue_release(s, 0, OC_SIG_CAUSE_UNREACHABLE);
            return;
        }
        if (s->call != C_NONE) {
            /* call id 0, as above: the caller has no call id yet, and must
             * see this RELEASE as its own (ENDED busy at once) */
            queue_release(s, 0, OC_SIG_CAUSE_BUSY);
            return;
        }
        s->call_id = ++n->next_call_id;
        memcpy(s->peer, m->u.call_setup.called, OC_SIG_NUMBER_LEN);
        s->call = C_MO_PROC;
        r.type = OC_SIG_CALL_PROC;
        r.u.call_proc.ref = m->u.call_setup.ref;
        r.u.call_proc.call_id = s->call_id;
        queue(s, &r);
        oc_sig_net_sess_t *callee = by_number(n, m->u.call_setup.called);
        if (callee != NULL) {
            local_setup(n, s, callee, now);
        } else {
            call_ev(n, s, OC_SIG_NET_MO, 0);
        }
        return;
    }
    case OC_SIG_ALERTING:
        if (s->call == C_MT_SETUP && m->u.call.call_id == s->call_id) {
            s->call = C_MT_ALERT;
            call_ev(n, s, OC_SIG_NET_ALERTING, 0);
            oc_sig_net_sess_t *o = other_leg(n, s);
            if (o != NULL && o->call == C_MO_PROC) { /* local call: the caller hears it ring */
                queue_call(o, OC_SIG_ALERTING, o->call_id);
                o->call = C_MO_ALERT;
                flush(n, o, now);
            }
        }
        return;
    case OC_SIG_CONNECT:
        if ((s->call == C_MT_SETUP || s->call == C_MT_ALERT) && m->u.connect.call_id == s->call_id) {
            queue_call(s, OC_SIG_CONNECT_ACK, s->call_id);
            call_up(s, now);
            call_ev(n, s, OC_SIG_NET_ANSWERED, 0);
            oc_sig_net_sess_t *o = other_leg(n, s);
            if (o != NULL && (o->call == C_MO_PROC || o->call == C_MO_ALERT)) { /* local call: connect the caller */
                oc_sig_msg_t c;
                memset(&c, 0, sizeof(c));
                c.type = OC_SIG_CONNECT;
                c.u.connect.call_id = o->call_id;
                c.u.connect.codec = 1;
                queue(o, &c);
                o->call = C_MO_CONNECTING;
                flush(n, o, now);
            }
        }
        return;
    case OC_SIG_CONNECT_ACK:
        if (s->call == C_MO_CONNECTING && m->u.call.call_id == s->call_id) call_up(s, now);
        return;
    case OC_SIG_RELEASE:
        queue_call(s, OC_SIG_RELEASE_COMPLETE, m->u.release.call_id);
        if (s->call != C_NONE && m->u.release.call_id == s->call_id) call_end(n, s, m->u.release.cause, now);
        return;
    case OC_SIG_RELEASE_COMPLETE:
        if (s->call == C_RELEASING && m->u.call.call_id == s->call_id) call_end(n, s, s->end_cause, now);
        return;
    case OC_SIG_CHAN_LIST_ACK: {
        s->cl_again = 0;
        char line[64];
        snprintf(line, sizeof(line), "channel list v%u taken by terminal %08x", m->u.chan_list_ack.ver,
                 (unsigned)s->tmid);
        logs(n, line);
        return;
    }
    default:
        return;
    }
}

static void flush(oc_sig_net_t *n, oc_sig_net_sess_t *s, uint64_t now)
{
    while (s->out_count > 0 && oc_sig_chan_send(&s->ch, &s->outq[0], now) == 0) {
        if (s->outq[0].type == OC_SIG_CHAN_LIST) s->cl_ver = s->outq[0].u.chan_list.ver;
        s->out_count--;
        memmove(s->outq, s->outq + 1, s->out_count * sizeof(s->outq[0]));
        s->last_sig = now;
    }
    const uint8_t *p;
    uint8_t len;
    while (oc_sig_chan_peek(&s->ch, &p, &len) == 0 && n->io.send(n->io.ctx, s->tmid, p, len) == 0) {
        oc_sig_chan_pop(&s->ch);
    }
}

/* Keep a channel while there is something to say or a call; release it after 5 s of quiet. */
static void channel(oc_sig_net_t *n, oc_sig_net_sess_t *s, uint64_t now)
{
    int wants = s->out_count > 0 || s->ch.txq_count > 0 || oc_sig_chan_busy(&s->ch) || s->call != C_NONE;
    if (n->io.channel == NULL) return;
    if (wants && !s->granted && (s->chan_req_at == 0 || now - s->chan_req_at >= US(3))) {
        n->io.channel(n->io.ctx, s->tmid, 1);
        s->chan_req_at = now;
    } else if (!wants && s->granted && now - s->last_sig >= US(5)) {
        n->io.channel(n->io.ctx, s->tmid, 0);
        s->granted = 0;
        s->chan_req_at = 0;
    }
}

void oc_sig_net_init(oc_sig_net_t *n, const oc_sig_net_io_t *io, const oc_sig_net_cfg_t *cfg)
{
    memset(n, 0, sizeof(*n));
    n->io = *io;
    n->cfg = *cfg;
    if (n->cfg.period_s == 0) n->cfg.period_s = 1800;
}

void oc_sig_net_rx(oc_sig_net_t *n, uint32_t tmid, const uint8_t *p, uint8_t len, uint64_t now_us)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 1);
    oc_sig_msg_t m;
    if (s == NULL) return;
    s->last_sig = now_us;
    if (oc_sig_chan_rx(&s->ch, p, len, &m, now_us) == 1) handle(n, s, &m, now_us);
    flush(n, s, now_us);
}

void oc_sig_net_set_chan_list(oc_sig_net_t *n, const oc_sig_chan_list_t *list)
{
    n->have_list = list != NULL;
    if (list != NULL) {
        n->list = *list;
        if (n->list.count > OC_SIG_CHAN_MAX) n->list.count = OC_SIG_CHAN_MAX;
    } else {
        memset(&n->list, 0, sizeof(n->list));
    }
    /* A CHAN_LIST already sitting in some session's outq (queued, but not yet
     * handed to its chan) still carries whatever the list was when it was
     * queued: refresh its body in place, or a terminal could be handed a
     * push already stale by the time it goes out (fix round 1, M4). One
     * already in flight (chan.pend) keeps the body it was sent with - it is
     * mid-air, and the terminal will ask again (cause 4) if it still
     * mismatches once it lands. */
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        oc_sig_net_sess_t *s = &n->s[i];
        if (!s->used) continue;
        for (uint8_t j = 0; j < s->out_count; j++) {
            if (s->outq[j].type != OC_SIG_CHAN_LIST) continue;
            if (n->have_list) s->outq[j].u.chan_list = n->list;
            else memset(&s->outq[j].u.chan_list, 0, sizeof(s->outq[j].u.chan_list));
        }
        /* one in flight with another version: the current list follows it */
        if (s->registered && s->ch.pend && s->ch.pend_type == OC_SIG_CHAN_LIST) queue_chan_list(n, s);
    }
}

void oc_sig_net_service_req(oc_sig_net_t *n, uint32_t tmid, uint8_t cause, uint64_t now_us)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 1);
    if (s == NULL || n->io.channel == NULL) return;
    s->last_sig = now_us;
    /* the terminal saw a cfg_ver that isn't its list's: send the list (only
     * a registered session has the keys CHAN_LIST needs) */
    if (cause == OC_SIG_SVC_CONFIG && s->registered) {
        s->cl_again = 0;
        queue_chan_list(n, s);
    }
    if (!s->granted) {
        n->io.channel(n->io.ctx, tmid, 1);
        s->chan_req_at = now_us;
    }
}

void oc_sig_net_link(oc_sig_net_t *n, uint32_t tmid, int granted, uint64_t now_us)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 1);
    if (s == NULL) return;
    if (granted && !s->granted) {
        s->last_sig = now_us; /* a fresh grant gets its 5 s */
        s->chan_req_at = 0;
    }
    s->granted = granted;
}

void oc_sig_net_heard(oc_sig_net_t *n, uint32_t tmid, uint64_t now_us)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 0);
    if (s != NULL) s->heard = now_us;
}

void oc_sig_net_tick(oc_sig_net_t *n, uint64_t now_us)
{
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        oc_sig_net_sess_t *s = &n->s[i];
        if (!s->used) continue;
        uint8_t exp;
        if (oc_sig_chan_tick(&s->ch, now_us, s->granted, &exp) == 1) {
            if (exp == OC_SIG_SETUP_IND && s->call != C_NONE) call_end(n, s, OC_SIG_CAUSE_UNREACHABLE, now_us);
            else if (exp == OC_SIG_CONNECT && s->call != C_NONE) call_end(n, s, OC_SIG_CAUSE_NET_FAILURE, now_us);
            else if (exp == OC_SIG_RELEASE && s->call != C_NONE) call_end(n, s, s->end_cause, now_us);
            else if (exp == OC_SIG_AUTH_REQ && !outq_has(s, OC_SIG_AUTH_REQ)) {
                /* if a fresh AUTH_REQ is already queued (drawn by a REG_REQ
                 * that landed while the old one's retries were exhausted but
                 * it hadn't formally expired yet), this expiry belongs to the
                 * OLD vector only - auth_pending now describes the NEW one
                 * about to be flushed, and must survive this expiry, or its
                 * own genuine AUTH_RSP gets dropped (fix round 3, Review
                 * Focus 1b) */
                s->auth_pending = 0;
            } else if (exp == OC_SIG_CHAN_LIST && s->registered && !s->cl_again) {
                /* unanswered, though the channel could send: the terminal
                 * may have been registering when it came (a lost REG_ACK:
                 * ignored, and its retransmissions then fail the replay
                 * window behind the resent REG_ACK). Once more, with a new
                 * seq; nothing else would push it until a cfg_ver change. */
                s->cl_again = 1;
                queue_chan_list(n, s);
            }
        }
        switch (s->call) {
        case C_MT_SETUP:
        case C_MT_ALERT:
            if (now_us >= s->call_at) {
                queue_release(s, s->call_id, OC_SIG_CAUSE_NO_ANSWER);
                s->end_cause = OC_SIG_CAUSE_NO_ANSWER;
                s->call = C_RELEASING;
                s->call_at = now_us + US(5);
            }
            break;
        case C_ACTIVE:
            if (now_us - s->heard > US(5)) call_end(n, s, OC_SIG_CAUSE_LINK_LOST, now_us);
            break;
        case C_RELEASING:
            if (now_us >= s->call_at) call_end(n, s, s->end_cause, now_us);
            break;
        default:
            break;
        }
        if (s->registered && now_us > s->reg_until) {
            s->registered = 0;
            /* the registration's keys go with it. A call still being set up
             * would derive its voice key from them (call_up): it is released
             * (as oc_sig_net_drop does); an active one has its voice key */
            if (s->call != C_NONE && s->call != C_ACTIVE && s->call != C_RELEASING) {
                release_leg(n, s, OC_SIG_CAUSE_LINK_LOST, now_us);
            }
            oc_sig_wipe(s->ck, sizeof(s->ck));
            oc_sig_wipe(s->ik, sizeof(s->ik));
            if (n->io.unregistered != NULL) n->io.unregistered(n->io.ctx, s->tmid, s->number);
        }
        flush(n, s, now_us);
        channel(n, s, now_us);
    }
}

int oc_sig_net_act_done(oc_sig_net_t *n, uint32_t tmid, const oc_sig_msg_t *msg, uint64_t now_us)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 0);
    if (s == NULL || !s->act_wait || (msg->type != OC_SIG_ACT_ACK && msg->type != OC_SIG_ACT_NAK)) return -1;
    s->act_wait = 0;
    char line[64];
    if (msg->type == OC_SIG_ACT_ACK) {
        /* Still registered, with the number the ACK names: the core answered
         * an ACT_REQ again (OC_SIG_ACT_AGAIN: the same token, terminal and
         * K - a retransmission, or a recording played back), and nothing
         * about the terminal changed. A fresh activation that replaces this
         * registration has dropped it first (the contract above). */
        int again = s->registered && memcmp(s->number, msg->u.act_ack.number, OC_SIG_NUMBER_LEN) == 0;
        /* otherwise any half-finished negotiation used the keys before this
         * activation (fix round 2, Review Focus 1c), and whatever call this
         * TMID's session still holds belongs to the terminal's previous life
         * (it rebooted, or was re-activated): end it */
        if (!again) {
            s->auth_pending = 0;
            s->av_wait = 0;
            if (s->call != C_NONE) call_end(n, s, OC_SIG_CAUSE_LINK_LOST, now_us);
        }
        char num[OC_SIG_NUMBER_TEXT];
        oc_sig_number_to_text(msg->u.act_ack.number, num);
        snprintf(line, sizeof(line), "activated %s on terminal %08x", num, (unsigned)tmid);
    } else {
        snprintf(line, sizeof(line), "activation %08x refused (%u)", (unsigned)tmid, msg->u.act_nak.reason);
    }
    logs(n, line);
    queue(s, msg);
    flush(n, s, now_us);
    channel(n, s, now_us);
    return 0;
}

int oc_sig_net_av_done(oc_sig_net_t *n, uint32_t tmid, uint8_t status, const uint8_t number[OC_SIG_NUMBER_LEN],
                       const oc_sig_cell_av_t *av, uint64_t now_us)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 0);
    if (s == NULL || !s->av_wait) return -1;
    s->av_wait = 0;
    if (status == OC_SIG_AV_UNAVAILABLE) return 0; /* no answer: the terminal times out and backs off */
    if (status != OC_SIG_AV_OK) {
        rej(s, status == OC_SIG_AV_AUTH_FAILED ? OC_SIG_REG_AUTH_FAILED : OC_SIG_REG_NOT_ACTIVATED);
    } else {
        memcpy(s->p_rand, av->rand, 16);
        memcpy(s->p_hxres, av->hxres, 16);
        memcpy(s->p_ck, av->ck, 16);
        memcpy(s->p_ik, av->ik, 16);
        memcpy(s->p_number, number, OC_SIG_NUMBER_LEN);
        oc_sig_msg_t m;
        memset(&m, 0, sizeof(m));
        m.type = OC_SIG_AUTH_REQ;
        memcpy(m.u.auth_req.rand, av->rand, 16);
        memcpy(m.u.auth_req.autn, av->autn, 16);
        s->auth_pending = 1;
        queue(s, &m);
    }
    flush(n, s, now_us);
    channel(n, s, now_us);
    return 0;
}

int oc_sig_net_drop(oc_sig_net_t *n, uint32_t tmid, uint8_t cause, uint64_t now_us)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 0);
    if (s == NULL) return -1;
    s->registered = 0;
    s->auth_pending = 0;
    s->av_wait = 0;
    /* the confirmed session keys and any pending vector are no longer good
     * for anything once the core has cancelled this registration */
    oc_sig_wipe(s->ck, sizeof(s->ck));
    oc_sig_wipe(s->ik, sizeof(s->ik));
    oc_sig_wipe(s->p_rand, sizeof(s->p_rand));
    oc_sig_wipe(s->p_hxres, sizeof(s->p_hxres));
    oc_sig_wipe(s->p_ck, sizeof(s->p_ck));
    oc_sig_wipe(s->p_ik, sizeof(s->p_ik));
    /* the terminal is told (RELEASE); once it answers (or 5 s), call_end
     * releases a local call's other leg too */
    if (s->call != C_NONE && s->call != C_RELEASING) release_leg(n, s, cause, now_us);
    return 0;
}

int oc_sig_net_peer_alert(oc_sig_net_t *n, uint32_t call_id, uint64_t now_us)
{
    oc_sig_net_sess_t *s = by_call(n, call_id);
    if (s == NULL || s->call != C_MO_PROC) return -1;
    queue_call(s, OC_SIG_ALERTING, call_id);
    s->call = C_MO_ALERT;
    flush(n, s, now_us);
    return 0;
}

int oc_sig_net_peer_answer(oc_sig_net_t *n, uint32_t call_id, uint64_t now_us)
{
    oc_sig_net_sess_t *s = by_call(n, call_id);
    if (s == NULL || (s->call != C_MO_PROC && s->call != C_MO_ALERT)) return -1;
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_SIG_CONNECT;
    m.u.connect.call_id = call_id;
    m.u.connect.codec = 1;
    queue(s, &m);
    s->call = C_MO_CONNECTING;
    flush(n, s, now_us);
    return 0;
}

int oc_sig_net_peer_release(oc_sig_net_t *n, uint32_t call_id, uint8_t cause, uint64_t now_us)
{
    oc_sig_net_sess_t *s = by_call(n, call_id);
    if (s == NULL || s->call == C_RELEASING) return -1;
    release_leg(n, s, cause, now_us);
    return 0;
}

int oc_sig_net_call_in(oc_sig_net_t *n, const uint8_t callee[OC_SIG_NUMBER_LEN],
                       const uint8_t caller[OC_SIG_NUMBER_LEN], uint64_t now_us, uint32_t *call_id)
{
    oc_sig_net_sess_t *s = by_number(n, callee);
    if (s == NULL) return OC_SIG_NET_IN_UNREACHABLE;
    if (s->call != C_NONE) return OC_SIG_NET_IN_BUSY;
    s->call_id = ++n->next_call_id;
    memcpy(s->peer, caller, OC_SIG_NUMBER_LEN);
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_SIG_SETUP_IND;
    m.u.setup_ind.call_id = s->call_id;
    memcpy(m.u.setup_ind.caller, caller, OC_SIG_NUMBER_LEN);
    m.u.setup_ind.codec_caps = 1;
    queue(s, &m);
    s->call = C_MT_SETUP;
    s->call_at = now_us + US(60);
    if (call_id != NULL) *call_id = s->call_id;
    flush(n, s, now_us);
    channel(n, s, now_us);
    return 0;
}

static int voice_crypt(oc_sig_net_sess_t *s, uint8_t dir, uint32_t fctr, uint8_t *d, size_t len)
{
    uint8_t nonce[14] = { 0 };
    nonce[0] = dir;
    oc_sig_put32(nonce + 1, s->call_id);
    oc_sig_put32(nonce + 5, fctr);
    return oc_sig_aes128_ctr(s->k_voice, nonce, d, len);
}

/* The media gate (core-test-services spec §14 F1; calls spec §5, §6): how an
 * app data frame may cross the air on this leg now. 1: encrypted with
 * K_voice; 0: in the clear; -1: not at all.
 * - An active leg (C_ACTIVE: call_up made K_voice), encrypted unless the
 *   session registered in Part 97 and the cell is still in Part 97.
 * - A registered Part 97 session with no call: the diagnostic loopback
 *   (ocbench net's echo), in the clear.
 * - Nothing else: not while the leg is being set up (C_MO_CONNECTING included:
 *   the far end answered but the terminal hasn't confirmed, so early media
 *   from the core is dropped) or released, and in Part 15 never in the clear.
 * The session's mode is its registration's (ch.sec.encrypt, as the terminal
 * keeps it), so a cell switched mid-call never changes a call's keying: a
 * Part 15 call stays encrypted, a Part 97 call on a cell now in Part 15 stops. */
static int media_gate(const oc_sig_net_t *n, const oc_sig_net_sess_t *s)
{
    int clear = s->ch.sec.encrypt == 0 && n->cfg.mode == OC_SIG_MODE_PART97;
    if (s->call == C_ACTIVE) return clear ? 0 : s->ch.sec.encrypt == 0 ? -1 : 1;
    if (s->call == C_NONE && s->registered && clear) return 0;
    return -1;
}

int oc_sig_net_data_in(oc_sig_net_t *n, uint32_t tmid, const uint8_t *p, uint8_t len, uint8_t out[OC_SIG_APP_MAX],
                       uint8_t *out_n)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 0);
    if (s == NULL || len < 2 || p[0] != OC_SIG_KIND_DATA || len - 2u > OC_SIG_APP_MAX) return -1;
    int gate = media_gate(n, s);
    if (gate < 0) return -1; /* not active (or Part 15 outside a call): dropped, never relayed */
    uint8_t dn = (uint8_t)(len - 2u);
    memcpy(out, p + 2, dn);
    if (gate == 1) {
        uint32_t cand = (s->d_rx_next & ~0xFFu) | p[1];
        if (cand < s->d_rx_next) cand += 256u;
        /* a replay/duplicate: same window as the terminal side and oc_sig_open
         * (controller ruling, task 7 fix round) - do not advance d_rx_next */
        if (cand - s->d_rx_next >= 128u) return -1;
        if (voice_crypt(s, 0, cand, out, dn) != 0) return -1; /* can't decrypt: dropped */
        s->d_rx_next = cand + 1u;
    }
    *out_n = dn;
    return 0;
}

int oc_sig_net_data_out(oc_sig_net_t *n, uint32_t tmid, const uint8_t *d, uint8_t len,
                        uint8_t out[OC_SIG_LINK_MAX], uint8_t *out_n)
{
    oc_sig_net_sess_t *s = sess(n, tmid, 0);
    if (s == NULL || len > OC_SIG_APP_MAX) return -1;
    int gate = media_gate(n, s);
    if (gate < 0) return -1; /* out untouched: no plaintext, and the counter doesn't move */
    out[0] = OC_SIG_KIND_DATA;
    out[1] = (uint8_t)s->d_tx;
    memcpy(out + 2, d, len);
    if (gate == 1 && voice_crypt(s, 1, s->d_tx, out + 2, len) != 0) {
        memset(out + 2, 0, len); /* fail closed: never the plaintext, and the counter doesn't move */
        return -1;
    }
    s->d_tx++;
    *out_n = (uint8_t)(len + 2u);
    return 0;
}

int oc_sig_net_registered(const oc_sig_net_t *n, uint32_t tmid)
{
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        if (n->s[i].used && n->s[i].tmid == tmid) return n->s[i].registered;
    }
    return 0;
}

int oc_sig_net_number(const oc_sig_net_t *n, uint32_t tmid, uint8_t out[OC_SIG_NUMBER_LEN])
{
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        if (n->s[i].used && n->s[i].tmid == tmid && n->s[i].registered) {
            memcpy(out, n->s[i].number, OC_SIG_NUMBER_LEN);
            return 0;
        }
    }
    return -1;
}

int oc_sig_net_local_peer(const oc_sig_net_t *n, uint32_t tmid, uint32_t *peer_tmid)
{
    const oc_sig_net_sess_t *a = NULL, *b = NULL;
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        if (n->s[i].used && n->s[i].tmid == tmid) a = &n->s[i];
    }
    if (a == NULL || a->call != C_ACTIVE || a->other == 0) return 0;
    for (unsigned i = 0; i < OC_SIG_NET_TERMS; i++) {
        if (n->s[i].used && n->s[i].tmid == a->other) b = &n->s[i];
    }
    if (b == NULL || b->call != C_ACTIVE || b->other != tmid) return 0;
    *peer_tmid = b->tmid;
    return 1;
}
