/* Message protection (spec §4.2): type | prot | ctr_lo | body | [mac4]. */
#ifndef OC_SIG_PROT_H
#define OC_SIG_PROT_H

#include "oc_sig_msg.h"

typedef struct {
    int      keyed;
    int      encrypt; /* 1 Part 15 (prot 2), 0 Part 97 (prot 1), -1 not known yet: accept either */
    uint8_t  k_int[16], k_enc[16];
    uint8_t  tx_dir;  /* 0 terminal (uplink), 1 network (downlink) */
    uint32_t tx_ctr, rx_next;
} oc_sig_sec_t;

void oc_sig_sec_init(oc_sig_sec_t *s, uint8_t tx_dir);
/* New session keys; both counters restart at 0. */
void oc_sig_sec_key(oc_sig_sec_t *s, const uint8_t k_int[16], const uint8_t k_enc[16], int encrypt);

/* Types that travel with prot 0: activation, REG_REQ, AUTH_*, REG_REJ. */
int oc_sig_preauth_type(uint8_t type);

/* Pre-auth types are sealed with prot 0, all others need keys (prot 2 when
 * encrypt == 1, else 1). Returns the message length, 0 on error. */
size_t oc_sig_seal(oc_sig_sec_t *s, const oc_sig_msg_t *m, uint8_t *out, size_t cap);

/* 0, or -1 to drop: bad format, wrong prot for the type or mode, bad MAC,
 * counter replayed or more than 127 ahead. */
int oc_sig_open(oc_sig_sec_t *s, const uint8_t *in, size_t len, oc_sig_msg_t *m);

#endif
