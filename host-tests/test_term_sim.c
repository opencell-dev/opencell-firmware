/* End-to-end: an oc_term terminal against an ocb_cell base station over a
 * simulated air interface.
 *
 * True time T (µs) is the base station's; frame F starts at bs_start(F).
 * The terminal's crystal runs +15 ppm fast with an arbitrary offset. Every
 * slot the cell schedules is stored; a terminal RX "hears" a stored TX with
 * the same frequency and modulation that starts inside its window, a stored
 * RX hears a terminal TX that starts within PREAMBLE_TOL_US of the slot
 * start. The host sends frame F's schedule at the start of frame F-2, as
 * ocbench and plan 4 do; RX_REPORTs arrive REPORT_DELAY_US after the packet. */
#include "unity.h"

#include <string.h>
#include <time.h>

#include "oc_term.h"
#include "ocb_cell.h"
#include "oc_sig_net.h"
#include "oc_term_sig.h"
#include "oc_sig_crypto.h"
#include "ocb_hss.h"
#include "ocb_net.h"
#include "sig_fake_core.h"

void setUp(void) {}
void tearDown(void) {}

#define PPM             15u
#define LOCAL_OFF       7000000u
#define F_BASE          60000u
#define PREAMBLE_TOL_US 30u
#define REPORT_DELAY_US 5000u
#define MAX_SLOTS       256u
#define MAX_REPORTS     32u

typedef struct {
    ocb_cell_t *owner;
    uint64_t  t0;
    uint32_t  len;
    uint32_t  freq;
    oc_mode_t mode;
    uint8_t   dir;
    uint8_t   band;
    uint32_t  frame;
    uint8_t   index;
    uint8_t   plen;
    uint8_t   payload[OC_AIR_MAX_FRAME];
} air_slot_t;

typedef struct {
    ocb_cell_t    *owner;
    uint64_t       due;
    uint8_t        band;
    oc_rx_report_t r;
    uint8_t        data[OC_AIR_MAX_FRAME];
} report_t;

static ocb_cell_t cell;
static ocb_cell_t cell2;     /* a second cell (channel-list tests) */
static int cell2_on;
static oc_term_t term;
static air_slot_t slots[MAX_SLOTS];
static unsigned slot_next;
static report_t reports[MAX_REPORTS];
static int blocked[OC_BAND_COUNT];
static int stuck;            /* radio never raises its IRQ */
static uint64_t now_local;

/* terminal radio state */
static uint32_t r_freq;
static oc_mode_t r_mode;
static int r_tx;
static uint8_t r_buf[OC_AIR_MAX_FRAME];
static uint8_t r_len;
static uint32_t r_timeout;
static int r_active;
static uint64_t r_start;   /* true time the op launched */
static uint64_t r_tx_end;

/* measurements */
static int64_t ul_err_max;
static uint32_t ul_heard;
static uint8_t down[64];
static uint8_t down_len;
static uint32_t downs;
static uint32_t rng = 12345u;

static uint64_t bs_start(uint32_t f) { return 1000000u + (uint64_t)(f - F_BASE) * OC_FRAME_US; }
static uint64_t to_local(uint64_t t) { return LOCAL_OFF + t + t * PPM / 1000000u; }
static uint64_t to_true(uint64_t l) { return (l - LOCAL_OFF) * 1000000u / (1000000u + PPM); }

static int same_mode(const oc_mode_t *a, const oc_mode_t *b) { return memcmp(a, b, sizeof(*a)) == 0; }
static uint8_t band_of(uint32_t hz) { return hz >= 1500000000u ? OC_BAND_2G4 : OC_BAND_915; }

static void store_schedule(ocb_cell_t *owner, uint8_t band, const oc_msg_t *m)
{
    const oc_schedule_t *s = &m->u.schedule;
    for (uint8_t i = 0; i < s->slot_count; i++) {
        air_slot_t *a = &slots[slot_next++ % MAX_SLOTS];
        const oc_slot_t *sl = &s->slots[i];
        a->owner = owner;
        a->t0 = bs_start(s->frame_number) + sl->offset_us;
        a->len = sl->length_us;
        a->freq = sl->freq_hz;
        a->mode = sl->mode;
        a->dir = sl->dir;
        a->band = band;
        a->frame = s->frame_number;
        a->index = i;
        a->plen = sl->payload_len;
        if (sl->payload_len) {
            memcpy(a->payload, sl->payload, sl->payload_len);
        }
    }
}

static void deliver_reports(uint64_t upto_true)
{
    for (unsigned i = 0; i < MAX_REPORTS; i++) {
        if (reports[i].due != 0 && reports[i].due <= upto_true) {
            reports[i].r.payload = reports[i].data;
            ocb_cell_on_rx(reports[i].owner, (oc_band_t)reports[i].band, &reports[i].r);
            reports[i].due = 0;
        }
    }
}

/* ---- fake terminal radio ---- */

/* The LR2021 needs time to reconfigure (bench 2026-09-26): an op must be
 * configured at least this long before its start, or it is late on hardware. */
static uint64_t r_cfg_local;
static uint32_t r_need_us;
static int r_last_band = -1, r_last_mod = -1;
static unsigned lead_violations;

static int f_configure(void *c, uint32_t freq, const oc_mode_t *m)
{
    (void)c;
    int band = freq >= 1500000000u ? 1 : 0;
    r_need_us = (r_last_band >= 0 && band != r_last_band) ? OC_TERM_BAND_SWITCH_US
                : (r_last_mod >= 0 && (int)m->modulation != r_last_mod) ? OC_TERM_MOD_SWITCH_US
                                                                        : OC_TERM_CONFIG_LEAD_US;
    r_last_band = band;
    r_last_mod = (int)m->modulation;
    r_cfg_local = now_local;
    r_freq = freq;
    r_mode = *m;
    return 0;
}

static int f_stage_tx(void *c, const uint8_t *d, uint8_t len)
{
    (void)c;
    memcpy(r_buf, d, len);
    r_len = len;
    r_tx = 1;
    return 0;
}

static int f_stage_rx(void *c, uint32_t timeout)
{
    (void)c;
    r_timeout = timeout;
    r_tx = 0;
    return 0;
}

