#include "oc_term_sig.h"

#include <string.h>

static int io_send(void *ctx, const uint8_t *p, uint8_t n)
{
    oc_term_sig_t *g = ctx;
    /* Only on a granted channel: oc_term_send_upper would send a short
     * fragment (<= 8 B) as RACH UPPER, which carries only the service request. */
    if (g->term->state != OC_TERM_GRANTED) return -1;
    return oc_term_send_upper(g->term, p, n) == 0 ? 0 : -1;
}

static int io_svc(void *ctx, uint8_t cause)
{
    oc_term_sig_t *g = ctx;
    uint8_t b = (uint8_t)(OC_SIG_KIND_SVC | (cause & 0x0Fu));
    return oc_term_send_upper(g->term, &b, 1) == 0 ? 0 : -1;
}

static void io_save(void *ctx, const oc_sig_ident_t *id)
{
    oc_term_sig_t *g = ctx;
    if (g->user.save != NULL) g->user.save(g->user.ctx, id);
}

static void io_event(void *ctx, const uint8_t *ev, uint8_t n)
{
    oc_term_sig_t *g = ctx;
    if (n >= 1 && ev[0] == OC_SIG_EV_DEACTIVATED) {
        oc_term_scan_deactivate(&g->term->scan); /* channel-list spec §15 Q5: the user's entries stay */
    }
    if (g->user.event != NULL) g->user.event(g->user.ctx, ev, n);
}

_Static_assert(OC_SIG_CHAN_MAX <= OC_SCAN_MAX_NET, "a CHAN_LIST must fit the scan list's network entries");
_Static_assert(OC_SIG_MODE_PART15 == OC_PHY_MODE_PART15 && OC_SIG_MODE_PART97 == OC_PHY_MODE_PART97,
               "REG_ACK's mode goes to the scan list as it is");

/* oc_sig_term's news for the scan list. The registered mode first (spec
 * §5.1): it decides which entries are active, and a FIXED last-serving entry
 * recorded before the first REG_ACK stays inactive in Part 15 until it
 * arrives. Then a CHAN_LIST oc_sig_term took (and acknowledged) becomes the
 * network entries (an unchanged re-push, after every registration, leaves the
 * list and its NVS copy alone: oc_term_scan_set_net). */
static void scan_sync(oc_term_sig_t *g)
{
    if (g->sig.reg_mode != 0) {
        oc_term_scan_set_mode(&g->term->scan, g->sig.reg_mode);
    }
    oc_sig_chan_list_t l;
    if (!oc_sig_term_chan_list(&g->sig, &l)) return;
    uint8_t count = l.count < OC_SIG_CHAN_MAX ? l.count : (uint8_t)OC_SIG_CHAN_MAX;
    oc_scan_ent_t e[OC_SIG_CHAN_MAX];
    for (uint8_t i = 0; i < count; i++) {
        e[i].freq_hz = l.freq_hz[i];
        e[i].flags = (l.flags[i] & OC_SIG_CHAN_FIXED) ? OC_SCAN_F_FIXED : 0u;
    }
    oc_term_scan_set_net(&g->term->scan, l.ver, count, e);
}

void oc_term_sig_init(oc_term_sig_t *g, oc_term_t *term, const oc_sig_term_io_t *user_io, oc_sig_ident_t *id,
                      uint32_t tmid, uint64_t now_us)
{
    memset(g, 0, sizeof(*g));
    g->term = term;
    g->user = *user_io;
    const oc_sig_term_io_t io = { g, io_send, io_svc, io_save, io_event };
    oc_sig_term_init(&g->sig, &io, id, tmid, now_us);
    g->sig.list_ver = term->scan.net_ver; /* load the scan list (term_scan_load) before this */
}

/* Media-gate review M1: app data frames still queued when the gate shuts
 * (the call ended or is ending, or the cell's mode no longer matches the
 * registration) don't go out after it. */
static void media_sync(oc_term_sig_t *g)
{
    if (oc_sig_term_media(&g->sig) < 0) oc_term_drop_upper(g->term, OC_SIG_KIND_DATA);
}

void oc_term_sig_downlink(oc_term_sig_t *g, const uint8_t *p, uint8_t n, uint64_t now_us)
{
    if (n == 0) return;
    if ((p[0] & 0xF0u) == OC_SIG_KIND_SIG) {
        oc_sig_term_rx(&g->sig, p, n, now_us);
        scan_sync(g); /* a REG_ACK's mode, a CHAN_LIST */
        media_sync(g);       /* a RELEASE */
    } else if (p[0] == OC_SIG_KIND_DATA) {
        uint8_t d[OC_SIG_APP_MAX], dn;
        if (oc_sig_term_data_in(&g->sig, p, n, d, &dn) == 0 && g->app_down != NULL) g->app_down(g->ctx, d, dn);
    }
}

int oc_term_sig_app_up(oc_term_sig_t *g, const uint8_t *d, uint8_t n)
{
    /* Only while GRANTED: an app frame must never go out as RACH UPPER (that
     * carries only the 1-byte service request), and a refusal here must not
     * touch d_tx (see below). */
    if (g->term->state != OC_TERM_GRANTED) {
        return OC_SIG_ATT_NOT_NOW;
    }
    uint8_t p[OC_SIG_LINK_MAX], pn;
    uint32_t d_tx_before = g->sig.d_tx;
    int err = oc_sig_term_data_out(&g->sig, d, n, p, &pn);
    if (err != 0) return err;
    if (oc_term_send_upper(g->term, p, pn) == 0) {
        return 0;
    }
    /* oc_sig_term_data_out already advanced d_tx; a failed send must not
     * consume it, or repeated refusals desynchronise the network's receiver. */
    g->sig.d_tx = d_tx_before;
    return OC_SIG_ATT_NOT_NOW;
}

uint64_t oc_term_sig_step(oc_term_sig_t *g, uint64_t now_us)
{
    int attached = g->term->state == OC_TERM_IDLE || g->term->state == OC_TERM_GRANTED;
    int granted = g->term->state == OC_TERM_GRANTED;
    oc_sig_term_link(&g->sig, attached, granted, now_us);
    if (attached) {
        uint8_t mode = (g->term->beacon.flags & OC_BCN_FLAG_PART97) ? OC_SIG_MODE_PART97 : OC_SIG_MODE_PART15;
        oc_sig_term_cell_mode(&g->sig, mode, now_us);
        oc_sig_term_cell_cfg(&g->sig, g->term->beacon.cfg_ver, now_us);
    }
    oc_sig_term_tick(&g->sig, now_us);
    scan_sync(g);
    media_sync(g); /* a hang-up, a new beacon mode, a timer */
    return now_us + 100000u;
}
