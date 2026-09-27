/* Signalling messages over link payloads: [0x10 | frag<<2 | last<<1][seq][<= 18 B]. */
#ifndef LC_SIG_FRAG_H
#define LC_SIG_FRAG_H

#include "lc_sig.h"

/* Split msg (1..LC_SIG_MAX_MSG bytes); every fragment but the last carries
 * exactly LC_SIG_FRAG_DATA bytes. Returns the fragment count, 0 if too long. */
uint8_t lc_sig_fragment(const uint8_t *msg, size_t len, uint8_t seq, uint8_t out[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX],
                        uint8_t out_len[LC_SIG_MAX_FRAGS]);

typedef struct {
    int     active;
    uint8_t seq;
    uint8_t have;  /* bit per fragment received */
    uint8_t total; /* 0 until the last fragment is seen */
    uint8_t last_len;
    uint8_t buf[LC_SIG_MAX_MSG];
} lc_sig_reasm_t;

void lc_sig_reasm_init(lc_sig_reasm_t *r);

/* Feed one kind-0x1_ payload. 1: a whole message is in msg/len/seq; 0: more
 * needed; -1: malformed. A fragment with another seq restarts reassembly. */
int lc_sig_reasm_push(lc_sig_reasm_t *r, const uint8_t *p, uint8_t n, uint8_t *msg, size_t *len, uint8_t *seq);

#endif
