/* Glue between the link (oc_term) and signalling (oc_sig_term): DL payloads
 * routed by kind, app data wrapped, oc_sig's link I/O carried by oc_term.
 * Also the scan list's signalling side (channel-list spec §5, §7): CHAN_LIST
 * becomes its network entries, the registered mode its mode, the serving
 * beacon's cfg_ver asks for a new list, and DEACTIVATE clears the network's
 * and learned entries. Used by the firmware (term_app.c) and the host
 * simulation. */
#ifndef OC_TERM_SIG_H
#define OC_TERM_SIG_H

#include "oc_sig_term.h"
#include "oc_term.h"

typedef struct {
    oc_term_t        *term;
    oc_sig_term_t     sig;
    oc_sig_term_io_t  user; /* caller's save/event and ctx */
    void             *ctx;
    void            (*app_down)(void *ctx, const uint8_t *d, uint8_t n); /* app data for the phone */
} oc_term_sig_t;

void     oc_term_sig_init(oc_term_sig_t *g, oc_term_t *term, const oc_sig_term_io_t *user_io, oc_sig_ident_t *id,
                          uint32_t tmid, uint64_t now_us);
void     oc_term_sig_downlink(oc_term_sig_t *g, const uint8_t *p, uint8_t n, uint64_t now_us);
int      oc_term_sig_app_up(oc_term_sig_t *g, const uint8_t *d, uint8_t n);
uint64_t oc_term_sig_step(oc_term_sig_t *g, uint64_t now_us);

#endif
