#include "unity.h"

#include "exec_fixture.h"
#include "lc_bsr.h"

static lc_bsr_t bsr;
static lc_fwupd_t fw;
static lc_config_t saved_cfg;
static int saves;
static int fail_save;

static int save(void *ctx, const lc_config_t *cfg)
{
    (void)ctx;
    saves++;
    saved_cfg = *cfg;
    return fail_save ? -1 : 0;
}

static int fw_begin(void *c) { (void)c; return 0; }
static int fw_write(void *c, uint32_t o, const uint8_t *d, uint16_t l) { (void)c; (void)o; (void)d; (void)l; return 0; }
static int fw_finish(void *c, uint32_t s) { (void)c; (void)s; return 0; }
static void fw_abort(void *c) { (void)c; }

static lc_msg_t in, ack;

void setUp(void)
{
    fixture_reset();
    saves = 0;
    fail_save = 0;
    const lc_fwupd_ops_t fops = { NULL, fw_begin, fw_write, fw_finish, fw_abort };
    lc_fwupd_init(&fw, &fops);
    const lc_bsr_ops_t ops = { NULL, save };
    lc_bsr_init(&bsr, &ops, &clk, &exec_, &fw, NULL);
    memset(&in, 0, sizeof(in));
}
void tearDown(void) {}

static uint8_t send(void)
{
    TEST_ASSERT_EQUAL_INT(1, lc_bsr_handle(&bsr, &in, T0 + 10000, &ack));
    TEST_ASSERT_EQUAL_UINT8(LC_MSG_ACK, ack.type);
    TEST_ASSERT_EQUAL_UINT8(in.seq, ack.u.ack.acked_seq);
    return ack.u.ack.status;
}

static void configure(uint8_t role)
{
    in.type = LC_MSG_CONFIG;
    in.seq = 1;
    in.u.config = (lc_config_t){ role, LC_BAND_915, 0, 0xCAFEF00Du };
}

static void test_config_is_saved_and_applied(void)
{
    configure(LC_ROLE_BS_RADIO);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, send());
    TEST_ASSERT_EQUAL_INT(1, saves);
    TEST_ASSERT_EQUAL_HEX32(0xCAFEF00Du, saved_cfg.cell_seed);
    TEST_ASSERT_TRUE(bsr.configured);
}

static void test_bad_or_terminal_config_rejected(void)
{
    configure(LC_ROLE_TERMINAL);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_UNSUPPORTED, send());
    configure(LC_ROLE_BS_RADIO);
    in.u.config.band = LC_BAND_COUNT;
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_MALFORMED, send());
    configure(LC_ROLE_BS_RADIO);
    in.u.config.radio_index = LC_MAX_RADIOS_PER_BAND;
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_MALFORMED, send());
    configure(9);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_MALFORMED, send());
    TEST_ASSERT_EQUAL_INT(0, saves);
    TEST_ASSERT_FALSE(bsr.configured);
}

static void test_config_save_failure_is_flash_error(void)
{
    fail_save = 1;
    configure(LC_ROLE_BS_RADIO);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_FLASH, send());
    TEST_ASSERT_FALSE(bsr.configured);
}

static void test_saved_config_restored_at_boot(void)
{
    const lc_config_t cfg = { LC_ROLE_BS_RADIO, LC_BAND_2G4, 1, 7 };
    const lc_bsr_ops_t ops = { NULL, save };
    lc_bsr_init(&bsr, &ops, &clk, &exec_, &fw, &cfg);
    TEST_ASSERT_TRUE(bsr.configured);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_2G4, bsr.config.band);

    const lc_config_t junk = { LC_ROLE_BS_RADIO, 0xEE, 0, 0 };
    lc_bsr_init(&bsr, &ops, &clk, &exec_, &fw, &junk);
    TEST_ASSERT_FALSE(bsr.configured);
}

static void test_schedule_needs_config(void)
{
    static const uint8_t v[28] = { 1 };
    in.type = LC_MSG_SCHEDULE;
    in.u.schedule.frame_number = F0 + 1;
    in.u.schedule.flags = LC_SCHED_FLAG_FIRST | LC_SCHED_FLAG_LAST;
    in.u.schedule.slot_count = 1;
    in.u.schedule.slots[0] = tx_slot(0, 17000, v, sizeof(v));
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_UNSUPPORTED, send());

    lc_msg_t sched = in;
    configure(LC_ROLE_BS_RADIO);
    send();
    in = sched;
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, send());
}

static void test_time_label_ack(void)
{
    in.type = LC_MSG_TIME;
    in.u.time.unix_s = UTC0;
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, send()); /* 10 ms after edge */
    TEST_ASSERT_EQUAL_INT(1, lc_bsr_handle(&bsr, &in, T0 + 950000, &ack));
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_LATE, ack.u.ack.status);
}

