/* ocb_hss — the network stand-in's subscriber store (spec §7): the network
 * X25519 key pair, and per subscriber the number, activation token, bound
 * TMID, K, OPc and SQN. A human-readable text file, one record per line:
 *
 *   network key_id=1 sk=<64 hex> pk=<64 hex> mode=part15 period=1800
 *   sub number=+883160655501234 token_id=<16 hex> token_secret=<32 hex> expiry=<unix s>
 *       used=0 tmid=00000000 activated=0 k=<32 hex> opc=<32 hex> sqn=<12 hex>   (one line)
 *
 * Numbers are in the full form (numbering-plan.md v0.2). A numbering-v1
 * number (13 digits) is refused with a message saying so: remove the sub
 * lines and issue new codes (numbering v2 spec §8).
 *
 * The file holds secrets: it is written 0600, and belongs outside the repo
 * (default ~/.config/opencell/hss.txt). */
#ifndef OCB_HSS_H
#define OCB_HSS_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "oc_sig_net.h"
#include "oc_sig_qr.h"

#define OCB_HSS_SUBS 16u

typedef struct {
    int          have_network;
    uint16_t     key_id;
    uint8_t      sk[32], pk[32];
    uint8_t      mode;     /* oc_sig_mode_t */
    uint16_t     period_s;
    oc_sig_sub_t subs[OCB_HSS_SUBS];
    unsigned     n;
    char         err[PATH_MAX + 128]; /* why ocb_hss_load failed: "FILE:LINE: reason" ("" otherwise);
                                        * sized so a long HSS path never crowds out the reason text */
} ocb_hss_t;

typedef void (*ocb_random_fn)(uint8_t *out, size_t n);

/* 0, or -1 (unreadable file or a malformed line: nothing is guessed; h->err
 * says which line and why). A missing file is an empty HSS. */
int  ocb_hss_load(ocb_hss_t *h, const char *path);
/* 0 or -1. Writes path.tmp (0600) and renames it over path. */
int  ocb_hss_save(const ocb_hss_t *h, const char *path);
/* Take the exclusive lock on path's HSS (the lock file path.lock; the HSS
 * itself is replaced on every save, so it can't carry the lock). Never waits.
 * Returns the fd that holds it (keep it open), -1 if another process holds
 * it, -2 if the lock file can't be opened. `ocbench net` holds it for its
 * lifetime, so a `mkqr` (whose token net's next save would erase) is refused. */
int  ocb_hss_lock(const char *path);
void ocb_hss_unlock(int fd);
/* Make the network key pair on first use (key id 1, Part 15, 1800 s). 0 or -1. */
int  ocb_hss_ensure_network(ocb_hss_t *h, ocb_random_fn rnd);
/* A fresh token for number (a new subscriber, or re-issued: used = 0). NULL if full. */
oc_sig_sub_t *ocb_hss_issue(ocb_hss_t *h, const uint8_t number[OC_SIG_NUMBER_LEN], uint32_t expiry,
                            ocb_random_fn rnd);
oc_sig_sub_t *ocb_hss_by_token(ocb_hss_t *h, const uint8_t token_id[8]);
oc_sig_sub_t *ocb_hss_by_tmid(ocb_hss_t *h, uint32_t tmid); /* activated and bound */
oc_sig_sub_t *ocb_hss_by_number(ocb_hss_t *h, const uint8_t number[OC_SIG_NUMBER_LEN]);
void ocb_hss_unbind(ocb_hss_t *h, uint32_t tmid);
/* The QR contents for sub (key id and pkn from the network record). */
void ocb_hss_qr(const ocb_hss_t *h, const oc_sig_sub_t *sub, oc_sig_qr_t *q);
/* 1 if number has 13 digits: a numbering-v1 number (+8836065551234). It is
 * still a well-formed v2 number (country code 60), but on this bench it can
 * only be a leftover, so ocbench refuses it (numbering v2 spec §6.4). */
int  ocb_hss_v1_number(const uint8_t number[OC_SIG_NUMBER_LEN]);

#endif
