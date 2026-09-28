#include "unity.h"

#include <string.h>

#include "lc_term_gatt.h"
#include "lc_term_screen.h"

void setUp(void) {}
void tearDown(void) {}

static void test_status_lines(void)
{
    lc_term_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = LC_TERM_GRANTED;
    st.band = LC_BAND_2G4;
    st.tier = LC_TIER_NEAR;
    st.rssi_dbm = -71;
    st.snr_qdb = 38;
    st.tmid = 0x75123456u;
    st.cell_seed = 0xCAFEF00Du;
    char lines[LC_TERM_SCREEN_LINES][LC_TERM_SCREEN_COLS + 1];
    lc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("OPENCELL GRANTED", lines[0]);
    TEST_ASSERT_EQUAL_STRING("2.4GHZ NEAR", lines[1]);
    TEST_ASSERT_EQUAL_STRING("RSSI -71 SNR 9", lines[2]);
    TEST_ASSERT_EQUAL_STRING("TMID 75123456", lines[3]);
    TEST_ASSERT_EQUAL_STRING("CELL CAFEF00D", lines[4]);
    st.state = LC_TERM_SEARCH;
    lc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("OPENCELL SEARCHING", lines[0]);
    TEST_ASSERT_EQUAL_STRING("NO SERVICE", lines[1]);
}

static void test_status_packing(void)
{
    /* heard, heard_age_s and noise_dbm (spec 2026-09-27 §3.1) are not sent */
    lc_term_status_t st = { LC_TERM_GRANTED, LC_BAND_915, LC_TIER_EDGE, -118, -27, 0x75123456u, 0x01020304u,
                            0xCAFEF00Du, 1, 4, -120, 0, 0, 0, 0, 0 };
    uint8_t out[LC_GATT_STATUS_LEN];
    lc_term_pack_status(&st, out);
    static const uint8_t expected[LC_GATT_STATUS_LEN] = {
        4, 0, 2, 0, 0x8A, 0xFF, 0xE5, 0xFF, 0x56, 0x34, 0x12, 0x75,
        0x04, 0x03, 0x02, 0x01, 0x0D, 0xF0, 0xFE, 0xCA,
    };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, LC_GATT_STATUS_LEN);
}

static void test_uuid_bytes_are_little_endian(void)
{
    /* 6c630002-7e2a-4b8e-9f2d-3c1a5e7b0d10, least significant byte first */
    static const uint8_t up[16] = { LC_GATT_UUID_BYTES(LC_GATT_ID_UP) };
    TEST_ASSERT_EQUAL_HEX8(0x10, up[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, up[12]);
    TEST_ASSERT_EQUAL_HEX8(0x6c, up[15]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_status_lines);
    RUN_TEST(test_status_packing);
    RUN_TEST(test_uuid_bytes_are_little_endian);
    return UNITY_END();
}