static int f_launch(void *c, uint64_t at_us)
{
    (void)c;
    if (at_us != 0 && at_us < r_cfg_local + r_need_us) {
        lead_violations++;
    }
    r_active = 1;
    r_start = to_true(at_us > now_local ? at_us : now_local); /* the radio waits for at_us */
    if (!r_tx) {
        return 0;
    }
    uint32_t air = oc_airtime_us(&r_mode, r_len);
    r_tx_end = r_start + air;
    uint8_t band = band_of(r_freq);
    if (blocked[band]) {
        return 0;
    }
    for (unsigned i = 0; i < MAX_SLOTS; i++) {
        air_slot_t *a = &slots[i];
        if (a->dir != OC_DIR_RX || a->freq != r_freq || !same_mode(&a->mode, &r_mode) ||
            r_start + PREAMBLE_TOL_US < a->t0 || r_start > a->t0 + PREAMBLE_TOL_US ||
            r_start + air > a->t0 + a->len) {
            continue;
        }
        int64_t err = (int64_t)r_start - (int64_t)a->t0;
        if (err < 0) err = -err;
        if (err > ul_err_max) ul_err_max = err;
        ul_heard++;
        for (unsigned k = 0; k < MAX_REPORTS; k++) {
            if (reports[k].due == 0) {
                reports[k].owner = a->owner;
                reports[k].due = r_tx_end + REPORT_DELAY_US;
                reports[k].band = a->band;
                reports[k].r = (oc_rx_report_t){ a->frame, a->index, -80, 40, 1, r_len, NULL, OC_RX_END_UNKNOWN };
                memcpy(reports[k].data, r_buf, r_len);
                break;
            }
        }
        break;
    }
    return 0;
}

static int f_poll(void *c, oc_radio_event_t *ev)
{
    (void)c;
    if (!r_active || stuck) {
        return 0;
    }
    uint64_t now = to_true(now_local);
    memset(ev, 0, sizeof(*ev));
    if (r_tx) {
        if (now < r_tx_end) {
            return 0;
        }
        r_active = 0;
        ev->type = OC_RADIO_EV_TX_DONE;
        return 1;
    }
    uint8_t band = band_of(r_freq);
    const air_slot_t *best = NULL;
    for (unsigned i = 0; i < MAX_SLOTS && !blocked[band]; i++) {
        const air_slot_t *a = &slots[i];
        if (a->dir != OC_DIR_TX || a->freq != r_freq || !same_mode(&a->mode, &r_mode) || a->plen == 0) {
            continue;
        }
        uint64_t end = a->t0 + oc_airtime_us(&a->mode, a->plen);
        if (a->t0 + PREAMBLE_TOL_US < r_start || end + oc_rx_done_lag_us(&a->mode) > now ||
            end > r_start + r_timeout) {
            continue;
        }
        if (best == NULL || a->t0 < best->t0) {
            best = a;
        }
    }
    if (best != NULL) {
        r_active = 0;
        ev->type = OC_RADIO_EV_RX_DONE;
        ev->crc_ok = 1;
        ev->len = best->plen;
        ev->rssi_dbm = -80;
        ev->snr_qdb = best->mode.modulation == OC_MOD_FLRC ? 0 : 40;
        memcpy(ev->data, best->payload, best->plen);
        /* As on the W12: RX_DONE comes oc_rx_done_lag_us after the formula end. */
        oc_term_note_irq(&term, to_local(best->t0 + oc_airtime_us(&best->mode, best->plen) +
                                         oc_rx_done_lag_us(&best->mode)));
        return 1;
    }
    if (now >= r_start + r_timeout) {
        r_active = 0;
        ev->type = OC_RADIO_EV_RX_TIMEOUT;
        return 1;
    }
    return 0;
}

static void f_standby(void *c)
{
    (void)c;
    r_active = 0;
}

static const oc_radio_ops_t radio = { NULL, f_configure, f_stage_tx, f_stage_rx, f_launch, f_poll, f_standby, NULL };

/* ---- signalling over the simulated air (oc_term_sig on the terminal, oc_sig_net at the cell) ---- */

static int sig_on;
static oc_term_sig_t glue;
static oc_sig_ident_t sig_id;
static oc_sig_net_t snet;
static oc_sig_sub_t ssub;
static int nssub = 1;
static oc_sig_qr_t sqr;
static uint8_t sig_evs[32];
static int sig_nevs, snet_mo, snet_ended;
static uint32_t snet_call;
static uint8_t snet_mo_number[OC_SIG_NUMBER_LEN]; /* the number the last outgoing call dialled */
static uint8_t app_rx[OC_SIG_APP_MAX], app_rx_n;
static uint64_t sim_now(void);

static int s_send(void *c, uint32_t tmid, const uint8_t *p, uint8_t n) { (void)c; return ocb_cell_dl_push(&cell, tmid, p, n); }
static void s_channel(void *c, uint32_t tmid, int on)
{
    (void)c;
    if (on && !ocb_cell_granted(&cell, tmid)) ocb_cell_page(&cell, tmid);
    if (!on) ocb_cell_release(&cell, tmid);
}
static void s_call(void *c, const oc_sig_net_call_ev_t *e)
{
    (void)c;
    if (e->what == OC_SIG_NET_MO) { snet_mo++; snet_call = e->call_id; memcpy(snet_mo_number, e->number, OC_SIG_NUMBER_LEN); }
    if (e->what == OC_SIG_NET_ENDED) snet_ended++;
}
static const oc_sig_net_io_t snet_io = { NULL, fc_act_req, fc_av_req, fc_resync_req, NULL, NULL, s_send,
                                         s_channel, s_call, NULL };

static void g_event(void *c, const uint8_t *e, uint8_t n) { (void)c; (void)n; sig_evs[sig_nevs++ % 32] = e[0]; }
static void g_app_down(void *c, const uint8_t *d, uint8_t n) { (void)c; memcpy(app_rx, d, n); app_rx_n = n; }
static const oc_sig_term_io_t glue_user_io = { NULL, NULL, NULL, NULL, g_event };

static void c_on_ul(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    oc_sig_net_heard(&snet, tmid, sim_now());
    if (n > 0 && (p[0] & 0xF0u) == OC_SIG_KIND_SIG) {
        oc_sig_net_rx(&snet, tmid, p, n, sim_now());
    } else if (n > 0 && p[0] == OC_SIG_KIND_DATA) { /* echo app data, as ocbench net does */
        uint8_t d[OC_SIG_APP_MAX], dn, out[OC_SIG_LINK_MAX], on;
        if (oc_sig_net_data_in(&snet, tmid, p, n, d, &dn) == 0 && oc_sig_net_data_out(&snet, tmid, d, dn, out, &on) == 0) {
            ocb_cell_dl_push(&cell, tmid, out, on);
        }
    }
}
static void c_on_upper(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    if (n == 1 && (p[0] & 0xF0u) == OC_SIG_KIND_SVC) oc_sig_net_service_req(&snet, tmid, p[0] & 0x0Fu, sim_now());
}

