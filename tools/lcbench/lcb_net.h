/* lcb_net — the network stand-in (spec §7): lc_sig's network role on top of
 * lcb_cell, subscribers in an lcb_hss, and a simulated far end that rings
 * for 3 s and answers every outgoing call to a number that isn't a local
 * subscriber, can place one incoming call, can hang up a connected call after
 * a set time, and echoes app data frames. A call to a local subscriber is
 * switched by lc_sig_net to that terminal; lcb_net forwards its app data. I/O-free like lcb_cell: lcbench net (and the host
 * simulation) move the radio messages and call lcb_net_tick. */
#ifndef LCB_NET_H
#define LCB_NET_H

#include <stdint.h>

#include "lc_sig_net.h"
#include "lcb_cell.h"
#include "lcb_hss.h"

#define LCB_NET_PEER_NUMBER "+8836065550100" /* the simulated far end */
#define LCB_NET_RING_US     3000000u

typedef struct {
    lcb_cell_t   *cell;
    lcb_hss_t    *hss;
    const char   *hss_path;          /* NULL: never written (tests) */
    lcb_random_fn random;
    uint64_t    (*now_us)(void);     /* the caller's clock, for hook callbacks */
    void        (*log)(const char *line);
    lc_sig_net_t  net;
    uint32_t      ring_call;         /* MO call the peer answers at answer_at */
    uint64_t      answer_at;
    uint8_t       call_in_to[LC_SIG_NUMBER_LEN];
    uint64_t      call_in_at;        /* 0: no incoming call planned */
    uint32_t      echoed;            /* app data frames echoed */
    uint32_t      forwarded;         /* app data frames forwarded between two local terminals */
    uint32_t      peer_hangup_us;    /* 0: the peer never hangs up; else it releases a call this long after connect */
    uint32_t      talk_call;         /* connected call the peer will release at hangup_at */
    uint64_t      hangup_at;
} lcb_net_t;

/* Sets the cell's hooks. The HSS must have its network record (lcb_hss_ensure_network). */
void lcb_net_init(lcb_net_t *n, lcb_cell_t *cell, lcb_hss_t *hss, const char *hss_path, lcb_random_fn rnd,
                  uint64_t (*now_us)(void), void (*log)(const char *line));
/* Place a call from the peer to callee at at_us (once). */
void lcb_net_call_in(lcb_net_t *n, const uint8_t callee[LC_SIG_NUMBER_LEN], uint64_t at_us);
/* Call at least every 100 ms: link state for every terminal, lc_sig timers, the peer. */
void lcb_net_tick(lcb_net_t *n, uint64_t now_us);

#endif
