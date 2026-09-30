/* oc_lr_cmd: the lean LR2021 commands oc_radio sends between a reception and
 * a back-to-back slot. Golden byte sequences per transaction (LR20xx
 * datasheet Rev 2.2, and what RadioLib 7.7.1 with RADIOLIB_SPI_PARANOID=0
 * put on the wire), against a fake bus that records every transaction and
 * answers from a script. */
#include <string.h>

#include "oc_lr_cmd.h"
#include "unity.h"

#define MAX_T 16

typedef struct {
    uint8_t out[2 + 255];
    size_t  len;
} xact_t;

static xact_t  sent[MAX_T];
static int     n_sent;
static uint8_t reply[MAX_T][2 + 255]; /* MISO bytes per transaction */
static int16_t fail_at = -1;          /* transaction index that times out on BUSY */
static int     stat_calls;

static int16_t f_xfer(void *ctx, uint8_t *out, uint8_t *in, size_t len)
{
    (void)ctx;
    int i = n_sent++;
    TEST_ASSERT_TRUE(i < MAX_T);
    memcpy(sent[i].out, out, len);
    sent[i].len = len;
    if (i == fail_at) {
        return OC_LR_ERR_SPI_CMD_TIMEOUT; /* nothing clocked: in[] untouched */
    }
    memcpy(in, reply[i], len);
    return 0;
}

static int16_t f_stat(uint8_t stat1)
{
    stat_calls++;
    return stat1 == 0xEE ? -707 : 0; /* any error code; RadioLib's mapping in the firmware */
}

static const oc_lr_bus_t bus = { NULL, f_xfer, f_stat };

void setUp(void)
{
    memset(sent, 0, sizeof(sent));
    memset(reply, 0, sizeof(reply));
    n_sent = 0;
    fail_at = -1;
    stat_calls = 0;
}
void tearDown(void) {}

static void expect(int i, const uint8_t *bytes, size_t len)
{
    TEST_ASSERT_EQUAL_size_t(len, sent[i].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, sent[i].out, len);
}

static void test_write_commands_are_one_transaction_opcode_then_arguments(void)
{
    static const uint8_t clr[] = { 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t fs[] = { 0x01, 0x29 };
    static const uint8_t rxf[] = { 0x01, 0x1E };
    static const uint8_t txf[] = { 0x01, 0x1F };
    static const uint8_t data[3] = { 0xA1, 0xB2, 0xC3 };
    static const uint8_t wr[] = { 0x00, 0x02, 0xA1, 0xB2, 0xC3 };
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_clear_irq(&bus));
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_set_fs(&bus));
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_clear_rx_fifo(&bus));
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_clear_tx_fifo(&bus));
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_write_tx_fifo(&bus, data, sizeof(data)));
    TEST_ASSERT_EQUAL_INT(5, n_sent);
    expect(0, clr, sizeof(clr));
    expect(1, fs, sizeof(fs));
    expect(2, rxf, sizeof(rxf));
    expect(3, txf, sizeof(txf));
    expect(4, wr, sizeof(wr));
}

static void test_write_reports_the_previous_commands_status_from_stat1(void)
{
    reply[0][0] = 0xEE;
    TEST_ASSERT_EQUAL_INT16(-707, oc_lr_set_fs(&bus));
    TEST_ASSERT_EQUAL_INT(1, stat_calls);
}

static void test_rf_frequency_is_the_exact_hz_big_endian(void)
{
    static const uint8_t f915[] = { 0x02, 0x00, 0x35, 0xDE, 0x21, 0x70 }; /* 903 750 000 */
    static const uint8_t f24[] = { 0x02, 0x00, 0x8F, 0x2B, 0x9C, 0x80 };  /* 2 402 000 000 */
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_set_rf_frequency(&bus, 903750000u));
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_set_rf_frequency(&bus, 2402000000u));
    expect(0, f915, sizeof(f915));
    expect(1, f24, sizeof(f24));
}

/* RadioLib's RADIOLIB_CHECK_PARAMS ranges: 150-1090, 1900-2200, 2400-2500 MHz. */
static void test_rf_frequency_outside_the_chip_ranges_is_refused_before_the_bus(void)
{
    static const uint32_t bad[] = { 0u, 149999999u, 1090000001u, 1899999999u, 2200000001u, 2399999999u, 2500000001u };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT16(OC_LR_ERR_INVALID_FREQUENCY, oc_lr_set_rf_frequency(&bus, bad[i]));
    }
    TEST_ASSERT_EQUAL_INT(0, n_sent);
    TEST_ASSERT_TRUE(oc_lr_freq_ok(150000000u));
    TEST_ASSERT_TRUE(oc_lr_freq_ok(1090000000u));
    TEST_ASSERT_TRUE(oc_lr_freq_ok(2500000000u));
}

