#include "unity.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lc_exec.h"
#include "lcbench_core.h"
#include "lcb_cell.h"
#include "lcb_hss.h"
#include "lcb_merge.h"
#include "exec_fixture.h" /* board A's lc_exec on a locked clock */
#include "lc_sig_crypto.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t payload[255];
static lc_msg_t m;

static void test_parse_tier_and_band(void)
{
    lc_tier_t t;
    TEST_ASSERT_EQUAL_INT(0, lcb_parse_tier("mid", &t));
    TEST_ASSERT_EQUAL_INT(LC_TIER_MID, t);
    TEST_ASSERT_EQUAL_INT(-1, lcb_parse_tier("far", &t));
    TEST_ASSERT_EQUAL_INT(LC_BAND_915, lcb_band_of(915250000u));
    TEST_ASSERT_EQUAL_INT(LC_BAND_2G4, lcb_band_of(2402000000u));
}

static void test_payload_roundtrip_and_corruption(void)
{
    uint32_t f;
    lcb_fill_payload(payload, 28, 123456);
    TEST_ASSERT_EQUAL_INT(0, lcb_check_payload(payload, 28, &f));
    TEST_ASSERT_EQUAL_UINT32(123456, f);
    payload[20] ^= 1;
    TEST_ASSERT_EQUAL_INT(-1, lcb_check_payload(payload, 28, &f));
    TEST_ASSERT_EQUAL_INT(-1, lcb_check_payload(payload, 4, &f));
}

static void test_tx_schedule_single_slot(void)
{
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_EDGE, 20000, 0, 28, 0, 0 };
    TEST_ASSERT_EQUAL_INT(0, lcb_link_schedule(&cfg, 77, 1, payload, &m));
    TEST_ASSERT_EQUAL_UINT8(LC_MSG_SCHEDULE, m.type);
    TEST_ASSERT_EQUAL_UINT8(LC_SCHED_FLAG_FIRST | LC_SCHED_FLAG_LAST, m.u.schedule.flags);
    TEST_ASSERT_EQUAL_UINT8(1, m.u.schedule.slot_count);
    const lc_slot_t *s = &m.u.schedule.slots[0];
    TEST_ASSERT_EQUAL_UINT8(LC_DIR_TX, s->dir);
    TEST_ASSERT_EQUAL_UINT32(20000, s->offset_us);
    TEST_ASSERT_EQUAL_UINT32(16704u + LC_GUARD_US, s->length_us);
    uint32_t f;
    TEST_ASSERT_EQUAL_INT(0, lcb_check_payload(s->payload, s->payload_len, &f));
    TEST_ASSERT_EQUAL_UINT32(77, f);
}

static void test_rx_window_is_centred_and_clamped(void)
{
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_EDGE, 20000, 20904, 28, 0, 0 };
    TEST_ASSERT_EQUAL_INT(0, lcb_link_schedule(&cfg, 1, 0, payload, &m));
    const lc_slot_t *s = &m.u.schedule.slots[0];
    TEST_ASSERT_EQUAL_UINT8(LC_DIR_RX, s->dir);
    /* centred on the TX slot (airtime 16704 + guard): half the spare before it */
    TEST_ASSERT_EQUAL_UINT32(20000u - (20904u - (16704u + LC_GUARD_US)) / 2u, s->offset_us);
    TEST_ASSERT_EQUAL_UINT32(20904, s->length_us);

    cfg.rx_window_us = 100000; /* wide window starting before 0 -> clamped */
    lcb_link_schedule(&cfg, 1, 0, payload, &m);
    TEST_ASSERT_EQUAL_UINT32(0, m.u.schedule.slots[0].offset_us);
    TEST_ASSERT_TRUE(m.u.schedule.slots[0].length_us <= LC_FRAME_US);
}

static void test_invalid_link_configs(void)
{
    lcb_link_cfg_t cfg = { 2402000000u, LC_TIER_EDGE, 0, 0, 28, 0, 0 }; /* no edge tier on 2.4 */
    TEST_ASSERT_EQUAL_INT(-1, lcb_link_schedule(&cfg, 1, 1, payload, &m));
    cfg = (lcb_link_cfg_t){ 915250000u, LC_TIER_EDGE, 110000, 0, 28, 0, 0 }; /* runs past frame end */
    TEST_ASSERT_EQUAL_INT(-1, lcb_link_schedule(&cfg, 1, 1, payload, &m));
    cfg.offset_us = 0;
    cfg.payload_len = 4;
    TEST_ASSERT_EQUAL_INT(-1, lcb_link_schedule(&cfg, 1, 1, payload, &m));
}

