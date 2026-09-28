/* Glue between the link (lc_term) and signalling (lc_sig_term): DL payloads
 * routed by kind, app data wrapped, lc_sig's link I/O carried by lc_term.
 * Also the scan list's signalling side (channel-list spec §5, §7): CHAN_LIST
 * becomes its network entries, the registered mode its mode, the serving
 * beacon's cfg_ver asks for a new list, and DEACTIVATE clears the network's
 * and learned entries. Used by the firmware (term_app.c) and the host
 * simulation. */
#ifndef LC_TERM_SIG_H
#define LC_TERM_SIG_H

#include "lc_sig_term.h"
#include "lc_term.h"

typedef struct {
    lc_term_t        *term;
    lc_sig_term_t     sig;
    lc_sig_term_io_t  user; /* caller's save/event and ctx */
    void             *ctx;
    void            (*app_down)(void *ctx, const uint8_t *d, uint8_t n); /* app data for the phone */
} lc_term_sig_t;

void     lc_term_sig_init(lc_term_sig_t *g, lc_term_t *term, const lc_sig_term_io_t *user_io, lc_sig_ident_t *id,
                          uint32_t tmid, uint64_t now_us);
void     lc_term_sig_downlink(lc_term_sig_t *g, const uint8_t *p, uint8_t n, uint64_t now_us);
int      lc_term_sig_app_up(lc_term_sig_t *g, const uint8_t *d, uint8_t n);
uint64_t lc_term_sig_step(lc_term_sig_t *g, uint64_t now_us);

#endif
