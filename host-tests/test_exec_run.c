#include "unity.h"

#include "exec_fixture.h"

void setUp(void) { fixture_reset(); }
void tearDown(void) {}

#define T1 (T0 + 120000ull) /* start of frame F0 + 1 */

static const uint8_t voice[28] = { 9, 8, 7 };
static oc_schedule_t part;

/* Frame F0+1: TX at 0 ms, RX at 20 ms (edge tier, 17 ms slots). */
static void schedule_tx_rx(void)
{
    memset(&part, 0, sizeof(part));
    part.frame_number = F0 + 1;
    part.flags = OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST;
    part.slot_count = 2;
    part.slots[0] = tx_slot(0, 17000, voice, sizeof(voice));
    part.slots[1] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, T0 + 10000));
}

static void test_slots_launch_exactly_on_time(void)
{
    schedule_tx_rx();
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(OC_RADIO_EV_RX_DONE, -97, 26);
    run_until(T0 + 20000, T1 + 120000);

    int c0 = find_call(CALL_CONFIGURE, 0);
    TEST_ASSERT_EQUAL_UINT64(T1 - OC_EXEC_CONFIG_LEAD_US, fake.calls[c0].at_us);
    TEST_ASSERT_EQUAL_UINT32(915250000u, fake.calls[c0].arg);
    TEST_ASSERT_EQUAL_UINT32(28, fake.calls[find_call(CALL_STAGE_TX, 0)].arg);
    /* The radio gets the slot start ahead of time and starts on it itself. */
    TEST_ASSERT_EQUAL_UINT64(T1 - OC_RADIO_ARM_US, fake.calls[find_call(CALL_LAUNCH, 0)].at_us);
    TEST_ASSERT_EQUAL_UINT64(T1, fake.calls[find_call(CALL_LAUNCH, 0)].target_us);

    int c1 = find_call(CALL_CONFIGURE, 1);
    TEST_ASSERT_EQUAL_UINT64(T1 + 20000 - OC_EXEC_CONFIG_LEAD_US, fake.calls[c1].at_us);
    TEST_ASSERT_EQUAL_UINT32(17000 - OC_GUARD_US, fake.calls[find_call(CALL_STAGE_RX, 0)].arg);
    TEST_ASSERT_EQUAL_UINT64(T1 + 20000 - OC_RADIO_ARM_US, fake.calls[find_call(CALL_LAUNCH, 1)].at_us);
    TEST_ASSERT_EQUAL_UINT64(T1 + 20000, fake.calls[find_call(CALL_LAUNCH, 1)].target_us);
}

static void test_rx_done_is_reported_with_frame_and_slot(void)
{
    schedule_tx_rx();
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(OC_RADIO_EV_RX_DONE, -97, 26);
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_INT(1, fake.rx_count);
    TEST_ASSERT_EQUAL_UINT32(F0 + 1, fake.rx_frame);
    TEST_ASSERT_EQUAL_UINT8(1, fake.rx_slot);
    TEST_ASSERT_EQUAL_INT16(-97, fake.rx_ev.rssi_dbm);
    TEST_ASSERT_EQUAL_INT16(26, fake.rx_ev.snr_qdb);
}

static void test_rx_timeout_is_not_reported(void)
{
    schedule_tx_rx();
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_INT(0, fake.rx_count);
    TEST_ASSERT_EQUAL_UINT32(0, exec_.overruns);
}

static void test_missing_schedule_counts_miss_only_after_first(void)
{
    run_until(T0 + 20000, T1 + 120000); /* never scheduled: not a miss */
    TEST_ASSERT_EQUAL_UINT16(0, exec_.schedule_misses);

    fixture_reset();
    schedule_tx_rx();
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T0 + 20000, T1 + 120000u + 5000u); /* runs F0+1, then F0+2 has nothing */
    TEST_ASSERT_EQUAL_UINT16(1, exec_.schedule_misses);
    TEST_ASSERT_EQUAL_INT(0, count_calls(CALL_STAGE_TX) - 1); /* nothing sent in F0+2 */
}

