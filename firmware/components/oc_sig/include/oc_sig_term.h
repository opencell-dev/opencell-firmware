/* Terminal role: runs on the W12 (spec §2): holds the subscriber identity,
 * activates, registers with MILENAGE, controls calls, and wraps app data
 * frames. Link I/O and BLE go through oc_sig_term_io_t. */
#ifndef OC_SIG_TERM_H
#define OC_SIG_TERM_H

#include "oc_sig_chan.h"
#include "oc_sig_qr.h"

typedef struct {
    int      activated;
    uint16_t key_id;
    uint8_t  sk[32], pk[32]; /* the terminal's X25519 key pair */
    uint8_t  k[16], opc[16];
    uint8_t  sqn[6];         /* highest SQN accepted */
    uint8_t  number[OC_SIG_NUMBER_LEN];
} oc_sig_ident_t;

/* Identity blob v2 (numbering-v2 spec §6.2): 0 version 2 | 1 activated |
 * 2 key id (LE) | 4 sk | 36 pk | 68 K | 84 OPc | 100 SQN (6) | 106 number (8). */
#define OC_SIG_IDENT_BLOB    114u
#define OC_SIG_IDENT_BLOB_V1 113u /* numbering v1: version 1, 7-byte number */
#define OC_SIG_IDENT_OLD     (-2)
#define OC_SIG_DIAL_MAX      24u  /* DIAL argument bytes: any dialled form (BLE contract v3) */

size_t oc_sig_ident_pack(const oc_sig_ident_t *id, uint8_t out[OC_SIG_IDENT_BLOB]);
/* 0; OC_SIG_IDENT_OLD for a v1 blob (113 bytes, version 1: the terminal must
 * be activated again); -1 for anything else. *id is untouched unless 0. */
int    oc_sig_ident_unpack(const uint8_t *in, size_t len, oc_sig_ident_t *id);
/* Not activated, with a key pair from 32 random bytes. */
int    oc_sig_ident_new(oc_sig_ident_t *id, const uint8_t random32[32]);

/* A scanned QR with its activation keys, derived ahead of ACT_ACK. */
typedef struct {
    oc_sig_qr_t qr;
    uint8_t     k[16], opc[16];
} oc_sig_act_prep_t;

typedef struct {
    void *ctx;
    int  (*send)(void *ctx, const uint8_t *p, uint8_t n); /* one UL link payload: 0 taken, -1 not now */
    int  (*service_req)(void *ctx, uint8_t cause);        /* RACH UPPER { 0x30 | cause }: 0 taken */
    void (*save)(void *ctx, const oc_sig_ident_t *id);    /* persist (may be deferred) */
    void (*event)(void *ctx, const uint8_t *ev, uint8_t n);
} oc_sig_term_io_t;

typedef struct {
    oc_sig_term_io_t io;
    oc_sig_ident_t  *id;
    uint32_t         tmid;
    uint8_t          state;
    oc_sig_chan_t    ch;
    int              attached, granted;
    uint64_t         grant_lost_at; /* in a call: when the grant went away (0 = granted) */
    uint64_t         svc_due;
    oc_sig_msg_t     outq[OC_SIG_OUTQ];
    uint8_t          out_count;
    oc_sig_qr_t      qr;
    uint8_t          act_k[16], act_opc[16]; /* keys for the pending activation */
    int              act_sent;
    int              reg_sent;
    int              auth_sent;     /* this attempt's AUTH_RSP went out: only then may REG_ACK end it */
    uint64_t         reg_retry_at;
    uint64_t         proc_deadline; /* supervision: give up (ACT_REQ/REG_REQ) if no answer by this time */
    uint32_t         backoff_s;
    uint64_t         rereg_at;
    uint8_t          reg_mode;      /* mode from the last REG_ACK (0: none) */
    uint8_t          cell_mode;     /* the serving cell's beacon mode, last oc_sig_term_cell_mode (0: not heard) */
    uint8_t          ck[16], ik[16], rand[16];
    uint8_t          call_ref;
    uint32_t         call_id;
    int              answered, hangup_pending;
    uint8_t          end_cause;
    uint64_t         call_timer_at;
    uint8_t          k_voice[16];
    int              k_voice_ok;    /* k_voice is this call's (call_up derived it); 0 outside a call */
    uint32_t         d_tx, d_rx_next;
    /* channel list (channel-list spec §7) */
    uint8_t          list_ver;      /* version of the network entries held (the caller sets it at boot) */
    oc_sig_chan_list_t list_in;     /* the last CHAN_LIST, until oc_sig_term_chan_list takes it */
    int              list_new;
    uint64_t         cfg_retry_at;  /* no config service request before this */
    uint8_t          cfg_asked_ver; /* cfg_ver of the outstanding/most recent config service request */
    int              cfg_asked;     /* a request for cfg_asked_ver was sent */
    int              cfg_answered;  /* ...and a CHAN_LIST arrived answering it: don't ask again for it (M1) */
    uint8_t          cfg_answered_list_ver; /* list_ver held when cfg_answered was set: M1 only while unchanged
                                              * (review fix: a list handed back at a DIFFERENT version, e.g. after
                                              * a restart, must reopen the same cfg_ver's question) */
    uint8_t          cfg_rereg_ver;    /* cfg_ver that already triggered cell_cfg's one re-registration */
    int              cfg_reregistered; /* ...so a still-unanswered cfg_rereg_ver falls back to plain asks instead
                                         * of re-registering again (review fix) */
    uint32_t         cfg_backoff_s;    /* backoff for those fallback asks (0: next is 30 s) */
} oc_sig_term_t;

