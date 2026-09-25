#include "unity.h"

#include "exec_fixture.h"

void setUp(void) { fixture_reset(); }
void tearDown(void) {}

#define T1 (T0 + 120000ull) /* start of frame F0 + 1 */

static const uint8_t voice[28] = { 9, 8, 7 };
static lc_schedule_t part;

/* Frame F0+1: TX at 0 ms, RX at 20 ms (edge tier, 17 ms slots). */
static void schedule_tx_rx(void)
{
    memset(&part, 0, sizeof(part));
    part.frame_number = F0 + 1;
    part.flags = LC_SCHED_FLAG_LAST;
    part.slot_count = 2;
    part.slots[0] = tx_slot(0, 17000, voice, sizeof(voice));
    part.slots[1] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, lc_exec_add_part(&exec_, &part, &clk, T0 + 10000));
}

static void test_slots_launch_exactly_on_time(void)
{
    schedule_tx_rx();
    queue_event(LC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(LC_RADIO_EV_RX_DONE, -97, 26);
    run_until(T0 + 20000, T1 + 120000);

    int c0 = find_call(CALL_CONFIGURE, 0);
    TEST_ASSERT_EQUAL_UINT64(T1 - LC_EXEC_CONFIG_LEAD_US, fake.calls[c0].at_us);
    TEST_ASSERT_EQUAL_UINT32(915250000u, fake.calls[c0].arg);
    TEST_ASSERT_EQUAL_UINT32(28, fake.calls[find_call(CALL_STAGE_TX, 0)].arg);
    TEST_ASSERT_EQUAL_UINT64(T1, fake.calls[find_call(CALL_LAUNCH, 0)].at_us);

    int c1 = find_call(CALL_CONFIGURE, 1);
    TEST_ASSERT_EQUAL_UINT64(T1 + 20000 - LC_EXEC_CONFIG_LEAD_US, fake.calls[c1].at_us);
    TEST_ASSERT_EQUAL_UINT32(17000 - LC_GUARD_US, fake.calls[find_call(CALL_STAGE_RX, 0)].arg);
    TEST_ASSERT_EQUAL_UINT64(T1 + 20000, fake.calls[find_call(CALL_LAUNCH, 1)].at_us);
}

static void test_rx_done_is_reported_with_frame_and_slot(void)
{
    schedule_tx_rx();
    queue_event(LC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(LC_RADIO_EV_RX_DONE, -97, 26);
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
    queue_event(LC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(LC_RADIO_EV_RX_TIMEOUT, 0, 0);
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
    queue_event(LC_RADIO_EV_TX_DONE, 0, 0);
    queue_event(LC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T0 + 20000, T1 + 120000u + 5000u); /* runs F0+1, then F0+2 has nothing */
    TEST_ASSERT_EQUAL_UINT16(1, exec_.schedule_misses);
    TEST_ASSERT_EQUAL_INT(0, count_calls(CALL_STAGE_TX) - 1); /* nothing sent in F0+2 */
}

static void test_tx_refused_when_disabled(void)
{
    schedule_tx_rx();
    lc_exec_set_tx_enabled(&exec_, 0);
    queue_event(LC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T0 + 20000, T1 + 120000);
    TEST_ASSERT_EQUAL_INT(0, count_calls(CALL_STAGE_TX));
    TEST_ASSERT_EQUAL_UINT32(1, exec_.tx_blocked);
    TEST_ASSERT_EQUAL_INT(1, count_calls(CALL_STAGE_RX));
}

static void test_stalled_task_skips_late_slot(void)
{
    schedule_tx_rx();
    queue_event(LC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T1 + 5000, T1 + 120000); /* first wake 5 ms after slot 0 start */
    TEST_ASSERT_EQUAL_UINT32(1, exec_.late_slots);
    TEST_ASSERT_EQUAL_INT(0, count_calls(CALL_STAGE_TX));
    TEST_ASSERT_EQUAL_INT(1, count_calls(CALL_STAGE_RX));
}

static void test_radio_that_never_finishes_is_stopped_at_slot_end(void)
{
    schedule_tx_rx();
    queue_event(LC_RADIO_EV_TX_DONE, 0, 0);
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
    fake.fail_configure = 1;
    run_until(T0 + 20000, T1 + 10000);
    fake.fail_configure = 0;
    queue_event(LC_RADIO_EV_RX_TIMEOUT, 0, 0);
    run_until(T1 + 10000, T1 + 120000);
    TEST_ASSERT_EQUAL_UINT32(1, exec_.radio_errors);
    TEST_ASSERT_EQUAL_INT(1, count_calls(CALL_STAGE_RX));
}

static void test_clock_loss_stops_frame(void)
{
    schedule_tx_rx();
    queue_event(LC_RADIO_EV_TX_DONE, 0, 0);
    run_until(T0 + 20000, T1 + 25000); /* RX slot active, no event */
    lc_clock_tick(&clk, T0 + 31000000u); /* holdover expired */
    fake.now = T1 + 26000;
    uint64_t next = lc_exec_step(&exec_, &clk, T1 + 26000);
    TEST_ASSERT_EQUAL_UINT64(T1 + 26000 + LC_EXEC_IDLE_US, next);
    TEST_ASSERT_EQUAL_UINT8(CALL_STANDBY, fake.calls[fake.n - 1].kind);
    TEST_ASSERT_EQUAL_UINT8(LC_EXEC_BUF_EMPTY, exec_.frames[0].state);
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
    return UNITY_END();
}