static void test_tx_refused_when_disabled(void)
{
    schedule_tx_rx();
    oc_exec_set_tx_enabled(&exec_, 0);
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_INT(0, count_calls(CALL_STAGE_TX));
    TEST_ASSERT_EQUAL_UINT32(1, exec_.tx_blocked);
    TEST_ASSERT_EQUAL_INT(1, count_calls(CALL_STAGE_RX));
}

static void test_stalled_task_skips_late_slot(void)
{
    schedule_tx_rx();
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T1 + 5000, T1 + 120000); /* first wake 5 ms after slot 0 start */
    TEST_ASSERT_EQUAL_UINT32(1, exec_.late_slots);
    TEST_ASSERT_EQUAL_INT(0, count_calls(CALL_STAGE_TX));
    TEST_ASSERT_EQUAL_INT(1, count_calls(CALL_STAGE_RX));
}

static void test_radio_that_never_finishes_is_stopped_at_slot_end(void)
{
    schedule_tx_rx();
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    /* no event for the RX slot */
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_UINT32(1, exec_.overruns);
    int sb = find_call(CALL_STANDBY, 0);
    TEST_ASSERT_TRUE(sb >= 0);
    TEST_ASSERT_EQUAL_UINT64(T1 + 20000 + 17000, fake.calls[sb].at_us);
}

static void test_configure_error_skips_only_that_slot(void)
{
    schedule_tx_rx();
    fake.fail_configure = -706;
    run_until(T0 + 20000, T1 + 10000);
    fake.fail_configure = 0;
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T1 + 10000, T1 + 120000);
    TEST_ASSERT_EQUAL_UINT32(1, exec_.radio_errors);
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_OP_CONFIGURE, exec_.last_radio_op); /* which step, for diagnosis */
    TEST_ASSERT_EQUAL_INT16(-706, exec_.last_radio_err);
    TEST_ASSERT_EQUAL_INT(1, count_calls(CALL_STAGE_RX));
}

static void test_clock_loss_stops_frame(void)
{
    schedule_tx_rx();
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    run_until(T0 + 20000, T1 + 25000); /* RX slot active, no event */
    oc_clock_tick(&clk, T0 + 31000000u); /* holdover expired */
    fake.now = T1 + 26000;
    uint64_t next = oc_exec_step(&exec_, &clk, T1 + 26000);
    TEST_ASSERT_EQUAL_UINT64(T1 + 26000 + OC_EXEC_IDLE_US, next);
    TEST_ASSERT_EQUAL_UINT8(CALL_STANDBY, fake.calls[fake.n - 1].kind);
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_EMPTY, exec_.frames[0].state);
}

/* Review #2: entering frame f+1 a configure-lead early must survive the next
 * step, where oc_clock_frame_at still reports f until the boundary. */
static void sched_one(uint32_t frame, uint32_t off)
{
    static const uint8_t v[28] = { 9, 8, 7 };
    static oc_schedule_t part;
    memset(&part, 0, sizeof(part));
    part.frame_number = frame;
    part.flags = OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST;
    part.slot_count = 1;
    part.slots[0] = tx_slot(off, 17000, v, sizeof(v));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, T0 + 10000));
}

static void test_first_slot_inside_config_lead_still_runs(void)
{
    sched_one(F0 + 1, OC_GUARD_US);
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    run_until(T0 + 20000, T0 + 120000u + 60000u);
    TEST_ASSERT_EQUAL_INT(1, count_calls(CALL_LAUNCH));
    TEST_ASSERT_EQUAL_UINT16(0, exec_.schedule_misses);
}

static void test_offset0_with_fast_crystal_still_runs(void)
{
    oc_clock_init(&clk, 30000000u); /* local clock 12 ppm fast */
    for (int i = 0; i < 4; i++) {
        oc_clock_on_pps(&clk, T0 - 3000036ull + (uint64_t)i * 1000012u);
    }
    oc_clock_on_time(&clk, UTC0, T0 + 1000);
    sched_one(F0 + 1, 0);
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    uint64_t s;
    TEST_ASSERT_EQUAL_INT(0, oc_clock_frame_start_us(&clk, F0 + 1, &s));
    run_until(T0 + 20000, s + 60000);
    TEST_ASSERT_EQUAL_INT(1, count_calls(CALL_LAUNCH));
}