static void sig_start(void)
{
    uint8_t skn[32], r[32];
    memset(skn, 0x11, 32);
    oc_sig_net_cfg_t cfg = { OC_SIG_MODE_PART15, 1800 };
    oc_sig_net_init(&snet, &snet_io, &cfg);
    fc_init(&snet, &ssub, &nssub, skn, 1790000000u, sim_now);
    memset(&ssub, 0, sizeof(ssub));
    oc_sig_number_to_bcd("+883160655501234", 16, ssub.number);
    memset(ssub.token_id, 0xa0, 8);
    memset(ssub.token_secret, 0xb0, 16);
    ssub.token_expiry = 1790003600u;
    memset(&sqr, 0, sizeof(sqr));
    sqr.key_id = 1;
    oc_sig_x25519_public(skn, sqr.pkn);
    memcpy(sqr.token_id, ssub.token_id, 8);
    memcpy(sqr.token_secret, ssub.token_secret, 16);
    memcpy(sqr.number, ssub.number, OC_SIG_NUMBER_LEN);
    memset(r, 0x42, 32);
    oc_sig_ident_new(&sig_id, r);
    oc_term_sig_init(&glue, &term, &glue_user_io, &sig_id, 0x75123456u, now_local);
    glue.app_down = g_app_down;
    ocb_cell_hooks_t h = { NULL, c_on_ul, c_on_upper };
    ocb_cell_set_hooks(&cell, &h);
    sig_nevs = snet_mo = snet_ended = 0;
    app_rx_n = 0;
    sig_on = 1;
}

/* ---- ocb_net (the stand-in ocbench net runs) instead of the bare oc_sig_net ---- */

static ocb_hss_t lhss;
static ocb_net_t lnet;
static int net_lines;
static uint8_t sim_rnd_ctr;

static void sim_rnd(uint8_t *o, size_t n) { for (size_t i = 0; i < n; i++) o[i] = (uint8_t)(sim_rnd_ctr++ * 29u + 3u); }
static char net_dials[96]; /* ocb_net's last "dials" line */
static int net_svc_config;  /* ocb_net logged a service request with cause 4 */
static int net_cl_taken;    /* ocb_net logged a CHAN_LIST_ACK */
static void sim_net_log(const char *line)
{
    net_lines++;
    if (strstr(line, " dials ") != NULL) snprintf(net_dials, sizeof(net_dials), "%s", line);
    if (strstr(line, "service request 4") != NULL) net_svc_config++;
    if (strstr(line, "channel list v") != NULL && strstr(line, " taken by terminal ") != NULL) net_cl_taken++;
}

static void net_start_mode(uint8_t mode)
{
    uint8_t num[OC_SIG_NUMBER_LEN], r[32];
    memset(&lhss, 0, sizeof(lhss));
    TEST_ASSERT_EQUAL_INT(0, ocb_hss_ensure_network(&lhss, sim_rnd));
    lhss.mode = mode; /* ocb_net_init sets the cell's PART97 flag from it */
    oc_sig_number_to_bcd("+883160655501234", 16, num);
    oc_sig_sub_t *s = ocb_hss_issue(&lhss, num, (uint32_t)time(NULL) + 3600u, sim_rnd);
    ocb_hss_qr(&lhss, s, &sqr);
    memset(r, 0x42, 32);
    oc_sig_ident_new(&sig_id, r);
    oc_term_sig_init(&glue, &term, &glue_user_io, &sig_id, 0x75123456u, now_local);
    glue.app_down = g_app_down;
    ocb_net_init(&lnet, &cell, &lhss, NULL, sim_rnd, sim_now, sim_net_log);
    sig_nevs = 0;
    app_rx_n = 0;
    net_lines = 0;
    net_svc_config = 0;
    net_cl_taken = 0;
    sig_on = 2;
}

static void net_start(void) { net_start_mode(OC_SIG_MODE_PART15); }

static void on_down(void *c, const uint8_t *d, uint8_t len)
{
    (void)c;
    if (sig_on) {
        oc_term_sig_downlink(&glue, d, len, now_local);
        return;
    }
    memcpy(down, d, len);
    down_len = len;
    downs++;
}

