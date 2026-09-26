#include "unity.h"

#include <string.h>

#include "lc_link.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t buf[LC_LINK_MAX_MSG];
static lc_msg_t in;
static lc_msg_t out;

static size_t roundtrip(void)
{
    size_t n = lc_msg_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(0, lc_msg_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(in.type, out.type);
    TEST_ASSERT_EQUAL_UINT8(in.seq, out.seq);
    return n;
}

static void test_config_roundtrip_and_layout(void)
{
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_CONFIG;
    in.seq = 7;
    in.u.config = (lc_config_t){ LC_ROLE_BS_RADIO, LC_BAND_2G4, 1, 0xA1B2C3D4u };
    size_t n = roundtrip();

    const uint8_t expected[] = { 0x01, 0x07, 0x00, 0x01, 0x01, 0xD4, 0xC3, 0xB2, 0xA1 };
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, n);
    TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4u, out.u.config.cell_seed);
    TEST_ASSERT_EQUAL_UINT8(1, out.u.config.radio_index);
}

static void test_schedule_roundtrip_with_payloads(void)
{
    static const uint8_t voice[28] = { 0xAA, 0x00, 0x55 };
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_SCHEDULE;
    in.seq = 200;
    in.u.schedule.frame_number = 0xFFFFFFFFu;
    in.u.schedule.flags = LC_SCHED_FLAG_LAST;
    in.u.schedule.slot_count = 2;
    in.u.schedule.slots[0] = (lc_slot_t){ 0, 1700, 915250000u,
                                          *lc_tier_mode(LC_BAND_915, LC_TIER_NEAR),
                                          LC_DIR_TX, sizeof(voice), voice };
    in.u.schedule.slots[1] = (lc_slot_t){ 60000, 16904, 903750000u,
                                          *lc_tier_mode(LC_BAND_915, LC_TIER_EDGE),
                                          LC_DIR_RX, 0, NULL };
    size_t n = roundtrip();

    TEST_ASSERT_EQUAL_size_t(2 + 6 + (27 + 28) + 27, n);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, out.u.schedule.frame_number);
    TEST_ASSERT_EQUAL_UINT8(2, out.u.schedule.slot_count);
    const lc_slot_t *s0 = &out.u.schedule.slots[0];
    TEST_ASSERT_EQUAL_UINT32(915250000u, s0->freq_hz);
    TEST_ASSERT_EQUAL_UINT8(LC_MOD_FLRC, s0->mode.modulation);
    TEST_ASSERT_EQUAL_UINT32(260000u, s0->mode.bitrate_bps);
    TEST_ASSERT_EQUAL_UINT8(28, s0->payload_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(voice, s0->payload, 28);
    const lc_slot_t *s1 = &out.u.schedule.slots[1];
    TEST_ASSERT_EQUAL_UINT8(LC_DIR_RX, s1->dir);
    TEST_ASSERT_EQUAL_UINT8(7, s1->mode.sf);
    TEST_ASSERT_EQUAL_UINT32(500000u, s1->mode.bw_hz);
    TEST_ASSERT_EQUAL_UINT8(0, s1->payload_len);
}

static void test_rx_report_roundtrip_negative_values(void)
{
    static const uint8_t pl[4] = { 1, 2, 3, 4 };
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_RX_REPORT;
    in.u.rx_report = (lc_rx_report_t){ 12345, 9, -118, -27, 1, sizeof(pl), pl, LC_RX_END_UNKNOWN };
    roundtrip();
    TEST_ASSERT_EQUAL_INT16(-118, out.u.rx_report.rssi_dbm);
    TEST_ASSERT_EQUAL_INT16(-27, out.u.rx_report.snr_qdb);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pl, out.u.rx_report.payload, 4);
}

static void test_status_fw_ack_roundtrip(void)
{
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_STATUS;
    in.u.status = (lc_status_t){ 86400000u, 1, -12, 3, 65535, 0, LC_RX_END_UNKNOWN, LC_RX_END_UNKNOWN };
    roundtrip();
    TEST_ASSERT_EQUAL_INT8(-12, out.u.status.temp_c);
    TEST_ASSERT_EQUAL_UINT16(65535, out.u.status.uart_crc_errors);

    static uint8_t chunk[LC_MAX_FW_CHUNK];
    memset(chunk, 0x5A, sizeof(chunk));
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_FW_CHUNK;
    in.u.fw_chunk = (lc_fw_chunk_t){ 4096, LC_MAX_FW_CHUNK, chunk };
    roundtrip();
    TEST_ASSERT_EQUAL_UINT16(LC_MAX_FW_CHUNK, out.u.fw_chunk.len);

    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_FW_COMMIT;
    in.u.fw_commit.image_size = 1234567;
    roundtrip();
    TEST_ASSERT_EQUAL_UINT32(1234567, out.u.fw_commit.image_size);

    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_ACK;
    in.u.ack = (lc_ack_t){ 200, LC_ACK_ERR_LATE };
    roundtrip();
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_LATE, out.u.ack.status);
}

static void test_encode_rejects_over_limits(void)
{
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_SCHEDULE;
    in.u.schedule.slot_count = LC_MAX_SLOTS_PER_SCHEDULE + 1;
    TEST_ASSERT_EQUAL_size_t(0, lc_msg_encode(&in, buf, sizeof(buf)));

    in.type = LC_MSG_FW_CHUNK;
    in.u.fw_chunk.len = LC_MAX_FW_CHUNK + 1;
    TEST_ASSERT_EQUAL_size_t(0, lc_msg_encode(&in, buf, sizeof(buf)));

    in.type = 0x7F;
    TEST_ASSERT_EQUAL_size_t(0, lc_msg_encode(&in, buf, sizeof(buf)));

    in.type = LC_MSG_CONFIG;
    TEST_ASSERT_EQUAL_size_t(0, lc_msg_encode(&in, buf, 5)); /* needs 9 */
}