static void test_guard_schedule_places_b_after_gap(void)
{
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_NEAR, 1000, 0, 28, 0, 0 };
    TEST_ASSERT_EQUAL_INT(0, lcb_guard_schedule(&cfg, 0, LC_TIER_EDGE, LC_DIR_TX, 150, 3, 1, payload, &m));
    TEST_ASSERT_EQUAL_UINT8(2, m.u.schedule.slot_count);
    const lc_slot_t *a = &m.u.schedule.slots[0];
    const lc_slot_t *b = &m.u.schedule.slots[1];
    TEST_ASSERT_EQUAL_UINT8(LC_MOD_LORA, a->mode.modulation);
    TEST_ASSERT_EQUAL_UINT32(16704u + 150u, a->length_us);
    TEST_ASSERT_EQUAL_UINT8(LC_MOD_FLRC, b->mode.modulation);
    TEST_ASSERT_EQUAL_UINT32(1000u + 16704u + 150u, b->offset_us);
    uint32_t b_start = b->offset_us;
    uint32_t b_end = b->offset_us + b->length_us;

    /* RX board listens around B */
    TEST_ASSERT_EQUAL_INT(0, lcb_guard_schedule(&cfg, 0, LC_TIER_EDGE, LC_DIR_TX, 150, 3, 0, payload, &m));
    const lc_slot_t *w = &m.u.schedule.slots[0];
    TEST_ASSERT_EQUAL_UINT8(1, m.u.schedule.slot_count);
    TEST_ASSERT_EQUAL_UINT8(LC_DIR_RX, w->dir);
    TEST_ASSERT_TRUE(w->offset_us <= b_start);
    TEST_ASSERT_TRUE(w->offset_us + w->length_us >= b_end);
}

static void test_guard_band_switch_puts_a_on_other_band(void)
{
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_EDGE, 1000, 0, 28, 0, 0 };
    TEST_ASSERT_EQUAL_INT(0, lcb_guard_schedule(&cfg, 2440000000u, LC_TIER_NEAR, LC_DIR_TX, 500, 3, 1, payload, &m));
    TEST_ASSERT_EQUAL_UINT32(2440000000u, m.u.schedule.slots[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(1300000u, m.u.schedule.slots[0].mode.bitrate_bps); /* 2.4 near tier */
    TEST_ASSERT_EQUAL_UINT32(915250000u, m.u.schedule.slots[1].freq_hz);
    TEST_ASSERT_EQUAL_INT(-1, lcb_guard_schedule(&cfg, 2440000000u, LC_TIER_EDGE, LC_DIR_TX, 500, 3, 1, payload, &m));
}

static void test_guard_schedules_pass_firmware_validation(void)
{
    /* Both boards' schedules must be accepted by lc_exec (sorted, in frame, airtime fits). */
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_NEAR, 1000, 0, 28, 0, 0 };
    for (int dir = LC_DIR_RX; dir <= LC_DIR_TX; dir++) {
        TEST_ASSERT_EQUAL_INT(0, lcb_guard_schedule(&cfg, 0, LC_TIER_MID, (uint8_t)dir, 300, 3, 1, payload, &m));
        for (uint8_t i = 0; i < m.u.schedule.slot_count; i++) {
            const lc_slot_t *s = &m.u.schedule.slots[i];
            uint32_t air = lc_airtime_us(&s->mode, s->dir == LC_DIR_TX ? s->payload_len : 0);
            TEST_ASSERT_TRUE(air > 0);
            if (s->dir == LC_DIR_TX) TEST_ASSERT_TRUE(air <= s->length_us);
            if (i > 0) {
                const lc_slot_t *p = &m.u.schedule.slots[i - 1];
                TEST_ASSERT_TRUE(s->offset_us >= p->offset_us + p->length_us);
            }
        }
    }
}

