#include "unity.h"

#include "exec_fixture.h"

void setUp(void) { fixture_reset(); }
void tearDown(void) {}

static const uint8_t voice[28] = { 1, 2, 3 };
static oc_schedule_t part;

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
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 2;
    part.slots[0] = tx_slot(0, 17000, voice, sizeof(voice));
    part.slots[1] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_READY, exec_.frames[0].state);
    TEST_ASSERT_EQUAL_UINT8(2, exec_.frames[0].slot_count);
    TEST_ASSERT_EQUAL_UINT16(28, exec_.frames[0].pool_used);
    TEST_ASSERT_EQUAL_UINT8(0, exec_.frames[0].slots[1].payload_len); /* RX payload ignored */
}

static void test_multi_part_assembles_until_last(void)
{
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST);
    part.slot_count = 1;
    part.slots[0] = tx_slot(0, 17000, voice, sizeof(voice));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_ASSEMBLING, exec_.frames[0].state);

    make_part(F0 + 1, OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW + 1000));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_READY, exec_.frames[0].state);
    TEST_ASSERT_EQUAL_UINT8(2, exec_.frames[0].slot_count);
}

static void test_payload_is_copied_not_aliased(void)
{
    uint8_t scratch[28];
    memset(scratch, 0xAB, sizeof(scratch));
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = tx_slot(0, 17000, scratch, sizeof(scratch));
    oc_exec_add_part(&exec_, &part, &clk, NOW);
    memset(scratch, 0, sizeof(scratch)); /* UART buffer reused */
    TEST_ASSERT_EQUAL_HEX8(0xAB, exec_.frames[0].pool[exec_.frames[0].slots[0].payload_off + 27]);
}

static void test_late_schedule_rejected(void)
{
    make_part(F0, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST); /* current frame */
    part.slot_count = 1;
    part.slots[0] = rx_slot(60000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_LATE, oc_exec_add_part(&exec_, &part, &clk, NOW));

    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST); /* next frame, but only 1 ms before it */
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_LATE, oc_exec_add_part(&exec_, &part, &clk, T0 + 119000));
}

static void test_late_last_part_discards_partial_frame(void)
{
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    make_part(F0 + 1, OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_LATE, oc_exec_add_part(&exec_, &part, &clk, T0 + 119500));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_EMPTY, exec_.frames[0].state);
}

static void test_no_clock_is_late(void)
{
    oc_clock_init(&clk, 30000000u);
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_LATE, oc_exec_add_part(&exec_, &part, &clk, NOW));
}

static void test_too_far_ahead_rejected(void)
{
    make_part(F0 + 1 + OC_EXEC_MAX_AHEAD, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, oc_exec_add_part(&exec_, &part, &clk, NOW));
}

static void test_bad_slots_rejected(void)
{
    const oc_slot_t bad[] = {
        rx_slot(110000, 17000),                     /* past end of frame */
        tx_slot(0, 1000, voice, sizeof(voice)),     /* airtime longer than slot */
        tx_slot(0, 17000, voice, 0),                /* TX without payload */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
        part.slot_count = 1;
        part.slots[0] = bad[i];
        TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, oc_exec_add_part(&exec_, &part, &clk, NOW));
    }
    oc_slot_t zero_freq = rx_slot(0, 17000);
    zero_freq.freq_hz = 0;
    oc_slot_t bad_dir = rx_slot(0, 17000);
    bad_dir.dir = 7;
    oc_slot_t bad_mode = rx_slot(0, 17000);
    bad_mode.mode.sf = 3;
    const oc_slot_t more[] = { zero_freq, bad_dir, bad_mode };
    for (size_t i = 0; i < 3; i++) {
        make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
        part.slot_count = 1;
        part.slots[0] = more[i];
        TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, oc_exec_add_part(&exec_, &part, &clk, NOW));
    }
}

static void test_overlap_across_parts_rejected(void)
{
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    make_part(F0 + 1, OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(30000, 17000); /* starts before 37000 */
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, oc_exec_add_part(&exec_, &part, &clk, NOW));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_EMPTY, exec_.frames[0].state);
}

static void test_part_after_last_rejected(void)
{
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    part.flags = OC_SCHED_FLAG_LAST; /* a continuation part after the frame is complete */
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, oc_exec_add_part(&exec_, &part, &clk, NOW));
}