static void test_irq_read_is_six_nops_and_ignores_stat1(void)
{
    static const uint8_t nops[6] = { 0 };
    const uint8_t r[6] = { 0xEE, 0x00, 0x00, 0x04, 0x00, 0x40 };
    memcpy(reply[0], r, sizeof(r));
    uint32_t irq = 0;
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_get_irq(&bus, &irq));
    expect(0, nops, sizeof(nops));
    TEST_ASSERT_EQUAL_HEX32(0x00040040u, irq);
    TEST_ASSERT_EQUAL_INT(0, stat_calls);
}

static void test_read_is_the_opcode_then_nops_with_the_response_after_two_status_bytes(void)
{
    static const uint8_t cmd[] = { 0x02, 0x12 };
    static const uint8_t nops[4] = { 0 };
    const uint8_t r[4] = { 0x06, 0x00, 0x00, 0x1C };
    memcpy(reply[1], r, sizeof(r));
    uint8_t resp[2] = { 0xAA, 0xAA };
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_read(&bus, OC_LR_OP_GET_RX_PKT_LENGTH, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_INT(2, n_sent);
    expect(0, cmd, sizeof(cmd));
    expect(1, nops, sizeof(nops));
    TEST_ASSERT_EQUAL_HEX8(0x00, resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x1C, resp[1]);
}

/* Review M2: a failed transfer must not hand back stale or uninitialised bytes. */
static void test_a_failed_read_returns_zeros_not_garbage(void)
{
    const uint8_t garbage[4] = { 0x5A, 0x5A, 0x5A, 0x5A };
    memcpy(reply[1], garbage, sizeof(garbage));
    fail_at = 1;
    uint8_t resp[2] = { 0xAA, 0xAA };
    TEST_ASSERT_EQUAL_INT16(OC_LR_ERR_SPI_CMD_TIMEOUT, oc_lr_read(&bus, OC_LR_OP_GET_RX_PKT_LENGTH, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_HEX8(0, resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0, resp[1]);

    setUp();
    fail_at = 0; /* the opcode transaction itself */
    resp[0] = resp[1] = 0xAA;
    TEST_ASSERT_EQUAL_INT16(OC_LR_ERR_SPI_CMD_TIMEOUT, oc_lr_read(&bus, OC_LR_OP_GET_RX_PKT_LENGTH, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_HEX8(0, resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0, resp[1]);
}

static void test_lora_readout_is_length_fifo_and_packet_status(void)
{
    static const uint8_t len_cmd[] = { 0x02, 0x12 };
    static const uint8_t st_cmd[] = { 0x02, 0x2A };
    const uint8_t len_r[4] = { 0x06, 0x00, 0x00, 0x03 };
    const uint8_t fifo_r[5] = { 0x06, 0x00, 0x11, 0x22, 0x33 };
    const uint8_t st_r[8] = { 0x06, 0x00, 0x14, 3, 0xF6, 77, 80, 0x02 };
    memcpy(reply[1], len_r, sizeof(len_r));
    memcpy(reply[2], fifo_r, sizeof(fifo_r));
    memcpy(reply[4], st_r, sizeof(st_r));
    oc_radio_event_t ev;
    memset(&ev, 0, sizeof(ev));
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_read_rx(&bus, 0, 1, OC_LR_IRQ_RX_DONE | OC_LR_IRQ_LORA_HEADER_VALID, &ev));
    TEST_ASSERT_EQUAL_INT(5, n_sent);
    static const uint8_t nops4[4] = { 0 };
    static const uint8_t fifo[5] = { 0x00, 0x01, 0, 0, 0 };
    static const uint8_t nops8[8] = { 0 };
    expect(0, len_cmd, sizeof(len_cmd));
    expect(1, nops4, sizeof(nops4));
    expect(2, fifo, sizeof(fifo)); /* ReadRxFifo: one transaction, data from byte 2 */
    expect(3, st_cmd, sizeof(st_cmd));
    expect(4, nops8, sizeof(nops8));
    TEST_ASSERT_EQUAL_UINT8(3, ev.len);
    TEST_ASSERT_EQUAL_HEX8(0x11, ev.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x33, ev.data[2]);
    TEST_ASSERT_EQUAL_UINT8(1, ev.crc_ok);
    TEST_ASSERT_EQUAL_INT16(-10, ev.snr_qdb);
    TEST_ASSERT_EQUAL_INT16(-77, ev.rssi_dbm); /* raw 77 << 1 | 1 = 155 -> -77.5 */
}

static void test_flrc_readout_reads_the_five_byte_flrc_status(void)
{
    static const uint8_t st_cmd[] = { 0x02, 0x4B };
    static const uint8_t nops7[7] = { 0 };
    const uint8_t len_r[4] = { 0x06, 0x00, 0x00, 0x01 };
    const uint8_t fifo_r[3] = { 0x06, 0x00, 0x42 };
    const uint8_t st_r[7] = { 0x06, 0x00, 0x00, 0x01, 90, 0xFF, 0x04 };
    memcpy(reply[1], len_r, sizeof(len_r));
    memcpy(reply[2], fifo_r, sizeof(fifo_r));
    memcpy(reply[4], st_r, sizeof(st_r));
    oc_radio_event_t ev;
    memset(&ev, 0, sizeof(ev));
    TEST_ASSERT_EQUAL_INT16(0, oc_lr_read_rx(&bus, 1, 1, OC_LR_IRQ_RX_DONE, &ev));
    expect(3, st_cmd, sizeof(st_cmd));
    expect(4, nops7, sizeof(nops7));
    TEST_ASSERT_EQUAL_UINT8(1, ev.len);
    TEST_ASSERT_EQUAL_HEX8(0x42, ev.data[0]);
    TEST_ASSERT_EQUAL_INT16(-90, ev.rssi_dbm); /* raw 90 << 1 | 1 = 181 -> -90.5 */
    TEST_ASSERT_EQUAL_INT16(0, ev.snr_qdb);
}

static void test_readout_with_a_failed_status_read_is_not_crc_ok_and_has_no_stale_quality(void)
{
    const uint8_t len_r[4] = { 0x06, 0x00, 0x00, 0x01 };
    memcpy(reply[1], len_r, sizeof(len_r));
    memset(reply[4], 0x5A, 8);
    fail_at = 4;
    oc_radio_event_t ev;
    memset(&ev, 0, sizeof(ev));
    TEST_ASSERT_NOT_EQUAL(0, oc_lr_read_rx(&bus, 0, 1, OC_LR_IRQ_RX_DONE | OC_LR_IRQ_LORA_HEADER_VALID, &ev));
    TEST_ASSERT_EQUAL_UINT8(0, ev.crc_ok);
    TEST_ASSERT_EQUAL_INT16(0, ev.rssi_dbm);
    TEST_ASSERT_EQUAL_INT16(0, ev.snr_qdb);
}

static void test_a_zero_length_packet_skips_the_fifo_read(void)
{
    oc_radio_event_t ev;
    memset(&ev, 0, sizeof(ev));
    oc_lr_read_rx(&bus, 0, 1, OC_LR_IRQ_RX_DONE | OC_LR_IRQ_LORA_HEADER_VALID, &ev);
    TEST_ASSERT_EQUAL_INT(4, n_sent);
    TEST_ASSERT_EQUAL_UINT8(0, ev.len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_write_commands_are_one_transaction_opcode_then_arguments);
    RUN_TEST(test_write_reports_the_previous_commands_status_from_stat1);
    RUN_TEST(test_rf_frequency_is_the_exact_hz_big_endian);
    RUN_TEST(test_rf_frequency_outside_the_chip_ranges_is_refused_before_the_bus);
    RUN_TEST(test_irq_read_is_six_nops_and_ignores_stat1);
    RUN_TEST(test_read_is_the_opcode_then_nops_with_the_response_after_two_status_bytes);
    RUN_TEST(test_a_failed_read_returns_zeros_not_garbage);
    RUN_TEST(test_lora_readout_is_length_fifo_and_packet_status);
    RUN_TEST(test_flrc_readout_reads_the_five_byte_flrc_status);
    RUN_TEST(test_readout_with_a_failed_status_read_is_not_crc_ok_and_has_no_stale_quality);
    RUN_TEST(test_a_zero_length_packet_skips_the_fifo_read);
    return UNITY_END();
}
