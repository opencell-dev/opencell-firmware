#include "ocb_net.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "oc_sig_keys.h" /* oc_sig_wipe */

static void say(ocb_net_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(ocb_net_t *n, const char *fmt, ...)
{
    char line[256]; /* room for a channel list line: 12 x " 927.75:fixed" */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n->log != NULL) n->log(line);
}

static void save(ocb_net_t *n)
{
    if (n->hss_path != NULL && ocb_hss_save(n->hss, n->hss_path) != 0) say(n, "HSS save FAILED: %s", n->hss_path);
}

/* ocbench is its own single-process core (network-core spec §4.3): the HSS
 * answers oc_sig_net's questions at once, and is saved before any answer
 * leaves (so a vector's SQN is on disk before the terminal can use it). */
static void io_act_req(void *c, uint32_t tmid, const uint8_t token_id[8], const uint8_t pkt[32], const uint8_t tag[8])
{
    ocb_net_t *n = c;
    oc_sig_msg_t out;
    uint32_t drop[2];
    unsigned nd = 0;
    uint64_t now = n->now_us();
    if (oc_sig_flat_act(n->hss->subs, n->hss->n, n->hss->sk, (uint32_t)time(NULL), tmid, token_id, pkt, tag, &out,
                        drop, &nd) == OC_SIG_ACT_FRESH) {
        save(n);
    }
    for (unsigned i = 0; i < nd; i++) oc_sig_net_drop(&n->net, drop[i], OC_SIG_CAUSE_NET_FAILURE, now);
    oc_sig_net_act_done(&n->net, tmid, &out, now);
}

static void answer_av(ocb_net_t *n, uint32_t tmid, const uint8_t *rand, const uint8_t *auts)
{
    uint8_t fresh[16], number[OC_SIG_NUMBER_LEN];
    oc_sig_cell_av_t av;
    memset(number, 0, sizeof(number));
    memset(&av, 0, sizeof(av));
    n->random(fresh, 16);
    uint8_t st = auts == NULL ? oc_sig_flat_av(n->hss->subs, n->hss->n, tmid, fresh, number, &av)
                              : oc_sig_flat_resync(n->hss->subs, n->hss->n, tmid, rand, auts, fresh, number, &av);
    if (st == OC_SIG_AV_OK) save(n);
    if (auts != NULL) say(n, "%08x: %s", tmid, st == OC_SIG_AV_OK ? "SQN resynchronized" : "resync refused");
    oc_sig_net_av_done(&n->net, tmid, st, number, &av, n->now_us());
    oc_sig_wipe(&av, sizeof(av)); /* CK/IK/HXRES: no need to keep them on the stack */
}

static void io_av_req(void *c, uint32_t tmid) { answer_av(c, tmid, NULL, NULL); }
static void io_resync_req(void *c, uint32_t tmid, const uint8_t rand[16], const uint8_t auts[14])
{
    answer_av(c, tmid, rand, auts);
}

static int io_send(void *c, uint32_t tmid, const uint8_t *p, uint8_t len)
{
    return ocb_cell_dl_push(((ocb_net_t *)c)->cell, tmid, p, len);
}

static void io_channel(void *c, uint32_t tmid, int on)
{
    ocb_net_t *n = c;
    if (on && !ocb_cell_granted(n->cell, tmid)) {
        ocb_cell_page(n->cell, tmid);
    } else if (!on) {
        ocb_cell_release(n->cell, tmid);
    }
}

static void talk_started(ocb_net_t *n, uint32_t call_id, uint64_t now)
{
    if (n->peer_hangup_us != 0) {
        n->talk_call = call_id;
        n->hangup_at = now + n->peer_hangup_us;
    }
}

/* For people: "+883-1-606-555-01234" (the digits as they are if not valid). */
static const char *show(const uint8_t number[OC_SIG_NUMBER_LEN], char out[OC_SIG_NUMBER_SHOW])
{
    if (oc_sig_number_format(number, out, OC_SIG_NUMBER_SHOW) == 0) oc_sig_number_to_text(number, out);
    return out;
}