static void test_payload_pool_exhaustion_rejected(void)
{
    static uint8_t big[255];
    /* 17 x 255 B > 4096 B pool; FLRC 2.4 near slots keep airtime short. */
    for (int i = 0; i < 17; i++) {
        make_part(F0 + 1, i == 16 ? OC_SCHED_FLAG_LAST : i == 0 ? OC_SCHED_FLAG_FIRST : 0);
        part.slot_count = 1;
        part.slots[0] = (oc_slot_t){ (uint32_t)i * 6000u, 6000, 2402000000u,
                                     *oc_tier_mode(OC_BAND_2G4, OC_TIER_NEAR), OC_DIR_TX, 255, big };
        uint8_t st = oc_exec_add_part(&exec_, &part, &clk, NOW);
        TEST_ASSERT_EQUAL_UINT8(i < 16 ? OC_ACK_OK : OC_ACK_ERR_MALFORMED, st);
    }
}

static void test_two_frames_buffered(void)
{
    for (uint32_t k = 1; k <= 2; k++) {
        make_part(F0 + k, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
        part.slot_count = 1;
        part.slots[0] = rx_slot(0, 17000);
        TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    }
    int ready = 0;
    for (unsigned i = 0; i < OC_EXEC_FRAMES; i++) {
        ready += exec_.frames[i].state == OC_EXEC_BUF_READY;
    }
    TEST_ASSERT_EQUAL_INT(2, ready);
}

/* Review #4: parts must be resendable after a lost ACK, and a frame only
 * starts on a FIRST part. */
static void test_resent_middle_part_is_acked_without_readding(void)
{
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    make_part(F0 + 1, 0);
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW + 500)); /* resend */
    make_part(F0 + 1, OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(40000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW + 1000));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_READY, exec_.frames[0].state);
    TEST_ASSERT_EQUAL_UINT8(3, exec_.frames[0].slot_count);
}

static void test_resent_complete_frame_is_acked(void)
{
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = tx_slot(0, 17000, voice, sizeof(voice));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW + 500));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_READY, exec_.frames[0].state);
    TEST_ASSERT_EQUAL_UINT8(1, exec_.frames[0].slot_count);
    TEST_ASSERT_EQUAL_UINT16(28, exec_.frames[0].pool_used);
}

static void test_parts_without_first_open_nothing(void)
{
    make_part(F0 + 1, 0);
    part.slot_count = 1;
    part.slots[0] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, oc_exec_add_part(&exec_, &part, &clk, NOW));
    make_part(F0 + 1, OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(40000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, oc_exec_add_part(&exec_, &part, &clk, NOW));
    for (unsigned i = 0; i < OC_EXEC_FRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_EMPTY, exec_.frames[i].state);
    }
}

static void test_first_part_restarts_assembly(void)
{
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST); /* host restarted this frame */
    part.slot_count = 1;
    part.slots[0] = rx_slot(50000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW + 1000));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_READY, exec_.frames[0].state);
    TEST_ASSERT_EQUAL_UINT8(1, exec_.frames[0].slot_count);
    TEST_ASSERT_EQUAL_UINT32(50000, exec_.frames[0].slots[0].offset_us);
}

/* Review F1-m1: the commit rebases a later part's payload offsets onto the
 * frame's pool; without that, slot 2 would transmit part 1's bytes. */
static void test_a_later_parts_payload_follows_the_earlier_ones(void)
{
    static const uint8_t other[20] = { 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
                                       0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3 };
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST);
    part.slot_count = 2;
    part.slots[0] = tx_slot(0, 17000, voice, sizeof(voice));
    part.slots[1] = rx_slot(20000, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    make_part(F0 + 1, OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = tx_slot(40000, 17000, other, sizeof(other));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));

    const oc_exec_frame_t *b = &exec_.frames[0];
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_READY, b->state);
    TEST_ASSERT_EQUAL_UINT8(3, b->slot_count);
    TEST_ASSERT_EQUAL_UINT16(48, b->pool_used);
    TEST_ASSERT_EQUAL_UINT16(0, b->slots[0].payload_off);
    TEST_ASSERT_EQUAL_UINT16(28, b->slots[2].payload_off);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(voice, &b->pool[b->slots[0].payload_off], sizeof(voice));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(other, &b->pool[b->slots[2].payload_off], sizeof(other));
}