static uint32_t rand32(void *c)
{
    (void)c;
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static const oc_term_sink_t sink = { NULL, on_down, NULL, rand32 };

/* ---- simulation driver ---- */

static uint32_t build_frame; /* next frame whose schedule the host sends */
static uint64_t next_step_local;

static void sim_start(uint32_t seed, oc_tier_t tier, oc_band_t dl, oc_band_t ul)
{
    sig_on = 0;
    memset(slots, 0, sizeof(slots));
    memset(reports, 0, sizeof(reports));
    memset(blocked, 0, sizeof(blocked));
    stuck = 0;
    slot_next = 0;
    r_active = 0;
    ul_err_max = 0;
    lead_violations = 0;
    r_last_band = r_last_mod = -1;
    ul_heard = 0;
    downs = 0;
    down_len = 0;
    cell2_on = 0;
    ocb_cell_init(&cell, seed, tier, dl, ul);
    oc_term_init(&term, &radio, &sink, 0x75123456u);
    build_frame = F_BASE + 2u;
    now_local = to_local(bs_start(F_BASE) + 37000u); /* terminal powers up mid-frame */
    next_step_local = now_local;
}

/* Run until true time `until`. */
static void sim_run_until(uint64_t until)
{
    for (;;) {
        uint64_t t_build = bs_start(build_frame - 2u) + 1000u;
        uint64_t t_step = to_true(next_step_local);
        if (t_build > until && t_step > until) {
            break;
        }
        if (t_build <= t_step) {
            deliver_reports(t_build);
            static oc_msg_t m;
            if (ocb_cell_schedule(&cell, OC_BAND_915, build_frame, &m) == 0) {
                store_schedule(&cell, OC_BAND_915, &m);
            }
            if (ocb_cell_schedule(&cell, OC_BAND_2G4, build_frame, &m) == 0) {
                store_schedule(&cell, OC_BAND_2G4, &m);
            }
            if (cell2_on && ocb_cell_schedule(&cell2, OC_BAND_915, build_frame, &m) == 0) {
                store_schedule(&cell2, OC_BAND_915, &m);
            }
            build_frame++;
            if (sig_on == 2) {
                ocb_net_tick(&lnet, t_build);
            } else if (sig_on) {
                oc_sig_net_link(&snet, 0x75123456u, ocb_cell_granted(&cell, 0x75123456u), t_build);
                oc_sig_net_tick(&snet, t_build);
            }
        } else {
            now_local = next_step_local;
            deliver_reports(t_step);
            uint64_t next = oc_term_step(&term, now_local);
            if (sig_on) {
                uint64_t sn = oc_term_sig_step(&glue, now_local);
                if (sn < next) next = sn;
            }
            next_step_local = next > now_local ? next : now_local + 1u;
        }
    }
}

static uint64_t sim_now(void) { return to_true(now_local); }

static void run_for(uint32_t ms) { sim_run_until(sim_now() + (uint64_t)ms * 1000u); }

/* The terminal task doesn't run for ms (the cell keeps going). */
static void stall_for(uint32_t ms)
{
    uint64_t until = sim_now() + (uint64_t)ms * 1000u;
    uint64_t saved = next_step_local;
    next_step_local = to_local(until + 10u * OC_FRAME_US); /* keep the loop from stepping */
    sim_run_until(until);
    now_local = to_local(until);
    next_step_local = saved < now_local ? now_local : saved;
}

/* ---- tests ---- */

static void test_attach_and_granted_edge_915(void)
{
    sim_start(0x12345678u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT32(1, cell.attaches);
    TEST_ASSERT_TRUE(cell.terms[0].ul_rx > 30);
    TEST_ASSERT_EQUAL_UINT32(0, term.bad_grants);
    TEST_ASSERT_TRUE(cell.grants_sent >= 2); /* the grant went out twice */
    TEST_ASSERT_FALSE(term.have_next);        /* ...and was applied once */
    /* Terminal UL lands within 20 µs of the base station's slot start. */
    TEST_ASSERT_TRUE(ul_err_max <= 20);
    TEST_ASSERT_EQUAL_UINT(0, lead_violations);
    TEST_ASSERT_EQUAL_UINT8(OC_BAND_915, term.grant.dl.band);
    TEST_ASSERT_EQUAL_UINT8(OC_TIER_EDGE, term.grant.dl.tier);
}

static void test_loopback_through_granted_slots(void)
{
    sim_start(0x0BADCAFEu, OC_TIER_MID, OC_BAND_915, OC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    const uint8_t msg[] = "hello, cell";
    TEST_ASSERT_EQUAL_INT(0, oc_term_send_upper(&term, msg, sizeof(msg)));
    run_for(1000);
    TEST_ASSERT_EQUAL_UINT32(1, downs);
    TEST_ASSERT_EQUAL_UINT8(sizeof(msg), down_len);
    TEST_ASSERT_EQUAL_MEMORY(msg, down, sizeof(msg));
}

static void test_cross_band_duplex_near(void)
{
    sim_start(0x00C0FFEEu, OC_TIER_NEAR, OC_BAND_915, OC_BAND_2G4);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT8(OC_BAND_915, term.grant.dl.band);
    TEST_ASSERT_EQUAL_UINT8(OC_BAND_2G4, term.grant.ul.band);
    uint32_t before = cell.terms[0].ul_rx;
    const uint8_t msg[] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL_INT(0, oc_term_send_upper(&term, msg, sizeof(msg)));
    run_for(1200); /* 10 frames */
    TEST_ASSERT_UINT32_WITHIN(1, 10, cell.terms[0].ul_rx - before); /* every UL heard on 2.4 */
    TEST_ASSERT_EQUAL_UINT32(1, downs);
    TEST_ASSERT_TRUE(ul_err_max <= 20);
    TEST_ASSERT_EQUAL_UINT(0, lead_violations);
}

static void test_page_from_idle(void)
{
    sim_start(0x13572468u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    cell.attach_idle = 1;
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_IDLE, term.state);
    cell.attach_idle = 0;
    ocb_cell_page(&cell, term.tmid);
    run_for(3000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT32(1, cell.page_replies);
    TEST_ASSERT_EQUAL_UINT32(0, cell.page_tmid);
}

static void test_regrant_in_dl_moves_ul_to_2g4(void)
{
    sim_start(0x2468ACE0u, OC_TIER_NEAR, OC_BAND_915, OC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    ocb_cell_set_bands(&cell, OC_BAND_915, OC_BAND_2G4);
    run_for(2000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT8(OC_BAND_2G4, term.grant.ul.band);
    TEST_ASSERT_EQUAL_UINT32(1, cell.attaches); /* no re-attach needed */
    uint32_t before = cell.terms[0].ul_rx;
    run_for(1200);
    TEST_ASSERT_UINT32_WITHIN(1, 10, cell.terms[0].ul_rx - before);
}

static void test_2g4_loss_falls_back_to_915(void)
{
    sim_start(0x55AA55AAu, OC_TIER_MID, OC_BAND_2G4, OC_BAND_2G4);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT8(OC_BAND_2G4, term.grant.dl.band);
    /* 2.4 GHz goes out of range: the terminal re-attaches on 915 and the
     * cell (like plan 4) re-grants it there. */
    cell.fallback_915 = 1;
    blocked[OC_BAND_2G4] = 1;
    run_for(10000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT8(OC_BAND_915, term.grant.dl.band);
    TEST_ASSERT_EQUAL_UINT8(OC_TIER_MID, term.grant.dl.tier);
    TEST_ASSERT_EQUAL_UINT32(2, cell.attaches); /* re-attached through 915 RACH */
}

static void test_sync_loss_and_recovery(void)
{
    sim_start(0x0F1E2D3Cu, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    cell.off = 1;
    run_for(5000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_SEARCH, term.state);
    TEST_ASSERT_EQUAL_UINT32(1, term.sync_losses);
    cell.off = 0;
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
}

static void test_stuck_radio_recovers(void)
{
    sim_start(0x0DDBA11u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    stuck = 1; /* IRQ line dead: every op must be stopped at its end */
    run_for(600);
    TEST_ASSERT_TRUE(term.overruns >= 5);
    stuck = 0;
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
}

static void test_stall_jumps_to_present(void)
{
    sim_start(0x5EED5EEDu, OC_TIER_MID, OC_BAND_915, OC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    uint32_t frame_before = term.cur_frame;
    uint32_t skipped_before = term.skipped_ops;
    stall_for(600); /* 5 frames with no oc_term_step() */
    run_for(240);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_TRUE(term.cur_frame - frame_before >= 6u);
    /* Stale frames are dropped, not replayed op by op. */
    TEST_ASSERT_TRUE(term.skipped_ops - skipped_before <= 3u);
}

static void test_search_covers_every_sync_candidate(void)
{
    /* Seeds whose frame%8==0 sync channel is each of the 6 candidates. */
    for (uint32_t off = 0; off < 6u; off++) {
        sim_start(0x10000000u + off, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
        run_for(12000);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(OC_TERM_GRANTED, term.state, "seed candidate not found");
    }
}

/* ---- channel list (spec 2026-09-27-channel-list-design.md §13) ---- */

static uint32_t chf(uint8_t c) { return oc_channel_freq_hz(OC_BAND_915, c); }

static void user_list(uint8_t n, const uint8_t *chans, uint8_t flags)
{
    oc_scan_ent_t e[OC_SCAN_MAX_USER];
    for (uint8_t i = 0; i < n; i++) e[i] = (oc_scan_ent_t){ chf(chans[i]), flags };
    TEST_ASSERT_EQUAL_INT(0, oc_term_scan_set_user(&term.scan, n, e));
}

/* A cell on anchor 30 in the user's list: found in the first dwell. */
static void test_listed_anchor_found_in_one_dwell(void)
{
    sim_start(0x10000000u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&cell, 30, 0));
    user_list(1, (const uint8_t[]){ 30 }, 0);
    run_for(1200);
    TEST_ASSERT_TRUE(term.state != OC_TERM_SEARCH);
    TEST_ASSERT_EQUAL_UINT8(30, term.anchor);
    run_for(12000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT32(chf(30), term.scan.last.freq_hz); /* attached: the last serving entry */
}

/* A beacon names its anchor: a cell on 30 crosses default ch 4 (frames
 * f % 8 == 4) and is followed on its own channels from then on. */
static void test_crossing_anchor_found_and_followed(void)
{
    sim_start(0x10000000u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&cell, 30, 0));
    run_for(8000); /* one pass of the defaults */
    TEST_ASSERT_TRUE(term.state != OC_TERM_SEARCH);
    TEST_ASSERT_EQUAL_UINT8(30, term.anchor);
    run_for(10000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    uint32_t before = term.beacons;
    run_for(4800); /* 40 frames */
    TEST_ASSERT_TRUE(term.beacons - before >= 35);
    TEST_ASSERT_EQUAL_UINT32(0, term.sync_losses);
}

/* Anchor 32's cycle (32 38 45 51 6 12 19 25) misses ch 0-5: with fallback
 * never it is not found; with 2 / 13 the first sweep (round 3, ch 6) finds it. */
static void test_unlisted_anchor_never_or_by_the_sweep(void)
{
    sim_start(0x20000000u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&cell, 32, 0));
    TEST_ASSERT_EQUAL_INT(0, oc_term_scan_set_fallback(&term.scan, OC_SCAN_NEVER, 13));
    run_for(60000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_SEARCH, term.state);

    sim_start(0x20000000u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&cell, 32, 0));
    run_for(21500); /* rounds 1-2 (2 x 7.2 s) and round 3's list (7.2 s) */
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_SEARCH, term.state);
    run_for(1500); /* round 3's first sweep dwell, ch 6 */
    TEST_ASSERT_TRUE(term.state != OC_TERM_SEARCH);
    TEST_ASSERT_EQUAL_UINT8(32, term.anchor);
    TEST_ASSERT_EQUAL_UINT8(OC_SCAN_SRC_SWEEP, term.scan.cur_src);
    TEST_ASSERT_EQUAL_UINT8(2, term.scan.passes);
}

/* Part 97 FIXED sync: every beacon on the anchor, found in one 0.36 s dwell,
 * and the whole attach runs on it. The cell refuses FIXED in Part 15. */
static void test_fixed_part97_found_in_a_short_dwell(void)
{
    sim_start(0x30000000u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    TEST_ASSERT_EQUAL_INT(-1, ocb_cell_set_sync(&cell, 30, 1));
    cell.part97 = 1;
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&cell, 30, 1));
    term.scan.mode = OC_PHY_MODE_PART97;
    user_list(1, (const uint8_t[]){ 30 }, OC_SCAN_F_FIXED);
    run_for(360);
    TEST_ASSERT_TRUE(term.state != OC_TERM_SEARCH);
    TEST_ASSERT_EQUAL_UINT8(1, term.fixed_sync);
    run_for(12000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_HEX8(OC_SCAN_F_FIXED, term.scan.last.flags);
}

/* A mis-set Part 15 cell sending FIXED beacons is never followed. */
static void test_fixed_without_part97_never_followed(void)
{
    sim_start(0x30000000u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    cell.sync_ch = 30; /* bypasses ocb_cell_set_sync, which refuses this */
    cell.fixed_sync = 1;
    term.scan.mode = OC_PHY_MODE_PART97; /* so the terminal does dwell on ch 30 */
    user_list(1, (const uint8_t[]){ 30 }, OC_SCAN_F_FIXED);
    run_for(10000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_SEARCH, term.state);
    TEST_ASSERT_TRUE(term.bad_beacons > 0);
}

/* Two cells on anchors 10 and 40: the list order decides; when the chosen
 * cell stops, the terminal is on the other within the 3 s loss and two dwells. */
static void test_two_cells_list_order_decides(void)
{
    static const uint32_t seed_a = 0x0A0A0A0Au, seed_b = 0x40404040u;
    for (int order = 0; order < 2; order++) {
        sim_start(seed_a, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
        TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&cell, 10, 0));
        ocb_cell_init(&cell2, seed_b, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
        TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&cell2, 40, 0));
        cell2_on = 1;
        user_list(2, order == 0 ? (const uint8_t[]){ 40, 10 } : (const uint8_t[]){ 10, 40 }, 0);
        run_for(15000);
        TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
        TEST_ASSERT_EQUAL_HEX32(order == 0 ? seed_b : seed_a, term.cell_seed);
    }
    /* order [10, 40] attached to 10: stop it */
    cell.off = 1;
    run_for(6000);
    TEST_ASSERT_TRUE(term.state != OC_TERM_SEARCH);
    TEST_ASSERT_EQUAL_HEX32(seed_b, term.cell_seed);
    TEST_ASSERT_EQUAL_UINT8(40, term.anchor);
    run_for(10000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT32(chf(40), term.scan.last.freq_hz);
    TEST_ASSERT_EQUAL_UINT32(chf(10), term.scan.learn[0].freq_hz);
}

/* A real bs-radio W12 only opens a frame on a FIRST part (plan 2 review #4):
 * ocbench cell's single-part frames must carry FIRST | LAST. */
static void test_cell_schedules_are_first_and_last(void)
{
    static oc_msg_t m;
    ocb_cell_init(&cell, 0xCAFEF00Du, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_schedule(&cell, OC_BAND_915, 100, &m));
    TEST_ASSERT_EQUAL_UINT8(OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST, m.u.schedule.flags);
}

static int sig_has(uint8_t code)
{
    for (int i = 0; i < sig_nevs && i < 32; i++) if (sig_evs[i] == code) return 1;
    return 0;
}

static void sig_command(const uint8_t *cmd, size_t n)
{
    TEST_ASSERT_EQUAL_UINT8(0, oc_sig_term_command(&glue.sig, cmd, n, now_local));
}

/* The whole C side over the simulated air: activation, MILENAGE registration,
 * the idle channel released, an outgoing call (service request, page, grant),
 * encrypted app data echoed by the network, hang-up. */
static void test_activation_registration_and_call_over_the_air(void)
{
    sim_start(0x4d2u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    sig_start();
    run_for(15000); /* search, sync, attach, grant */
    uint8_t cmd[1 + OC_SIG_QR_TEXT + 1];
    cmd[0] = OC_SIG_CMD_ACTIVATE;
    size_t n = oc_sig_qr_format(&sqr, (char *)cmd + 1, sizeof(cmd) - 1);
    sig_command(cmd, 1 + n);
    run_for(20000);
    TEST_ASSERT_TRUE(sig_has(OC_SIG_EV_ACTIVATED));
    TEST_ASSERT_TRUE(sig_has(OC_SIG_EV_REGISTERED));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    run_for(10000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_IDLE, term.state); /* the idle channel was released */

    static const uint8_t dial[] = "\x02" "1 606 555 1235"; /* numbering v2: the country code, no 883, no 0 */
    sig_command(dial, sizeof(dial) - 1);
    run_for(15000);
    TEST_ASSERT_EQUAL_INT(1, snet_mo);
    uint8_t want[OC_SIG_NUMBER_LEN];
    oc_sig_number_to_bcd("+883160655501235", 16, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, snet_mo_number, OC_SIG_NUMBER_LEN); /* the full form on the air */
    TEST_ASSERT_EQUAL_INT(0, oc_sig_net_peer_answer(&snet, snet_call, sim_now()));
    run_for(10000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_IN_CALL, oc_sig_term_state(&glue.sig));

    TEST_ASSERT_EQUAL_INT(0, oc_term_sig_app_up(&glue, (const uint8_t *)"PING", 4));
    run_for(3000);
    TEST_ASSERT_EQUAL_UINT8(4, app_rx_n);
    TEST_ASSERT_EQUAL_MEMORY("PING", app_rx, 4);

    uint8_t hang = OC_SIG_CMD_HANGUP;
    sig_command(&hang, 1);
    run_for(5000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_EQUAL_INT(1, snet_ended);
}

/* oc_term_sig_app_up must never spend an uplink frame counter (d_tx) on a
 * refusal, and must never let a short app frame go out as RACH UPPER (that
 * slot carries only the 1-byte service request) while not GRANTED. */
static void test_app_up_refuses_when_not_granted(void)
{
    sim_start(0xAB12CD34u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    sig_start();

    /* Right after start: SEARCH, not attached, definitely not GRANTED. */
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_SEARCH, term.state);
    uint32_t d_tx_before = glue.sig.d_tx;
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ATT_NOT_NOW, oc_term_sig_app_up(&glue, (const uint8_t *)"X", 1));
    TEST_ASSERT_EQUAL_UINT32(d_tx_before, glue.sig.d_tx);
    TEST_ASSERT_FALSE(term.rach_pending); /* nothing was queued as RACH UPPER */

    /* Reach IDLE (attached, but not granted a channel): still refused. */
    cell.attach_idle = 1;
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_IDLE, term.state);
    d_tx_before = glue.sig.d_tx;
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ATT_NOT_NOW, oc_term_sig_app_up(&glue, (const uint8_t *)"X", 1));
    TEST_ASSERT_EQUAL_UINT32(d_tx_before, glue.sig.d_tx);
    TEST_ASSERT_FALSE(term.rach_pending);
    cell.attach_idle = 0;

    /* Now get GRANTED, then fill the UL queue directly so oc_term_send_upper
     * itself refuses: d_tx must be rolled back after that refusal too. */
    ocb_cell_page(&cell, term.tmid);
    run_for(3000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_GRANTED, term.state);
    for (unsigned i = 0; i < OC_TERM_UPQ_DEPTH; i++) {
        TEST_ASSERT_EQUAL_INT(0, oc_term_send_upper(&term, (const uint8_t *)"Q", 1));
    }
    d_tx_before = glue.sig.d_tx;
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ATT_NOT_NOW, oc_term_sig_app_up(&glue, (const uint8_t *)"PING", 4));
    TEST_ASSERT_EQUAL_UINT32(d_tx_before, glue.sig.d_tx);
}

/* ocbench net's stand-in end to end: the peer rings and answers an outgoing
 * call by itself, echoes app data, places an incoming call and hangs it up. */
static void test_ocb_net_peer_answers_echoes_and_calls_in(void)
{
    sim_start(0x4d2u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    net_start();
    run_for(15000);
    uint8_t cmd[1 + OC_SIG_QR_TEXT + 1];
    cmd[0] = OC_SIG_CMD_ACTIVATE;
    size_t n = oc_sig_qr_format(&sqr, (char *)cmd + 1, sizeof(cmd) - 1);
    sig_command(cmd, 1 + n);
    run_for(20000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_TRUE(lhss.subs[0].activated);
    TEST_ASSERT_EQUAL_HEX32(0x75123456u, lhss.subs[0].tmid);
    TEST_ASSERT_TRUE(lhss.subs[0].token_used);

    static const uint8_t dial[] = "\x02" "606-555-0100"; /* the echo service, dialled in-country */
    net_dials[0] = '\0';
    sig_command(dial, sizeof(dial) - 1);
    run_for(4000); /* page from IDLE and grant ~2 s, then CALL_SETUP / CALL_PROC / ALERTING */
    TEST_ASSERT_TRUE(sig_has(OC_SIG_EV_RINGING));
    TEST_ASSERT_FALSE(sig_has(OC_SIG_EV_CONNECTED));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(net_dials, "dials +883-1-606-555-00100;"), net_dials); /* shown to people */
    run_for(6000); /* the peer answers 3 s after it starts ringing */
    TEST_ASSERT_TRUE(sig_has(OC_SIG_EV_CONNECTED));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_IN_CALL, oc_sig_term_state(&glue.sig));

    TEST_ASSERT_EQUAL_INT(0, oc_term_sig_app_up(&glue, (const uint8_t *)"PING", 4));
    run_for(3000);
    TEST_ASSERT_EQUAL_UINT8(4, app_rx_n);
    TEST_ASSERT_EQUAL_MEMORY("PING", app_rx, 4);
    TEST_ASSERT_EQUAL_UINT32(1, lnet.echoed);

    uint8_t c = OC_SIG_CMD_HANGUP;
    sig_command(&c, 1);
    run_for(5000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));

    lnet.peer_hangup_us = 8000000u; /* this time the peer hangs up, 8 s after connect */
    ocb_net_call_in(&lnet, lhss.subs[0].number, sim_now() + 1000000u);
    run_for(10000);
    TEST_ASSERT_TRUE(sig_has(OC_SIG_EV_INCOMING));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_RINGING_IN, oc_sig_term_state(&glue.sig));
    c = OC_SIG_CMD_ANSWER;
    sig_command(&c, 1);
    run_for(5000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_IN_CALL, oc_sig_term_state(&glue.sig));
    sig_nevs = 0;
    run_for(8000);
    TEST_ASSERT_TRUE(sig_has(OC_SIG_EV_ENDED));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_TRUE(net_lines >= 6); /* calls logged */
}

/* Channel-list spec §7 over the simulated air with ocbench net's stand-in:
 * CHAN_LIST after registration fills the scan list's network entries; a new
 * list version in the beacon brings SERVICE_REQ(4) and the new list;
 * DEACTIVATE clears the network's and learned entries, not the user's. */
static void test_chan_list_over_the_air(void)
{
    sim_start(0x4d2u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    net_start();
    user_list(1, (const uint8_t[]){ 20 }, 0);
    oc_sig_chan_list_t l;
    char err[96];
    TEST_ASSERT_EQUAL_INT(0, ocb_net_parse_chan_list("902.25,917.25", 1, &l, err, sizeof(err)));
    ocb_net_set_chan_list(&lnet, &l);
    TEST_ASSERT_EQUAL_UINT8(1, cell.cfg_ver);
    run_for(15000);
    uint8_t cmd[1 + OC_SIG_QR_TEXT + 1];
    cmd[0] = OC_SIG_CMD_ACTIVATE;
    size_t n = oc_sig_qr_format(&sqr, (char *)cmd + 1, sizeof(cmd) - 1);
    sig_command(cmd, 1 + n);
    run_for(20000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.net_ver);
    TEST_ASSERT_EQUAL_UINT8(2, term.scan.n_net);
    TEST_ASSERT_EQUAL_UINT32(917250000u, term.scan.net[1].freq_hz);
    TEST_ASSERT_EQUAL_UINT8(OC_PHY_MODE_PART15, term.scan.mode);
    TEST_ASSERT_EQUAL_INT(0, net_svc_config);
    run_for(10000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_IDLE, term.state); /* the idle channel was released */

    TEST_ASSERT_EQUAL_INT(0, ocb_net_parse_chan_list("922.25:fixed", 2, &l, err, sizeof(err)));
    ocb_net_set_chan_list(&lnet, &l); /* the beacon now says cfg_ver 2 */
    run_for(15000);
    TEST_ASSERT_EQUAL_INT(1, net_svc_config);
    TEST_ASSERT_EQUAL_UINT8(2, term.scan.net_ver);
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.n_net);
    TEST_ASSERT_EQUAL_HEX8(OC_SCAN_F_FIXED, term.scan.net[0].flags);
    run_for(40000);
    TEST_ASSERT_EQUAL_INT(1, net_svc_config); /* up to date: asked once */

    term.scan.n_learn = 1; /* as if an earlier cell had served */
    term.scan.learn[0] = (oc_scan_ent_t){ chf(44), 0 };
    static const uint8_t deact[2] = { OC_SIG_CMD_DEACTIVATE, 0xA5 };
    sig_command(deact, 2);
    TEST_ASSERT_EQUAL_UINT8(0, term.scan.n_net);
    TEST_ASSERT_EQUAL_UINT8(0, term.scan.net_ver);
    TEST_ASSERT_EQUAL_UINT8(0, term.scan.n_learn);
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.n_user);
    TEST_ASSERT_TRUE(term.scan.dirty);
}

/* Task 9 review fix, points 2/3 regression: M1 must not survive a list handed
 * back at a DIFFERENT list_ver after a restart. Register with list v3, bump
 * to v4 (asked and answered, M1 recorded at list_ver 4); the cell then goes
 * away and comes back as a freshly restarted network that only knows v3 -
 * the terminal re-registers (through the ordinary reattach path, not
 * cell_cfg's own) and takes v3. When the beacon then bumps back to the same
 * cfg_ver as v4 before, cell_cfg must ask again: on 421a422 the stale M1
 * record survived reg_start and the terminal never asked, silently missing
 * v4 forever. */
static void test_bump_after_a_restart(void)
{
    sim_start(0x4d2u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    net_start();
    user_list(1, (const uint8_t[]){ 20 }, 0);
    oc_sig_chan_list_t l;
    char err[96];
    TEST_ASSERT_EQUAL_INT(0, ocb_net_parse_chan_list("902.25,917.25", 3, &l, err, sizeof(err)));
    ocb_net_set_chan_list(&lnet, &l);
    TEST_ASSERT_EQUAL_UINT8(3, cell.cfg_ver);
    run_for(15000);
    uint8_t cmd[1 + OC_SIG_QR_TEXT + 1];
    cmd[0] = OC_SIG_CMD_ACTIVATE;
    size_t n = oc_sig_qr_format(&sqr, (char *)cmd + 1, sizeof(cmd) - 1);
    sig_command(cmd, 1 + n);
    run_for(20000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_EQUAL_UINT8(3, term.scan.net_ver);

    TEST_ASSERT_EQUAL_INT(0, ocb_net_parse_chan_list("922.25:fixed", 4, &l, err, sizeof(err)));
    ocb_net_set_chan_list(&lnet, &l); /* the beacon now says cfg_ver 4 */
    run_for(15000);
    TEST_ASSERT_EQUAL_INT(1, net_svc_config); /* asked once, and answered */
    TEST_ASSERT_EQUAL_UINT8(4, term.scan.net_ver);
    TEST_ASSERT_EQUAL_UINT8(4, glue.sig.list_ver);

    /* the cell goes away and comes back as a freshly restarted network that
     * only knows list v3 (like a reboot: the stand-in HSS's subscriber
     * record survives - a real HSS would too - but the live session and the
     * chan-list config in RAM don't). */
    cell.off = 1;
    run_for(5000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_SEARCH, term.state);
    ocb_net_init(&lnet, &cell, &lhss, NULL, sim_rnd, sim_now, sim_net_log);
    net_svc_config = 0;
    net_cl_taken = 0;
    TEST_ASSERT_EQUAL_INT(0, ocb_net_parse_chan_list("902.25,917.25", 3, &l, err, sizeof(err)));
    ocb_net_set_chan_list(&lnet, &l);
    cell.off = 0;
    run_for(20000);
    TEST_ASSERT_TRUE(term.state != OC_TERM_SEARCH); /* re-attached (idle or granted) */
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_EQUAL_UINT8(3, glue.sig.list_ver); /* v3 taken from the restarted network */
    TEST_ASSERT_EQUAL_UINT8(3, term.scan.net_ver);

    /* the beacon bumps back to cfg_ver 4 - the same value that was already
     * "answered" before the restart. list_ver has since moved to 3, so M1
     * must not still think this exact ask was answered: it must ask again. */
    TEST_ASSERT_EQUAL_INT(0, ocb_net_parse_chan_list("922.25:fixed", 4, &l, err, sizeof(err)));
    ocb_net_set_chan_list(&lnet, &l);
    run_for(15000);
    TEST_ASSERT_EQUAL_INT(1, net_svc_config); /* asked once, not silently skipped */
    TEST_ASSERT_EQUAL_UINT8(4, term.scan.net_ver);
    TEST_ASSERT_EQUAL_UINT8(4, glue.sig.list_ver);
}

/* At boot the saved list's version is the one oc_sig_term compares with the
 * beacon's cfg_ver (oc_term_sig_init runs after the scan list is loaded). */
static void test_sig_init_takes_the_saved_list_version(void)
{
    sim_start(0x4d2u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    term.scan.net_ver = 3; /* as if loaded from NVS */
    net_start();           /* oc_term_sig_init */
    TEST_ASSERT_EQUAL_UINT8(3, glue.sig.list_ver);
}

/* Task 9 review note (b): a Part 97 FIXED cell first heard through a CYCLE
 * entry while the list is still in Part 15 is recorded as a FIXED last-serving
 * entry, inactive until REG_ACK's mode reaches the scan list. Once it has, a
 * sync loss finds the cell again straight from that entry (a 0.36 s dwell),
 * with no user entry left to help. Note (a): the identical CHAN_LIST pushed
 * after the re-registration leaves the list alone (nothing new to save). */
static void test_fixed_part97_found_again_after_sync_loss(void)
{
    sim_start(0x30000000u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    net_start_mode(OC_SIG_MODE_PART97);
    TEST_ASSERT_EQUAL_INT(1, cell.part97);
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&cell, 30, 1));
    oc_sig_chan_list_t l;
    ocb_net_own_chan_list(&cell, 1, &l); /* ocbench net's default: { 917.25:fixed } */
    ocb_net_set_chan_list(&lnet, &l);
    user_list(1, (const uint8_t[]){ 30 }, 0); /* CYCLE: active in Part 15, dwells on ch 30 */
    TEST_ASSERT_EQUAL_UINT8(OC_PHY_MODE_PART15, term.scan.mode);
    run_for(15000);
    TEST_ASSERT_TRUE(term.state == OC_TERM_IDLE || term.state == OC_TERM_GRANTED); /* attached */
    TEST_ASSERT_EQUAL_UINT32(chf(30), term.scan.last.freq_hz);
    TEST_ASSERT_EQUAL_HEX8(OC_SCAN_F_FIXED, term.scan.last.flags); /* recorded before any REG_ACK */
    TEST_ASSERT_EQUAL_UINT8(OC_PHY_MODE_PART15, term.scan.mode);

    uint8_t cmd[1 + OC_SIG_QR_TEXT + 1];
    cmd[0] = OC_SIG_CMD_ACTIVATE;
    size_t n = oc_sig_qr_format(&sqr, (char *)cmd + 1, sizeof(cmd) - 1);
    sig_command(cmd, 1 + n);
    run_for(20000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_EQUAL_UINT8(OC_PHY_MODE_PART97, term.scan.mode);
    TEST_ASSERT_EQUAL_INT(1, net_cl_taken);
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.net_ver);
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.n_net);
    TEST_ASSERT_EQUAL_HEX8(OC_SCAN_F_FIXED, term.scan.net[0].flags);
    TEST_ASSERT_EQUAL_INT(0, oc_term_scan_set_user(&term.scan, 0, NULL)); /* no CYCLE entry on 30 any more */
    term.scan.dirty = 0; /* as if saved */

    cell.off = 1;
    run_for(5000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_SEARCH, term.state);
    cell.off = 0;
    run_for(8500); /* within one round: last (0.36 s) and the six defaults (7.2 s) */
    TEST_ASSERT_TRUE(term.state != OC_TERM_SEARCH);
    TEST_ASSERT_EQUAL_UINT8(30, term.anchor);
    TEST_ASSERT_EQUAL_UINT8(1, term.fixed_sync);
    TEST_ASSERT_EQUAL_UINT8(OC_SCAN_SRC_LAST, term.scan.cur_src);

    run_for(20000); /* registers again; the network pushes v1 again */
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_EQUAL_INT(2, net_cl_taken);
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.net_ver);
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.n_net);
    TEST_ASSERT_FALSE(term.scan.dirty); /* identical list and version: no NVS write */

    /* A different list under the same version (no cfg_ver change, so no ask):
     * taken at the next registration, and applied. */
    char err[96];
    TEST_ASSERT_EQUAL_INT(0, ocb_net_parse_chan_list("902.25", 1, &l, err, sizeof(err))); /* same count too */
    ocb_net_set_chan_list(&lnet, &l);
    cell.off = 1;
    run_for(5000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_SEARCH, term.state);
    cell.off = 0;
    run_for(30000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&glue.sig));
    TEST_ASSERT_EQUAL_INT(3, net_cl_taken);
    TEST_ASSERT_EQUAL_INT(0, net_svc_config);
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.net_ver);
    TEST_ASSERT_EQUAL_UINT8(1, term.scan.n_net);
    TEST_ASSERT_EQUAL_UINT32(chf(0), term.scan.net[0].freq_hz);
    TEST_ASSERT_EQUAL_HEX8(0, term.scan.net[0].flags);
    TEST_ASSERT_TRUE(term.scan.dirty);
}