static void io_call(void *c, const oc_sig_net_call_ev_t *e)
{
    ocb_net_t *n = c;
    char num[OC_SIG_NUMBER_SHOW];
    show(e->number, num);
    if (e->what == OC_SIG_NET_MO && ocb_hss_by_number(n->hss, e->number) != NULL) {
        /* a subscriber of this HSS that isn't registered here: not the far end */
        say(n, "call %u: %08x dials %s: not registered", e->call_id, e->tmid, num);
        oc_sig_net_peer_release(&n->net, e->call_id, OC_SIG_CAUSE_UNREACHABLE, n->now_us());
    } else if (e->what == OC_SIG_NET_MO) {
        say(n, "call %u: %08x dials %s; peer rings, answers in 3 s", e->call_id, e->tmid, num);
        oc_sig_net_peer_alert(&n->net, e->call_id, n->now_us());
        n->ring_call = e->call_id;
        n->answer_at = n->now_us() + OCB_NET_RING_US;
    } else if (e->what == OC_SIG_NET_LOCAL) {
        say(n, "call %u: %08x calls %s (terminal %08x)", e->call_id, e->tmid, num, e->peer_tmid);
    } else if (e->what == OC_SIG_NET_ANSWERED) {
        say(n, "call %u: %08x answered", e->call_id, e->tmid);
        if (e->peer_tmid == 0) talk_started(n, e->call_id, n->now_us()); /* only the far end hangs up by itself */
    } else if (e->what == OC_SIG_NET_ENDED) {
        say(n, "call %u: %08x ended, cause %u (%u frames echoed, %u forwarded)", e->call_id, e->tmid, e->cause,
            n->echoed, n->forwarded);
        if (n->ring_call == e->call_id) n->ring_call = 0;
        if (n->talk_call == e->call_id) n->talk_call = 0;
    }
}

static void io_log(void *c, const char *line) { say((ocb_net_t *)c, "%s", line); }

static void on_ul(void *c, uint32_t tmid, const uint8_t *p, uint8_t len)
{
    ocb_net_t *n = c;
    uint64_t now = n->now_us();
    oc_sig_net_heard(&n->net, tmid, now);
    if (len > 0 && (p[0] & 0xF0u) == OC_SIG_KIND_SIG) {
        oc_sig_net_rx(&n->net, tmid, p, len, now);
    } else if (len > 0 && p[0] == OC_SIG_KIND_DATA) { /* to the other terminal of a local call, else echoed */
        uint8_t d[OC_SIG_APP_MAX], dn, out[OC_SIG_LINK_MAX], on;
        uint32_t to = tmid;
        int local = oc_sig_net_local_peer(&n->net, tmid, &to);
        if (oc_sig_net_data_in(&n->net, tmid, p, len, d, &dn) == 0 &&
            oc_sig_net_data_out(&n->net, to, d, dn, out, &on) == 0 && ocb_cell_dl_push(n->cell, to, out, on) == 0) {
            if (local) n->forwarded++;
            else n->echoed++;
        }
    }
}

static void on_upper(void *c, uint32_t tmid, const uint8_t *p, uint8_t len)
{
    ocb_net_t *n = c;
    if (len == 1 && (p[0] & 0xF0u) == OC_SIG_KIND_SVC) {
        say(n, "%08x: service request %u", tmid, p[0] & 0x0Fu);
        oc_sig_net_service_req(&n->net, tmid, p[0] & 0x0Fu, n->now_us());
    }
}

void ocb_net_init(ocb_net_t *n, ocb_cell_t *cell, ocb_hss_t *hss, const char *hss_path, ocb_random_fn rnd,
                  uint64_t (*now_us)(void), void (*log)(const char *line))
{
    memset(n, 0, sizeof(*n));
    n->cell = cell;
    n->hss = hss;
    n->hss_path = hss_path;
    n->random = rnd;
    n->now_us = now_us;
    n->log = log;
    const oc_sig_net_io_t io = { n, io_act_req, io_av_req, io_resync_req, NULL, NULL, io_send,
                                 io_channel, io_call, io_log };
    const oc_sig_net_cfg_t cfg = { hss->mode, hss->period_s };
    oc_sig_net_init(&n->net, &io, &cfg);
    const ocb_cell_hooks_t h = { n, on_ul, on_upper };
    ocb_cell_set_hooks(cell, &h);
    cell->part97 = hss->mode == OC_SIG_MODE_PART97; /* the beacon announces the mode */
}

void ocb_net_call_in(ocb_net_t *n, const uint8_t callee[OC_SIG_NUMBER_LEN], uint64_t at_us)
{
    memcpy(n->call_in_to, callee, OC_SIG_NUMBER_LEN);
    n->call_in_at = at_us;
}

void ocb_net_tick(ocb_net_t *n, uint64_t now_us)
{
    for (unsigned k = 0; k < OCB_CELL_MAX_TERMS; k++) {
        if (n->cell->terms[k].used) {
            oc_sig_net_link(&n->net, n->cell->terms[k].tmid, ocb_cell_granted(n->cell, n->cell->terms[k].tmid), now_us);
        }
    }
    oc_sig_net_tick(&n->net, now_us);
    if (n->ring_call != 0 && now_us >= n->answer_at) {
        if (oc_sig_net_peer_answer(&n->net, n->ring_call, now_us) == 0) {
            say(n, "call %u: peer answered", n->ring_call);
            talk_started(n, n->ring_call, now_us);
        }
        n->ring_call = 0;
    }
    if (n->talk_call != 0 && now_us >= n->hangup_at) {
        say(n, "call %u: peer hangs up", n->talk_call);
        oc_sig_net_peer_release(&n->net, n->talk_call, OC_SIG_CAUSE_NORMAL, now_us);
        n->talk_call = 0;
    }
    if (n->call_in_at != 0 && now_us >= n->call_in_at) {
        uint8_t peer[OC_SIG_NUMBER_LEN];
        uint32_t id = 0;
        char num[OC_SIG_NUMBER_SHOW];
        n->call_in_at = 0;
        oc_sig_number_to_bcd(OCB_NET_PEER_NUMBER, strlen(OCB_NET_PEER_NUMBER), peer);
        show(n->call_in_to, num);
        int err = oc_sig_net_call_in(&n->net, n->call_in_to, peer, now_us, &id);
        say(n, "peer calls %s: %s (call %u)", num, err == 0 ? "setting up" : "refused", id);
    }
}

