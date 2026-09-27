/* What the terminal hears while searching (spec 2026-09-27-ble-pairing-design.md
 * §3.1): the strongest packet of the recent scan, CRC good or not, from any
 * cell, and the noise floor from the optional rssi_inst radio op.
 *
 * A scripted fake radio: RX runs from launch() until its timeout; a packet
 * scripted at time `at` ends the RX that is running then. */
#include "unity.h"

#include <string.h>

#include "lc_term.h"
#include "lc_term_gatt.h"

void setUp(void) {}
void tearDown(void) {}

#define T0 1000000u

typedef struct {
    uint64_t at;
    uint8_t  crc_ok;
    int16_t  rssi;
    int16_t  snr;
    uint8_t  len;
    uint8_t  data[LC_AIR_MAX_FRAME];
} pkt_t;

static lc_term_t term;
static uint64_t now;        /* the time of the lc_term_step being run */
static int rx_on;
static uint64_t rx_end;
static uint32_t rx_timeout;
static uint32_t launches;
static uint32_t rssi_calls;
static int16_t noise_now;   /* what rssi_inst reads */
static uint32_t statuses;   /* on_status calls */
static pkt_t pkts[8];
static unsigned n_pkts, next_pkt;

static int f_configure(void *c, uint32_t f, const lc_mode_t *m) { (void)c; (void)f; (void)m; return 0; }
static int f_stage_tx(void *c, const uint8_t *d, uint8_t l) { (void)c; (void)d; (void)l; return -1; }
static int f_stage_rx(void *c, uint32_t t) { (void)c; rx_timeout = t; return 0; }
static void f_standby(void *c) { (void)c; rx_on = 0; }

static int f_launch(void *c, uint64_t at_us)
{
    (void)c;
    rx_on = 1;
    rx_end = (at_us > now ? at_us : now) + rx_timeout;
    launches++;
    return 0;
}

static int f_poll(void *c, lc_radio_event_t *ev)
{
    (void)c;
    if (!rx_on) {
        return 0;
    }
    memset(ev, 0, sizeof(*ev));
    if (next_pkt < n_pkts && pkts[next_pkt].at <= now) {
        const pkt_t *p = &pkts[next_pkt++];
        rx_on = 0;
        ev->type = LC_RADIO_EV_RX_DONE;
        ev->crc_ok = p->crc_ok;
        ev->len = p->len;
        memcpy(ev->data, p->data, p->len);
        ev->rssi_dbm = p->rssi;
        ev->snr_qdb = p->snr;
        return 1;
    }
    if (now >= rx_end) {
        rx_on = 0;
        ev->type = LC_RADIO_EV_RX_TIMEOUT;
        return 1;
    }
    return 0;
}

static int f_rssi_inst(void *c, int16_t *dbm)
{
    (void)c;
    rssi_calls++;
    TEST_ASSERT_TRUE_MESSAGE(rx_on, "rssi_inst outside RX");
    *dbm = noise_now;
    return 0;
}

static void on_status(void *ctx)
{
    (void)ctx;
    statuses++;
}

static uint64_t pass_us(void)
{
    return (uint64_t)(lc_num_channels(LC_BAND_915) / LC_NUM_SYNC_CHANNELS) * LC_TERM_SEARCH_DWELL_US;
}

static void start(int with_rssi_inst)
{
    const lc_radio_ops_t ops = { NULL,     f_configure, f_stage_tx, f_stage_rx,
                                 f_launch, f_poll,      f_standby,  with_rssi_inst ? f_rssi_inst : NULL };
    const lc_term_sink_t sink = { NULL, NULL, on_status, NULL };
    lc_term_init(&term, &ops, &sink, 0x11223344u);
    now = T0;
    rx_on = 0;
    launches = rssi_calls = statuses = 0;
    n_pkts = next_pkt = 0;
    noise_now = -118;
}

/* A packet that decodes to nothing (bytes that are no air message). */
static void add_pkt(uint64_t at, uint8_t crc_ok, int16_t rssi, int16_t snr)
{
    pkt_t *p = &pkts[n_pkts++];
    memset(p, 0, sizeof(*p));
    p->at = at;
    p->crc_ok = crc_ok;
    p->rssi = rssi;
    p->snr = snr;
    p->len = 4;
    memset(p->data, 0xA5, p->len);
}

