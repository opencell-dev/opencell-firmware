/* oc_lr_rx: the LR2021 RX readout that oc_radio's fast path reads in a few
 * SPI transactions must decode exactly as the RadioLib calls it replaces
 * (getIrqFlags, readData, getRSSI, getSNR in RadioLib 7.7.1). */
#include <string.h>

#include "oc_lr_rx.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_irq_flags_follow_the_two_status_bytes_big_endian(void)
{
    const uint8_t s[6] = { 0x05, 0x00, 0x00, 0x44, 0x00, 0x40 };
    TEST_ASSERT_EQUAL_HEX32(0x00440040u, oc_lr_irq_of(s));
}

static void test_event_priority_matches_the_old_poll(void)
{
    TEST_ASSERT_EQUAL(OC_RADIO_EV_RX_DONE, oc_lr_event(OC_LR_IRQ_RX_DONE));
    TEST_ASSERT_EQUAL(OC_RADIO_EV_RX_DONE, oc_lr_event(OC_LR_IRQ_RX_DONE | OC_LR_IRQ_CRC_ERROR));
    TEST_ASSERT_EQUAL(OC_RADIO_EV_RX_DONE, oc_lr_event(OC_LR_IRQ_RX_DONE | OC_LR_IRQ_TIMEOUT));
    TEST_ASSERT_EQUAL(OC_RADIO_EV_TX_DONE, oc_lr_event(OC_LR_IRQ_TX_DONE));
    TEST_ASSERT_EQUAL(OC_RADIO_EV_TX_DONE, oc_lr_event(OC_LR_IRQ_TX_DONE | OC_LR_IRQ_TIMEOUT));
    TEST_ASSERT_EQUAL(OC_RADIO_EV_RX_TIMEOUT, oc_lr_event(OC_LR_IRQ_TIMEOUT));
    TEST_ASSERT_EQUAL(OC_RADIO_EV_ERROR, oc_lr_event(OC_LR_IRQ_ERROR));
    TEST_ASSERT_EQUAL(OC_RADIO_EV_ERROR, oc_lr_event(OC_LR_IRQ_CMD_ERROR));
}

static void test_preamble_and_header_flags_alone_keep_waiting(void)
{
    TEST_ASSERT_EQUAL(OC_RADIO_EV_NONE, oc_lr_event(0));
    TEST_ASSERT_EQUAL(OC_RADIO_EV_NONE, oc_lr_event((1u << 5) | OC_LR_IRQ_LORA_HEADER_VALID));
}

/* RadioLib: rssi = raw / -2.0f with raw = b3 << 1 | (b5 & 2) >> 1, then oc_radio
 * truncated it to int16; snr = (int8)b2 / 4.0f, times 4 into 0.25 dB units. */
static void test_lora_rssi_and_snr_decode_as_radiolib_for_every_byte(void)
{
    for (int b = 0; b < 256; b++) {
        for (int bit = 0; bit < 2; bit++) {
            uint8_t ps[OC_LR_LORA_STATUS_LEN] = { 0x14, 28, (uint8_t)b, (uint8_t)b, 0, (uint8_t)(bit << 1) };
            oc_radio_event_t ev;
            memset(&ev, 0, sizeof(ev));
            oc_lr_rx_quality(0, OC_LR_IRQ_RX_DONE | OC_LR_IRQ_LORA_HEADER_VALID, ps, &ev);
            uint16_t raw = (uint16_t)(((uint16_t)b << 1) | (uint16_t)bit);
            TEST_ASSERT_EQUAL_INT16((int16_t)((float)raw / -2.0f), ev.rssi_dbm);
            TEST_ASSERT_EQUAL_INT16((int16_t)(((float)((int8_t)b) / 4.0f) * 4.0f), ev.snr_qdb);
        }
    }
}

static void test_lora_crc_ok_needs_a_valid_header_and_no_crc_error(void)
{
    const uint8_t ps[OC_LR_LORA_STATUS_LEN] = { 0x14, 28, 40, 80, 80, 0 };
    oc_radio_event_t ev;
    oc_lr_rx_quality(0, OC_LR_IRQ_RX_DONE | OC_LR_IRQ_LORA_HEADER_VALID, ps, &ev);
    TEST_ASSERT_EQUAL_UINT8(1, ev.crc_ok);
    oc_lr_rx_quality(0, OC_LR_IRQ_RX_DONE | OC_LR_IRQ_LORA_HEADER_VALID | OC_LR_IRQ_CRC_ERROR, ps, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, ev.crc_ok);
    oc_lr_rx_quality(0, OC_LR_IRQ_RX_DONE, ps, &ev); /* explicit header never validated */
    TEST_ASSERT_EQUAL_UINT8(0, ev.crc_ok);
}

/* RadioLib's FLRC getRSSI: rssiAvg = raw / -2.0f, raw = b2 << 1 | (b4 & 4) >> 2;
 * getSNR is 0 for FLRC; readData only checks CRC_ERROR. */
static void test_flrc_rssi_is_the_average_and_snr_zero(void)
{
    for (int b = 0; b < 256; b++) {
        for (int bit = 0; bit < 2; bit++) {
            uint8_t ps[OC_LR_FLRC_STATUS_LEN] = { 0x00, 28, (uint8_t)b, 0xFF, (uint8_t)((bit << 2) | 0x01) };
            oc_radio_event_t ev;
            memset(&ev, 0, sizeof(ev));
            oc_lr_rx_quality(1, OC_LR_IRQ_RX_DONE, ps, &ev);
            uint16_t raw = (uint16_t)(((uint16_t)b << 1) | (uint16_t)bit);
            TEST_ASSERT_EQUAL_INT16((int16_t)((float)raw / -2.0f), ev.rssi_dbm);
            TEST_ASSERT_EQUAL_INT16(0, ev.snr_qdb);
            TEST_ASSERT_EQUAL_UINT8(1, ev.crc_ok);
        }
    }
    const uint8_t ps[OC_LR_FLRC_STATUS_LEN] = { 0x00, 28, 80, 80, 0 };
    oc_radio_event_t ev;
    oc_lr_rx_quality(1, OC_LR_IRQ_RX_DONE | OC_LR_IRQ_CRC_ERROR, ps, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, ev.crc_ok);
}

static void test_rx_length_is_big_endian_and_capped_to_the_event_buffer(void)
{
    const uint8_t a[2] = { 0x00, 0x1C };
    const uint8_t b[2] = { 0x01, 0x05 };
    TEST_ASSERT_EQUAL_UINT16(28, oc_lr_rx_len(a));
    TEST_ASSERT_EQUAL_UINT16(sizeof(((oc_radio_event_t *)0)->data), oc_lr_rx_len(b));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_irq_flags_follow_the_two_status_bytes_big_endian);
    RUN_TEST(test_event_priority_matches_the_old_poll);
    RUN_TEST(test_preamble_and_header_flags_alone_keep_waiting);
    RUN_TEST(test_lora_rssi_and_snr_decode_as_radiolib_for_every_byte);
    RUN_TEST(test_lora_crc_ok_needs_a_valid_header_and_no_crc_error);
    RUN_TEST(test_flrc_rssi_is_the_average_and_snr_zero);
    RUN_TEST(test_rx_length_is_big_endian_and_capped_to_the_event_buffer);
    return UNITY_END();
}