void ocb_net_set_chan_list(ocb_net_t *n, const oc_sig_chan_list_t *list)
{
    oc_sig_net_set_chan_list(&n->net, list);
    const oc_sig_chan_list_t *l = &n->net.list; /* as stored: at most OC_SIG_CHAN_MAX entries */
    n->cell->cfg_ver = (uint8_t)(l->ver & OC_BCN_MAX_CFG_VER);
    char line[256];
    int k = snprintf(line, sizeof(line), "channel list v%u:", l->ver);
    for (uint8_t i = 0; i < l->count && k > 0 && (size_t)k < sizeof(line); i++) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " %u.%02u%s", (unsigned)(l->freq_hz[i] / 1000000u),
                      (unsigned)(l->freq_hz[i] % 1000000u / 10000u),
                      (l->flags[i] & OC_SIG_CHAN_FIXED) ? ":fixed" : "");
    }
    say(n, "%s%s", line, l->count == 0 ? " (empty)" : "");
}

/* "917.25" -> 917250000, exactly (no floating point): at most 3 decimals. */
static int parse_mhz(const char *s, size_t len, uint32_t *hz)
{
    uint32_t whole = 0, frac = 0, scale = 1000000u;
    size_t i = 0;
    if (len == 0) return -1;
    for (; i < len && s[i] != '.'; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        whole = whole * 10u + (uint32_t)(s[i] - '0');
        if (whole > 999u) return -1; /* no band above 999 MHz here; keeps whole * 1000000 in range */
    }
    if (i < len) { /* the '.' */
        if (++i == len || len - i > 3) return -1;
        for (; i < len; i++) {
            if (s[i] < '0' || s[i] > '9') return -1;
            scale /= 10u;
            frac += (uint32_t)(s[i] - '0') * scale;
        }
    }
    *hz = whole * 1000000u + frac;
    return 0;
}

int ocb_net_parse_chan_list(const char *text, uint8_t ver, oc_sig_chan_list_t *out, char *err, size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    out->ver = ver;
    if (text[0] == '\0' || strcmp(text, "none") == 0) return 0;
    for (const char *c = text; *c != '\0'; c++) {
        if (isspace((unsigned char)*c)) {
            snprintf(err, err_cap, "'%s': no spaces allowed (e.g. 917.25,922.25:fixed)", text);
            return -1;
        }
    }
    const char *p = text;
    for (;;) {
        const char *end = strchr(p, ',');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        size_t num = len;
        uint8_t flags = 0;
        const char *colon = memchr(p, ':', len);
        if (colon != NULL) {
            num = (size_t)(colon - p);
            if (len - num != 6 || strncmp(colon, ":fixed", 6) != 0) {
                snprintf(err, err_cap, "'%.*s': only ':fixed' may follow a frequency", (int)len, p);
                return -1;
            }
            flags = OC_SIG_CHAN_FIXED;
        }
        uint32_t hz;
        if (parse_mhz(p, num, &hz) != 0 || oc_channel_of_freq(OC_BAND_915, hz) == OC_INVALID_CHANNEL) {
            snprintf(err, err_cap, "'%.*s' is not a 915 grid channel (902.25-927.75 MHz, 0.5 MHz steps)", (int)num, p);
            return -1;
        }
        if (out->count == OC_SIG_CHAN_MAX) {
            snprintf(err, err_cap, "more than %u entries", OC_SIG_CHAN_MAX);
            return -1;
        }
        out->freq_hz[out->count] = hz;
        out->flags[out->count] = flags;
        out->count++;
        if (end == NULL) return 0;
        p = end + 1;
    }
}

void ocb_net_own_chan_list(const ocb_cell_t *cell, uint8_t ver, oc_sig_chan_list_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ver = ver;
    out->count = 1;
    out->freq_hz[0] = oc_channel_freq_hz(OC_BAND_915, cell->sync_ch);
    out->flags[0] = cell->fixed_sync ? OC_SIG_CHAN_FIXED : 0u;
}