/* A slot on the other band starts its configuration OC_EXEC_BAND_SWITCH_LEAD_US
 * early (the LR2021 needs ms to move RX path and PA); same band keeps the
 * normal lead. */
static void test_band_change_gets_longer_lead(void)
{
    static oc_schedule_t part;
    static const uint8_t pl[28] = { 1 };
    memset(&part, 0, sizeof(part));
    part.frame_number = F0 + 1;
    part.flags = OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST;
    part.slot_count = 3;
    part.slots[0] = tx_slot(0, 17000, pl, sizeof(pl));
    part.slots[1] = rx_slot(40000, 17000);
    part.slots[1].freq_hz = 2440000000u;
    part.slots[1].mode = *oc_tier_mode(OC_BAND_2G4, OC_TIER_NEAR);
    part.slots[2] = rx_slot(80000, 17000);
    part.slots[2].freq_hz = 2450000000u;
    part.slots[2].mode = *oc_tier_mode(OC_BAND_2G4, OC_TIER_NEAR);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, T0 + 10000));
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_UINT64(T1 - OC_EXEC_CONFIG_LEAD_US, fake.calls[find_call(CALL_CONFIGURE, 0)].at_us);
    TEST_ASSERT_EQUAL_UINT64(T1 + 40000 - OC_EXEC_BAND_SWITCH_LEAD_US, fake.calls[find_call(CALL_CONFIGURE, 1)].at_us);
    TEST_ASSERT_EQUAL_UINT64(T1 + 80000 - OC_EXEC_CONFIG_LEAD_US, fake.calls[find_call(CALL_CONFIGURE, 2)].at_us);
    TEST_ASSERT_EQUAL_UINT32(0, exec_.late_slots);
}

/* LoRa <-> FLRC on the same band: OC_EXEC_MOD_SWITCH_LEAD_US; same
 * modulation keeps the normal lead. */
static void test_modulation_change_gets_longer_lead(void)
{
    static oc_schedule_t part;
    static const uint8_t pl[28] = { 1 };
    memset(&part, 0, sizeof(part));
    part.frame_number = F0 + 1;
    part.flags = OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST;
    part.slot_count = 3;
    part.slots[0] = tx_slot(0, 17000, pl, sizeof(pl));                         /* LoRa edge */
    part.slots[1] = rx_slot(40000, 17000);
    part.slots[1].mode = *oc_tier_mode(OC_BAND_915, OC_TIER_NEAR);             /* FLRC */
    part.slots[2] = rx_slot(80000, 17000);
    part.slots[2].mode = *oc_tier_mode(OC_BAND_915, OC_TIER_NEAR);             /* FLRC again */
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, T0 + 10000));
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_UINT64(T1 + 40000 - OC_EXEC_MOD_SWITCH_LEAD_US, fake.calls[find_call(CALL_CONFIGURE, 1)].at_us);
    TEST_ASSERT_EQUAL_UINT64(T1 + 80000 - OC_EXEC_CONFIG_LEAD_US, fake.calls[find_call(CALL_CONFIGURE, 2)].at_us);
}

