#include "unity.h"

#include <string.h>

#include "oc_link.h"

void setUp(void) {}
void tearDown(void) {}

static oc_framer_t framer;
static uint8_t wire[OC_FRAMER_RAW_CAP + 1];

static oc_msg_t make_ack(uint8_t seq)
{
    oc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_MSG_ACK;
    m.seq = seq;
    m.u.ack = (oc_ack_t){ seq, OC_ACK_OK };
    return m;
}

/* Push bytes; return how many complete messages came out, last one in *msg. */
static int push_all(const uint8_t *bytes, size_t n, oc_msg_t *msg)
{
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        count += oc_framer_push(&framer, bytes[i], msg);
    }
    return count;
}

static void test_write_frame_is_delimited_on_both_ends(void)
{
    oc_msg_t m = make_ack(1);
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_HEX8(0x00, wire[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, wire[n - 1]);
    for (size_t i = 1; i + 1 < n; i++) {
        TEST_ASSERT_NOT_EQUAL(0x00, wire[i]);
    }
}

static void test_frame_roundtrip(void)
{
    oc_framer_init(&framer);
    oc_msg_t m = make_ack(42);
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    oc_msg_t got;
    TEST_ASSERT_EQUAL_INT(1, push_all(wire, n, &got));
    TEST_ASSERT_EQUAL_UINT8(OC_MSG_ACK, got.type);
    TEST_ASSERT_EQUAL_UINT8(42, got.u.ack.acked_seq);
}

static void test_back_to_back_frames_and_idle_zeros(void)
{
    oc_framer_init(&framer);
    oc_msg_t got;
    const uint8_t idle[] = { 0x00, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_INT(0, push_all(idle, sizeof(idle), &got));
    for (uint8_t seq = 0; seq < 5; seq++) {
        oc_msg_t m = make_ack(seq);
        size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(1, push_all(wire, n, &got));
        TEST_ASSERT_EQUAL_UINT8(seq, got.seq);
    }
    TEST_ASSERT_EQUAL_UINT32(0, framer.crc_errors + framer.cobs_errors + framer.malformed);
}

static void test_boot_garbage_then_valid_frame(void)
{
    oc_framer_init(&framer);
    oc_msg_t got;
    const uint8_t garbage[] = { 0x13, 0x37, 0xFF, 0x42, 0x00 };
    TEST_ASSERT_EQUAL_INT(0, push_all(garbage, sizeof(garbage), &got));
    TEST_ASSERT_EQUAL_UINT32(1, framer.crc_errors + framer.cobs_errors + framer.malformed);

    oc_msg_t m = make_ack(9);
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(1, push_all(wire, n, &got));
    TEST_ASSERT_EQUAL_UINT8(9, got.seq);
}

static void test_undelimited_garbage_does_not_eat_next_frame(void)
{
    oc_framer_init(&framer);
    oc_msg_t got;
    const uint8_t garbage[] = { 0x13, 0x37, 0xFF, 0x42 }; /* no 0x00 */
    TEST_ASSERT_EQUAL_INT(0, push_all(garbage, sizeof(garbage), &got));

    oc_msg_t m = make_ack(11);
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(1, push_all(wire, n, &got));
    TEST_ASSERT_EQUAL_UINT8(11, got.seq);
}

static void test_torn_frame_does_not_eat_next_frame(void)
{
    oc_framer_init(&framer);
    oc_msg_t got;
    oc_msg_t m = make_ack(12);
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(0, push_all(wire, n / 2, &got)); /* sender reset mid-frame */
    TEST_ASSERT_EQUAL_INT(1, push_all(wire, n, &got));
    TEST_ASSERT_EQUAL_UINT8(12, got.seq);
}

static void test_corrupted_byte_counts_crc_error_and_recovers(void)
{
    oc_framer_init(&framer);
    oc_msg_t got;
    oc_msg_t m = make_ack(3);
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    wire[2] ^= 0x01; /* flip a data bit; stays non-zero for this frame */
    TEST_ASSERT_NOT_EQUAL(0x00, wire[2]);
    TEST_ASSERT_EQUAL_INT(0, push_all(wire, n, &got));
    TEST_ASSERT_EQUAL_UINT32(1, framer.crc_errors + framer.cobs_errors);

    n = oc_link_write_frame(&m, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(1, push_all(wire, n, &got));
}

static void test_truncated_frame_is_dropped(void)
{
    oc_framer_init(&framer);
    oc_msg_t got;
    oc_msg_t m = make_ack(4);
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    push_all(wire, n / 2, &got);        /* lose the tail */
    TEST_ASSERT_EQUAL_INT(0, oc_framer_push(&framer, 0x00, &got));
    TEST_ASSERT_EQUAL_UINT32(1, framer.crc_errors + framer.cobs_errors + framer.malformed);
}

static void test_oversized_frame_dropped_then_next_frame_ok(void)
{
    oc_framer_init(&framer);
    oc_msg_t got;
    for (size_t i = 0; i < OC_FRAMER_RAW_CAP + 100; i++) {
        TEST_ASSERT_EQUAL_INT(0, oc_framer_push(&framer, 0x55, &got));
    }
    TEST_ASSERT_EQUAL_INT(0, oc_framer_push(&framer, 0x00, &got));
    TEST_ASSERT_EQUAL_UINT32(1, framer.overflows);

    oc_msg_t m = make_ack(5);
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(1, push_all(wire, n, &got));
    TEST_ASSERT_EQUAL_UINT8(5, got.seq);
}

static void test_max_size_schedule_survives_framing(void)
{
    static uint8_t payload[255];
    for (int i = 0; i < 255; i++) {
        payload[i] = (uint8_t)i; /* includes zeros to exercise COBS */
    }
    static oc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_MSG_SCHEDULE;
    m.u.schedule.slot_count = 7; /* 2 + 6 + 7 * (27 + 255) = 1982 <= 2048 */
    for (int i = 0; i < 7; i++) {
        m.u.schedule.slots[i].payload_len = 255;
        m.u.schedule.slots[i].payload = payload;
    }
    size_t n = oc_link_write_frame(&m, wire, sizeof(wire));
    TEST_ASSERT_TRUE(n > 0);

    oc_framer_init(&framer);
    static oc_msg_t got;
    TEST_ASSERT_EQUAL_INT(1, push_all(wire, n, &got));
    TEST_ASSERT_EQUAL_UINT8(7, got.u.schedule.slot_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, got.u.schedule.slots[6].payload, 255);
}

static void test_write_frame_rejects_small_buffer(void)
{
    oc_msg_t m = make_ack(1);
    TEST_ASSERT_EQUAL_size_t(0, oc_link_write_frame(&m, wire, 4));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_write_frame_is_delimited_on_both_ends);
    RUN_TEST(test_frame_roundtrip);
    RUN_TEST(test_back_to_back_frames_and_idle_zeros);
    RUN_TEST(test_boot_garbage_then_valid_frame);
    RUN_TEST(test_undelimited_garbage_does_not_eat_next_frame);
    RUN_TEST(test_torn_frame_does_not_eat_next_frame);
    RUN_TEST(test_corrupted_byte_counts_crc_error_and_recovers);
    RUN_TEST(test_truncated_frame_is_dropped);
    RUN_TEST(test_oversized_frame_dropped_then_next_frame_ok);
    RUN_TEST(test_max_size_schedule_survives_framing);
    RUN_TEST(test_write_frame_rejects_small_buffer);
    return UNITY_END();
}
