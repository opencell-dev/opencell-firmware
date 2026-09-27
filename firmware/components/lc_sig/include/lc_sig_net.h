/* Network role (spec §2, §3.2, §4.3, §5): the laptop stand-in now, the Pi
 * later. Host-only. Subscribers live in the caller's HSS (callbacks); one
 * session per terminal. */
#ifndef LC_SIG_NET_H
#define LC_SIG_NET_H

#include "lc_sig_chan.h"

typedef struct {
    uint8_t  number[LC_SIG_NUMBER_LEN];
    uint8_t  token_id[8], token_secret[16];
    uint32_t token_expiry; /* unix seconds */
    int      token_used;
    uint32_t tmid;         /* bound terminal; 0 = none */
    int      activated;
    uint8_t  k[16], opc[16], sqn[6];
} lc_sig_sub_t;

typedef enum { LC_SIG_NET_MO = 1, LC_SIG_NET_ANSWERED = 2, LC_SIG_NET_ENDED = 3 } lc_sig_net_what_t;

typedef struct {
    uint8_t  what;
    uint32_t tmid;
    uint32_t call_id;
    uint8_t  number[LC_SIG_NUMBER_LEN]; /* MO: the number dialled; MT: the caller */
    uint8_t  cause;                     /* ENDED */
} lc_sig_net_call_ev_t;

typedef struct {
    void *ctx;
    lc_sig_sub_t *(*by_token)(void *ctx, const uint8_t token_id[8]);
    lc_sig_sub_t *(*by_tmid)(void *ctx, uint32_t tmid);       /* activated and bound to tmid */
    lc_sig_sub_t *(*by_number)(void *ctx, const uint8_t number[LC_SIG_NUMBER_LEN]);
    void (*unbind)(void *ctx, uint32_t tmid);                 /* clear any subscriber bound to tmid */
    void (*save)(void *ctx);
    int  (*send)(void *ctx, uint32_t tmid, const uint8_t *p, uint8_t n); /* one DL payload: 0 queued */
    void (*channel)(void *ctx, uint32_t tmid, int on);        /* on: page and grant; off: release */
    void (*call)(void *ctx, const lc_sig_net_call_ev_t *ev);
    void (*random)(void *ctx, uint8_t *out, size_t n);
    uint32_t (*unix_now)(void *ctx);
    void (*log)(void *ctx, const char *line);
} lc_sig_net_io_t;

typedef struct {
    uint16_t key_id;
    uint8_t  sk[32];   /* network X25519 private key */
    uint8_t  mode;     /* lc_sig_mode_t */
    uint16_t period_s; /* re-registration period */
} lc_sig_net_cfg_t;

#define LC_SIG_NET_TERMS 4u

typedef struct {
    int          used;
    uint32_t     tmid;
    lc_sig_chan_t ch;
    lc_sig_msg_t outq[LC_SIG_OUTQ];
    uint8_t      out_count;
    int          granted;
    uint64_t     chan_req_at; /* last channel request (0 = none) */
    uint64_t     last_sig;
    int          auth_pending;
    uint8_t      rand[16], ck[16], ik[16];                  /* the last confirmed (registered) vector */
    uint8_t      p_rand[16], p_xres[8], p_ck[16], p_ik[16]; /* pending vector: not believed until AUTH_RSP matches */
    int          registered;
    uint64_t     reg_until;
    uint8_t      call;        /* internal call state */
    uint32_t     call_id;
    uint8_t      peer[LC_SIG_NUMBER_LEN];
    uint64_t     call_at;
    uint64_t     heard;
    uint8_t      end_cause;
    uint8_t      k_voice[16];
    uint32_t     d_tx, d_rx_next;
} lc_sig_net_sess_t;

typedef struct {
    lc_sig_net_io_t   io;
    lc_sig_net_cfg_t  cfg;
    lc_sig_net_sess_t s[LC_SIG_NET_TERMS];
    uint32_t          next_call_id;
} lc_sig_net_t;

void lc_sig_net_init(lc_sig_net_t *n, const lc_sig_net_io_t *io, const lc_sig_net_cfg_t *cfg);
void lc_sig_net_rx(lc_sig_net_t *n, uint32_t tmid, const uint8_t *p, uint8_t len, uint64_t now_us);
void lc_sig_net_service_req(lc_sig_net_t *n, uint32_t tmid, uint8_t cause, uint64_t now_us);
void lc_sig_net_link(lc_sig_net_t *n, uint32_t tmid, int granted, uint64_t now_us);
void lc_sig_net_heard(lc_sig_net_t *n, uint32_t tmid, uint64_t now_us);
void lc_sig_net_tick(lc_sig_net_t *n, uint64_t now_us);
int  lc_sig_net_peer_alert(lc_sig_net_t *n, uint32_t call_id, uint64_t now_us);
int  lc_sig_net_peer_answer(lc_sig_net_t *n, uint32_t call_id, uint64_t now_us);
int  lc_sig_net_peer_release(lc_sig_net_t *n, uint32_t call_id, uint8_t cause, uint64_t now_us);
int  lc_sig_net_call_in(lc_sig_net_t *n, const uint8_t callee[LC_SIG_NUMBER_LEN],
                        const uint8_t caller[LC_SIG_NUMBER_LEN], uint64_t now_us, uint32_t *call_id);
int  lc_sig_net_data_in(lc_sig_net_t *n, uint32_t tmid, const uint8_t *p, uint8_t len, uint8_t out[LC_SIG_APP_MAX],
                        uint8_t *out_n);
int  lc_sig_net_data_out(lc_sig_net_t *n, uint32_t tmid, const uint8_t *d, uint8_t len,
                         uint8_t out[LC_SIG_LINK_MAX], uint8_t *out_n);
int  lc_sig_net_registered(const lc_sig_net_t *n, uint32_t tmid);

#endif
