/* Reliable request/response signalling over link payloads: one request in
 * flight per direction, retransmitted every LC_SIG_RETX_US up to
 * LC_SIG_RETX_MAX times (only while the link can send). A repeated message
 * (same sequence number) is not processed again; the last message sent is
 * sent again as its answer (spec §4.4). */
#ifndef LC_SIG_CHAN_H
#define LC_SIG_CHAN_H

#include "lc_sig_frag.h"
#include "lc_sig_prot.h"

#define LC_SIG_RETX_US  1000000u
#define LC_SIG_RETX_MAX 3u
#define LC_SIG_TXQ      12u
#define LC_SIG_OUTQ     3u /* messages an owner holds while waiting for a channel */

typedef struct {
    lc_sig_sec_t   sec;
    lc_sig_reasm_t rx;
    uint8_t        tx_seq;
    int            have_rx_seq;
    uint8_t        rx_seq;
    int            have_last;
    uint8_t        last_msg[LC_SIG_MAX_MSG];
    size_t         last_len;
    uint8_t        last_seq;
    int            pend;
    uint8_t        pend_type;
    uint8_t        pend_msg[LC_SIG_MAX_MSG];
    size_t         pend_len;
    uint8_t        pend_seq;
    uint64_t       pend_due;
    uint8_t        pend_tries;
    uint8_t        txq[LC_SIG_TXQ][LC_SIG_LINK_MAX];
    uint8_t        txq_len[LC_SIG_TXQ];
    uint8_t        txq_head, txq_count;
} lc_sig_chan_t;

void lc_sig_chan_init(lc_sig_chan_t *c, uint8_t tx_dir);
/* Forget queued/pending/received state (keys are kept). */
void lc_sig_chan_reset(lc_sig_chan_t *c);
int  lc_sig_is_request(uint8_t type);
int  lc_sig_is_reply(uint8_t req, uint8_t rsp);
int  lc_sig_chan_send(lc_sig_chan_t *c, const lc_sig_msg_t *m, uint64_t now_us);
int  lc_sig_chan_rx(lc_sig_chan_t *c, const uint8_t *p, uint8_t n, lc_sig_msg_t *m, uint64_t now_us);
int  lc_sig_chan_tick(lc_sig_chan_t *c, uint64_t now_us, int can_send, uint8_t *expired_type);
int  lc_sig_chan_busy(const lc_sig_chan_t *c);
int  lc_sig_chan_peek(const lc_sig_chan_t *c, const uint8_t **p, uint8_t *n);
void lc_sig_chan_pop(lc_sig_chan_t *c);

#endif
