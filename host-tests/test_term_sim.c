/* End-to-end: an lc_term terminal against an lcb_cell base station over a
 * simulated air interface.
 *
 * True time T (µs) is the base station's; frame F starts at bs_start(F).
 * The terminal's crystal runs +15 ppm fast with an arbitrary offset. Every
 * slot the cell schedules is stored; a terminal RX "hears" a stored TX with
 * the same frequency and modulation that starts inside its window, a stored
 * RX hears a terminal TX that starts within PREAMBLE_TOL_US of the slot
 * start. The host sends frame F's schedule at the start of frame F-2, as
 * lcbench and plan 4 do; RX_REPORTs arrive REPORT_DELAY_US after the packet. */
#include "unity.h"

#include <string.h>

#include "lc_term.h"
#include "lcb_cell.h"

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
    uint64_t  t0;
    uint32_t  len;
    uint32_t  freq;
    lc_mode_t mode;
    uint8_t   dir;
    uint8_t   band;
    uint32_t  frame;
    uint8_t   index;
    uint8_t   plen;
    uint8_t   payload[LC_AIR_MAX_FRAME];
} air_slot_t;

typedef struct {
    uint64_t       due;
    uint8_t        band;
    lc_rx_report_t r;
    uint8_t        data[LC_AIR_MAX_FRAME];
} report_t;

static lcb_cell_t cell;
static lc_term_t term;
static air_slot_t slots[MAX_SLOTS];
static unsigned slot_next;
static report_t reports[MAX_REPORTS];
static int blocked[LC_BAND_COUNT];
static int stuck;            /* radio never raises its IRQ */
static uint64_t now_local;

/* terminal radio state */
static uint32_t r_freq;
static lc_mode_t r_mode;
static int r_tx;
static uint8_t r_buf[LC_AIR_MAX_FRAME];
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

static uint64_t bs_start(uint32_t f) { return 1000000u + (uint64_t)(f - F_BASE) * LC_FRAME_US; }
static uint64_t to_local(uint64_t t) { return LOCAL_OFF + t + t * PPM / 1000000u; }
static uint64_t to_true(uint64_t l) { return (l - LOCAL_OFF) * 1000000u / (1000000u + PPM); }

static int same_mode(const lc_mode_t *a, const lc_mode_t *b) { return memcmp(a, b, sizeof(*a)) == 0; }
static uint8_t band_of(uint32_t hz) { return hz >= 1500000000u ? LC_BAND_2G4 : LC_BAND_915; }

static void store_schedule(uint8_t band, const lc_msg_t *m)
{
    const lc_schedule_t *s = &m->u.schedule;
    for (uint8_t i = 0; i < s->slot_count; i++) {
        air_slot_t *a = &slots[slot_next++ % MAX_SLOTS];
        const lc_slot_t *sl = &s->slots[i];
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
            lcb_cell_on_rx(&cell, (lc_band_t)reports[i].band, &reports[i].r);
            reports[i].due = 0;
        }
    }
}

/* ---- fake terminal radio ---- */

static int f_configure(void *c, uint32_t freq, const lc_mode_t *m)
{
    (void)c;
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
    r_active = 1;
    r_start = to_true(at_us > now_local ? at_us : now_local); /* the radio waits for at_us */
    if (!r_tx) {
        return 0;
    }
    uint32_t air = lc_airtime_us(&r_mode, r_len);
    r_tx_end = r_start + air;
    uint8_t band = band_of(r_freq);
    if (blocked[band]) {
        return 0;
    }
    for (unsigned i = 0; i < MAX_SLOTS; i++) {
        air_slot_t *a = &slots[i];
        if (a->dir != LC_DIR_RX || a->freq != r_freq || !same_mode(&a->mode, &r_mode) ||
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
                reports[k].due = r_tx_end + REPORT_DELAY_US;
                reports[k].band = a->band;
                reports[k].r = (lc_rx_report_t){ a->frame, a->index, -80, 40, 1, r_len, NULL, LC_RX_END_UNKNOWN };
                memcpy(reports[k].data, r_buf, r_len);
                break;
            }
        }
        break;
    }
    return 0;
}

