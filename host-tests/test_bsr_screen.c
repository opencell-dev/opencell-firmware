#include "unity.h"

#include <string.h>

#include "exec_fixture.h"
#include "oc_bsr.h"

static oc_bsr_t bsr;
static oc_fwupd_t fw;
static char lines[OC_BSR_SCREEN_LINES][OC_BSR_SCREEN_COLS + 1];

static int save(void *ctx, const oc_config_t *cfg) { (void)ctx; (void)cfg; return 0; }
static int fw_begin(void *c) { (void)c; return 0; }
static int fw_write(void *c, uint32_t o, const uint8_t *d, uint16_t l) { (void)c; (void)o; (void)d; (void)l; return 0; }
static int fw_finish(void *c, uint32_t s) { (void)c; (void)s; return 0; }
static void fw_abort(void *c) { (void)c; }

void setUp(void)
{
    fixture_reset();
    oc_clock_init(&clk, 30000000u); /* the fixture pre-locks the clock; start unlocked here */
    const oc_fwupd_ops_t fops = { NULL, fw_begin, fw_write, fw_finish, fw_abort };
    oc_fwupd_init(&fw, &fops);
    const oc_bsr_ops_t ops = { NULL, save };
    oc_bsr_init(&bsr, &ops, &clk, &exec_, &fw, NULL);
}
void tearDown(void) {}

static void test_lines_unconfigured_no_pps(void)
{
    oc_bsr_view_t v;
    memset(&v, 0, sizeof(v));
    oc_bsr_status_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("OPENCELL BS-RADIO", lines[0]);
    TEST_ASSERT_EQUAL_STRING("NOT CONFIGURED", lines[1]);
    TEST_ASSERT_EQUAL_STRING("CLK NO PPS", lines[2]);
    TEST_ASSERT_EQUAL_STRING("FRAME --", lines[3]);
    TEST_ASSERT_EQUAL_STRING("TX OFF  MISS 0", lines[4]);
    TEST_ASSERT_EQUAL_STRING("HOST --  CRC 0", lines[5]);
}

static void test_lines_running_on_2g4(void)
{
    oc_bsr_view_t v = { .configured = 1, .band = OC_BAND_2G4, .radio_index = 1,
                        .clock_state = OC_CLOCK_LOCKED, .ppm_valid = 1, .ppm = 3,
                        .have_frame = 1, .frame = 123456, .tx_on = 1, .misses = 2,
                        .host_ok = 1, .uart_errors = 7 };
    oc_bsr_status_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("2.4 GHZ  RADIO 1", lines[1]);
    TEST_ASSERT_EQUAL_STRING("CLK LOCKED +3PPM", lines[2]);
    TEST_ASSERT_EQUAL_STRING("FRAME 123456", lines[3]);
    TEST_ASSERT_EQUAL_STRING("TX ON   MISS 2", lines[4]);
    TEST_ASSERT_EQUAL_STRING("HOST OK  CRC 7", lines[5]);
}

static void test_lines_915_holdover_and_locked_without_ppm(void)
{
    oc_bsr_view_t v = { .configured = 1, .band = OC_BAND_915, .clock_state = OC_CLOCK_HOLDOVER,
                        .ppm_valid = 1, .ppm = -12 };
    oc_bsr_status_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("915 MHZ  RADIO 0", lines[1]);
    TEST_ASSERT_EQUAL_STRING("CLK HOLDOVER -12PPM", lines[2]);
    v.clock_state = OC_CLOCK_LOCKED;
    v.ppm_valid = 0;
    oc_bsr_status_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("CLK LOCKED", lines[2]);
}

static void test_lines_never_overflow_the_screen(void)
{
    oc_bsr_view_t v = { .configured = 1, .band = 9, .radio_index = 255, .clock_state = 9,
                        .ppm_valid = 1, .ppm = -2000000000, .have_frame = 1, .frame = 0xFFFFFFFFu,
                        .misses = 65535, .uart_errors = 0xFFFFFFFFu };
    oc_bsr_status_lines(&v, lines);
    for (int i = 0; i < OC_BSR_SCREEN_LINES; i++) {
        TEST_ASSERT_TRUE(strlen(lines[i]) <= OC_BSR_SCREEN_COLS);
    }
    TEST_ASSERT_EQUAL_STRING("CLK ?", lines[2]);
    TEST_ASSERT_EQUAL_STRING("HOST --  CRC 99999+", lines[5]);
    v.clock_state = OC_CLOCK_HOLDOVER; /* longest clock word + clamped ppm = 21 chars */
    oc_bsr_status_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("CLK HOLDOVER -9999PPM", lines[2]);
}

static void test_view_reads_bsr_clock_and_exec(void)
{
    oc_bsr_view_t v;
    oc_bsr_view(&bsr, T0, &v);
    TEST_ASSERT_EQUAL_UINT8(0, v.configured);
    TEST_ASSERT_EQUAL_UINT8(OC_CLOCK_UNLOCKED, v.clock_state);
    TEST_ASSERT_EQUAL_UINT8(0, v.have_frame);
    TEST_ASSERT_EQUAL_UINT8(0, v.tx_on);

    oc_msg_t in, ack;
    memset(&in, 0, sizeof(in));
    in.type = OC_MSG_CONFIG;
    in.u.config = (oc_config_t){ OC_ROLE_BS_RADIO, OC_BAND_2G4, 1, 0xCAFEF00Du };
    oc_bsr_handle(&bsr, &in, T0, &ack);
    for (int i = 0; i < 4; i++) {
        oc_clock_on_pps(&clk, T0 + (uint64_t)i * 1000000u);
    }
    TEST_ASSERT_EQUAL_INT(0, oc_clock_on_time(&clk, UTC0 + 3u, T0 + 3000000u + 200000u));
    oc_bsr_tick(&bsr, T0 + 3000000u + 200000u);

    oc_bsr_view(&bsr, T0 + 3000000u + 200000u, &v);
    TEST_ASSERT_EQUAL_UINT8(1, v.configured);
    TEST_ASSERT_EQUAL_UINT8(OC_BAND_2G4, v.band);
    TEST_ASSERT_EQUAL_UINT8(1, v.radio_index);
    TEST_ASSERT_EQUAL_UINT8(OC_CLOCK_LOCKED, v.clock_state);
    TEST_ASSERT_EQUAL_UINT8(1, v.ppm_valid);
    TEST_ASSERT_EQUAL_INT32(0, v.ppm);
    TEST_ASSERT_EQUAL_UINT8(1, v.have_frame);
    uint32_t f;
    TEST_ASSERT_EQUAL_INT(0, oc_clock_frame_at(&clk, T0 + 3000000u + 200000u, &f));
    TEST_ASSERT_EQUAL_UINT32(f, v.frame);
    TEST_ASSERT_EQUAL_UINT8(1, v.tx_on);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lines_unconfigured_no_pps);
    RUN_TEST(test_lines_running_on_2g4);
    RUN_TEST(test_lines_915_holdover_and_locked_without_ppm);
    RUN_TEST(test_lines_never_overflow_the_screen);
    RUN_TEST(test_view_reads_bsr_clock_and_exec);
    return UNITY_END();
}
