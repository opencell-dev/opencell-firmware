#include "unity.h"

#include <string.h>

#include "oc_term_screen.h"

void setUp(void) {}
void tearDown(void) {}

static oc_term_lines_t lines;

static void assert_blank_from(int first)
{
    for (int i = first; i < OC_TERM_SCREEN_LINES; i++) {
        TEST_ASSERT_EQUAL_STRING("", lines[i]);
    }
}

static void test_pairing_screen_shows_the_code_with_leading_zeros(void)
{
    oc_term_pair_view_t v = { .code = 4271, .bonds = 1, .max_bonds = 3, .phone = 0 };
    memset(lines, 'x', sizeof(lines));
    oc_term_pair_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("PAIR CODE", lines[0]);
    TEST_ASSERT_EQUAL_STRING("004271", lines[1]);
    TEST_ASSERT_EQUAL_STRING("BONDED 1/3", lines[2]);
    TEST_ASSERT_EQUAL_STRING("NO PHONE", lines[3]);
    assert_blank_from(4);
}

static void test_pairing_screen_locked_connected_and_cleared(void)
{
    oc_term_pair_view_t v = { .code = 123456, .locked_s = 42, .bonds = 0, .max_bonds = 3, .phone = 1, .cleared = 1 };
    oc_term_pair_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("LOCKED 42S", lines[1]); /* the code is not shown while locked */
    TEST_ASSERT_EQUAL_STRING("BONDS CLEARED", lines[2]);
    TEST_ASSERT_EQUAL_STRING("PHONE CONNECTED", lines[3]);
}

static void test_subscriber_screen(void)
{
    oc_term_sub_view_t v = { .sig_ok = 1, .state = OC_SIG_ST_REGISTERED, .activated = 1, .mode = OC_SIG_MODE_PART15,
                             .number = { 0x88, 0x31, 0x60, 0x65, 0x55, 0x01, 0x23, 0x4F } };
    oc_term_sub_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("SUBSCRIBER", lines[0]);
    TEST_ASSERT_EQUAL_STRING("+883-1-606-555-01234", lines[1]); /* 20 characters: fits the 21 columns */
    TEST_ASSERT_EQUAL_STRING("REGISTERED", lines[2]);
    TEST_ASSERT_EQUAL_STRING("MODE PART 15", lines[3]);
    assert_blank_from(4);

    v.state = OC_SIG_ST_IN_CALL;
    v.mode = OC_SIG_MODE_PART97;
    oc_term_sub_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("IN CALL", lines[2]);
    TEST_ASSERT_EQUAL_STRING("MODE PART 97", lines[3]);

    static const uint8_t v1[OC_SIG_NUMBER_LEN] = { 0x88, 0x36, 0x06, 0x55, 0x51, 0x23, 0x4F, 0x00 };
    memcpy(v.number, v1, sizeof(v1)); /* not a valid number: shown as its digits */
    oc_term_sub_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("+8836065551234", lines[1]);
}

static void test_subscriber_screen_before_activation_and_without_signalling(void)
{
    oc_term_sub_view_t v = { .sig_ok = 1, .state = OC_SIG_ST_NOT_ACTIVATED };
    oc_term_sub_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("NO NUMBER", lines[1]);
    TEST_ASSERT_EQUAL_STRING("NOT ACTIVATED", lines[2]);
    TEST_ASSERT_EQUAL_STRING("MODE -", lines[3]);
    v.state = 42;
    oc_term_sub_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("?", lines[2]);
    v.sig_ok = 0;
    oc_term_sub_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("SIGNALLING OFF", lines[1]);
    assert_blank_from(2);
}

