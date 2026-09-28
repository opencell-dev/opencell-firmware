#include "lc_term_sig.h"

#include <string.h>

static int io_send(void *ctx, const uint8_t *p, uint8_t n)
{
    lc_term_sig_t *g = ctx;
    /* Only on a granted channel: lc_term_send_upper would send a short
     * fragment (<= 8 B) as RACH UPPER, which carries only the service request. */
    if (g->term->state != LC_TERM_GRANTED) return -1;
    return lc_term_send_upper(g->term, p, n) == 0 ? 0 : -1;
}

static int io_svc(void *ctx, uint8_t cause)
{
    lc_term_sig_t *g = ctx;
    uint8_t b = (uint8_t)(LC_SIG_KIND_SVC | (cause & 0x0Fu));
    return lc_term_send_upper(g->term, &b, 1) == 0 ? 0 : -1;
}

static void io_save(void *ctx, const lc_sig_ident_t *id)
{
    lc_term_sig_t *g = ctx;
    if (g->user.save != NULL) g->user.save(g->user.ctx, id);
}

static void io_event(void *ctx, const uint8_t *ev, uint8_t n)
{
    lc_term_sig_t *g = ctx;
    if (n >= 1 && ev[0] == LC_SIG_EV_DEACTIVATED) {
        lc_term_scan_deactivate(&g->term->scan); /* channel-list spec §15 Q5: the user's entries stay */
    }
    if (g->user.event != NULL) g->user.event(g->user.ctx, ev, n);
}

_Static_assert(LC_SIG_CHAN_MAX <= LC_SCAN_MAX_NET, "a CHAN_LIST must fit the scan list's network entries");
_Static_assert(LC_SIG_MODE_PART15 == LC_PHY_MODE_PART15 && LC_SIG_MODE_PART97 == LC_PHY_MODE_PART97,
               "REG_ACK's mode goes to the scan list as it is");

/* lc_sig_term's news for the scan list. The registered mode first (spec
 * §5.1): it decides which entries are active, and a FIXED last-serving entry
 * recorded before the first REG_ACK stays inactive in Part 15 until it
 * arrives. Then a CHAN_LIST lc_sig_term took (and acknowledged) becomes the
 * network entries (an unchanged re-push, after every registration, leaves the
 * list and its NVS copy alone: lc_term_scan_set_net). */
static void scan_sync(lc_term_sig_t *g)
{
    if (g->sig.reg_mode != 0) {
        lc_term_scan_set_mode(&g->term->scan, g->sig.reg_mode);
    }
    lc_sig_chan_list_t l;
    if (!lc_sig_term_chan_list(&g->sig, &l)) return;
    uint8_t count = l.count < LC_SIG_CHAN_MAX ? l.count : (uint8_t)LC_SIG_CHAN_MAX;
    lc_scan_ent_t e[LC_SIG_CHAN_MAX];
    for (uint8_t i = 0; i < count; i++) {
        e[i].freq_hz = l.freq_hz[i];
        e[i].flags = (l.flags[i] & LC_SIG_CHAN_FIXED) ? LC_SCAN_F_FIXED : 0u;
    }
    lc_term_scan_set_net(&g->term->scan, l.ver, count, e);
}

void lc_term_sig_init(lc_term_sig_t *g, lc_term_t *term, const lc_sig_term_io_t *user_io, lc_sig_ident_t *id,
                      uint32_t tmid, uint64_t now_us)
{
    memset(g, 0, sizeof(*g));
    g->term = term;
    g->user = *user_io;
    const lc_sig_term_io_t io = { g, io_send, io_svc, io_save, io_event };
    lc_sig_term_init(&g->sig, &io, id, tmid, now_us);
    g->sig.list_ver = term->scan.net_ver; /* load the scan list (term_scan_load) before this */
}

void lc_term_sig_downlink(lc_term_sig_t *g, const uint8_t *p, uint8_t n, uint64_t now_us)
{
    if (n == 0) return;
    if ((p[0] & 0xF0u) == LC_SIG_KIND_SIG) {
        lc_sig_term_rx(&g->sig, p, n, now_us);
        scan_sync(g); /* a REG_ACK's mode, a CHAN_LIST */
    } else if (p[0] == LC_SIG_KIND_DATA) {
        uint8_t d[LC_SIG_APP_MAX], dn;
        if (lc_sig_term_data_in(&g->sig, p, n, d, &dn) == 0 && g->app_down != NULL) g->app_down(g->ctx, d, dn);
    }
}

int lc_term_sig_app_up(lc_term_sig_t *g, const uint8_t *d, uint8_t n)
{
    /* Only while GRANTED: an app frame must never go out as RACH UPPER (that
     * carries only the 1-byte service request), and a refusal here must not
     * touch d_tx (see below). */
    if (g->term->state != LC_TERM_GRANTED) {
        return LC_SIG_ATT_NOT_NOW;
    }
    uint8_t p[LC_SIG_LINK_MAX], pn;
    uint32_t d_tx_before = g->sig.d_tx;
    int err = lc_sig_term_data_out(&g->sig, d, n, p, &pn);
    if (err != 0) return err;
    if (lc_term_send_upper(g->term, p, pn) == 0) {
        return 0;
    }
    /* lc_sig_term_data_out already advanced d_tx; a failed send must not
     * consume it, or repeated refusals desynchronise the network's receiver. */
    g->sig.d_tx = d_tx_before;
    return LC_SIG_ATT_NOT_NOW;
}

uint64_t lc_term_sig_step(lc_term_sig_t *g, uint64_t now_us)
{
    int attached = g->term->state == LC_TERM_IDLE || g->term->state == LC_TERM_GRANTED;
    int granted = g->term->state == LC_TERM_GRANTED;
    lc_sig_term_link(&g->sig, attached, granted, now_us);
    if (attached) {
        uint8_t mode = (g->term->beacon.flags & LC_BCN_FLAG_PART97) ? LC_SIG_MODE_PART97 : LC_SIG_MODE_PART15;
        lc_sig_term_cell_mode(&g->sig, mode, now_us);
        lc_sig_term_cell_cfg(&g->sig, g->term->beacon.cfg_ver, now_us);
    }
    lc_sig_term_tick(&g->sig, now_us);
    scan_sync(g);
    return now_us + 100000u;
}