/* Final review I3: the signalling send hook must refuse unless GRANTED, or a
 * short fragment (<= 8 B, e.g. AUTH_FAIL cause 1, or a short last fragment)
 * would leave as RACH UPPER when oc_sig_term's "granted" is a step stale. */
static void test_sig_fragment_never_goes_out_as_rach_upper(void)
{
    sim_start(0xAB12CD34u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    sig_start();
    cell.attach_idle = 1;
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(OC_TERM_IDLE, term.state);
    const uint8_t frag[6] = { OC_SIG_KIND_SIG | 0x02u, 0x00, OC_SIG_AUTH_FAIL, 0x00, 0x00, 0x01 };
    TEST_ASSERT_EQUAL_INT(-1, glue.sig.io.send(glue.sig.io.ctx, frag, sizeof(frag)));
    TEST_ASSERT_FALSE(term.rach_pending);
    cell.attach_idle = 0;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_attach_and_granted_edge_915);
    RUN_TEST(test_loopback_through_granted_slots);
    RUN_TEST(test_cross_band_duplex_near);
    RUN_TEST(test_page_from_idle);
    RUN_TEST(test_regrant_in_dl_moves_ul_to_2g4);
    RUN_TEST(test_2g4_loss_falls_back_to_915);
    RUN_TEST(test_sync_loss_and_recovery);
    RUN_TEST(test_stuck_radio_recovers);
    RUN_TEST(test_stall_jumps_to_present);
    RUN_TEST(test_search_covers_every_sync_candidate);
    RUN_TEST(test_cell_schedules_are_first_and_last);
    RUN_TEST(test_listed_anchor_found_in_one_dwell);
    RUN_TEST(test_crossing_anchor_found_and_followed);
    RUN_TEST(test_unlisted_anchor_never_or_by_the_sweep);
    RUN_TEST(test_fixed_part97_found_in_a_short_dwell);
    RUN_TEST(test_fixed_without_part97_never_followed);
    RUN_TEST(test_two_cells_list_order_decides);
    RUN_TEST(test_activation_registration_and_call_over_the_air);
    RUN_TEST(test_app_up_refuses_when_not_granted);
    RUN_TEST(test_sig_fragment_never_goes_out_as_rach_upper);
    RUN_TEST(test_ocb_net_peer_answers_echoes_and_calls_in);
    RUN_TEST(test_chan_list_over_the_air);
    RUN_TEST(test_bump_after_a_restart);
    RUN_TEST(test_sig_init_takes_the_saved_list_version);
    RUN_TEST(test_fixed_part97_found_again_after_sync_loss);
    return UNITY_END();
}