/* Timing: TX done is kept (µs from frame start) for STATUS. */
static void test_tx_done_offset_is_kept(void)
{
    TEST_ASSERT_EQUAL_INT32(OC_RX_END_UNKNOWN, exec_.last_tx_end_us);
    schedule_tx_rx();
    queue_event(OC_RADIO_EV_TX_DONE, 0, 0);
    fake.queue[fake.qn - 1].irq_us = T1 + 12345u;
    fake.queue[fake.qn - 1].start_us = T1 + 110u;
    queue_event(OC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_INT32(12345, exec_.last_tx_end_us);
    TEST_ASSERT_EQUAL_INT32(110, exec_.last_tx_start_us);
}

/* Timing: the executor turns the radio's IRQ timestamp into µs from frame start. */
static void test_rx_event_gets_frame_offset(void)
{
    static oc_schedule_t part;
    memset(&part, 0, sizeof(part));
    part.frame_number = F0 + 1;
    part.flags = OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST;
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, T0 + 10000));
    queue_event(OC_RADIO_EV_RX_DONE, -70, 20);
    fake.queue[fake.qn - 1].irq_us = T0 + 120000u + 25000u; /* frame F0+1 starts at T0 + 120 ms */
    run_until(T0 + 20000, T0 + 120000u + 60000u);
    TEST_ASSERT_EQUAL_INT(1, fake.rx_count);
    TEST_ASSERT_EQUAL_INT32(25000, fake.rx_ev.frame_offset_us);

    queue_event(OC_RADIO_EV_RX_DONE, -70, 20); /* no timestamp: unknown */
    memset(&part, 0, sizeof(part));
    part.frame_number = F0 + 3;
    part.flags = OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST;
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, T0 + 120000u + 70000u));
    run_until(T0 + 120000u + 70000u, T0 + 360000u + 60000u);
    TEST_ASSERT_EQUAL_INT(2, fake.rx_count);
    TEST_ASSERT_EQUAL_INT32(OC_RX_END_UNKNOWN, fake.rx_ev.frame_offset_us);
}

/* At the edge tier a full-length UL packet's RX done comes ~975 µs before
 * the next back-to-back slot starts (1200 µs guard minus the LR2021's
 * 225 µs RX-done lag), and the readout, configure and stage must fit in
 * that: the executor has to notice the done within 50 µs, not a 200 µs
 * poll period later (bench 2026-09-30: 73 µs on average, 187 worst). */
static void test_rx_done_is_picked_up_within_50us(void)
{
    static oc_schedule_t p2;
    memset(&p2, 0, sizeof(p2));
    p2.frame_number = F0 + 1;
    p2.flags = OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST;
    p2.slot_count = 2;
    p2.slots[0] = rx_slot(20000, 17910);
    p2.slots[1] = rx_slot(37910, 17910);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &p2, &clk, T0 + 10000));
    const uint64_t done = T1 + 20000u + 16929u;
    queue_event(OC_RADIO_EV_RX_DONE, -70, 20);
    fake.ready_at[fake.qn - 1] = done;
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_INT(1, fake.rx_count);
    TEST_ASSERT_TRUE(fake.rx_at >= done);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(50, fake.rx_at - done);
    TEST_ASSERT_EQUAL_INT(2, count_calls(CALL_LAUNCH));
    TEST_ASSERT_EQUAL_UINT64(T1 + 37910u, fake.calls[find_call(CALL_LAUNCH, 1)].target_us);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_slots_launch_exactly_on_time);
    RUN_TEST(test_rx_done_is_reported_with_frame_and_slot);
    RUN_TEST(test_rx_timeout_is_not_reported);
    RUN_TEST(test_missing_schedule_counts_miss_only_after_first);
    RUN_TEST(test_tx_refused_when_disabled);
    RUN_TEST(test_stalled_task_skips_late_slot);
    RUN_TEST(test_radio_that_never_finishes_is_stopped_at_slot_end);
    RUN_TEST(test_configure_error_skips_only_that_slot);
    RUN_TEST(test_clock_loss_stops_frame);
    RUN_TEST(test_first_slot_inside_config_lead_still_runs);
    RUN_TEST(test_offset0_with_fast_crystal_still_runs);
    RUN_TEST(test_rx_event_gets_frame_offset);
    RUN_TEST(test_tx_done_offset_is_kept);
    RUN_TEST(test_band_change_gets_longer_lead);
    RUN_TEST(test_modulation_change_gets_longer_lead);
    RUN_TEST(test_rx_done_is_picked_up_within_50us);
    return UNITY_END();
}
