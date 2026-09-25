#include "unity.h"

#include "exec_fixture.h"

void setUp(void) { fixture_reset(); }
void tearDown(void) {}

static const uint8_t voice[28] = { 1, 2, 3 };
static lc_schedule_t part;

static void make_part(uint32_t frame, uint8_t flags)
{
    memset(&part, 0, sizeof(part));
    part.frame_number = frame;
    part.flags = flags;
}

/* "now" = 10 ms into frame F0 */
#define NOW (T0 + 10000)

static void test_single_part_schedule_becomes_ready(void)
{
    make_part(F0 + 1, LC_SCHED_FLAG_LAST);
    part.slot_count = 2;
    part.slots[0] = tx_slot(0, 17000, voice, sizeof(voice));
    part.slots[1] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, lc_exec_add_part(&exec_, &part, &clk, NOW));
    TEST_ASSERT_EQUAL_UINT8(LC_EXEC_BUF_READY, exec_.frames[0].state);
    TEST_ASSERT_EQUAL_UINT8(2, exec_.frames[0].slot_count);
    TEST_ASSERT_EQUAL_UINT16(28, exec_.frames[0].pool_used);
    TEST_ASSERT_EQUAL_UINT8(0, exec_.frames[0].slots[1].payload_len); /* RX payload ignored */
}

static void test_multi_part_assembles_until_last(void)
{
    make_part(F0 + 1, 0);
    part.slot_count = 1;
    part.slots[0] = tx_slot(0, 17000, voice, sizeof(voice));
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, lc_exec_add_part(&exec_, &part, &clk, NOW));
    TEST_ASSERT_EQUAL_UINT8(LC_EXEC_BUF_ASSEMBLING, exec_.frames[0].state);

    make_part(F0 + 1, LC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, lc_exec_add_part(&exec_, &part, &clk, NOW + 1000));
    TEST_ASSERT_EQUAL_UINT8(LC_EXEC_BUF_READY, exec_.frames[0].state);
    TEST_ASSERT_EQUAL_UINT8(2, exec_.frames[0].slot_count);
}

static void test_payload_is_copied_not_aliased(void)
{
    uint8_t scratch[28];
    memset(scratch, 0xAB, sizeof(scratch));
    make_part(F0 + 1, LC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = tx_slot(0, 17000, scratch, sizeof(scratch));
    lc_exec_add_part(&exec_, &part, &clk, NOW);
    memset(scratch, 0, sizeof(scratch)); /* UART buffer reused */
    TEST_ASSERT_EQUAL_HEX8(0xAB, exec_.frames[0].pool[exec_.frames[0].slots[0].payload_off + 27]);
}

static void test_late_schedule_rejected(void)
{
    make_part(F0, LC_SCHED_FLAG_LAST); /* current frame */
    part.slot_count = 1;
    part.slots[0] = rx_slot(60000, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_LATE, lc_exec_add_part(&exec_, &part, &clk, NOW));

    make_part(F0 + 1, LC_SCHED_FLAG_LAST); /* next frame, but only 1 ms before it */
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_LATE, lc_exec_add_part(&exec_, &part, &clk, T0 + 119000));
}

static void test_late_last_part_discards_partial_frame(void)
{
    make_part(F0 + 1, 0);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, lc_exec_add_part(&exec_, &part, &clk, NOW));
    make_part(F0 + 1, LC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_LATE, lc_exec_add_part(&exec_, &part, &clk, T0 + 119500));
    TEST_ASSERT_EQUAL_UINT8(LC_EXEC_BUF_EMPTY, exec_.frames[0].state);
}

static void test_no_clock_is_late(void)
{
    lc_clock_init(&clk, 30000000u);
    make_part(F0 + 1, LC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_LATE, lc_exec_add_part(&exec_, &part, &clk, NOW));
}

static void test_too_far_ahead_rejected(void)
{
    make_part(F0 + 1 + LC_EXEC_MAX_AHEAD, LC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_MALFORMED, lc_exec_add_part(&exec_, &part, &clk, NOW));
}

