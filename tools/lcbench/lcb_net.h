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

#define LCB_NET_PEER_NUMBER "+883160655500100" /* the simulated far end: the echo service, 00100 */
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
/* The cell's channel list (channel-list spec §8): lc_sig_net pushes it as
 * CHAN_LIST, and the beacon's cfg_ver becomes list->ver mod 4. Call again
 * with a new ver to make registered terminals ask for it. */
void lcb_net_set_chan_list(lcb_net_t *n, const lc_sig_chan_list_t *list);
/* "903.25,922.25:fixed" (MHz, 915 grid channels, at most 12; "" or "none"
 * for none) into out with version ver. 0, or -1 (err names the problem). */
int  lcb_net_parse_chan_list(const char *text, uint8_t ver, lc_sig_chan_list_t *out, char *err, size_t err_cap);
/* The default list: the cell's own anchor only (fixed if the cell is). */
void lcb_net_own_chan_list(const lcb_cell_t *cell, uint8_t ver, lc_sig_chan_list_t *out);

/* Place a call from the peer to callee at at_us (once). */
void lcb_net_call_in(lcb_net_t *n, const uint8_t callee[LC_SIG_NUMBER_LEN], uint64_t at_us);
/* Call at least every 100 ms: link state for every terminal, lc_sig timers, the peer. */
void lcb_net_tick(lcb_net_t *n, uint64_t now_us);

#endif