static void test_cw_schedule_fits_firmware_limits(void)
{
    const lcb_link_cfg_t cfgs[] = {
        { 915250000u, LC_TIER_EDGE, 0, 0, 28, 0, 0 },
        { 915250000u, LC_TIER_NEAR, 0, 0, 255, 0, 0 },
        { 2402000000u, LC_TIER_NEAR, 0, 0, 28, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++) {
        int n = lcb_cw_schedule(&cfgs[i], 5, payload, &m);
        TEST_ASSERT_TRUE(n > 0);
        TEST_ASSERT_TRUE((uint32_t)n <= LC_EXEC_MAX_SLOTS);
        TEST_ASSERT_TRUE((uint32_t)n * cfgs[i].payload_len <= LC_EXEC_PAYLOAD_POOL);
        const lc_slot_t *last = &m.u.schedule.slots[n - 1];
        TEST_ASSERT_TRUE(last->offset_us + last->length_us <= LC_FRAME_US);
        uint8_t buf[LC_LINK_MAX_MSG];
        TEST_ASSERT_TRUE(lc_msg_encode(&m, buf, sizeof(buf)) > 0); /* fits one message */
    }
}

/* Packet-end timing: only good packets with a measured end count. */
static void test_stats_end_timing(void)
{
    lcb_stats_t st;
    lcb_stats_init(&st);
    lcb_fill_payload(payload, 28, 9);
    lc_rx_report_t a = { 9, 0, -90, 20, 1, 28, payload, 20100 };
    lc_rx_report_t b = { 9, 0, -90, 20, 1, 28, payload, 20140 };
    lc_rx_report_t unknown = { 9, 0, -90, 20, 1, 28, payload, LC_RX_END_UNKNOWN };
    lc_rx_report_t crc = { 9, 0, -90, 20, 0, 28, payload, 99999 };
    lcb_stats_add_rx(&st, &a);
    lcb_stats_add_rx(&st, &b);
    lcb_stats_add_rx(&st, &unknown);
    lcb_stats_add_rx(&st, &crc);
    TEST_ASSERT_EQUAL_UINT32(3, st.received);
    TEST_ASSERT_EQUAL_UINT32(2, st.timed);
    TEST_ASSERT_EQUAL_INT32(20100, st.end_min);
    TEST_ASSERT_EQUAL_INT32(20140, st.end_max);
    TEST_ASSERT_EQUAL_INT(20120000, (int)(lcb_stats_end_mean(&st) * 1000.0 + 0.5));
    TEST_ASSERT_EQUAL_INT(20000, (int)(lcb_stats_end_sd(&st) * 1000.0 + 0.5));
}

static void test_stats_accumulate(void)
{
    lcb_stats_t st;
    lcb_stats_init(&st);
    lcb_fill_payload(payload, 28, 9);
    lc_rx_report_t good = { 9, 0, -90, 20, 1, 28, payload, LC_RX_END_UNKNOWN };
    lc_rx_report_t weak = { 10, 0, -110, -8, 1, 28, payload, LC_RX_END_UNKNOWN };
    lc_rx_report_t crc = { 11, 0, -120, 0, 0, 28, payload, LC_RX_END_UNKNOWN };
    lcb_stats_add_rx(&st, &good);
    lcb_stats_add_rx(&st, &weak);
    lcb_stats_add_rx(&st, &crc);
    TEST_ASSERT_EQUAL_UINT32(2, st.received);
    TEST_ASSERT_EQUAL_UINT32(1, st.crc_fail);
    TEST_ASSERT_EQUAL_INT16(-110, st.rssi_min);
    TEST_ASSERT_EQUAL_INT16(-90, st.rssi_max);
    TEST_ASSERT_EQUAL_INT32(-200, st.rssi_sum);
    TEST_ASSERT_EQUAL_INT32(12, st.snr_sum_qdb);

    uint8_t junk[28] = { 0 };
    lc_rx_report_t bad = { 12, 0, -90, 0, 1, 28, junk, LC_RX_END_UNKNOWN };
    lcb_stats_add_rx(&st, &bad);
    TEST_ASSERT_EQUAL_UINT32(1, st.bad_payload);
}

/* Cross-band duplex probe: DL then UL in one frame, possibly on different bands. */
static void test_duplex_schedule_base_and_terminal_mirror(void)
{
    static uint8_t dl[28], ul[28];
    lcb_duplex_cfg_t cfg = { 915250000u, LC_TIER_EDGE, 2440000000u, LC_TIER_NEAR, 20000, 1500, 28 };
    uint32_t dl_len = lc_slot_len_us(lc_tier_mode(LC_BAND_915, LC_TIER_EDGE), 28);
    uint32_t ul_len = lc_slot_len_us(lc_tier_mode(LC_BAND_2G4, LC_TIER_NEAR), 28);

    TEST_ASSERT_EQUAL_INT(0, lcb_duplex_schedule(&cfg, 500, 1, dl, ul, &m));
    TEST_ASSERT_EQUAL_UINT8(LC_SCHED_FLAG_FIRST | LC_SCHED_FLAG_LAST, m.u.schedule.flags);
    TEST_ASSERT_EQUAL_UINT8(2, m.u.schedule.slot_count);
    const lc_slot_t *a = &m.u.schedule.slots[0], *b = &m.u.schedule.slots[1];
    TEST_ASSERT_EQUAL_UINT8(LC_DIR_TX, a->dir);
    TEST_ASSERT_EQUAL_UINT32(915250000u, a->freq_hz);
    TEST_ASSERT_EQUAL_UINT32(20000u, a->offset_us);
    TEST_ASSERT_EQUAL_UINT32(dl_len, a->length_us);
    uint32_t f;
    TEST_ASSERT_EQUAL_INT(0, lcb_check_payload(a->payload, a->payload_len, &f));
    TEST_ASSERT_EQUAL_UINT32(500, f);
    TEST_ASSERT_EQUAL_UINT8(LC_DIR_RX, b->dir);
    TEST_ASSERT_EQUAL_UINT32(2440000000u, b->freq_hz);
    TEST_ASSERT_EQUAL_UINT32(20000u + dl_len + 1500u, b->offset_us);
    TEST_ASSERT_EQUAL_UINT32(ul_len, b->length_us);

    TEST_ASSERT_EQUAL_INT(0, lcb_duplex_schedule(&cfg, 500, 0, dl, ul, &m));
    a = &m.u.schedule.slots[0];
    b = &m.u.schedule.slots[1];
    TEST_ASSERT_EQUAL_UINT8(LC_DIR_RX, a->dir);
    TEST_ASSERT_EQUAL_UINT32(20000u, a->offset_us);
    TEST_ASSERT_EQUAL_UINT32(dl_len, a->length_us);
    TEST_ASSERT_EQUAL_UINT8(LC_DIR_TX, b->dir);
    TEST_ASSERT_EQUAL_UINT32(20000u + dl_len + 1500u, b->offset_us);
    TEST_ASSERT_EQUAL_INT(0, lcb_check_payload(b->payload, b->payload_len, &f));
    TEST_ASSERT_EQUAL_UINT32(500, f);
}

static void test_duplex_schedule_rejects_bad_config(void)
{
    static uint8_t dl[28], ul[28];
    lcb_duplex_cfg_t cfg = { 915250000u, LC_TIER_EDGE, 2440000000u, LC_TIER_EDGE, 20000, 1500, 28 };
    TEST_ASSERT_EQUAL_INT(-1, lcb_duplex_schedule(&cfg, 1, 1, dl, ul, &m)); /* no edge tier on 2.4 */
    cfg.ul_tier = LC_TIER_NEAR;
    cfg.offset_us = 110000; /* the pair doesn't fit in the frame */
    TEST_ASSERT_EQUAL_INT(-1, lcb_duplex_schedule(&cfg, 1, 1, dl, ul, &m));
    cfg.offset_us = 20000;
    cfg.payload_len = 4; /* below LCB_MIN_PAYLOAD */
    TEST_ASSERT_EQUAL_INT(-1, lcb_duplex_schedule(&cfg, 1, 1, dl, ul, &m));
}

static uint32_t hook_tmid;
static uint8_t hook_p[32], hook_n;
static int hook_ul, hook_upper;
static void on_ul(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    hook_ul++;
    hook_tmid = tmid;
    memcpy(hook_p, p, n);
    hook_n = n;
}
static void on_upper(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    hook_upper++;
    hook_tmid = tmid;
    memcpy(hook_p, p, n);
    hook_n = n;
}

/* Slot index of the first slot of `kind` in frame f on 915, or -1. */
static int kind_slot(const lcb_cell_t *c, uint32_t f, uint8_t kind)
{
    const lcb_cell_kinds_t *kd = &c->kinds[LC_BAND_915][f % LCB_CELL_KIND_FRAMES];
    for (uint8_t i = 0; kd->frame == f && i < kd->count; i++) {
        if (kd->kind[i] == kind) return i;
    }
    return -1;
}

static void cell_rx(lcb_cell_t *c, uint32_t f, int slot, const lc_air_msg_t *a)
{
    static uint8_t buf[64];
    size_t n = lc_air_encode(a, buf, sizeof(buf));
    lc_rx_report_t r = { f, (uint8_t)slot, -60, 40, 1, (uint8_t)n, buf, LC_RX_END_UNKNOWN };
    lcb_cell_on_rx(c, LC_BAND_915, &r);
}

static void test_cell_beacon_carries_part97_flag(void)
{
    static lcb_cell_t c;
    lc_air_msg_t b;
    lcb_cell_init(&c, 0x1234u, LC_TIER_EDGE, LC_BAND_915, LC_BAND_915);
    TEST_ASSERT_EQUAL_INT(0, lcb_cell_schedule(&c, LC_BAND_915, 10, &m));
    TEST_ASSERT_EQUAL_INT(0, lc_air_decode(m.u.schedule.slots[0].payload, m.u.schedule.slots[0].payload_len, &b));
    TEST_ASSERT_EQUAL_HEX8(0, b.u.beacon.flags & LC_BCN_FLAG_PART97);
    c.part97 = 1;
    TEST_ASSERT_EQUAL_INT(0, lcb_cell_schedule(&c, LC_BAND_915, 11, &m));
    TEST_ASSERT_EQUAL_INT(0, lc_air_decode(m.u.schedule.slots[0].payload, m.u.schedule.slots[0].payload_len, &b));
    TEST_ASSERT_EQUAL_HEX8(LC_BCN_FLAG_PART97, b.u.beacon.flags & LC_BCN_FLAG_PART97);
}

/* Hooks take UL DATA and RACH UPPER; queued DL payloads go out in the DL
 * slot; release takes the legs away. */
static void test_cell_hooks_dl_queue_and_release(void)
{
    static lcb_cell_t c;
    lcb_cell_init(&c, 0x1234u, LC_TIER_EDGE, LC_BAND_915, LC_BAND_915);
    lcb_cell_hooks_t h = { NULL, on_ul, on_upper };
    lcb_cell_set_hooks(&c, &h);
    hook_ul = hook_upper = 0;
    lc_air_msg_t a;

    uint32_t f = 100;
    TEST_ASSERT_EQUAL_INT(0, lcb_cell_schedule(&c, LC_BAND_915, f, &m));
    memset(&a, 0, sizeof(a));
    a.type = LC_AIR_RACH;
    a.u.rach = (lc_rach_t){ 0x42u, LC_RACH_ATTACH, 0, NULL };
    cell_rx(&c, f, kind_slot(&c, f, LCB_SLOT_RACH), &a);
    TEST_ASSERT_FALSE(lcb_cell_granted(&c, 0x42u)); /* grant queued, not yet in force */
    for (f++; f < 120 && !lcb_cell_granted(&c, 0x42u); f++) lcb_cell_schedule(&c, LC_BAND_915, f, &m);
    TEST_ASSERT_TRUE(lcb_cell_granted(&c, 0x42u));

    static const uint8_t payload[3] = { 0x80, 0x07, 0x55 };
    TEST_ASSERT_EQUAL_INT(0, lcb_cell_dl_push(&c, 0x42u, payload, 3));
    TEST_ASSERT_EQUAL_INT(-1, lcb_cell_dl_push(&c, 0x99u, payload, 3)); /* unknown terminal */
    TEST_ASSERT_EQUAL_INT(0, lcb_cell_schedule(&c, LC_BAND_915, f, &m));
    int dl = kind_slot(&c, f, LCB_SLOT_DL), ul = kind_slot(&c, f, LCB_SLOT_UL);
    TEST_ASSERT_TRUE(dl >= 0 && ul >= 0);
    lc_air_msg_t d;
    TEST_ASSERT_EQUAL_INT(0, lc_air_decode(m.u.schedule.slots[dl].payload, m.u.schedule.slots[dl].payload_len, &d));
    TEST_ASSERT_EQUAL_UINT8(LC_AIR_DATA, d.type);
    TEST_ASSERT_EQUAL_UINT8(3, d.u.data.payload_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, d.u.data.payload, 3);

    memset(&a, 0, sizeof(a));
    a.type = LC_AIR_DATA;
    a.u.data = (lc_data_t){ 0x42u, 1, 0, 3, payload };
    cell_rx(&c, f, ul, &a);
    TEST_ASSERT_EQUAL_INT(1, hook_ul);
    TEST_ASSERT_EQUAL_UINT32(0x42u, hook_tmid);
    TEST_ASSERT_EQUAL_UINT8(3, hook_n);
    TEST_ASSERT_EQUAL_UINT8(0, c.terms[0].loop_len); /* hooked: no echo */

    static const uint8_t svc = 0x32;
    f++;
    TEST_ASSERT_EQUAL_INT(0, lcb_cell_schedule(&c, LC_BAND_915, f, &m));
    memset(&a, 0, sizeof(a));
    a.type = LC_AIR_RACH;
    a.u.rach = (lc_rach_t){ 0x42u, LC_RACH_UPPER, 1, &svc };
    cell_rx(&c, f, kind_slot(&c, f, LCB_SLOT_RACH), &a);
    TEST_ASSERT_EQUAL_INT(1, hook_upper);
    TEST_ASSERT_EQUAL_UINT8(1, hook_n);
    TEST_ASSERT_EQUAL_HEX8(0x32, hook_p[0]);

    lcb_cell_release(&c, 0x42u);
    for (f++; f < 140 && lcb_cell_granted(&c, 0x42u); f++) lcb_cell_schedule(&c, LC_BAND_915, f, &m);
    TEST_ASSERT_FALSE(lcb_cell_granted(&c, 0x42u));
}

/* Board A's lc_exec must accept every --one-board SCHEDULE once two
 * terminals hold grants (bench 2026-09-27: every one came back MALFORMED).
 * Each frame goes through lc_exec_add_part() one frame ahead, as lcbench
 * sends it; both terminals attach over RACH like on the air. */
static void check_two_terms_one_board(lc_tier_t tier, lc_band_t dl, lc_band_t ul)
{
    static lcb_cell_t c;
    static lcb_one_map_t maps[LCB_ONE_MAP_FRAMES];
    char msg[96];
    fixture_reset();
    lcb_cell_init(&c, 0x1234u, tier, dl, ul);
    const uint32_t tmid[2] = { 0x76ad0488u, 0x76ae2064u };
    int attached = 0;
    for (uint32_t f = F0 + 1; f < F0 + 70; f++) {
        if (f == F0 + 40) { /* both in force; then one released (net: 5 s of silence), one sending */
            TEST_ASSERT_TRUE_MESSAGE(lcb_cell_granted(&c, tmid[0]) && lcb_cell_granted(&c, tmid[1]), msg);
            lcb_cell_release(&c, tmid[0]);
            TEST_ASSERT_EQUAL_INT(0, lcb_cell_dl_push(&c, tmid[1], payload, LCB_CELL_PAYLOAD));
        }
        TEST_ASSERT_EQUAL_INT(0, lcb_merge_bands(&c, f, 0, maps, &m));
        uint64_t prev_start;
        TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, f - 1, &prev_start));
        snprintf(msg, sizeof(msg), "tier %d dl %d ul %d frame +%u, %d attached, %u slots", (int)tier, (int)dl,
                 (int)ul, (unsigned)(f - F0), attached, m.u.schedule.slot_count);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(LC_ACK_OK, lc_exec_add_part(&exec_, &m.u.schedule, &clk, prev_start + 10000),
                                        msg);
        if (attached < 2 && (f - F0) % 4 == 1) { /* the next terminal's ATTACH in this frame's RACH */
            lc_air_msg_t a;
            memset(&a, 0, sizeof(a));
            a.type = LC_AIR_RACH;
            a.u.rach = (lc_rach_t){ tmid[attached++], LC_RACH_ATTACH, 0, NULL };
            cell_rx(&c, f, kind_slot(&c, f, LCB_SLOT_RACH), &a);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(!lcb_cell_granted(&c, tmid[0]) && lcb_cell_granted(&c, tmid[1]), msg);
}

static void test_cell_two_terminals_one_board_pass_firmware_validation(void)
{
    for (int t = 0; t < (int)LC_TIER_COUNT; t++) {
        for (int d = 0; d < (int)LC_BAND_COUNT; d++) {
            for (int u = 0; u < (int)LC_BAND_COUNT; u++) {
                static lcb_cell_t probe;
                lc_grant_leg_t l1, l2;
                lcb_cell_init(&probe, 0x1234u, (lc_tier_t)t, (lc_band_t)d, (lc_band_t)u);
                if (lcb_cell_legs(&probe, 1, &l1, &l2) == 0) { /* lcbench cell refuses the others */
                    check_two_terms_one_board((lc_tier_t)t, (lc_band_t)d, (lc_band_t)u);
                }
            }
        }
    }
}

static uint8_t rnd_ctr;
static void test_rnd(uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(rnd_ctr++ * 73u + 5u);
}

/* The HSS file round-trips every field, re-issuing a number replaces its
 * token, a missing file is an empty HSS and a damaged one is refused. */
static void test_hss_file_roundtrip(void)
{
    static const char *path = "test_hss.txt";
    static lcb_hss_t a, b;
    unlink(path);
    TEST_ASSERT_EQUAL_INT(0, lcb_hss_load(&a, path));
    TEST_ASSERT_FALSE(a.have_network);
    TEST_ASSERT_EQUAL_INT(0, lcb_hss_ensure_network(&a, test_rnd));
    uint8_t pk[32];
    lc_sig_x25519_public(a.sk, pk);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pk, a.pk, 32);
    uint8_t num[LC_SIG_NUMBER_LEN];
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+883160655501234", 16, num));
    lc_sig_sub_t *s = lcb_hss_issue(&a, num, 1790003600u, test_rnd);
    TEST_ASSERT_NOT_NULL(s);
    uint8_t first_token[8];
    memcpy(first_token, s->token_id, 8);
    s->tmid = 0x76ad0488u;
    s->activated = 1;
    s->token_used = 1;
    memset(s->k, 0x4b, 16);
    memset(s->opc, 0x0c, 16);
    s->sqn[5] = 0x20;
    a.mode = LC_SIG_MODE_PART97;
    TEST_ASSERT_EQUAL_INT(0, lcb_hss_save(&a, path));
    TEST_ASSERT_EQUAL_INT(0, lcb_hss_load(&b, path));
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    TEST_ASSERT_EQUAL_PTR(&b.subs[0], lcb_hss_by_tmid(&b, 0x76ad0488u));
    TEST_ASSERT_EQUAL_PTR(&b.subs[0], lcb_hss_by_token(&b, first_token));
    char text[LC_SIG_NUMBER_TEXT];
    lc_sig_number_to_text(b.subs[0].number, text);
    TEST_ASSERT_EQUAL_STRING("+883160655501234", text);
    TEST_ASSERT_EQUAL_STRING("", b.err);

    s = lcb_hss_issue(&b, num, 1790007200u, test_rnd); /* re-issued: same record, new unused token */
    TEST_ASSERT_EQUAL_PTR(&b.subs[0], s);
    TEST_ASSERT_EQUAL_UINT(1, b.n);
    TEST_ASSERT_FALSE(s->token_used);
    TEST_ASSERT_TRUE(memcmp(first_token, s->token_id, 8) != 0);
    lc_sig_qr_t q;
    lcb_hss_qr(&b, s, &q);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b.pk, q.pkn, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s->token_secret, q.token_secret, 16);

    FILE *f = fopen(path, "a");
    fputs("sub number=+883160655500000 token_id=zz\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT(-1, lcb_hss_load(&b, path));
    TEST_ASSERT_EQUAL_STRING("test_hss.txt:4: malformed line", b.err);
    unlink(path);
}