static void test_radio_screen(void)
{
    oc_term_view_t v;
    memset(&v, 0, sizeof(v));
    v.link.state = OC_TERM_GRANTED;
    v.link.band = OC_BAND_915;
    v.link.tier = OC_TIER_EDGE;
    v.link.rssi_dbm = -118;
    v.link.snr_qdb = -27; /* -6.75 dB */
    v.link.frame = 123456;
    v.beacons = 4321;
    v.sync_losses = 2;
    oc_term_radio_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("RADIO", lines[0]);
    TEST_ASSERT_EQUAL_STRING("915MHZ EDGE", lines[1]);
    TEST_ASSERT_EQUAL_STRING("RSSI -118 DBM", lines[2]);
    TEST_ASSERT_EQUAL_STRING("SNR -6.7 DB", lines[3]);
    TEST_ASSERT_EQUAL_STRING("FRAME 123456", lines[4]);
    TEST_ASSERT_EQUAL_STRING("BEACONS 4321", lines[5]);
    TEST_ASSERT_EQUAL_STRING("SYNC LOSS 2", lines[6]);

    v.link.snr_qdb = 2; /* 0.5 dB */
    v.link.state = OC_TERM_SEARCH;
    v.link.heard = 1; /* searching, and the scan heard a packet (spec §3.1) */
    oc_term_radio_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("NO SERVICE", lines[1]);
    TEST_ASSERT_EQUAL_STRING("SIG -118 DBM", lines[2]);
    TEST_ASSERT_EQUAL_STRING("SNR 0.5 DB", lines[3]);
    TEST_ASSERT_EQUAL_STRING("NOISE -", lines[4]);
    v.link.snr_qdb = -1; /* -0.25 dB: the sign survives a zero integer part */
    oc_term_radio_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("SNR -0.2 DB", lines[3]);

    /* channel-list spec §9: source letter, position, frequency, pass */
    v.link.scan_pos = 3;
    v.link.scan_len = 8;
    v.link.scan_src = OC_SCAN_SRC_NET;
    v.link.scan_pass = 2;
    v.link.freq_khz = 903250u;
    oc_term_radio_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("N 3/8 903.25 P2", lines[1]);
    v.link.scan_src = OC_SCAN_SRC_SWEEP;
    v.link.scan_pass = 150;
    oc_term_radio_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("S 3/8 903.25 P99", lines[1]);
}

/* Channel-list spec §9: "SCAN 3/8 903.25" instead of NO SERVICE while
 * searching; "CH 917.25" (the serving anchor) once on a cell. */
static void test_status_screen_scan_and_channel(void)
{
    oc_term_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = OC_TERM_SEARCH;
    st.scan_pos = 3;
    st.scan_len = 8;
    st.freq_khz = 903250u;
    oc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("SCAN 3/8 903.25", lines[1]);
    st.state = OC_TERM_IDLE;
    st.tier = OC_TIER_EDGE;
    st.scan_pos = 0;
    st.freq_khz = 917250u;
    oc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("915MHZ EDGE", lines[1]);
    TEST_ASSERT_EQUAL_STRING("CH 917.25", lines[5]);
    st.state = OC_TERM_SYNCED;
    oc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("NO SERVICE", lines[1]);
    TEST_ASSERT_EQUAL_STRING("CH 917.25", lines[5]);
}

static void test_status_screen_while_searching(void)
{
    oc_term_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = OC_TERM_SEARCH;
    st.tmid = 0x75123456u;
    st.cell_seed = 0xCAFEF00Du; /* a lost cell's: not shown while searching */
    st.heard = 1;
    st.rssi_dbm = -97;
    st.snr_qdb = -13; /* -3.25 dB */
    st.heard_age_s = 4;
    st.noise_dbm = -118;
    oc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("OPENCELL SEARCHING", lines[0]);
    TEST_ASSERT_EQUAL_STRING("NO SERVICE", lines[1]);
    TEST_ASSERT_EQUAL_STRING("SIG -97 SNR -3", lines[2]);
    TEST_ASSERT_EQUAL_STRING("HEARD 4S AGO", lines[3]);
    TEST_ASSERT_EQUAL_STRING("TMID 75123456", lines[4]);
    assert_blank_from(5);

    st.heard = 0;
    oc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("NO SIGNAL", lines[2]);
    TEST_ASSERT_EQUAL_STRING("NOISE -118 DBM", lines[3]);
    TEST_ASSERT_EQUAL_STRING("TMID 75123456", lines[4]);
    assert_blank_from(5);

    st.noise_dbm = OC_TERM_NO_DBM; /* no sample yet, or a radio without rssi_inst */
    oc_term_status_lines(&st, lines);
    TEST_ASSERT_EQUAL_STRING("NOISE -", lines[3]);
}

static void test_radio_screen_while_searching_with_nothing_heard(void)
{
    oc_term_view_t v;
    memset(&v, 0, sizeof(v));
    v.link.state = OC_TERM_SEARCH;
    v.link.noise_dbm = -118;
    v.beacons = 7;
    v.sync_losses = 1;
    oc_term_radio_lines(&v, lines);
    TEST_ASSERT_EQUAL_STRING("RADIO", lines[0]);
    TEST_ASSERT_EQUAL_STRING("NO SERVICE", lines[1]);
    TEST_ASSERT_EQUAL_STRING("NO SIGNAL", lines[2]);
    TEST_ASSERT_EQUAL_STRING("SNR -", lines[3]);
    TEST_ASSERT_EQUAL_STRING("NOISE -118 DBM", lines[4]);
    TEST_ASSERT_EQUAL_STRING("BEACONS 7", lines[5]);
    TEST_ASSERT_EQUAL_STRING("SYNC LOSS 1", lines[6]);
}

