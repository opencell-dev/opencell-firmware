#include "unity.h"

#include <string.h>

#include "oc_sig.h"
#include "oc_term_gatt.h"
#include "oc_term_screen.h"

void setUp(void) {}
void tearDown(void) {}

static void test_status_lines(void)
{
    oc_term_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = OC_TERM_GRANTED;
    st.band = OC_BAND_2G4;
    st.tier = OC_TIER_NEAR;
    st.rssi_dbm = -71;
    st.snr_qdb = 38;
    st.tmid = 0x75123456u;
    st.cell_seed = 0xCAFEF00Du;
    char lines[OC_TERM_SCREEN_LINES][OC_TERM_SCREEN_COLS + 1];
    oc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("OPENCELL GRANTED", lines[0]);
    TEST_ASSERT_EQUAL_STRING("2.4GHZ NEAR", lines[1]);
    TEST_ASSERT_EQUAL_STRING("RSSI -71 SNR 9", lines[2]);
    TEST_ASSERT_EQUAL_STRING("TMID 75123456", lines[3]);
    TEST_ASSERT_EQUAL_STRING("CELL CAFEF00D", lines[4]);
    st.state = OC_TERM_SEARCH;
    oc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("OPENCELL SEARCHING", lines[0]);
    TEST_ASSERT_EQUAL_STRING("NO SERVICE", lines[1]);
}

static void test_status_packing(void)
{
    /* heard, heard_age_s, noise_dbm (spec 2026-09-27 §3.1) and scan_pass are
     * not sent; the scan fields are appended (channel-list spec §9) */
    oc_term_status_t st = { OC_TERM_SEARCH, OC_BAND_915, OC_TIER_EDGE, -118, -27, 0x75123456u, 0x01020304u,
                            0xCAFEF00Du, 1, 4, -120, 3, 8, OC_SCAN_SRC_NET, 2, 903250u };
    uint8_t out[OC_GATT_STATUS_LEN];
    oc_term_pack_status(&st, out);
    static const uint8_t expected[OC_GATT_STATUS_LEN] = {
        0, 0, 2, 0, 0x8A, 0xFF, 0xE5, 0xFF, 0x56, 0x34, 0x12, 0x75, 0x04, 0x03,
        0x02, 0x01, 0x0D, 0xF0, 0xFE, 0xCA, 0x03, 0x08, 0x03, 0x52, 0xC8, 0x0D, 0x00,
    };
    TEST_ASSERT_EQUAL_UINT(27, OC_GATT_STATUS_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, OC_GATT_STATUS_LEN);
}

static uint32_t ch(uint8_t c) { return oc_channel_freq_hz(OC_BAND_915, c); }

/* The SCAN characteristic: header, then the assembled list (spec §9). */
static void test_scan_packing(void)
{
    oc_term_scan_t s;
    oc_term_scan_init(&s);
    s.mode = OC_PHY_MODE_PART97;
    s.net_ver = 4;
    s.n_user = 1;
    s.user[0] = (oc_scan_ent_t){ ch(10), OC_SCAN_F_FIXED };
    uint8_t out[OC_GATT_SCAN_MAX];
    TEST_ASSERT_EQUAL_size_t(6 + 7 * 5, oc_term_pack_scan(&s, out));
    static const uint8_t head[16] = { 0x01, 0x02, 0x02, 0x0D, 0x04, 0x07, 0x50, 0x89, 0x13, 0x36,
                                      0x15, 0x10, 0x3E, 0xC7, 0x35, 0x1A };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, out, sizeof(head)); /* ch 10 FIXED user active, ch 0 default active */

    s.n_user = OC_SCAN_MAX_USER; /* the longest value fits */
    s.n_net = OC_SCAN_MAX_NET;
    s.n_learn = OC_SCAN_MAX_LEARN;
    s.last = (oc_scan_ent_t){ ch(51), 0 };
    for (uint8_t i = 0; i < OC_SCAN_MAX_USER; i++) s.user[i] = (oc_scan_ent_t){ ch((uint8_t)(6 + i)), 0 };
    for (uint8_t i = 0; i < OC_SCAN_MAX_NET; i++) s.net[i] = (oc_scan_ent_t){ ch((uint8_t)(10 + i)), 0 };
    for (uint8_t i = 0; i < OC_SCAN_MAX_LEARN; i++) s.learn[i] = (oc_scan_ent_t){ ch((uint8_t)(30 + i)), 0 };
    TEST_ASSERT_EQUAL_size_t(OC_GATT_SCAN_MAX, oc_term_pack_scan(&s, out));
    TEST_ASSERT_EQUAL_UINT(141, OC_GATT_SCAN_MAX);
}

/* SCAN carries every stored entry, even one the walk skips as a duplicate:
 * the app rebuilds "your channels" from SCAN's USER entries, so a user entry
 * shadowed by the last serving cell must still be there. */
