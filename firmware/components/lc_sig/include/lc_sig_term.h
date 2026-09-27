/* Terminal role: runs on the W12 (spec §2): holds the subscriber identity,
 * activates, registers with MILENAGE, controls calls, and wraps app data
 * frames. Link I/O and BLE go through lc_sig_term_io_t. */
#ifndef LC_SIG_TERM_H
#define LC_SIG_TERM_H

#include "lc_sig_chan.h"
#include "lc_sig_qr.h"

typedef struct {
    int      activated;
    uint16_t key_id;
    uint8_t  sk[32], pk[32]; /* the terminal's X25519 key pair */
    uint8_t  k[16], opc[16];
    uint8_t  sqn[6];         /* highest SQN accepted */
    uint8_t  number[LC_SIG_NUMBER_LEN];
} lc_sig_ident_t;

#define LC_SIG_IDENT_BLOB 113u

size_t lc_sig_ident_pack(const lc_sig_ident_t *id, uint8_t out[LC_SIG_IDENT_BLOB]);
int    lc_sig_ident_unpack(const uint8_t *in, size_t len, lc_sig_ident_t *id);
/* Not activated, with a key pair from 32 random bytes. */
int    lc_sig_ident_new(lc_sig_ident_t *id, const uint8_t random32[32]);

/* A scanned QR with its activation keys, derived ahead of ACT_ACK. */
typedef struct {
    lc_sig_qr_t qr;
    uint8_t     k[16], opc[16];
} lc_sig_act_prep_t;

typedef struct {
    void *ctx;
    int  (*send)(void *ctx, const uint8_t *p, uint8_t n); /* one UL link payload: 0 taken, -1 not now */
    int  (*service_req)(void *ctx, uint8_t cause);        /* RACH UPPER { 0x30 | cause }: 0 taken */
    void (*save)(void *ctx, const lc_sig_ident_t *id);    /* persist (may be deferred) */
    void (*event)(void *ctx, const uint8_t *ev, uint8_t n);
} lc_sig_term_io_t;

typedef struct {
    lc_sig_term_io_t io;
    lc_sig_ident_t  *id;
    uint32_t         tmid;
    uint8_t          state;
    lc_sig_chan_t    ch;
    int              attached, granted;
    uint64_t         grant_lost_at; /* in a call: when the grant went away (0 = granted) */
    uint64_t         svc_due;
    lc_sig_msg_t     outq[LC_SIG_OUTQ];
    uint8_t          out_count;
    lc_sig_qr_t      qr;
    uint8_t          act_k[16], act_opc[16]; /* keys for the pending activation */
    int              act_sent;
    int              reg_sent;
    uint64_t         reg_retry_at;
    uint64_t         proc_deadline; /* supervision: give up (ACT_REQ/REG_REQ) if no answer by this time */
    uint32_t         backoff_s;
    uint64_t         rereg_at;
    uint8_t          reg_mode;      /* mode from the last REG_ACK (0: none) */
    uint8_t          ck[16], ik[16], rand[16];
    uint8_t          call_ref;
    uint32_t         call_id;
    int              answered, hangup_pending;
    uint8_t          end_cause;
    uint64_t         call_timer_at;
    uint8_t          k_voice[16];
    uint32_t         d_tx, d_rx_next;
} lc_sig_term_t;

void    lc_sig_term_init(lc_sig_term_t *t, const lc_sig_term_io_t *io, lc_sig_ident_t *id, uint32_t tmid,
                         uint64_t now_us);
int     lc_sig_term_command(lc_sig_term_t *t, const uint8_t *cmd, size_t len, uint64_t now_us);
/* ACTIVATE in two halves. X25519 takes ~150 ms on the ESP32-S3, so the
 * firmware prepares (QR parse, K/OPc) outside the link lock, then activates
 * under it. lc_sig_term_command(ACTIVATE) does both. */
int     lc_sig_term_act_prepare(const lc_sig_ident_t *id, uint32_t tmid, const uint8_t *text, size_t len,
                                lc_sig_act_prep_t *p);
int     lc_sig_term_activate(lc_sig_term_t *t, const lc_sig_act_prep_t *p, uint64_t now_us);
void    lc_sig_term_link(lc_sig_term_t *t, int attached, int granted, uint64_t now_us);
/* The serving cell's beacon mode (LC_SIG_MODE_*). A registered terminal whose
 * REG_ACK said otherwise registers again (spec §4.3); in a call, after it. */
void    lc_sig_term_cell_mode(lc_sig_term_t *t, uint8_t mode, uint64_t now_us);
void    lc_sig_term_rx(lc_sig_term_t *t, const uint8_t *p, uint8_t n, uint64_t now_us);
void    lc_sig_term_tick(lc_sig_term_t *t, uint64_t now_us);
uint8_t lc_sig_term_state(const lc_sig_term_t *t);
int     lc_sig_term_data_out(lc_sig_term_t *t, const uint8_t *d, uint8_t n, uint8_t out[LC_SIG_LINK_MAX],
                             uint8_t *out_n);
int     lc_sig_term_data_in(lc_sig_term_t *t, const uint8_t *p, uint8_t n, uint8_t out[LC_SIG_APP_MAX],
                            uint8_t *out_n);

#endif
