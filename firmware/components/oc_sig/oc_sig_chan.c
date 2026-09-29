#include "oc_sig_chan.h"

#include <string.h>

void oc_sig_chan_init(oc_sig_chan_t *c, uint8_t tx_dir)
{
    memset(c, 0, sizeof(*c));
    oc_sig_sec_init(&c->sec, tx_dir);
    oc_sig_reasm_init(&c->rx);
}

void oc_sig_chan_reset(oc_sig_chan_t *c)
{
    oc_sig_sec_t sec = c->sec;
    uint8_t seq = c->tx_seq;
    oc_sig_chan_init(c, sec.tx_dir);
    c->sec = sec;
    c->tx_seq = seq; /* keep numbering moving so the peer never mistakes a new message for a repeat */
}

void oc_sig_chan_forget_rx(oc_sig_chan_t *c)
{
    /* Only the last seq: the cached request/reply stays, so a request the
     * old peer repeats (a RELEASE whose RELEASE_COMPLETE was lost) is still
     * answered. A new peer can't match it: a repeat must be the cached
     * request's own bytes (fix round 3). */
    c->have_rx_seq = 0;
}

int oc_sig_is_request(uint8_t t)
{
    switch (t) {
    case OC_SIG_ACT_REQ: case OC_SIG_REG_REQ: case OC_SIG_AUTH_REQ: case OC_SIG_AUTH_RSP: case OC_SIG_AUTH_FAIL:
    case OC_SIG_CALL_SETUP: case OC_SIG_SETUP_IND: case OC_SIG_CONNECT: case OC_SIG_RELEASE:
    case OC_SIG_CHAN_LIST:
        return 1;
    default:
        return 0;
    }
}

int oc_sig_is_reply(uint8_t req, uint8_t rsp)
{
    switch (req) {
    case OC_SIG_ACT_REQ:    return rsp == OC_SIG_ACT_ACK || rsp == OC_SIG_ACT_NAK;
    case OC_SIG_REG_REQ:    return rsp == OC_SIG_AUTH_REQ || rsp == OC_SIG_REG_REJ;
    case OC_SIG_AUTH_REQ:   return rsp == OC_SIG_AUTH_RSP || rsp == OC_SIG_AUTH_FAIL;
    case OC_SIG_AUTH_RSP:   return rsp == OC_SIG_REG_ACK || rsp == OC_SIG_REG_REJ;
    case OC_SIG_AUTH_FAIL:  return rsp == OC_SIG_AUTH_REQ || rsp == OC_SIG_REG_REJ;
    case OC_SIG_CALL_SETUP: return rsp == OC_SIG_CALL_PROC || rsp == OC_SIG_RELEASE;
    case OC_SIG_SETUP_IND:  return rsp == OC_SIG_ALERTING || rsp == OC_SIG_CONNECT || rsp == OC_SIG_RELEASE;
    case OC_SIG_CONNECT:    return rsp == OC_SIG_CONNECT_ACK || rsp == OC_SIG_RELEASE;
    case OC_SIG_RELEASE:    return rsp == OC_SIG_RELEASE_COMPLETE;
    case OC_SIG_CHAN_LIST:  return rsp == OC_SIG_CHAN_LIST_ACK;
    default:                return 0;
    }
}

static int enqueue(oc_sig_chan_t *c, const uint8_t *msg, size_t len, uint8_t seq)
{
    uint8_t frag[OC_SIG_MAX_FRAGS][OC_SIG_LINK_MAX], flen[OC_SIG_MAX_FRAGS];
    uint8_t n = oc_sig_fragment(msg, len, seq, frag, flen);
    if (n == 0 || c->txq_count + n > OC_SIG_TXQ) return -1;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t slot = (uint8_t)((c->txq_head + c->txq_count) % OC_SIG_TXQ);
        memcpy(c->txq[slot], frag[i], flen[i]);
        c->txq_len[slot] = flen[i];
        c->txq_count++;
    }
    return 0;
}

int oc_sig_chan_send(oc_sig_chan_t *c, const oc_sig_msg_t *m, uint64_t now_us)
{
    int req = oc_sig_is_request(m->type);
    if ((req && c->pend) || c->txq_count + OC_SIG_MAX_FRAGS > OC_SIG_TXQ) return -1;
    uint8_t buf[OC_SIG_MAX_MSG];
    size_t n = oc_sig_seal(&c->sec, m, buf, sizeof(buf));
    if (n == 0) return -1;
    uint8_t seq = c->tx_seq++;
    enqueue(c, buf, n, seq); /* room was checked above */
    if (c->rq_have && !c->have_reply && oc_sig_is_reply(c->rq_type, m->type)) {
        c->reply = *m; /* kept for a repeat of that request */
        c->reply_seq = seq;
        c->have_reply = 1;
    }
    if (req) {
        c->pend = 1;
        c->pend_type = m->type;
        memcpy(c->pend_msg, buf, n);
        c->pend_len = n;
        c->pend_seq = seq;
        c->pend_due = now_us + OC_SIG_RETX_US;
        c->pend_tries = 0;
    }
    return 0;
}