static uint8_t add_rx_run(uint32_t frame, uint8_t flags, unsigned first, unsigned n)
{
    make_part(frame, flags);
    part.slot_count = (uint8_t)n;
    for (unsigned i = 0; i < n; i++) {
        part.slots[i] = rx_slot((first + i) * 1500u, 1500);
    }
    return oc_exec_add_part(&exec_, &part, &clk, NOW);
}

/* Review F1-m1: the frame's 64-slot cap counts every part; 40 + 40 would
 * write past slots[] into the pool. */
static void test_the_frame_slot_cap_spans_parts(void)
{
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, add_rx_run(F0 + 1, OC_SCHED_FLAG_FIRST, 0, 40));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, add_rx_run(F0 + 1, OC_SCHED_FLAG_LAST, 40, 40));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_EMPTY, exec_.frames[0].state);
}

static void test_the_frame_slot_cap_is_64_across_parts(void)
{
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, add_rx_run(F0 + 1, OC_SCHED_FLAG_FIRST, 0, 32));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, add_rx_run(F0 + 1, OC_SCHED_FLAG_LAST, 32, 32));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_READY, exec_.frames[0].state);
    TEST_ASSERT_EQUAL_UINT8(64, exec_.frames[0].slot_count);
    TEST_ASSERT_EQUAL_UINT32(63u * 1500u, exec_.frames[0].slots[63].offset_us);
}

/* A part's payloads all come from one message (<= OC_LINK_MAX_MSG bytes),
 * so the prepared part's pool is that big (review F1-m4). A hand-built part
 * over it can't come off the wire and is refused. */
static void test_one_parts_payloads_are_bounded_by_a_message(void)
{
    static uint8_t big[255];
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 8; /* 2040 B: fits a message */
    for (unsigned i = 0; i < part.slot_count; i++) {
        part.slots[i] = (oc_slot_t){ i * 6000u, 6000, 2402000000u, *oc_tier_mode(OC_BAND_2G4, OC_TIER_NEAR),
                                     OC_DIR_TX, 255, big };
    }
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));

    make_part(F0 + 2, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 9; /* 2295 B: more than any message carries */
    for (unsigned i = 0; i < part.slot_count; i++) {
        part.slots[i] = (oc_slot_t){ i * 6000u, 6000, 2402000000u, *oc_tier_mode(OC_BAND_2G4, OC_TIER_NEAR),
                                     OC_DIR_TX, 255, big };
    }
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, oc_exec_add_part(&exec_, &part, &clk, NOW));
}

/* Review F1-m2: the commit's RUNNING guard, reached directly. The exec task
 * entered F0+1 (a configure-lead early); a FIRST part for it, committed with
 * a now_us whose setup deadline still passes, must not rewrite the frame. */
static void test_a_running_frame_is_never_rewritten(void)
{
    const uint64_t f1 = T0 + OC_FRAME_US;
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(0, 17000);
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, oc_exec_add_part(&exec_, &part, &clk, NOW));
    run_until(NOW, f1 - 1000);
    TEST_ASSERT_NOT_NULL(exec_.run);
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_RUNNING, exec_.run->state);

    static oc_exec_part_t p;
    make_part(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    part.slot_count = 1;
    part.slots[0] = rx_slot(50000, 17000);
    oc_exec_prepare_part(&part, &p);
    TEST_ASSERT_TRUE(f1 >= (f1 - 1600) + OC_EXEC_SETUP_US); /* the deadline check passes */
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_LATE, oc_exec_commit_part(&exec_, &p, &clk, f1 - 1600));
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_RUNNING, exec_.run->state);
    TEST_ASSERT_EQUAL_UINT8(1, exec_.run->slot_count);
    TEST_ASSERT_EQUAL_UINT32(0, exec_.run->slots[0].offset_us);
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
    RUN_TEST(test_resent_middle_part_is_acked_without_readding);
    RUN_TEST(test_resent_complete_frame_is_acked);
    RUN_TEST(test_parts_without_first_open_nothing);
    RUN_TEST(test_first_part_restarts_assembly);
    RUN_TEST(test_a_later_parts_payload_follows_the_earlier_ones);
    RUN_TEST(test_the_frame_slot_cap_spans_parts);
    RUN_TEST(test_the_frame_slot_cap_is_64_across_parts);
    RUN_TEST(test_one_parts_payloads_are_bounded_by_a_message);
    RUN_TEST(test_a_running_frame_is_never_rewritten);
    return UNITY_END();
}
