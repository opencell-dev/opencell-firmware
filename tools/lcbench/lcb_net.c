#include "lcb_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void say(lcb_net_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(lcb_net_t *n, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n->log != NULL) n->log(line);
}

static lc_sig_sub_t *io_by_token(void *c, const uint8_t t[8]) { return lcb_hss_by_token(((lcb_net_t *)c)->hss, t); }
static lc_sig_sub_t *io_by_tmid(void *c, uint32_t tmid) { return lcb_hss_by_tmid(((lcb_net_t *)c)->hss, tmid); }
static lc_sig_sub_t *io_by_number(void *c, const uint8_t num[LC_SIG_NUMBER_LEN])
{
    return lcb_hss_by_number(((lcb_net_t *)c)->hss, num);
}
static void io_unbind(void *c, uint32_t tmid) { lcb_hss_unbind(((lcb_net_t *)c)->hss, tmid); }

static void io_save(void *c)
{
    lcb_net_t *n = c;
    if (n->hss_path != NULL && lcb_hss_save(n->hss, n->hss_path) != 0) say(n, "HSS save FAILED: %s", n->hss_path);
}

static int io_send(void *c, uint32_t tmid, const uint8_t *p, uint8_t len)
{
    return lcb_cell_dl_push(((lcb_net_t *)c)->cell, tmid, p, len);
}

static void io_channel(void *c, uint32_t tmid, int on)
{
    lcb_net_t *n = c;
    if (on && !lcb_cell_granted(n->cell, tmid)) {
        lcb_cell_page(n->cell, tmid);
    } else if (!on) {
        lcb_cell_release(n->cell, tmid);
    }
}

static void talk_started(lcb_net_t *n, uint32_t call_id, uint64_t now)
{
    if (n->peer_hangup_us != 0) {
        n->talk_call = call_id;
        n->hangup_at = now + n->peer_hangup_us;
    }
}

static void io_call(void *c, const lc_sig_net_call_ev_t *e)
{
    lcb_net_t *n = c;
    char num[16];
    lcb_number_text(e->number, num);
    if (e->what == LC_SIG_NET_MO) {
        say(n, "call %u: %08x dials %s; peer rings, answers in 3 s", e->call_id, e->tmid, num);
        lc_sig_net_peer_alert(&n->net, e->call_id, n->now_us());
        n->ring_call = e->call_id;
        n->answer_at = n->now_us() + LCB_NET_RING_US;
    } else if (e->what == LC_SIG_NET_LOCAL) {
        say(n, "call %u: %08x calls %s (terminal %08x)", e->call_id, e->tmid, num, e->peer_tmid);
    } else if (e->what == LC_SIG_NET_ANSWERED) {
        say(n, "call %u: %08x answered", e->call_id, e->tmid);
        if (e->peer_tmid == 0) talk_started(n, e->call_id, n->now_us()); /* only the far end hangs up by itself */
    } else if (e->what == LC_SIG_NET_ENDED) {
        say(n, "call %u: %08x ended, cause %u (%u frames echoed, %u forwarded)", e->call_id, e->tmid, e->cause,
            n->echoed, n->forwarded);
        if (n->ring_call == e->call_id) n->ring_call = 0;
        if (n->talk_call == e->call_id) n->talk_call = 0;
    }
}

static void io_random(void *c, uint8_t *out, size_t len) { ((lcb_net_t *)c)->random(out, len); }
static uint32_t io_unix(void *c) { (void)c; return (uint32_t)time(NULL); }
static void io_log(void *c, const char *line) { say((lcb_net_t *)c, "%s", line); }

static void on_ul(void *c, uint32_t tmid, const uint8_t *p, uint8_t len)
{
    lcb_net_t *n = c;
    uint64_t now = n->now_us();
    lc_sig_net_heard(&n->net, tmid, now);
    if (len > 0 && (p[0] & 0xF0u) == LC_SIG_KIND_SIG) {
        lc_sig_net_rx(&n->net, tmid, p, len, now);
    } else if (len > 0 && p[0] == LC_SIG_KIND_DATA) { /* to the other terminal of a local call, else echoed */
        uint8_t d[LC_SIG_APP_MAX], dn, out[LC_SIG_LINK_MAX], on;
        uint32_t to = tmid;
        int local = lc_sig_net_local_peer(&n->net, tmid, &to);
        if (lc_sig_net_data_in(&n->net, tmid, p, len, d, &dn) == 0 &&
            lc_sig_net_data_out(&n->net, to, d, dn, out, &on) == 0 && lcb_cell_dl_push(n->cell, to, out, on) == 0) {
            if (local) n->forwarded++;
            else n->echoed++;
        }
    }
}