int oc_sig_chan_rx(oc_sig_chan_t *c, const uint8_t *p, uint8_t n, oc_sig_msg_t *m, uint64_t now_us)
{
    (void)now_us;
    uint8_t msg[OC_SIG_MAX_MSG], seq;
    size_t len;
    if (oc_sig_reasm_push(&c->rx, p, n, msg, &len, &seq) != 1) return 0;
    /* The cached request/reply survives a later NON-request reply crossing it
     * (e.g. CHAN_LIST_ACK arriving after a CALL_SETUP): only a genuinely new
     * REQUEST is allowed to replace rq_/reply below (fix round 1, controller
     * ruling A). Checked ahead of the plain "same as rx_seq" repeat test, so
     * it still fires even once a later, different-seq message has moved
     * rx_seq past the cached request's own seq. Seq is 8 bits: after 256
     * messages on this chan without a fresh request the wrap could, in
     * principle, alias a stale rq_seq onto an unrelated new message of the
     * same type - accepted here as in the rest of this module (session
     * lifetimes never approach that many signalling messages).
     * A repeat must be the cached request's own bytes, not merely its seq
     * and type (fix round 2, controller ruling): this runs before
     * oc_sig_open, and seq + type are anyone's to forge - a made-up prot-0
     * AUTH_RSP under the right seq drew the cached REG_ACK, sealed afresh
     * with the live keys, for a re-registering terminal to take without an
     * AKA. The real peer's retransmission is byte-identical (oc_sig_chan_tick
     * resends pend_msg), so nothing genuine is lost. What this can't stop is
     * a recorded frame played back while that request is still the last one
     * heard: it is indistinguishable from a retransmission, and draws the
     * same reply (sealed afresh) as one would. */
    int rq_repeat = c->rq_have && c->have_reply && seq == c->rq_seq && len == c->rq_len &&
                    memcmp(msg, c->rq_msg, len) == 0;
    /* A plain repeat (resent, or a reply sealed afresh for a repeated
     * request) has the seq AND the type of the last message. Another type
     * under that seq is a peer that restarted its numbering (a terminal
     * rebooting between ACT_ACK and its REG_REQ sends REG_REQ as seq 0, the
     * ACT_REQ's): dropped, every retransmission was lost (fix round 2). */
    if (rq_repeat || (c->have_rx_seq && seq == c->rx_seq && msg[0] == c->rx_type)) {
        /* A repeat. Only a request is answered, and only with its own reply:
         * answering with whatever went out last gave a lost CALL_PROC's
         * retransmitted CALL_SETUP an ALERTING, and let a reply crossing a
         * retransmission bounce between the two ends for ever. The reply is
         * sealed afresh (a later message may have moved the peer's replay
         * window past it) but keeps its sequence number, so a peer that did
         * get it and nothing since drops it as a repeat. */
        if (rq_repeat && c->pend && c->pend_seq == c->reply_seq) {
            /* the reply is a request of ours still in flight (a RELEASE
             * answering CALL_SETUP): its own bytes again, so every copy under
             * that seq is the same and the peer, whichever it cached, answers
             * our retransmissions too (fix round 3) */
            enqueue(c, c->pend_msg, c->pend_len, c->pend_seq); /* no room: the peer asks again */
        } else if (rq_repeat) {
            uint8_t buf[OC_SIG_MAX_MSG];
            size_t bn = oc_sig_seal(&c->sec, &c->reply, buf, sizeof(buf));
            if (bn != 0) enqueue(c, buf, bn, c->reply_seq); /* no room: the peer asks again */
        }
        return 0;
    }
    if (oc_sig_open(&c->sec, msg, len, m) != 0) return 0;
    c->have_rx_seq = 1;
    c->rx_seq = seq;
    c->rx_type = m->type;
    if (oc_sig_is_request(m->type)) { /* a non-request (e.g. an ACK) keeps the last request's reply cached */
        c->rq_have = 1;
        c->rq_seq = seq;
        c->rq_type = m->type;
        memcpy(c->rq_msg, msg, len);
        c->rq_len = len;
        c->have_reply = 0;
    }
    if (c->pend && oc_sig_is_reply(c->pend_type, m->type)) c->pend = 0;
    return 1;
}

int oc_sig_chan_tick(oc_sig_chan_t *c, uint64_t now_us, int can_send, uint8_t *expired_type)
{
    if (!c->pend) return 0;
    if (!can_send) {
        c->pend_due = now_us + OC_SIG_RETX_US; /* the clock only runs while it could have been heard */
        return 0;
    }
    if (now_us < c->pend_due) return 0;
    if (c->pend_tries >= OC_SIG_RETX_MAX) {
        c->pend = 0;
        if (expired_type != NULL) *expired_type = c->pend_type;
        return 1;
    }
    if (enqueue(c, c->pend_msg, c->pend_len, c->pend_seq) != 0) return 0;
    c->pend_tries++;
    c->pend_due = now_us + OC_SIG_RETX_US;
    return 0;
}

int oc_sig_chan_busy(const oc_sig_chan_t *c)
{
    return c->pend;
}

int oc_sig_chan_cancel(oc_sig_chan_t *c, uint8_t type)
{
    if (!c->pend || c->pend_type != type) return 0;
    c->pend = 0;
    return 1;
}

int oc_sig_chan_peek(const oc_sig_chan_t *c, const uint8_t **p, uint8_t *n)
{
    if (c->txq_count == 0) return -1;
    *p = c->txq[c->txq_head];
    *n = c->txq_len[c->txq_head];
    return 0;
}

void oc_sig_chan_pop(oc_sig_chan_t *c)
{
    if (c->txq_count == 0) return;
    c->txq_head = (uint8_t)((c->txq_head + 1u) % OC_SIG_TXQ);
    c->txq_count--;
}