static void test_encode_rejects_schedule_too_big_for_one_message(void)
{
    static uint8_t big[255];
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_SCHEDULE;
    in.u.schedule.slot_count = 10; /* 10 * (27 + 255) > LC_LINK_MAX_MSG */
    for (int i = 0; i < 10; i++) {
        in.u.schedule.slots[i].payload_len = 255;
        in.u.schedule.slots[i].payload = big;
    }
    TEST_ASSERT_EQUAL_size_t(0, lc_msg_encode(&in, buf, sizeof(buf)));
}

static void test_decode_rejects_payload_len_past_end(void)
{
    /* Valid RX_REPORT header claiming 200 payload bytes but carrying 2. */
    const uint8_t bad[] = { 0x03, 0x00, 1, 0, 0, 0, 0, 0x8A, 0xFF, 0xE5, 0xFF, 1, 200, 0xAB, 0xCD };
    TEST_ASSERT_EQUAL_INT(-1, lc_msg_decode(bad, sizeof(bad), &out));
}

static void test_decode_rejects_truncated_trailing_and_unknown(void)
{
    const uint8_t short_cfg[] = { 0x01, 0x00, 0x00, 0x01 };
    TEST_ASSERT_EQUAL_INT(-1, lc_msg_decode(short_cfg, sizeof(short_cfg), &out));

    const uint8_t long_ack[] = { 0x07, 0x00, 0x01, 0x00, 0x99 };
    TEST_ASSERT_EQUAL_INT(-1, lc_msg_decode(long_ack, sizeof(long_ack), &out));

    const uint8_t unknown[] = { 0x7F, 0x00 };
    TEST_ASSERT_EQUAL_INT(-1, lc_msg_decode(unknown, sizeof(unknown), &out));

    const uint8_t too_many_slots[] = { 0x02, 0x00, 0, 0, 0, 0, 0x01, 65 };
    TEST_ASSERT_EQUAL_INT(-1, lc_msg_decode(too_many_slots, sizeof(too_many_slots), &out));

    TEST_ASSERT_EQUAL_INT(-1, lc_msg_decode(buf, 0, &out));
}

static void test_time_roundtrip_and_layout(void)
{
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_TIME;
    in.seq = 3;
    in.u.time.unix_s = 1767225603u;
    size_t n = roundtrip();

    const uint8_t expected[] = { 0x08, 0x03, 0x03, 0xB9, 0x55, 0x69 };
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, n);
    TEST_ASSERT_EQUAL_UINT32(1767225603u, out.u.time.unix_s);
}

static void test_time_rejects_short_body(void)
{
    const uint8_t bad[] = { 0x08, 0x00, 0x01, 0x02, 0x03 };
    TEST_ASSERT_EQUAL_INT(-1, lc_msg_decode(bad, sizeof(bad), &out));
}

static void test_status_carries_frame_number(void)
{
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_STATUS;
    in.u.status = (lc_status_t){ 5000, 1, 30, 0, 0, 0xA1B2C3D4u, 25630, 20310 };
    TEST_ASSERT_EQUAL_size_t(2 + 22, roundtrip());
    TEST_ASSERT_EQUAL_INT32(20310, out.u.status.last_tx_start_us);
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3D4u, out.u.status.frame_number);
    TEST_ASSERT_EQUAL_INT32(25630, out.u.status.last_tx_end_us);
}

/* Timing: when the packet finished, in µs from the RX board's frame start. */
static void test_rx_report_carries_end_offset(void)
{
    static const uint8_t pl[2] = { 7, 8 };
    memset(&in, 0, sizeof(in));
    in.type = LC_MSG_RX_REPORT;
    in.u.rx_report = (lc_rx_report_t){ 77, 1, -60, 40, 1, sizeof(pl), pl, 20123 };
    size_t n = roundtrip();
    TEST_ASSERT_EQUAL_size_t(2 + 4 + 1 + 2 + 2 + 1 + 1 + 2 + 4, n);
    TEST_ASSERT_EQUAL_INT32(20123, out.u.rx_report.end_us);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pl, out.u.rx_report.payload, 2);
    in.u.rx_report.end_us = LC_RX_END_UNKNOWN;
    roundtrip();
    TEST_ASSERT_EQUAL_INT32(LC_RX_END_UNKNOWN, out.u.rx_report.end_us);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_config_roundtrip_and_layout);
    RUN_TEST(test_schedule_roundtrip_with_payloads);
    RUN_TEST(test_rx_report_roundtrip_negative_values);
    RUN_TEST(test_status_fw_ack_roundtrip);
    RUN_TEST(test_encode_rejects_over_limits);
    RUN_TEST(test_encode_rejects_schedule_too_big_for_one_message);
    RUN_TEST(test_decode_rejects_payload_len_past_end);
    RUN_TEST(test_decode_rejects_truncated_trailing_and_unknown);
    RUN_TEST(test_time_roundtrip_and_layout);
    RUN_TEST(test_time_rejects_short_body);
    RUN_TEST(test_status_carries_frame_number);
    RUN_TEST(test_rx_report_carries_end_offset);
    return UNITY_END();
}