void    oc_sig_term_init(oc_sig_term_t *t, const oc_sig_term_io_t *io, oc_sig_ident_t *id, uint32_t tmid,
                         uint64_t now_us);
int     oc_sig_term_command(oc_sig_term_t *t, const uint8_t *cmd, size_t len, uint64_t now_us);
/* ACTIVATE in two halves. X25519 takes ~150 ms on the ESP32-S3, so the
 * firmware prepares (QR parse, K/OPc) outside the link lock, then activates
 * under it. oc_sig_term_command(ACTIVATE) does both. */
int     oc_sig_term_act_prepare(const oc_sig_ident_t *id, uint32_t tmid, const uint8_t *text, size_t len,
                                oc_sig_act_prep_t *p);
int     oc_sig_term_activate(oc_sig_term_t *t, const oc_sig_act_prep_t *p, uint64_t now_us);
void    oc_sig_term_link(oc_sig_term_t *t, int attached, int granted, uint64_t now_us);
/* The serving cell's beacon mode (OC_SIG_MODE_*). A registered terminal whose
 * REG_ACK said otherwise registers again (spec §4.3); in a call, after it. */
void    oc_sig_term_cell_mode(oc_sig_term_t *t, uint8_t mode, uint64_t now_us);
/* The serving cell's beacon cfg_ver (channel-list spec §7). Registered,
 * attached and not granted, with cfg_ver != list_ver mod 4: sends a service
 * request with cause OC_SIG_SVC_CONFIG, at most every 30 s; the network
 * answers with a grant and CHAN_LIST. An ask still unanswered at its own
 * retry time re-registers once per cfg_ver (a REG_ACK is followed by the
 * network's own CHAN_LIST push, so the list still arrives that way); if
 * still unanswered after that, falls back to plain asks with backoff
 * (30 s, 60, 120 ... capped at 600 s) instead of re-registering again. */
void    oc_sig_term_cell_cfg(oc_sig_term_t *t, uint8_t cfg_ver, uint64_t now_us);
/* 1 once per CHAN_LIST received (already acknowledged): *out is its body. */
int     oc_sig_term_chan_list(oc_sig_term_t *t, oc_sig_chan_list_t *out);
void    oc_sig_term_rx(oc_sig_term_t *t, const uint8_t *p, uint8_t n, uint64_t now_us);
void    oc_sig_term_tick(oc_sig_term_t *t, uint64_t now_us);
uint8_t oc_sig_term_state(const oc_sig_term_t *t);
/* The media gate: 1 app data goes encrypted, 0 in the clear, -1 not at all
 * (see oc_sig_term.c). oc_term_sig drops queued app data frames on -1. */
int     oc_sig_term_media(const oc_sig_term_t *t);
/* App data frames (calls spec §5). Only in a connected call (encrypted with
 * K_voice, or clear when registered in Part 97). Decision #25 (2026-10-01):
 * there is no out-of-call path - the Part 97 diagnostic loopback is gone.
 * Anywhere outside a connected call data_out returns OC_SIG_ATT_NOT_NOW
 * without touching out or d_tx, and data_in returns -1. */
int     oc_sig_term_data_out(oc_sig_term_t *t, const uint8_t *d, uint8_t n, uint8_t out[OC_SIG_LINK_MAX],
                             uint8_t *out_n);
int     oc_sig_term_data_in(oc_sig_term_t *t, const uint8_t *p, uint8_t n, uint8_t out[OC_SIG_APP_MAX],
                            uint8_t *out_n);

#endif
