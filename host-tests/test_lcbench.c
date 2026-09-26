#include "unity.h"

#include <string.h>

#include "lc_exec.h"
#include "lcbench_core.h"

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
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_EDGE, 20000, 0, 28 };
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
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_EDGE, 20000, 20904, 28 };
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
    lcb_link_cfg_t cfg = { 2402000000u, LC_TIER_EDGE, 0, 0, 28 }; /* no edge tier on 2.4 */
    TEST_ASSERT_EQUAL_INT(-1, lcb_link_schedule(&cfg, 1, 1, payload, &m));
    cfg = (lcb_link_cfg_t){ 915250000u, LC_TIER_EDGE, 110000, 0, 28 }; /* runs past frame end */
    TEST_ASSERT_EQUAL_INT(-1, lcb_link_schedule(&cfg, 1, 1, payload, &m));
    cfg.offset_us = 0;
    cfg.payload_len = 4;
    TEST_ASSERT_EQUAL_INT(-1, lcb_link_schedule(&cfg, 1, 1, payload, &m));
}

static void test_guard_schedule_places_b_after_gap(void)
{
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_NEAR, 1000, 0, 28 };
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
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_EDGE, 1000, 0, 28 };
    TEST_ASSERT_EQUAL_INT(0, lcb_guard_schedule(&cfg, 2440000000u, LC_TIER_NEAR, LC_DIR_TX, 500, 3, 1, payload, &m));
    TEST_ASSERT_EQUAL_UINT32(2440000000u, m.u.schedule.slots[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(1300000u, m.u.schedule.slots[0].mode.bitrate_bps); /* 2.4 near tier */
    TEST_ASSERT_EQUAL_UINT32(915250000u, m.u.schedule.slots[1].freq_hz);
    TEST_ASSERT_EQUAL_INT(-1, lcb_guard_schedule(&cfg, 2440000000u, LC_TIER_EDGE, LC_DIR_TX, 500, 3, 1, payload, &m));
}

static void test_guard_schedules_pass_firmware_validation(void)
{
    /* Both boards' schedules must be accepted by lc_exec (sorted, in frame, airtime fits). */
    lcb_link_cfg_t cfg = { 915250000u, LC_TIER_NEAR, 1000, 0, 28 };
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
        { 915250000u, LC_TIER_EDGE, 0, 0, 28 },
        { 915250000u, LC_TIER_NEAR, 0, 0, 255 },
        { 2402000000u, LC_TIER_NEAR, 0, 0, 28 },
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

static void test_stats_accumulate(void)
{
    lcb_stats_t st;
    lcb_stats_init(&st);
    lcb_fill_payload(payload, 28, 9);
    lc_rx_report_t good = { 9, 0, -90, 20, 1, 28, payload };
    lc_rx_report_t weak = { 10, 0, -110, -8, 1, 28, payload };
    lc_rx_report_t crc = { 11, 0, -120, 0, 0, 28, payload };
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
    lc_rx_report_t bad = { 12, 0, -90, 0, 1, 28, junk };
    lcb_stats_add_rx(&st, &bad);
    TEST_ASSERT_EQUAL_UINT32(1, st.bad_payload);
}

int main(void)
{
    UNITY_BEGIN();
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
    return UNITY_END();
}
