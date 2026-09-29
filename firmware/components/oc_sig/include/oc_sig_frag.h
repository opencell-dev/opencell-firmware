/* Signalling messages over link payloads: [0x10 | frag<<2 | last<<1][seq][<= 18 B]. */
#ifndef OC_SIG_FRAG_H
#define OC_SIG_FRAG_H

#include "oc_sig.h"

/* Split msg (1..OC_SIG_MAX_MSG bytes); every fragment but the last carries
 * exactly OC_SIG_FRAG_DATA bytes. Returns the fragment count, 0 if too long. */
uint8_t oc_sig_fragment(const uint8_t *msg, size_t len, uint8_t seq, uint8_t out[OC_SIG_MAX_FRAGS][OC_SIG_LINK_MAX],
                        uint8_t out_len[OC_SIG_MAX_FRAGS]);

typedef struct {
    int     active;
    uint8_t seq;
    uint8_t have;  /* bit per fragment received */
    uint8_t total; /* 0 until the last fragment is seen */
    uint8_t last_len;
    uint8_t buf[OC_SIG_MAX_MSG];
} oc_sig_reasm_t;

void oc_sig_reasm_init(oc_sig_reasm_t *r);

/* Feed one kind-0x1_ payload. 1: a whole message is in msg/len/seq; 0: more
 * needed; -1: malformed. A fragment with another seq restarts reassembly. */
int oc_sig_reasm_push(oc_sig_reasm_t *r, const uint8_t *p, uint8_t n, uint8_t *msg, size_t *len, uint8_t *seq);

#endif