/* The user's requirement: never just "searching" - a signal line in every
 * link state, on both screens that show the link. */
static void test_every_state_shows_a_signal_line(void)
{
    static const uint8_t screens[] = { OC_SCREEN_STATUS, OC_SCREEN_RADIO };
    oc_term_view_t v;
    memset(&v, 0, sizeof(v));
    v.link.rssi_dbm = -90;
    for (uint8_t s = OC_TERM_SEARCH; s <= OC_TERM_GRANTED; s++) {
        for (uint8_t heard = 0; heard < 2; heard++) {
            v.link.state = s;
            v.link.heard = heard;
            for (unsigned k = 0; k < 2; k++) {
                oc_term_screen_lines(screens[k], &v, lines);
                if (s == OC_TERM_SEARCH && !heard) {
                    TEST_ASSERT_EQUAL_STRING("NO SIGNAL", lines[2]);
                } else if (s == OC_TERM_SEARCH) {
                    TEST_ASSERT_EQUAL_INT(0, strncmp(lines[2], "SIG -90 ", 8));
                } else {
                    TEST_ASSERT_EQUAL_INT(0, strncmp(lines[2], "RSSI -90 ", 9));
                }
            }
        }
    }
}

static void test_screen_dispatch(void)
{
    oc_term_view_t v;
    memset(&v, 0, sizeof(v));
    v.pair.max_bonds = 3;
    oc_term_screen_lines(OC_SCREEN_STATUS, &v, lines);
    TEST_ASSERT_EQUAL_STRING("OPENCELL SEARCHING", lines[0]);
    assert_blank_from(5);
    oc_term_screen_lines(OC_SCREEN_PAIRING, &v, lines);
    TEST_ASSERT_EQUAL_STRING("PAIR CODE", lines[0]);
    oc_term_screen_lines(OC_SCREEN_SUBSCRIBER, &v, lines);
    TEST_ASSERT_EQUAL_STRING("SUBSCRIBER", lines[0]);
    oc_term_screen_lines(OC_SCREEN_RADIO, &v, lines);
    TEST_ASSERT_EQUAL_STRING("RADIO", lines[0]);
    oc_term_screen_lines(OC_SCREEN_COUNT, &v, lines);
    TEST_ASSERT_EQUAL_STRING("OPENCELL SEARCHING", lines[0]);
}

static void test_every_line_fits_the_panel(void)
{
    static const uint8_t states[] = { 0xFF, OC_TERM_SEARCH }; /* SEARCH has its own lines (§3.1) */
    oc_term_view_t v;
    memset(&v, 0xFF, sizeof(v)); /* worst case: every field at its widest */
    v.link.rssi_dbm = -32768;
    v.link.snr_qdb = -32768;
    v.link.noise_dbm = -32768;
    v.pair.locked_s = 0xFFFFFFFFu;
    for (unsigned k = 0; k < 4; k++) {
        v.link.state = states[k / 2];
        v.link.heard = (uint8_t)(k % 2);
        for (uint8_t s = 0; s < OC_SCREEN_COUNT; s++) {
            oc_term_screen_lines(s, &v, lines);
            for (int i = 0; i < OC_TERM_SCREEN_LINES; i++) {
                TEST_ASSERT_TRUE(strlen(lines[i]) <= OC_TERM_SCREEN_COLS);
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pairing_screen_shows_the_code_with_leading_zeros);
    RUN_TEST(test_pairing_screen_locked_connected_and_cleared);
    RUN_TEST(test_subscriber_screen);
    RUN_TEST(test_subscriber_screen_before_activation_and_without_signalling);
    RUN_TEST(test_radio_screen);
    RUN_TEST(test_status_screen_while_searching);
    RUN_TEST(test_status_screen_scan_and_channel);
    RUN_TEST(test_radio_screen_while_searching_with_nothing_heard);
    RUN_TEST(test_every_state_shows_a_signal_line);
    RUN_TEST(test_screen_dispatch);
    RUN_TEST(test_every_line_fits_the_panel);
    return UNITY_END();
}
