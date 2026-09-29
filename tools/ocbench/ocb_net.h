/* ocb_net — the network stand-in (spec §7): oc_sig's network role on top of
 * ocb_cell, subscribers in an ocb_hss, and a simulated far end that rings
 * for 3 s and answers every outgoing call to a number that isn't a local
 * subscriber, can place one incoming call, can hang up a connected call after
 * a set time, and echoes app data frames. A call to a local subscriber is
 * switched by oc_sig_net to that terminal; ocb_net forwards its app data. I/O-free like ocb_cell: ocbench net (and the host
 * simulation) move the radio messages and call ocb_net_tick. */
#ifndef OCB_NET_H
#define OCB_NET_H

#include <stdint.h>

#include "oc_sig_net.h"
#include "ocb_cell.h"
#include "ocb_hss.h"

#define OCB_NET_PEER_NUMBER "+883160655500100" /* the simulated far end: the echo service, 00100 */
#define OCB_NET_RING_US     3000000u

typedef struct {
    ocb_cell_t   *cell;
    ocb_hss_t    *hss;
    const char   *hss_path;          /* NULL: never written (tests) */
    ocb_random_fn random;
    uint64_t    (*now_us)(void);     /* the caller's clock, for hook callbacks */
    void        (*log)(const char *line);
    oc_sig_net_t  net;
    uint32_t      ring_call;         /* MO call the peer answers at answer_at */
    uint64_t      answer_at;
    uint8_t       call_in_to[OC_SIG_NUMBER_LEN];
    uint64_t      call_in_at;        /* 0: no incoming call planned */
    uint32_t      echoed;            /* app data frames echoed */
    uint32_t      forwarded;         /* app data frames forwarded between two local terminals */
    uint32_t      peer_hangup_us;    /* 0: the peer never hangs up; else it releases a call this long after connect */
    uint32_t      talk_call;         /* connected call the peer will release at hangup_at */
    uint64_t      hangup_at;
} ocb_net_t;

/* Sets the cell's hooks. The HSS must have its network record (ocb_hss_ensure_network). */
void ocb_net_init(ocb_net_t *n, ocb_cell_t *cell, ocb_hss_t *hss, const char *hss_path, ocb_random_fn rnd,
                  uint64_t (*now_us)(void), void (*log)(const char *line));
/* The cell's channel list (channel-list spec §8): oc_sig_net pushes it as
 * CHAN_LIST, and the beacon's cfg_ver becomes list->ver mod 4. Call after
 * ocb_net_init (which resets the network role and its list). Call again
 * with a new ver to make registered terminals ask for it. */
void ocb_net_set_chan_list(ocb_net_t *n, const oc_sig_chan_list_t *list);
/* "903.25,922.25:fixed" (MHz, 915 grid channels, at most 12, no spaces; ""
 * or "none" for none) into out with version ver. 0, or -1 (err names the problem). */
int  ocb_net_parse_chan_list(const char *text, uint8_t ver, oc_sig_chan_list_t *out, char *err, size_t err_cap);
/* The default list: the cell's own anchor only (fixed if the cell is). */
void ocb_net_own_chan_list(const ocb_cell_t *cell, uint8_t ver, oc_sig_chan_list_t *out);

/* Place a call from the peer to callee at at_us (once). */
void ocb_net_call_in(ocb_net_t *n, const uint8_t callee[OC_SIG_NUMBER_LEN], uint64_t at_us);
/* Call at least every 100 ms: link state for every terminal, oc_sig timers, the peer. */
void ocb_net_tick(ocb_net_t *n, uint64_t now_us);

#endif