static int f_poll(void *c, lc_radio_event_t *ev)
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
        ev->type = LC_RADIO_EV_TX_DONE;
        return 1;
    }
    uint8_t band = band_of(r_freq);
    const air_slot_t *best = NULL;
    for (unsigned i = 0; i < MAX_SLOTS && !blocked[band]; i++) {
        const air_slot_t *a = &slots[i];
        if (a->dir != LC_DIR_TX || a->freq != r_freq || !same_mode(&a->mode, &r_mode) || a->plen == 0) {
            continue;
        }
        uint64_t end = a->t0 + lc_airtime_us(&a->mode, a->plen);
        if (a->t0 + PREAMBLE_TOL_US < r_start || end + lc_rx_done_lag_us(&a->mode) > now ||
            end > r_start + r_timeout) {
            continue;
        }
        if (best == NULL || a->t0 < best->t0) {
            best = a;
        }
    }
    if (best != NULL) {
        r_active = 0;
        ev->type = LC_RADIO_EV_RX_DONE;
        ev->crc_ok = 1;
        ev->len = best->plen;
        ev->rssi_dbm = -80;
        ev->snr_qdb = best->mode.modulation == LC_MOD_FLRC ? 0 : 40;
        memcpy(ev->data, best->payload, best->plen);
        /* As on the W12: RX_DONE comes lc_rx_done_lag_us after the formula end. */
        lc_term_note_irq(&term, to_local(best->t0 + lc_airtime_us(&best->mode, best->plen) +
                                         lc_rx_done_lag_us(&best->mode)));
        return 1;
    }
    if (now >= r_start + r_timeout) {
        r_active = 0;
        ev->type = LC_RADIO_EV_RX_TIMEOUT;
        return 1;
    }
    return 0;
}

static void f_standby(void *c)
{
    (void)c;
    r_active = 0;
}

static const lc_radio_ops_t radio = { NULL, f_configure, f_stage_tx, f_stage_rx, f_launch, f_poll, f_standby };

static void on_down(void *c, const uint8_t *d, uint8_t len)
{
    (void)c;
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

static const lc_term_sink_t sink = { NULL, on_down, NULL, rand32 };

/* ---- simulation driver ---- */

static uint32_t build_frame; /* next frame whose schedule the host sends */
static uint64_t next_step_local;

static void sim_start(uint32_t seed, lc_tier_t tier, lc_band_t dl, lc_band_t ul)
{
    memset(slots, 0, sizeof(slots));
    memset(reports, 0, sizeof(reports));
    memset(blocked, 0, sizeof(blocked));
    stuck = 0;
    slot_next = 0;
    r_active = 0;
    ul_err_max = 0;
    ul_heard = 0;
    downs = 0;
    down_len = 0;
    lcb_cell_init(&cell, seed, tier, dl, ul);
    lc_term_init(&term, &radio, &sink, 0x75123456u);
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
            static lc_msg_t m;
            if (lcb_cell_schedule(&cell, LC_BAND_915, build_frame, &m) == 0) {
                store_schedule(LC_BAND_915, &m);
            }
            if (lcb_cell_schedule(&cell, LC_BAND_2G4, build_frame, &m) == 0) {
                store_schedule(LC_BAND_2G4, &m);
            }
            build_frame++;
        } else {
            now_local = next_step_local;
            deliver_reports(t_step);
            uint64_t next = lc_term_step(&term, now_local);
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
    next_step_local = to_local(until + 10u * LC_FRAME_US); /* keep the loop from stepping */
    sim_run_until(until);
    now_local = to_local(until);
    next_step_local = saved < now_local ? now_local : saved;
}

/* ---- tests ---- */

static void test_attach_and_granted_edge_915(void)
{
    sim_start(0x12345678u, LC_TIER_EDGE, LC_BAND_915, LC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT32(1, cell.attaches);
    TEST_ASSERT_TRUE(cell.terms[0].ul_rx > 30);
    TEST_ASSERT_EQUAL_UINT32(0, term.bad_grants);
    TEST_ASSERT_TRUE(cell.grants_sent >= 2); /* the grant went out twice */
    TEST_ASSERT_FALSE(term.have_next);        /* ...and was applied once */
    /* Terminal UL lands within 20 µs of the base station's slot start. */
    TEST_ASSERT_TRUE(ul_err_max <= 20);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_915, term.grant.dl.band);
    TEST_ASSERT_EQUAL_UINT8(LC_TIER_EDGE, term.grant.dl.tier);
}

static void test_loopback_through_granted_slots(void)
{
    sim_start(0x0BADCAFEu, LC_TIER_MID, LC_BAND_915, LC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    const uint8_t msg[] = "hello, cell";
    TEST_ASSERT_EQUAL_INT(0, lc_term_send_upper(&term, msg, sizeof(msg)));
    run_for(1000);
    TEST_ASSERT_EQUAL_UINT32(1, downs);
    TEST_ASSERT_EQUAL_UINT8(sizeof(msg), down_len);
    TEST_ASSERT_EQUAL_MEMORY(msg, down, sizeof(msg));
}

static void test_cross_band_duplex_near(void)
{
    sim_start(0x00C0FFEEu, LC_TIER_NEAR, LC_BAND_915, LC_BAND_2G4);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_915, term.grant.dl.band);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_2G4, term.grant.ul.band);
    uint32_t before = cell.terms[0].ul_rx;
    const uint8_t msg[] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL_INT(0, lc_term_send_upper(&term, msg, sizeof(msg)));
    run_for(1200); /* 10 frames */
    TEST_ASSERT_UINT32_WITHIN(1, 10, cell.terms[0].ul_rx - before); /* every UL heard on 2.4 */
    TEST_ASSERT_EQUAL_UINT32(1, downs);
    TEST_ASSERT_TRUE(ul_err_max <= 20);
}

