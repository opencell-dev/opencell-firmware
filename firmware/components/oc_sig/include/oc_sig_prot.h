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
    uint32_t chk_ctr; /* check-ins: the terminal's next to send, the network's lowest it takes */
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

/* An idle terminal's check-in (sparse beacon spec 2026-11-01 §4.4), the RACH
 * CHECKIN payload: ctr (4, big-endian) | HMAC-SHA256(K_int, 0xC1 | tmid (4,
 * BE) | ctr (4, BE))[0:4]. Its own counter (chk_ctr, restarted with the
 * keys), and a first byte no signalling MAC input has (those start with the
 * direction, 0 or 1). seal: 0 with out filled and chk_ctr advanced, or -1
 * (no keys). open: 0 when the MAC is good and ctr >= chk_ctr (chk_ctr then
 * ctr + 1), or -1. */
#define OC_SIG_CHECKIN_LEN 8u
int oc_sig_checkin_seal(oc_sig_sec_t *s, uint32_t tmid, uint8_t out[OC_SIG_CHECKIN_LEN]);
int oc_sig_checkin_open(oc_sig_sec_t *s, uint32_t tmid, const uint8_t *in, size_t len);

#endif