static void test_fw_commit_sets_reboot_pending(void)
{
    static uint8_t img[16];
    in.type = LC_MSG_FW_CHUNK;
    in.u.fw_chunk = (lc_fw_chunk_t){ 0, sizeof(img), img };
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, send());
    in.type = LC_MSG_FW_COMMIT;
    in.u.fw_commit.image_size = sizeof(img);
    TEST_ASSERT_EQUAL_UINT8(LC_ACK_OK, send());
    TEST_ASSERT_TRUE(bsr.reboot_pending);
}

static void test_device_to_host_types_unsupported(void)
{
    const uint8_t types[] = { LC_MSG_RX_REPORT, LC_MSG_STATUS, LC_MSG_ACK, 0x7F };
    for (size_t i = 0; i < sizeof(types); i++) {
        in.type = types[i];
        TEST_ASSERT_EQUAL_UINT8(LC_ACK_ERR_UNSUPPORTED, send());
    }
}

static void test_tx_policy_follows_config_and_clock(void)
{
    lc_bsr_tick(&bsr, T0 + 10000);
    TEST_ASSERT_FALSE(exec_.tx_enabled); /* not configured */
    configure(LC_ROLE_BS_RADIO);
    send();
    lc_bsr_tick(&bsr, T0 + 10000);
    TEST_ASSERT_TRUE(exec_.tx_enabled);
    lc_bsr_tick(&bsr, T0 + 2000000); /* PPS missing: holdover, TX still allowed */
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_HOLDOVER, clk.state);
    TEST_ASSERT_TRUE(exec_.tx_enabled);
    lc_bsr_tick(&bsr, T0 + 31000000); /* holdover expired */
    TEST_ASSERT_FALSE(exec_.tx_enabled);
}

static void test_status_and_rx_report_contents(void)
{
    exec_.schedule_misses = 4;
    exec_.last_tx_end_us = 25630;
    exec_.last_tx_start_us = 20310;
    exec_.late_slots = 9;
    exec_.radio_errors = 70000; /* saturates on the wire */
    exec_.last_radio_err = -2;
    exec_.last_radio_op = LC_EXEC_OP_LAUNCH;
    lc_msg_t st;
    lc_bsr_make_status(&bsr, T0 + 130000, 1234, -5, 2, &st);
    TEST_ASSERT_EQUAL_UINT8(LC_MSG_STATUS, st.type);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_LOCKED, st.u.status.pps_locked);
    TEST_ASSERT_EQUAL_UINT16(4, st.u.status.schedule_misses);
    TEST_ASSERT_EQUAL_UINT16(2, st.u.status.uart_crc_errors);
    TEST_ASSERT_EQUAL_UINT32(F0 + 1, st.u.status.frame_number);
    TEST_ASSERT_EQUAL_INT32(25630, st.u.status.last_tx_end_us);
    TEST_ASSERT_EQUAL_INT32(20310, st.u.status.last_tx_start_us);
    TEST_ASSERT_EQUAL_UINT16(9, st.u.status.late_slots);
    TEST_ASSERT_EQUAL_UINT16(65535, st.u.status.radio_errors);
    TEST_ASSERT_EQUAL_INT16(-2, st.u.status.last_radio_err);
    TEST_ASSERT_EQUAL_UINT8(LC_EXEC_OP_LAUNCH, st.u.status.last_radio_op);

    lc_radio_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LC_RADIO_EV_RX_DONE;
    ev.crc_ok = 1;
    ev.len = 3;
    ev.rssi_dbm = -101;
    ev.snr_qdb = -12;
    ev.data[2] = 0x77;
    lc_msg_t rr;
    lc_bsr_make_rx_report(&bsr, F0 + 1, 5, &ev, &rr);
    TEST_ASSERT_EQUAL_UINT8(LC_MSG_RX_REPORT, rr.type);
    TEST_ASSERT_EQUAL_UINT8(5, rr.u.rx_report.slot_index);
    TEST_ASSERT_EQUAL_INT16(-12, rr.u.rx_report.snr_qdb);
    TEST_ASSERT_EQUAL_HEX8(0x77, rr.u.rx_report.payload[2]);
    TEST_ASSERT_NOT_EQUAL(st.seq, rr.seq);

    uint8_t wire[64];
    TEST_ASSERT_TRUE(lc_link_write_frame(&rr, wire, sizeof(wire)) > 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_config_is_saved_and_applied);
    RUN_TEST(test_bad_or_terminal_config_rejected);
    RUN_TEST(test_config_save_failure_is_flash_error);
    RUN_TEST(test_saved_config_restored_at_boot);
    RUN_TEST(test_schedule_needs_config);
    RUN_TEST(test_time_label_ack);
    RUN_TEST(test_fw_commit_sets_reboot_pending);
    RUN_TEST(test_device_to_host_types_unsupported);
    RUN_TEST(test_tx_policy_follows_config_and_clock);
    RUN_TEST(test_status_and_rx_report_contents);
    return UNITY_END();
}