static void test_bad_slots_rejected(void)
{
    const lc_slot_t bad[] = {
        rx_slot(110000, 17000),                     /* past end of frame */
        tx_slot(0, 1000, voice, sizeof(voice)),     /* airtime longer than slot */
        tx_slot(0, 17000, voice, 0),                /* TX without payload */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        make_part(F0 + 1, LC_SCHED_FLAG_LAST);
        part.slot_count = 1;
        part.slots[0] = bad[i];
        TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_MALFORMED, lc_exec_add_part(&exec_, &part, &clk, NOW));
    }
    lc_slot_t zero_freq = rx_slot(0, 17000);
    zero_freq.freq_hz = 0;
    lc_slot_t bad_dir = rx_slot(0, 17000);
    bad_dir.dir = 7;
    lc_slot_t bad_mode = rx_slot(0, 17000);
    bad_mode.mode.sf = 3;
    const lc_slot_t more[] = { zero_freq, bad_dir, bad_mode };
    for (size_t i = 0; i < 3; i++) {
        make_part(F0 + 1, LC_SCHED_FLAG_LAST);
        part.slot_count = 1;
        part.slots[0] = more[i];
        TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_MALFORMED, lc_exec_add_part(&exec_, &part, &clk, NOW));
    }
}

static void test_overlap_across_parts_rejected(void)
{
    make_part(F0 + 1, 0);
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, lc_exec_add_part(&exec_, &part, &clk, NOW));
    make_part(F0 + 1, LC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(30000, 17000); /* starts before 37000 */
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_MALFORMED, lc_exec_add_part(&exec_, &part, &clk, NOW));
    TEST_ASSERT_EQUAL_UINT8(LC_EXEC_BUF_EMPTY, exec_.frames[0].state);
}

static void test_part_after_last_rejected(void)
{
    make_part(F0 + 1, LC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, lc_exec_add_part(&exec_, &part, &clk, NOW));
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_MALFORMED, lc_exec_add_part(&exec_, &part, &clk, NOW));
}

static void test_payload_pool_exhaustion_rejected(void)
{
    static uint8_t big[255];
    /* 17 x 255 B > 4096 B pool; FLRC 2.4 near slots keep airtime short. */
    for (int i = 0; i < 17; i++) {
        make_part(F0 + 1, i == 16 ? LC_SCHED_FLAG_LAST : 0);
        part.slot_count = 1;
        part.slots[0] = (lc_slot_t){ (uint32_t)i * 6000u, 6000, 2402000000u,
                                     *lc_tier_mode(LC_BAND_2G4, LC_TIER_NEAR), LC_DIR_TX, 255, big };
        uint8_t st = lc_exec_add_part(&exec_, &part, &clk, NOW);
        TEST_ASSERT_EQUAL_UINT8(i < 16 ? LC_ACK_OK : LC_ACK_ERR_MALFORMED, st);
    }
}

static void test_two_frames_buffered(void)
{
    for (uint32_t k = 1; k <= 2; k++) {
        make_part(F0 + k, LC_SCHED_FLAG_LAST);
        part.slot_count = 1;
        part.slots[0] = rx_slot(0, 17000);
        TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, lc_exec_add_part(&exec_, &part, &clk, NOW));
    }
    int ready = 0;
    for (unsigned i = 0; i < LC_EXEC_FRAMES; i++) {
        ready += exec_.frames[i].state == LC_EXEC_BUF_READY;
    }
    TEST_ASSERT_EQUAL_INT(2, ready);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_part_schedule_becomes_ready);
    RUN_TEST(test_multi_part_assembles_until_last);
    RUN_TEST(test_payload_is_copied_not_aliased);
    RUN_TEST(test_late_schedule_rejected);
    RUN_TEST(test_late_last_part_discards_partial_frame);
    RUN_TEST(test_no_clock_is_late);
    RUN_TEST(test_too_far_ahead_rejected);
    RUN_TEST(test_bad_slots_rejected);
    RUN_TEST(test_overlap_across_parts_rejected);
    RUN_TEST(test_part_after_last_rejected);
    RUN_TEST(test_payload_pool_exhaustion_rejected);
    RUN_TEST(test_two_frames_buffered);
    return UNITY_END();
}