/* numbering v2 §6.4: an HSS written before numbering v2 is refused with a
 * message that says what to do, not just "malformed". */
static void test_hss_v1_number_refused_with_migration_message(void)
{
    static const char *path = "test_hss_v1.txt";
    static lcb_hss_t h;
    FILE *f = fopen(path, "w");
    fputs("# OpenCell network stand-in HSS (lcbench). Holds secrets: keep it private.\n", f);
    fputs("sub number=+8836065551234 token_id=a0a1a2a3a4a5a6a7 token_secret=b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
          " expiry=1790003600 used=1 tmid=76ad0488 activated=1 k=4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
          " opc=0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c sqn=000000000020\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT(-1, lcb_hss_load(&h, path));
    TEST_ASSERT_EQUAL_STRING(
        "test_hss_v1.txt:2: 13-digit number (numbering v1): remove the sub lines and issue new codes", h.err);
    unlink(path);
    uint8_t num[LC_SIG_NUMBER_LEN];
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+8836065551234", 14, num)); /* well-formed (CC 60)... */
    TEST_ASSERT_TRUE(lcb_hss_v1_number(num));                                   /* ...but a v1 leftover */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+883160655501234", 16, num));
    TEST_ASSERT_FALSE(lcb_hss_v1_number(num));
}

/* Final review M1: the migration message must survive a long HSS path.
 * err[] used to be sized only for a short path, so the "%s:%u: %s" snprintf
 * cut the reason text off the end when path was long. */
static void test_hss_v1_migration_message_survives_long_path(void)
{
    char dir[200];
    memset(dir, 'x', sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(0, mkdir(dir, 0700));
    char path[240];
    snprintf(path, sizeof(path), "%s/hss.txt", dir);

    FILE *f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("# OpenCell network stand-in HSS (lcbench). Holds secrets: keep it private.\n", f);
    fputs("sub number=+8836065551234 token_id=a0a1a2a3a4a5a6a7 token_secret=b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
          " expiry=1790003600 used=1 tmid=76ad0488 activated=1 k=4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
          " opc=0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c sqn=000000000020\n", f);
    fclose(f);

    static lcb_hss_t h;
    TEST_ASSERT_EQUAL_INT(-1, lcb_hss_load(&h, path));
    TEST_ASSERT_NOT_NULL(strstr(h.err, "13-digit number (numbering v1): remove the sub lines and issue new codes"));

    unlink(path);
    rmdir(dir);
}

/* Final review I4: `lcbench net` holds the HSS lock for its lifetime, so a
 * `mkqr` meanwhile (whose token net's next save would erase) is refused.
 * flock locks belong to the open file description, so two lock attempts in
 * one process conflict exactly as two processes would. */
static void test_hss_lock_is_exclusive(void)
{
    static const char *path = "test_hss_lock.txt";
    int a = lcb_hss_lock(path);
    TEST_ASSERT_TRUE(a >= 0);
    TEST_ASSERT_EQUAL_INT(-1, lcb_hss_lock(path)); /* held: refused at once, not waited for */
    lcb_hss_unlock(a);
    int b = lcb_hss_lock(path);
    TEST_ASSERT_TRUE(b >= 0); /* free again */
    lcb_hss_unlock(b);
    TEST_ASSERT_EQUAL_INT(-2, lcb_hss_lock("no-such-dir/hss.txt")); /* can't make the lock file */
    unlink("test_hss_lock.txt.lock");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hss_file_roundtrip);
    RUN_TEST(test_hss_v1_number_refused_with_migration_message);
    RUN_TEST(test_hss_v1_migration_message_survives_long_path);
    RUN_TEST(test_hss_lock_is_exclusive);
    RUN_TEST(test_parse_tier_and_band);
    RUN_TEST(test_payload_roundtrip_and_corruption);
    RUN_TEST(test_tx_schedule_single_slot);
    RUN_TEST(test_rx_window_is_centred_and_clamped);
    RUN_TEST(test_invalid_link_configs);
    RUN_TEST(test_guard_schedule_places_b_after_gap);
    RUN_TEST(test_guard_band_switch_puts_a_on_other_band);
    RUN_TEST(test_guard_schedules_pass_firmware_validation);
    RUN_TEST(test_cw_schedule_fits_firmware_limits);
    RUN_TEST(test_stats_accumulate);
    RUN_TEST(test_stats_end_timing);
    RUN_TEST(test_duplex_schedule_base_and_terminal_mirror);
    RUN_TEST(test_duplex_schedule_rejects_bad_config);
    RUN_TEST(test_cell_hooks_dl_queue_and_release);
    RUN_TEST(test_cell_beacon_carries_part97_flag);
    RUN_TEST(test_cell_two_terminals_one_board_pass_firmware_validation);
    return UNITY_END();
}
