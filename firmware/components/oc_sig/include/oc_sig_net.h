/* Network role (spec §2, §3.2, §4.3, §5): the cell's half of oc_sig, one
 * session per terminal. Host-only.
 *
 * It holds no subscriber keys (network-core spec §4.3). Activation, vectors
 * and resync are asked of the core through io (act_req, av_req, resync_req)
 * and answered later with oc_sig_net_act_done / oc_sig_net_av_done, or from
 * inside the call by a single-process core. A session learns its number from
 * the vector's answer, and calls to a number registered here are switched
 * here; every other call goes out as OC_SIG_NET_MO. */
#ifndef OC_SIG_NET_H
#define OC_SIG_NET_H

#include "oc_sig_chan.h"
#include "oc_sig_hss.h" /* oc_sig_cell_av_t, oc_sig_av_status_t, oc_sig_hxres */

/* MO: a call to the far end (the caller answers it with oc_sig_net_peer_*). LOCAL: a call to
 * another local subscriber, which the network switches itself (two legs, relayed).
 * ALERTING / ANSWERED: an incoming leg's terminal rings / answers. */
typedef enum {
    OC_SIG_NET_MO = 1, OC_SIG_NET_ANSWERED = 2, OC_SIG_NET_ENDED = 3, OC_SIG_NET_LOCAL = 4, OC_SIG_NET_ALERTING = 5
} oc_sig_net_what_t;

/* oc_sig_net_call_in: why no call was set up */
#define OC_SIG_NET_IN_UNREACHABLE (-1) /* no registered session has that number */
#define OC_SIG_NET_IN_BUSY        (-2) /* it has a call */

typedef struct {
    uint8_t  what;
    uint32_t tmid;
    uint32_t call_id;
    uint8_t  number[OC_SIG_NUMBER_LEN]; /* MO: the number dialled; MT: the caller */
    uint8_t  cause;                     /* ENDED */
    uint32_t peer_tmid;                 /* local call: the other leg's terminal; 0 = the far end */
} oc_sig_net_call_ev_t;

typedef struct {
    void *ctx;
    /* questions for the core: answered with oc_sig_net_act_done / _av_done */
    void (*act_req)(void *ctx, uint32_t tmid, const uint8_t token_id[8], const uint8_t pkt[32], const uint8_t tag[8]);
    void (*av_req)(void *ctx, uint32_t tmid);
    void (*resync_req)(void *ctx, uint32_t tmid, const uint8_t rand[16], const uint8_t auts[14]);
    /* AUTH_RSP matched (the cell sends LOC_UPDATE), and a registration lapsed (LOC_PURGE) */
    void (*registered)(void *ctx, uint32_t tmid, const uint8_t number[OC_SIG_NUMBER_LEN], const uint8_t rand[16],
                       const uint8_t res[8]);
    void (*unregistered)(void *ctx, uint32_t tmid, const uint8_t number[OC_SIG_NUMBER_LEN]);
    int  (*send)(void *ctx, uint32_t tmid, const uint8_t *p, uint8_t n); /* one DL payload: 0 queued */
    void (*channel)(void *ctx, uint32_t tmid, int on);        /* on: page and grant; off: release */
    void (*call)(void *ctx, const oc_sig_net_call_ev_t *ev);
    void (*log)(void *ctx, const char *line);
} oc_sig_net_io_t;

typedef struct {
    uint8_t  mode;     /* oc_sig_mode_t */
    uint16_t period_s; /* re-registration period */
} oc_sig_net_cfg_t;

#ifndef OC_SIG_NET_TERMS
#define OC_SIG_NET_TERMS 32u /* sessions; build-time (CMake OC_SIG_NET_TERMS), at least plan 4's RHU_MAX_TERMS */
#endif
#define OC_SIG_NET_ASK_US 3000000u /* a question to the core stands this long; a later request asks again */

typedef struct {
    int          used;
    uint32_t     tmid;
    oc_sig_chan_t ch;
    oc_sig_msg_t outq[OC_SIG_OUTQ];
    uint8_t      out_count;
    int          granted;
    uint64_t     chan_req_at; /* last channel request (0 = none) */
    uint64_t     last_sig;
    int          act_wait, av_wait;                         /* a question to the core is open... */
    uint64_t     act_at, av_at;                             /* ...since then */
    int          auth_pending;
    uint8_t      rand[16], ck[16], ik[16];                  /* the last confirmed (registered) vector */
    uint8_t      p_rand[16], p_hxres[16], p_ck[16], p_ik[16]; /* pending vector: not believed until AUTH_RSP matches */
    uint8_t      p_number[OC_SIG_NUMBER_LEN];               /* ...and the number it came with */
    int          registered;
    uint8_t      number[OC_SIG_NUMBER_LEN];                 /* registered: the subscriber's number */
    uint64_t     reg_until;
    uint8_t      call;        /* internal call state */
    uint32_t     call_id;
    uint8_t      peer[OC_SIG_NUMBER_LEN];
    uint64_t     call_at;
    uint64_t     heard;
    uint8_t      end_cause;
    uint8_t      k_voice[16];
    uint32_t     d_tx, d_rx_next;
    uint32_t     other;       /* local call: the other leg's terminal (0 = the far end) */
    uint8_t      cl_ver;      /* version of the CHAN_LIST last handed to ch (the one in flight, if any) */
    int          cl_again;    /* a CHAN_LIST expired and was pushed once more: not again until one is taken */
} oc_sig_net_sess_t;

