/* lcb_hss — the network stand-in's subscriber store (spec §7): the network
 * X25519 key pair, and per subscriber the number, activation token, bound
 * TMID, K, OPc and SQN. A human-readable text file, one record per line:
 *
 *   network key_id=1 sk=<64 hex> pk=<64 hex> mode=part15 period=1800
 *   sub number=+8836065551234 token_id=<16 hex> token_secret=<32 hex> expiry=<unix s>
 *       used=0 tmid=00000000 activated=0 k=<32 hex> opc=<32 hex> sqn=<12 hex>   (one line)
 *
 * The file holds secrets: it is written 0600, and belongs outside the repo
 * (default ~/.config/opencell/hss.txt). */
#ifndef LCB_HSS_H
#define LCB_HSS_H

#include <stddef.h>
#include <stdint.h>

#include "lc_sig_net.h"
#include "lc_sig_qr.h"

#define LCB_HSS_SUBS 16u

typedef struct {
    int          have_network;
    uint16_t     key_id;
    uint8_t      sk[32], pk[32];
    uint8_t      mode;     /* lc_sig_mode_t */
    uint16_t     period_s;
    lc_sig_sub_t subs[LCB_HSS_SUBS];
    unsigned     n;
} lcb_hss_t;

typedef void (*lcb_random_fn)(uint8_t *out, size_t n);

/* 0, or -1 (unreadable file or a malformed line: nothing is guessed). A missing file is an empty HSS. */
int  lcb_hss_load(lcb_hss_t *h, const char *path);
/* 0 or -1. Writes path.tmp (0600) and renames it over path. */
int  lcb_hss_save(const lcb_hss_t *h, const char *path);
/* Take the exclusive lock on path's HSS (the lock file path.lock; the HSS
 * itself is replaced on every save, so it can't carry the lock). Never waits.
 * Returns the fd that holds it (keep it open), -1 if another process holds
 * it, -2 if the lock file can't be opened. `lcbench net` holds it for its
 * lifetime, so a `mkqr` (whose token net's next save would erase) is refused. */
int  lcb_hss_lock(const char *path);
void lcb_hss_unlock(int fd);
/* Make the network key pair on first use (key id 1, Part 15, 1800 s). 0 or -1. */
int  lcb_hss_ensure_network(lcb_hss_t *h, lcb_random_fn rnd);
/* A fresh token for number (a new subscriber, or re-issued: used = 0). NULL if full. */
lc_sig_sub_t *lcb_hss_issue(lcb_hss_t *h, const uint8_t number[LC_SIG_NUMBER_LEN], uint32_t expiry,
                            lcb_random_fn rnd);
lc_sig_sub_t *lcb_hss_by_token(lcb_hss_t *h, const uint8_t token_id[8]);
lc_sig_sub_t *lcb_hss_by_tmid(lcb_hss_t *h, uint32_t tmid); /* activated and bound */
lc_sig_sub_t *lcb_hss_by_number(lcb_hss_t *h, const uint8_t number[LC_SIG_NUMBER_LEN]);
void lcb_hss_unbind(lcb_hss_t *h, uint32_t tmid);
/* The QR contents for sub (key id and pkn from the network record). */
void lcb_hss_qr(const lcb_hss_t *h, const lc_sig_sub_t *sub, lc_sig_qr_t *q);
/* "+883..." from 7 BCD bytes; out needs 16 bytes. */
void lcb_number_text(const uint8_t bcd[LC_SIG_NUMBER_LEN], char out[16]);

#endif