static void test_scan_keeps_shadowed_user_entries(void)
{
    oc_term_scan_t s;
    oc_term_scan_init(&s);
    s.last = (oc_scan_ent_t){ 922250000u, 0 };
    s.n_user = 2;
    s.user[0] = (oc_scan_ent_t){ 922250000u, 0 };
    s.user[1] = (oc_scan_ent_t){ 917250000u, 0 };
    uint8_t out[OC_GATT_SCAN_MAX];
    TEST_ASSERT_EQUAL_size_t(6 + 9 * 5, oc_term_pack_scan(&s, out));
    TEST_ASSERT_EQUAL_UINT8(9, out[5]);
    static const uint8_t first3[15] = {
        0x10, 0x6B, 0xF8, 0x36, 0x10 | (OC_SCAN_SRC_LAST << 1), /* last 922.25 */
        0x10, 0x6B, 0xF8, 0x36, 0x10 | (OC_SCAN_SRC_USER << 1), /* user 922.25 */
        0xD0, 0x1F, 0xAC, 0x36, 0x10 | (OC_SCAN_SRC_USER << 1), /* user 917.25 */
    };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first3, out + 6, sizeof(first3));
    /* the walk still dwells on 922.25 once: 922.25, 917.25, the six defaults */
    int n922 = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t f, d;
        oc_term_scan_next(&s, &f, &d);
        TEST_ASSERT_EQUAL_UINT8(8, s.cur_len);
        n922 += f == 922250000u;
        TEST_ASSERT_EQUAL_INT(i == 7, oc_term_scan_advance(&s));
    }
    TEST_ASSERT_EQUAL_INT(1, n922);
}

static int scan_cmd(oc_term_scan_t *s, const uint8_t *c, size_t n) { return oc_term_gatt_scan_command(s, c, n); }

/* COMMAND 0x07 SCAN: sub-ops, lengths and arguments (spec §9). */
static void test_scan_command_codec(void)
{
    oc_term_scan_t s;
    oc_term_scan_init(&s);
    /* SET_USER 917.25 MHz FIXED, 902.75 MHz */
    static const uint8_t set2[13] = { 0x07, 0x01, 0x02, 0xD0, 0x1F, 0xAC, 0x36, 0x01, 0x30, 0xDF, 0xCE, 0x35, 0x00 };
    TEST_ASSERT_EQUAL_INT(0, scan_cmd(&s, set2, sizeof(set2)));
    TEST_ASSERT_EQUAL_UINT8(2, s.n_user);
    TEST_ASSERT_EQUAL_UINT32(917250000u, s.user[0].freq_hz);
    TEST_ASSERT_EQUAL_HEX8(OC_SCAN_F_FIXED, s.user[0].flags);
    TEST_ASSERT_EQUAL_UINT32(902750000u, s.user[1].freq_hz);
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_LEN, scan_cmd(&s, set2, sizeof(set2) - 1));
    uint8_t bad[13];
    memcpy(bad, set2, sizeof(bad));
    bad[3] = 0xD1; /* 917250001 Hz: off the grid */
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_ARG, scan_cmd(&s, bad, sizeof(bad)));
    TEST_ASSERT_EQUAL_UINT32(917250000u, s.user[0].freq_hz); /* unchanged */
    /* the same frequency twice (flags other than FIXED ignored): refused */
    static const uint8_t dup[13] = { 0x07, 0x01, 0x02, 0xD0, 0x1F, 0xAC, 0x36, 0x01, 0xD0, 0x1F, 0xAC, 0x36, 0x03 };
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_ARG, scan_cmd(&s, dup, sizeof(dup)));
    TEST_ASSERT_EQUAL_UINT32(902750000u, s.user[1].freq_hz); /* unchanged */
    uint8_t five[3 + 25] = { 0x07, 0x01, 0x05 };
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_ARG, scan_cmd(&s, five, sizeof(five)));
    static const uint8_t clear[3] = { 0x07, 0x01, 0x00 };
    TEST_ASSERT_EQUAL_INT(0, scan_cmd(&s, clear, sizeof(clear)));
    TEST_ASSERT_EQUAL_UINT8(0, s.n_user);

    static const uint8_t fb[4] = { 0x07, 0x02, 15, 52 };
    TEST_ASSERT_EQUAL_INT(0, scan_cmd(&s, fb, sizeof(fb)));
    TEST_ASSERT_EQUAL_UINT8(OC_SCAN_NEVER, s.fallback_after);
    TEST_ASSERT_EQUAL_UINT8(52, s.fallback_chunk);
    static const uint8_t fb_bad[4] = { 0x07, 0x02, 16, 13 };
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_ARG, scan_cmd(&s, fb_bad, sizeof(fb_bad)));
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_LEN, scan_cmd(&s, fb, 3));

    s.n_learn = 2;
    static const uint8_t forget[2] = { 0x07, 0x03 };
    TEST_ASSERT_EQUAL_INT(0, scan_cmd(&s, forget, sizeof(forget)));
    TEST_ASSERT_EQUAL_UINT8(0, s.n_learn);
    static const uint8_t forget_long[3] = { 0x07, 0x03, 0x00 };
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_LEN, scan_cmd(&s, forget_long, sizeof(forget_long)));
    static const uint8_t unknown[2] = { 0x07, 0x09 };
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_ARG, scan_cmd(&s, unknown, sizeof(unknown)));
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_BAD_LEN, scan_cmd(&s, unknown, 1));
}

static void test_uuid_bytes_are_little_endian(void)
{
    /* 6c630002-7e2a-4b8e-9f2d-3c1a5e7b0d10, least significant byte first */
    static const uint8_t up[16] = { OC_GATT_UUID_BYTES(OC_GATT_ID_UP) };
    TEST_ASSERT_EQUAL_HEX8(0x10, up[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, up[12]);
    TEST_ASSERT_EQUAL_HEX8(0x6c, up[15]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_status_lines);
    RUN_TEST(test_status_packing);
    RUN_TEST(test_scan_packing);
    RUN_TEST(test_scan_keeps_shadowed_user_entries);
    RUN_TEST(test_scan_command_codec);
    RUN_TEST(test_uuid_bytes_are_little_endian);
    return UNITY_END();
}