static void test_page_from_idle(void)
{
    sim_start(0x13572468u, LC_TIER_EDGE, LC_BAND_915, LC_BAND_915);
    cell.attach_idle = 1;
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_IDLE, term.state);
    cell.attach_idle = 0;
    lcb_cell_page(&cell, term.tmid);
    run_for(3000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT32(1, cell.page_replies);
    TEST_ASSERT_EQUAL_UINT32(0, cell.page_tmid);
}

static void test_regrant_in_dl_moves_ul_to_2g4(void)
{
    sim_start(0x2468ACE0u, LC_TIER_NEAR, LC_BAND_915, LC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    lcb_cell_set_bands(&cell, LC_BAND_915, LC_BAND_2G4);
    run_for(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_2G4, term.grant.ul.band);
    TEST_ASSERT_EQUAL_UINT32(1, cell.attaches); /* no re-attach needed */
    uint32_t before = cell.terms[0].ul_rx;
    run_for(1200);
    TEST_ASSERT_UINT32_WITHIN(1, 10, cell.terms[0].ul_rx - before);
}

static void test_2g4_loss_falls_back_to_915(void)
{
    sim_start(0x55AA55AAu, LC_TIER_MID, LC_BAND_2G4, LC_BAND_2G4);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_2G4, term.grant.dl.band);
    /* 2.4 GHz goes out of range: the terminal re-attaches on 915 and the
     * cell (like plan 4) re-grants it there. */
    cell.fallback_915 = 1;
    blocked[LC_BAND_2G4] = 1;
    run_for(10000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_915, term.grant.dl.band);
    TEST_ASSERT_EQUAL_UINT8(LC_TIER_MID, term.grant.dl.tier);
    TEST_ASSERT_EQUAL_UINT32(2, cell.attaches); /* re-attached through 915 RACH */
}

static void test_sync_loss_and_recovery(void)
{
    sim_start(0x0F1E2D3Cu, LC_TIER_EDGE, LC_BAND_915, LC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    cell.off = 1;
    run_for(5000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_SEARCH, term.state);
    TEST_ASSERT_EQUAL_UINT32(1, term.sync_losses);
    cell.off = 0;
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
}

static void test_stuck_radio_recovers(void)
{
    sim_start(0x0DDBA11u, LC_TIER_EDGE, LC_BAND_915, LC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    stuck = 1; /* IRQ line dead: every op must be stopped at its end */
    run_for(600);
    TEST_ASSERT_TRUE(term.overruns >= 5);
    stuck = 0;
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
}

static void test_stall_jumps_to_present(void)
{
    sim_start(0x5EED5EEDu, LC_TIER_MID, LC_BAND_915, LC_BAND_915);
    run_for(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    uint32_t frame_before = term.cur_frame;
    uint32_t skipped_before = term.skipped_ops;
    stall_for(600); /* 5 frames with no lc_term_step() */
    run_for(240);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, term.state);
    TEST_ASSERT_TRUE(term.cur_frame - frame_before >= 6u);
    /* Stale frames are dropped, not replayed op by op. */
    TEST_ASSERT_TRUE(term.skipped_ops - skipped_before <= 3u);
}

static void test_search_covers_every_sync_candidate(void)
{
    /* Seeds whose frame%8==0 sync channel is each of the 6 candidates. */
    for (uint32_t off = 0; off < 6u; off++) {
        sim_start(0x10000000u + off, LC_TIER_EDGE, LC_BAND_915, LC_BAND_915);
        run_for(12000);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(LC_TERM_GRANTED, term.state, "seed candidate not found");
    }
}

/* A real bs-radio W12 only opens a frame on a FIRST part (plan 2 review #4):
 * lcbench cell's single-part frames must carry FIRST | LAST. */
static void test_cell_schedules_are_first_and_last(void)
{
    static lc_msg_t m;
    lcb_cell_init(&cell, 0xCAFEF00Du, LC_TIER_EDGE, LC_BAND_915, LC_BAND_915);
    TEST_ASSERT_EQUAL_INT(0, lcb_cell_schedule(&cell, LC_BAND_915, 100, &m));
    TEST_ASSERT_EQUAL_UINT8(LC_SCHED_FLAG_FIRST | LC_SCHED_FLAG_LAST, m.u.schedule.flags);
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
    return UNITY_END();
}