typedef struct {
    oc_sig_net_io_t   io;
    oc_sig_net_cfg_t  cfg;
    oc_sig_net_sess_t s[OC_SIG_NET_TERMS];
    uint32_t          next_call_id;
    int               have_list;   /* oc_sig_net_set_chan_list was called */
    oc_sig_chan_list_t list;
} oc_sig_net_t;

void oc_sig_net_init(oc_sig_net_t *n, const oc_sig_net_io_t *io, const oc_sig_net_cfg_t *cfg);
/* The cell's channel list (channel-list spec §7-8), pushed as CHAN_LIST after
 * every REG_ACK and on a config service request (cause 4). NULL: none (no
 * push; a config request is answered with an empty list, version 0). */
void oc_sig_net_set_chan_list(oc_sig_net_t *n, const oc_sig_chan_list_t *list);
/* The cell's mode changed (OC_SIG_MODE_*). 0, or -1 for an unknown mode. */
int  oc_sig_net_set_mode(oc_sig_net_t *n, uint8_t mode, uint64_t now_us);
void oc_sig_net_rx(oc_sig_net_t *n, uint32_t tmid, const uint8_t *p, uint8_t len, uint64_t now_us);
void oc_sig_net_service_req(oc_sig_net_t *n, uint32_t tmid, uint8_t cause, uint64_t now_us);
void oc_sig_net_link(oc_sig_net_t *n, uint32_t tmid, int granted, uint64_t now_us);
void oc_sig_net_heard(oc_sig_net_t *n, uint32_t tmid, uint64_t now_us);
void oc_sig_net_tick(oc_sig_net_t *n, uint64_t now_us);
/* The core's answer to act_req: msg is the finished ACT_ACK or ACT_NAK. 0, or
 * -1 when tmid has no open activation question (a late or unasked answer).
 * On a fresh activation the core must call oc_sig_net_drop for every TMID in
 * oc_sig_flat_act's drop list (including this TMID itself, if it was already
 * bound) BEFORE calling this: act_done never deregisters anyone on its own.
 * An ACK naming the number tmid is still registered with is an ACT_REQ
 * answered again (OC_SIG_ACT_AGAIN): the session and its call are left as
 * they are. */
int  oc_sig_net_act_done(oc_sig_net_t *n, uint32_t tmid, const oc_sig_msg_t *msg, uint64_t now_us);
/* The core's answer to av_req or resync_req (av and number only for
 * OC_SIG_AV_OK). The vector carries HXRES, never XRES (network-core spec
 * §19.1): AUTH_RSP is taken when SHA-256(RAND || RES)[0..16) matches it, and
 * that RES goes to registered() for LOC_UPDATE. 0, or -1 when tmid has no
 * open vector question. */
int  oc_sig_net_av_done(oc_sig_net_t *n, uint32_t tmid, uint8_t status, const uint8_t number[OC_SIG_NUMBER_LEN],
                        const oc_sig_cell_av_t *av, uint64_t now_us);
/* The core cancelled tmid's registration (LOC_CANCEL): it is no longer
 * registered, and a call it holds is released with cause. 0, or -1 if unknown. */
int  oc_sig_net_drop(oc_sig_net_t *n, uint32_t tmid, uint8_t cause, uint64_t now_us);
int  oc_sig_net_peer_alert(oc_sig_net_t *n, uint32_t call_id, uint64_t now_us);
int  oc_sig_net_peer_answer(oc_sig_net_t *n, uint32_t call_id, uint64_t now_us);
int  oc_sig_net_peer_release(oc_sig_net_t *n, uint32_t call_id, uint8_t cause, uint64_t now_us);
/* An incoming call for callee: 0 (SETUP_IND sent, *call_id set),
 * OC_SIG_NET_IN_UNREACHABLE or OC_SIG_NET_IN_BUSY. */
int  oc_sig_net_call_in(oc_sig_net_t *n, const uint8_t callee[OC_SIG_NUMBER_LEN],
                        const uint8_t caller[OC_SIG_NUMBER_LEN], uint64_t now_us, uint32_t *call_id);
/* App data frames (calls spec §5). Only on an active leg (encrypted with
 * K_voice, or clear when registered in Part 97 on a Part 97 cell), or for a
 * registered Part 97 session with no call (the diagnostic loopback, clear).
 * Anywhere else - a leg still being set up, including after the far end's
 * answer and before the terminal's CONNECT_ACK - both return -1: data_out
 * without touching out or the counter, data_in without handing anything up. */
int  oc_sig_net_data_in(oc_sig_net_t *n, uint32_t tmid, const uint8_t *p, uint8_t len, uint8_t out[OC_SIG_APP_MAX],
                        uint8_t *out_n);
int  oc_sig_net_data_out(oc_sig_net_t *n, uint32_t tmid, const uint8_t *d, uint8_t len,
                         uint8_t out[OC_SIG_LINK_MAX], uint8_t *out_n);
int  oc_sig_net_registered(const oc_sig_net_t *n, uint32_t tmid);
/* The number tmid registered with: 0, or -1 when it isn't registered. */
int  oc_sig_net_number(const oc_sig_net_t *n, uint32_t tmid, uint8_t out[OC_SIG_NUMBER_LEN]);
/* 1 when tmid is in a connected local call; *peer_tmid is where its app data goes
 * (decrypted with one leg's voice key by data_in, re-encrypted by data_out). */
int  oc_sig_net_local_peer(const oc_sig_net_t *n, uint32_t tmid, uint32_t *peer_tmid);

#endif