static void run_until(uint64_t t_end)
{
    while (now < t_end) {
        uint64_t next = lc_term_step(&term, now);
        now = next > now ? next : now;
    }
}

static lc_term_status_t status(void)
{
    lc_term_status_t st;
    lc_term_status(&term, &st);
    return st;
}

static void test_every_packet_counts_and_the_strongest_is_kept(void)
{
    start(1);
    add_pkt(T0 + 100000, 1, -105, -20); /* CRC good, but no air message */
    add_pkt(T0 + 200000, 0, -97, -13);  /* CRC error: still heard, and the strongest */
    add_pkt(T0 + 300000, 0, -110, -40); /* weaker: doesn't replace -97 */
    run_until(T0 + 400000);
    lc_term_status_t st = status();
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_SEARCH, st.state);
    TEST_ASSERT_EQUAL_UINT8(1, st.heard);
    TEST_ASSERT_EQUAL_INT16(-97, st.rssi_dbm);
    TEST_ASSERT_EQUAL_INT16(-13, st.snr_qdb);
    TEST_ASSERT_EQUAL_UINT16(0, st.heard_age_s);
    TEST_ASSERT_EQUAL_UINT32(1, statuses); /* one notification: NO SIGNAL -> a signal */
    TEST_ASSERT_EQUAL_UINT32(4, launches); /* RX re-armed after each packet */

    run_until(T0 + 2500000);
    st = status();
    TEST_ASSERT_EQUAL_UINT16(2, st.heard_age_s); /* 2.2 s after the last packet */
    TEST_ASSERT_EQUAL_INT16(-97, st.rssi_dbm);
}

static void test_the_record_lasts_one_full_pass_then_clears(void)
{
    start(0);
    add_pkt(T0 + 100000, 0, -101, -30);
    run_until(T0 + pass_us() + 100000); /* first pass over: now the last full pass */
    lc_term_status_t st = status();
    TEST_ASSERT_EQUAL_UINT8(1, st.heard);
    TEST_ASSERT_EQUAL_INT16(-101, st.rssi_dbm);
    run_until(T0 + 2 * pass_us() - 100000); /* nearly through a pass with nothing heard */
    TEST_ASSERT_EQUAL_UINT8(1, status().heard);
    run_until(T0 + 2 * pass_us() + 100000); /* a whole pass heard nothing */
    st = status();
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_SEARCH, st.state);
    TEST_ASSERT_EQUAL_UINT8(0, st.heard);
    TEST_ASSERT_EQUAL_INT16(0, st.rssi_dbm);
    TEST_ASSERT_EQUAL_INT16(0, st.snr_qdb);
    TEST_ASSERT_EQUAL_UINT16(0, st.heard_age_s);
    TEST_ASSERT_EQUAL_UINT32(3, statuses); /* the first packet, and the end of each pass */
}

static void test_noise_floor_is_the_lowest_sample_of_the_scan(void)
{
    uint32_t cands = lc_num_channels(LC_BAND_915) / LC_NUM_SYNC_CHANNELS;
    start(1);
    noise_now = -112;
    run_until(T0 + LC_TERM_SEARCH_DWELL_US - 100000); /* sampled only near the dwell's end */
    TEST_ASSERT_EQUAL_UINT32(0, rssi_calls);
    TEST_ASSERT_EQUAL_INT16(LC_TERM_NO_DBM, status().noise_dbm);
    run_until(T0 + LC_TERM_SEARCH_DWELL_US + 100000);
    TEST_ASSERT_EQUAL_UINT32(1, rssi_calls);
    TEST_ASSERT_EQUAL_INT16(-112, status().noise_dbm);
    noise_now = -120;
    run_until(T0 + 2 * LC_TERM_SEARCH_DWELL_US + 100000);
    TEST_ASSERT_EQUAL_INT16(-120, status().noise_dbm);
    noise_now = -115;
    run_until(T0 + pass_us() + 100000);
    TEST_ASSERT_EQUAL_UINT32(cands, rssi_calls); /* once per dwell */
    TEST_ASSERT_EQUAL_INT16(-120, status().noise_dbm);
    TEST_ASSERT_EQUAL_UINT8(0, status().heard); /* noise alone is not a signal */
    run_until(T0 + 2 * pass_us() + 100000); /* a pass of -115 only: -120 is forgotten */
    TEST_ASSERT_EQUAL_UINT32(2 * cands, rssi_calls);
    TEST_ASSERT_EQUAL_INT16(-115, status().noise_dbm);
}

