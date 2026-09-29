/* A synchronous fake core for lc_sig_net tests (network-core spec §4.3): an
 * HSS over the test's flat subscriber array (lc_sig_flat_*), answering
 * lc_sig_net's questions from inside the call, as a single-process core may.
 * With FC.hold set, a question waits for fc_answer() instead (an answer that
 * comes later); with FC.down set, nothing is answered (no core link).
 * A fresh activation drops the sessions it replaces, as the core's
 * LOC_CANCEL(reactivated) makes a cell do. */
#ifndef SIG_FAKE_CORE_H
#define SIG_FAKE_CORE_H

#include <string.h>

#include "lc_sig_keys.h" /* lc_sig_wipe */
#include "lc_sig_net.h"

typedef struct {
    lc_sig_net_t *net;
    lc_sig_sub_t *subs;
    int          *nsubs;
    uint8_t       sk[32];
    uint32_t      unix_s;
    uint64_t    (*now)(void);
    uint32_t      rng;
    int           saves;              /* how often the records changed (an HSS save) */
    int           acts, avs, resyncs; /* questions asked */
    int           hold, down;
    int           held;               /* the question held: 0 none, 1 act, 2 av, 3 resync */
    uint32_t      h_tmid;
    uint8_t       h_token[8], h_pkt[32], h_tag[8], h_rand[16], h_auts[14];
} fc_t;

static fc_t FC;

static inline void fc_init(lc_sig_net_t *net, lc_sig_sub_t *subs, int *nsubs, const uint8_t sk[32],
                           uint32_t unix_s, uint64_t (*now)(void))
{
    memset(&FC, 0, sizeof(FC));
    FC.net = net;
    FC.subs = subs;
    FC.nsubs = nsubs;
    memcpy(FC.sk, sk, 32);
    FC.unix_s = unix_s;
    FC.now = now;
    FC.rng = 99;
}

static inline void fc_rand(uint8_t out[16])
{
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)((FC.rng = FC.rng * 1103515245u + 12345u) >> 16);
}

static inline void fc_do_act(uint32_t tmid, const uint8_t token[8], const uint8_t pkt[32], const uint8_t tag[8])
{
    lc_sig_msg_t out;
    uint32_t drop[2];
    unsigned nd = 0;
    int r = lc_sig_flat_act(FC.subs, (unsigned)*FC.nsubs, FC.sk, FC.unix_s, tmid, token, pkt, tag, &out, drop, &nd);
    if (r == LC_SIG_ACT_FRESH) FC.saves++;
    for (unsigned i = 0; i < nd; i++) lc_sig_net_drop(FC.net, drop[i], LC_SIG_CAUSE_NET_FAILURE, FC.now());
    lc_sig_net_act_done(FC.net, tmid, &out, FC.now());
}

static inline void fc_do_av(uint32_t tmid, const uint8_t *rand, const uint8_t *auts)
{
    uint8_t fresh[16], number[LC_SIG_NUMBER_LEN];
    lc_sig_cell_av_t av;
    memset(number, 0, sizeof(number));
    memset(&av, 0, sizeof(av));
    fc_rand(fresh);
    uint8_t st = auts == NULL ? lc_sig_flat_av(FC.subs, (unsigned)*FC.nsubs, tmid, fresh, number, &av)
                              : lc_sig_flat_resync(FC.subs, (unsigned)*FC.nsubs, tmid, rand, auts, fresh, number, &av);
    if (st == LC_SIG_AV_OK) FC.saves++;
    lc_sig_net_av_done(FC.net, tmid, st, number, &av, FC.now());
    lc_sig_wipe(&av, sizeof(av)); /* CK/IK/HXRES: no need to keep them on the stack */
}

static inline void fc_act_req(void *c, uint32_t tmid, const uint8_t token[8], const uint8_t pkt[32],
                              const uint8_t tag[8])
{
    (void)c;
    FC.acts++;
    if (FC.down) return;
    if (FC.hold) {
        FC.held = 1;
        FC.h_tmid = tmid;
        memcpy(FC.h_token, token, 8);
        memcpy(FC.h_pkt, pkt, 32);
        memcpy(FC.h_tag, tag, 8);
        return;
    }
    fc_do_act(tmid, token, pkt, tag);
}

static inline void fc_av_req(void *c, uint32_t tmid)
{
    (void)c;
    FC.avs++;
    if (FC.down) return;
    if (FC.hold) {
        FC.held = 2;
        FC.h_tmid = tmid;
        return;
    }
    fc_do_av(tmid, NULL, NULL);
}

static inline void fc_resync_req(void *c, uint32_t tmid, const uint8_t rand[16], const uint8_t auts[14])
{
    (void)c;
    FC.resyncs++;
    if (FC.down) return;
    if (FC.hold) {
        FC.held = 3;
        FC.h_tmid = tmid;
        memcpy(FC.h_rand, rand, 16);
        memcpy(FC.h_auts, auts, 14);
        return;
    }
    fc_do_av(tmid, rand, auts);
}

/* Answer the held question now. */
static inline void fc_answer(void)
{
    int h = FC.held;
    FC.held = 0;
    if (h == 1) fc_do_act(FC.h_tmid, FC.h_token, FC.h_pkt, FC.h_tag);
    if (h == 2) fc_do_av(FC.h_tmid, NULL, NULL);
    if (h == 3) fc_do_av(FC.h_tmid, FC.h_rand, FC.h_auts);
}

#endif