static void on_upper(void *c, uint32_t tmid, const uint8_t *p, uint8_t len)
{
    lcb_net_t *n = c;
    if (len == 1 && (p[0] & 0xF0u) == LC_SIG_KIND_SVC) {
        say(n, "%08x: service request %u", tmid, p[0] & 0x0Fu);
        lc_sig_net_service_req(&n->net, tmid, p[0] & 0x0Fu, n->now_us());
    }
}

void lcb_net_init(lcb_net_t *n, lcb_cell_t *cell, lcb_hss_t *hss, const char *hss_path, lcb_random_fn rnd,
                  uint64_t (*now_us)(void), void (*log)(const char *line))
{
    memset(n, 0, sizeof(*n));
    n->cell = cell;
    n->hss = hss;
    n->hss_path = hss_path;
    n->random = rnd;
    n->now_us = now_us;
    n->log = log;
    const lc_sig_net_io_t io = { n, io_by_token, io_by_tmid, io_by_number, io_unbind, io_save, io_send,
                                 io_channel, io_call, io_random, io_unix, io_log };
    lc_sig_net_cfg_t cfg = { hss->key_id, { 0 }, hss->mode, hss->period_s };
    memcpy(cfg.sk, hss->sk, 32);
    lc_sig_net_init(&n->net, &io, &cfg);
    memset(cfg.sk, 0, 32);
    const lcb_cell_hooks_t h = { n, on_ul, on_upper };
    lcb_cell_set_hooks(cell, &h);
    cell->part97 = hss->mode == LC_SIG_MODE_PART97; /* the beacon announces the mode */
}

void lcb_net_call_in(lcb_net_t *n, const uint8_t callee[LC_SIG_NUMBER_LEN], uint64_t at_us)
{
    memcpy(n->call_in_to, callee, LC_SIG_NUMBER_LEN);
    n->call_in_at = at_us;
}

void lcb_net_tick(lcb_net_t *n, uint64_t now_us)
{
    for (unsigned k = 0; k < LCB_CELL_MAX_TERMS; k++) {
        if (n->cell->terms[k].used) {
            lc_sig_net_link(&n->net, n->cell->terms[k].tmid, lcb_cell_granted(n->cell, n->cell->terms[k].tmid), now_us);
        }
    }
    lc_sig_net_tick(&n->net, now_us);
    if (n->ring_call != 0 && now_us >= n->answer_at) {
        if (lc_sig_net_peer_answer(&n->net, n->ring_call, now_us) == 0) {
            say(n, "call %u: peer answered", n->ring_call);
            talk_started(n, n->ring_call, now_us);
        }
        n->ring_call = 0;
    }
    if (n->talk_call != 0 && now_us >= n->hangup_at) {
        say(n, "call %u: peer hangs up", n->talk_call);
        lc_sig_net_peer_release(&n->net, n->talk_call, LC_SIG_CAUSE_NORMAL, now_us);
        n->talk_call = 0;
    }
    if (n->call_in_at != 0 && now_us >= n->call_in_at) {
        uint8_t peer[LC_SIG_NUMBER_LEN];
        uint32_t id = 0;
        char num[16];
        n->call_in_at = 0;
        lc_sig_number_to_bcd(LCB_NET_PEER_NUMBER, strlen(LCB_NET_PEER_NUMBER), peer);
        lcb_number_text(n->call_in_to, num);
        int err = lc_sig_net_call_in(&n->net, n->call_in_to, peer, now_us, &id);
        say(n, "peer calls %s: %s (call %u)", num, err == 0 ? "setting up" : "refused", id);
    }
}