static void test_without_rssi_inst_there_is_no_noise_floor(void)
{
    start(0);
    run_until(T0 + pass_us() + 100000);
    lc_term_status_t st = status();
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_SEARCH, st.state);
    TEST_ASSERT_EQUAL_INT16(LC_TERM_NO_DBM, st.noise_dbm);
    TEST_ASSERT_EQUAL_UINT8(0, st.heard);
    TEST_ASSERT_EQUAL_UINT32(0, rssi_calls);
}

/* BLE STATUS keeps its 20 bytes: while searching, bytes 4-7 carry the
 * strongest packet heard, 0 when nothing was. */
static void test_status_bytes_while_searching(void)
{
    uint8_t out[LC_GATT_STATUS_LEN];
    start(1);
    add_pkt(T0 + 100000, 0, -97, -13);
    run_until(T0 + 200000);
    lc_term_status_t st = status();
    lc_term_pack_status(&st, out);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_SEARCH, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x9F, out[4]); /* -97 */
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[5]);
    TEST_ASSERT_EQUAL_HEX8(0xF3, out[6]); /* -13 */
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[7]);
    run_until(T0 + 2 * pass_us() + 100000);
    st = status();
    lc_term_pack_status(&st, out);
    static const uint8_t none[4] = { 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(none, out + 4, 4);
}

/* On sync the cell's packets take over; after a sync loss the scan starts
 * afresh (nothing heard before the loss is shown). */
static void test_sync_hands_over_and_a_sync_loss_starts_a_fresh_scan(void)
{
    start(1);
    add_pkt(T0 + 100000, 0, -101, -30);
    lc_air_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_AIR_BEACON;
    m.u.beacon.cell_seed = 0xCAFEF00Du;
    m.u.beacon.frame_number = 5000;
    m.u.beacon.band = LC_BAND_915; /* not accepting attach: the terminal stays SYNCED */
    pkt_t *b = &pkts[n_pkts++];
    memset(b, 0, sizeof(*b));
    b->at = T0 + 500000;
    b->crc_ok = 1;
    b->rssi = -88;
    b->snr = 24;
    b->len = (uint8_t)lc_air_encode(&m, b->data, sizeof(b->data));
    TEST_ASSERT_TRUE(b->len > 0);
    run_until(T0 + 600000);
    lc_term_status_t st = status();
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_SYNCED, st.state);
    TEST_ASSERT_EQUAL_INT16(-88, st.rssi_dbm); /* the cell's beacon, not the scan's -101 */
    TEST_ASSERT_EQUAL_INT16(24, st.snr_qdb);
    TEST_ASSERT_EQUAL_UINT8(0, st.heard);
    TEST_ASSERT_EQUAL_INT16(LC_TERM_NO_DBM, st.noise_dbm);

    run_until(T0 + 600000 + (LC_TERM_SYNC_LOSS_FRAMES + 3u) * LC_FRAME_US); /* no more beacons */
    st = status();
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_SEARCH, st.state);
    TEST_ASSERT_EQUAL_UINT32(1, term.sync_losses);
    TEST_ASSERT_EQUAL_UINT8(0, st.heard);
    TEST_ASSERT_EQUAL_INT16(0, st.rssi_dbm);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_packet_counts_and_the_strongest_is_kept);
    RUN_TEST(test_the_record_lasts_one_full_pass_then_clears);
    RUN_TEST(test_noise_floor_is_the_lowest_sample_of_the_scan);
    RUN_TEST(test_without_rssi_inst_there_is_no_noise_floor);
    RUN_TEST(test_status_bytes_while_searching);
    RUN_TEST(test_sync_hands_over_and_a_sync_loss_starts_a_fresh_scan);
    return UNITY_END();
}
