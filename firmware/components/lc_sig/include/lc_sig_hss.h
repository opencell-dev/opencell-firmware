/* lc_sig_hss: the home side of activation and authentication (spec §3.2,
 * §4.3; network-core spec §4.2-4.3) as pure functions, for whoever holds the
 * keys: lc_core, and the single-process stand-ins (lcbench's HSS, the host
 * tests' fake core) through the flat-array helpers at the end. lc_sig_net
 * itself never calls them: it asks its core. Host-only, no OS calls. */
#ifndef LC_SIG_HSS_H
#define LC_SIG_HSS_H

#include "lc_sig_msg.h"

/* One authentication vector (TS 33.102 §6.3.2, AMF 8000). */
typedef struct {
    uint8_t rand[16], autn[16], xres[8], ck[16], ik[16];
} lc_sig_av_t;

/* AV answer status: lc_sig_net_av_done and the AV_RES status byte
 * (network-core spec §4.3, §6). */
typedef enum {
    LC_SIG_AV_OK = 0,
    LC_SIG_AV_NOT_ACTIVATED = 1,  /* no subscriber bound to the TMID */
    LC_SIG_AV_BOUND_ELSEWHERE = 2, /* reserved: the TMID's subscriber is bound to another terminal */
    LC_SIG_AV_DISABLED = 3,
    LC_SIG_AV_UNAVAILABLE = 4,    /* no answer possible now (store, core link): the terminal retries */
    LC_SIG_AV_AUTH_FAILED = 5     /* resync refused: AUTS did not verify */
} lc_sig_av_status_t;

/* A vector for sequence number sqn and the given RAND. 0 or -1. */
int lc_sig_av_make(const uint8_t k[16], const uint8_t opc[16], const uint8_t sqn[6], const uint8_t rand[16],
                   lc_sig_av_t *av);

/* The AUTS of AUTH_FAIL cause 2, answering a challenge with rand: 0 with the
 * terminal's SQN in sqn_ms, or -1 when MAC-S does not verify. */
int lc_sig_av_auts(const uint8_t k[16], const uint8_t opc[16], const uint8_t rand[16], const uint8_t auts[14],
                   uint8_t sqn_ms[6]);

/* An activation token as its holder sees it. */
typedef struct {
    int      known;         /* 0: no such token */
    int      used;
    uint32_t expiry;        /* unix s */
    uint8_t  secret[16];
    uint32_t bound_tmid;    /* used: the terminal its subscriber is bound to now (0 = none) */
    uint8_t  bound_k[16];   /* ...and that binding's K */
} lc_sig_act_token_t;

typedef enum { LC_SIG_ACT_REFUSED = 0, LC_SIG_ACT_FRESH = 1, LC_SIG_ACT_AGAIN = 2 } lc_sig_act_result_t;

/* Answer an ACT_REQ from tmid (spec §3.2 step 3). *out is the finished
 * ACT_ACK or ACT_NAK (an unknown token gets a zero tag). Returns
 *   LC_SIG_ACT_FRESH: bind the token's subscriber to tmid with k and opc, SQN 0;
 *   LC_SIG_ACT_AGAIN: the terminal already bound by this used token, with the
 *     same key pair, asking again (its ACT_ACK was lost): nothing changes;
 *   LC_SIG_ACT_REFUSED: an ACT_NAK (reasons 1-4); k and opc are zeroed. */
int lc_sig_act_answer(const lc_sig_act_token_t *tok, const uint8_t sk[32], uint32_t unix_now, uint32_t tmid,
                      const uint8_t token_id[8], const uint8_t pkt[32], const uint8_t tag[8],
                      const uint8_t number[LC_SIG_NUMBER_LEN], lc_sig_msg_t *out, uint8_t k[16], uint8_t opc[16]);

/* ---- a single-process HSS over a flat array of records ---- */

/* One subscriber of lcbench's text-file HSS or of a test's fake core. */
typedef struct {
    uint8_t  number[LC_SIG_NUMBER_LEN];
    uint8_t  token_id[8], token_secret[16];
    uint32_t token_expiry; /* unix seconds */
    int      token_used;
    uint32_t tmid;         /* bound terminal; 0 = none */
    int      activated;
    uint8_t  k[16], opc[16], sqn[6];
} lc_sig_sub_t;

/* ACT_REQ over subs[0..n). On LC_SIG_ACT_FRESH the records changed (save
 * them) and drop[0..*ndrop) are the terminals whose sessions must be
 * dropped, as the core's LOC_CANCEL(reactivated) does: every terminal this
 * binding replaces (the subscriber's old one, and tmid itself if it was bound
 * before). */
int lc_sig_flat_act(lc_sig_sub_t *subs, unsigned n, const uint8_t sk[32], uint32_t unix_now, uint32_t tmid,
                    const uint8_t token_id[8], const uint8_t pkt[32], const uint8_t tag[8], lc_sig_msg_t *out,
                    uint32_t drop[2], unsigned *ndrop);

/* A vector for the subscriber bound to tmid, with SQN + 1 (records changed
 * when LC_SIG_AV_OK: save them before the vector is used). */
uint8_t lc_sig_flat_av(lc_sig_sub_t *subs, unsigned n, uint32_t tmid, const uint8_t rand[16],
                       uint8_t number[LC_SIG_NUMBER_LEN], lc_sig_av_t *av);

/* AUTH_FAIL cause 2: SQN from AUTS (the challenge was rand), then a vector
 * with fresh_rand as lc_sig_flat_av. */
uint8_t lc_sig_flat_resync(lc_sig_sub_t *subs, unsigned n, uint32_t tmid, const uint8_t rand[16],
                           const uint8_t auts[14], const uint8_t fresh_rand[16], uint8_t number[LC_SIG_NUMBER_LEN],
                           lc_sig_av_t *av);

#endif
