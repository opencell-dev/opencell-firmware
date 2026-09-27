#include "lc_sig_chan.h"

#include <string.h>

void lc_sig_chan_init(lc_sig_chan_t *c, uint8_t tx_dir)
{
    memset(c, 0, sizeof(*c));
    lc_sig_sec_init(&c->sec, tx_dir);
    lc_sig_reasm_init(&c->rx);
}

void lc_sig_chan_reset(lc_sig_chan_t *c)
{
    lc_sig_sec_t sec = c->sec;
    uint8_t seq = c->tx_seq;
    lc_sig_chan_init(c, sec.tx_dir);
    c->sec = sec;
    c->tx_seq = seq; /* keep numbering moving so the peer never mistakes a new message for a repeat */
}

int lc_sig_is_request(uint8_t t)
{
    switch (t) {
    case LC_SIG_ACT_REQ: case LC_SIG_REG_REQ: case LC_SIG_AUTH_REQ: case LC_SIG_AUTH_RSP: case LC_SIG_AUTH_FAIL:
    case LC_SIG_CALL_SETUP: case LC_SIG_SETUP_IND: case LC_SIG_CONNECT: case LC_SIG_RELEASE:
        return 1;
    default:
        return 0;
    }
}

int lc_sig_is_reply(uint8_t req, uint8_t rsp)
{
    switch (req) {
    case LC_SIG_ACT_REQ:    return rsp == LC_SIG_ACT_ACK || rsp == LC_SIG_ACT_NAK;
    case LC_SIG_REG_REQ:    return rsp == LC_SIG_AUTH_REQ || rsp == LC_SIG_REG_REJ;
    case LC_SIG_AUTH_REQ:   return rsp == LC_SIG_AUTH_RSP || rsp == LC_SIG_AUTH_FAIL;
    case LC_SIG_AUTH_RSP:   return rsp == LC_SIG_REG_ACK || rsp == LC_SIG_REG_REJ;
    case LC_SIG_AUTH_FAIL:  return rsp == LC_SIG_AUTH_REQ || rsp == LC_SIG_REG_REJ;
    case LC_SIG_CALL_SETUP: return rsp == LC_SIG_CALL_PROC || rsp == LC_SIG_RELEASE;
    case LC_SIG_SETUP_IND:  return rsp == LC_SIG_ALERTING || rsp == LC_SIG_CONNECT || rsp == LC_SIG_RELEASE;
    case LC_SIG_CONNECT:    return rsp == LC_SIG_CONNECT_ACK || rsp == LC_SIG_RELEASE;
    case LC_SIG_RELEASE:    return rsp == LC_SIG_RELEASE_COMPLETE;
    default:                return 0;
    }
}

static int enqueue(lc_sig_chan_t *c, const uint8_t *msg, size_t len, uint8_t seq)
{
    uint8_t frag[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], flen[LC_SIG_MAX_FRAGS];
    uint8_t n = lc_sig_fragment(msg, len, seq, frag, flen);
    if (n == 0 || c->txq_count + n > LC_SIG_TXQ) return -1;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t slot = (uint8_t)((c->txq_head + c->txq_count) % LC_SIG_TXQ);
        memcpy(c->txq[slot], frag[i], flen[i]);
        c->txq_len[slot] = flen[i];
        c->txq_count++;
    }
    return 0;
}

int lc_sig_chan_send(lc_sig_chan_t *c, const lc_sig_msg_t *m, uint64_t now_us)
{
    int req = lc_sig_is_request(m->type);
    if ((req && c->pend) || c->txq_count + LC_SIG_MAX_FRAGS > LC_SIG_TXQ) return -1;
    uint8_t buf[LC_SIG_MAX_MSG];
    size_t n = lc_sig_seal(&c->sec, m, buf, sizeof(buf));
    if (n == 0) return -1;
    uint8_t seq = c->tx_seq++;
    enqueue(c, buf, n, seq); /* room was checked above */
    if (c->rq_have && !c->have_reply && lc_sig_is_reply(c->rq_type, m->type)) {
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
        c->pend_due = now_us + LC_SIG_RETX_US;
        c->pend_tries = 0;
    }
    return 0;
}

int lc_sig_chan_rx(lc_sig_chan_t *c, const uint8_t *p, uint8_t n, lc_sig_msg_t *m, uint64_t now_us)
{
    (void)now_us;
    uint8_t msg[LC_SIG_MAX_MSG], seq;
    size_t len;
    if (lc_sig_reasm_push(&c->rx, p, n, msg, &len, &seq) != 1) return 0;
    if (c->have_rx_seq && seq == c->rx_seq) {
        /* A repeat. Only a request is answered, and only with its own reply:
         * answering with whatever went out last gave a lost CALL_PROC's
         * retransmitted CALL_SETUP an ALERTING, and let a reply crossing a
         * retransmission bounce between the two ends for ever. The reply is
         * sealed afresh (a later message may have moved the peer's replay
         * window past it) but keeps its sequence number, so a peer that did
         * get it and nothing since drops it as a repeat. */
        if (c->rq_have && c->have_reply && seq == c->rq_seq && msg[0] == c->rq_type) {
            uint8_t buf[LC_SIG_MAX_MSG];
            size_t bn = lc_sig_seal(&c->sec, &c->reply, buf, sizeof(buf));
            if (bn != 0) enqueue(c, buf, bn, c->reply_seq); /* no room: the peer asks again */
        }
        return 0;
    }
    if (lc_sig_open(&c->sec, msg, len, m) != 0) return 0;
    c->have_rx_seq = 1;
    c->rx_seq = seq;
    c->rq_have = lc_sig_is_request(m->type);
    c->rq_seq = seq;
    c->rq_type = m->type;
    c->have_reply = 0;
    if (c->pend && lc_sig_is_reply(c->pend_type, m->type)) c->pend = 0;
    return 1;
}

int lc_sig_chan_tick(lc_sig_chan_t *c, uint64_t now_us, int can_send, uint8_t *expired_type)
{
    if (!c->pend) return 0;
    if (!can_send) {
        c->pend_due = now_us + LC_SIG_RETX_US; /* the clock only runs while it could have been heard */
        return 0;
    }
    if (now_us < c->pend_due) return 0;
    if (c->pend_tries >= LC_SIG_RETX_MAX) {
        c->pend = 0;
        if (expired_type != NULL) *expired_type = c->pend_type;
        return 1;
    }
    if (enqueue(c, c->pend_msg, c->pend_len, c->pend_seq) != 0) return 0;
    c->pend_tries++;
    c->pend_due = now_us + LC_SIG_RETX_US;
    return 0;
}

int lc_sig_chan_busy(const lc_sig_chan_t *c)
{
    return c->pend;
}

int lc_sig_chan_peek(const lc_sig_chan_t *c, const uint8_t **p, uint8_t *n)
{
    if (c->txq_count == 0) return -1;
    *p = c->txq[c->txq_head];
    *n = c->txq_len[c->txq_head];
    return 0;
}

void lc_sig_chan_pop(lc_sig_chan_t *c)
{
    if (c->txq_count == 0) return;
    c->txq_head = (uint8_t)((c->txq_head + 1u) % LC_SIG_TXQ);
    c->txq_count--;
}
