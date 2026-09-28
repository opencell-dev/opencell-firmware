#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "lc_sig.h"
#include "lc_sig_msg.h"
#include "lc_sig_qr.h"

void setUp(void) {}
void tearDown(void) {}

static const char k_num[] = "+883160655501234";

static void set_number(uint8_t out[LC_SIG_NUMBER_LEN])
{
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd(k_num, strlen(k_num), out));
}

static void roundtrip(const lc_sig_msg_t *m, size_t want_len)
{
    uint8_t buf[64];
    lc_sig_msg_t back;
    size_t n = lc_sig_body_encode(m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(want_len, n);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_body_decode(m->type, buf, n, &back));
    TEST_ASSERT_EQUAL_MEMORY(m, &back, sizeof(back));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_body_decode(m->type, buf, n - 1, &back)); /* exact length only */
}

static void test_every_message_roundtrips(void)
{
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_ACT_REQ; memset(m.u.act_req.pkt, 7, 32); roundtrip(&m, 48);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_ACT_ACK; m.u.act_ack.confirm[3] = 9;
    set_number(m.u.act_ack.number); roundtrip(&m, 16);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_ACT_NAK; m.u.act_nak.reason = 2; roundtrip(&m, 9);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_REG_REQ; m.u.reg_req.caps = 1; roundtrip(&m, 4);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_AUTH_REQ; m.u.auth_req.rand[0] = 1; roundtrip(&m, 32);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_AUTH_RSP; m.u.auth_rsp.res[7] = 5; roundtrip(&m, 8);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_AUTH_FAIL; m.u.auth_fail.cause = 1; roundtrip(&m, 1);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_AUTH_FAIL; m.u.auth_fail.cause = 2; m.u.auth_fail.auts[13] = 3;
    roundtrip(&m, 15);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_REG_ACK; m.u.reg_ack.mode = 1; m.u.reg_ack.period_s = 1800;
    set_number(m.u.reg_ack.number); roundtrip(&m, 11);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_REG_REJ; m.u.reg_rej.cause = 1; roundtrip(&m, 1);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_CALL_SETUP; m.u.call_setup.ref = 3;
    set_number(m.u.call_setup.called); roundtrip(&m, 10);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_CALL_PROC; m.u.call_proc.call_id = 0xdeadbeef; roundtrip(&m, 5);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_ALERTING; m.u.call.call_id = 42; roundtrip(&m, 4);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_CONNECT; m.u.connect.codec = 1; roundtrip(&m, 5);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_CONNECT_ACK; m.u.call.call_id = 7; roundtrip(&m, 4);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_SETUP_IND; m.u.setup_ind.call_id = 9;
    set_number(m.u.setup_ind.caller); roundtrip(&m, 13);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_RELEASE; m.u.release.cause = LC_SIG_CAUSE_BUSY; roundtrip(&m, 5);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_RELEASE_COMPLETE; roundtrip(&m, 4);
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_body_decode(0x7F, NULL, 0, &m)); /* unknown type */
}

/* numbering v2 §4.2: a message whose number is not canonical does not decode. */
static void test_decode_rejects_malformed_numbers(void)
{
    static const struct { uint8_t type; size_t len; size_t at; } k[] = {
        { LC_SIG_ACT_ACK, 16, 0 }, { LC_SIG_REG_ACK, 11, 3 }, { LC_SIG_CALL_SETUP, 10, 1 }, { LC_SIG_SETUP_IND, 13, 4 },
    };
    static const uint8_t bad[][LC_SIG_NUMBER_LEN] = {
        { 0x88, 0x31, 0x60, 0x65, 0x55, 0x12, 0x34, 0xFF }, /* CC 1 with 14 digits */
        { 0x98, 0x31, 0x60, 0x65, 0x55, 0x01, 0x23, 0x4F }, /* not 883 */
        { 0x88, 0x31, 0x60, 0x6A, 0x55, 0x01, 0x23, 0x4F }, /* nibble A */
        { 0x88, 0x34, 0x42, 0x07, 0x94, 0x60, 0x0F, 0x0F }, /* a digit after the filler */
        { 0 },                                              /* all zero */
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        uint8_t body[16] = { 0 };
        lc_sig_msg_t m;
        set_number(body + k[i].at);
        TEST_ASSERT_EQUAL_INT(0, lc_sig_body_decode(k[i].type, body, k[i].len, &m));
        for (size_t j = 0; j < sizeof(bad) / sizeof(bad[0]); j++) {
            memcpy(body + k[i].at, bad[j], LC_SIG_NUMBER_LEN);
            TEST_ASSERT_EQUAL_INT(-1, lc_sig_body_decode(k[i].type, body, k[i].len, &m));
        }
    }
}

/* The four messages that carry a number, byte for byte (spec §4.2). */
static void test_golden_bytes(void)
{
    lc_sig_msg_t m;
    uint8_t buf[32];
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CALL_SETUP;
    m.u.call_setup.ref = 7;
    set_number(m.u.call_setup.called);
    m.u.call_setup.codec_caps = 1;
    static const uint8_t setup[10] = { 0x07, 0x88, 0x31, 0x60, 0x65, 0x55, 0x01, 0x23, 0x4F, 0x01 };
    TEST_ASSERT_EQUAL_size_t(10, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(setup, buf, 10);

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_ACT_ACK;
    set_number(m.u.act_ack.number);
    for (int i = 0; i < 8; i++) m.u.act_ack.confirm[i] = (uint8_t)(0xc0 + i);
    static const uint8_t ack[16] = { 0x88, 0x31, 0x60, 0x65, 0x55, 0x01, 0x23, 0x4F,
                                     0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7 };
    TEST_ASSERT_EQUAL_size_t(16, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ack, buf, 16);

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_REG_ACK;
    m.u.reg_ack.mode = LC_SIG_MODE_PART15;
    m.u.reg_ack.period_s = 1800;
    set_number(m.u.reg_ack.number);
    static const uint8_t reg[11] = { 0x01, 0x07, 0x08, 0x88, 0x31, 0x60, 0x65, 0x55, 0x01, 0x23, 0x4F };
    TEST_ASSERT_EQUAL_size_t(11, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reg, buf, 11);

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_SETUP_IND;
    m.u.setup_ind.call_id = 0x01020304;
    set_number(m.u.setup_ind.caller);
    m.u.setup_ind.codec_caps = 1;
    static const uint8_t ind[13] = { 0x01, 0x02, 0x03, 0x04, 0x88, 0x31, 0x60, 0x65, 0x55, 0x01, 0x23, 0x4F, 0x01 };
    TEST_ASSERT_EQUAL_size_t(13, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ind, buf, 13);

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_RELEASE;
    m.u.release.call_id = 0x01020304;
    m.u.release.cause = LC_SIG_CAUSE_BUSY;
    static const uint8_t rel[5] = { 0x01, 0x02, 0x03, 0x04, 0x02 };
    TEST_ASSERT_EQUAL_size_t(5, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rel, buf, 5);
}

/* Channel-list spec §7: CHAN_LIST 0x16 and CHAN_LIST_ACK 0x17, big-endian. */
static void test_chan_list_golden_bytes(void)
{
    lc_sig_msg_t m, back;
    uint8_t buf[64];
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CHAN_LIST;
    m.u.chan_list.ver = 3;
    m.u.chan_list.count = 2;
    m.u.chan_list.freq_hz[0] = 917250000u; /* 0x36AC1FD0: ch 30 */
    m.u.chan_list.freq_hz[1] = 907250000u; /* 0x36138950: ch 10 */
    m.u.chan_list.flags[1] = LC_SIG_CHAN_FIXED;
    static const uint8_t list[12] = { 0x03, 0x02, 0x36, 0xAC, 0x1F, 0xD0, 0x00, 0x36, 0x13, 0x89, 0x50, 0x01 };
    TEST_ASSERT_EQUAL_size_t(12, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(list, buf, 12);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_body_decode(LC_SIG_CHAN_LIST, list, 12, &back));
    TEST_ASSERT_EQUAL_MEMORY(&m, &back, sizeof(m));
    roundtrip(&m, 12);

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CHAN_LIST_ACK;
    m.u.chan_list_ack.ver = 3;
    roundtrip(&m, 1);

    memset(&m, 0, sizeof(m)); /* count 0 clears the network's entries */
    m.type = LC_SIG_CHAN_LIST;
    m.u.chan_list.ver = 4;
    roundtrip(&m, 2);
}

/* At most 12 entries (62 bytes): 13 neither encode nor decode. */
static void test_chan_list_limits(void)
{
    lc_sig_msg_t m;
    uint8_t buf[80];
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CHAN_LIST;
    m.u.chan_list.count = LC_SIG_CHAN_MAX;
    for (int i = 0; i < 12; i++) m.u.chan_list.freq_hz[i] = 902250000u + 500000u * (uint32_t)i;
    TEST_ASSERT_EQUAL_size_t(62, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_body_decode(LC_SIG_CHAN_LIST, buf, 62, &m));
    m.u.chan_list.count = 13;
    TEST_ASSERT_EQUAL_size_t(0, lc_sig_body_encode(&m, buf, sizeof(buf)));
    memset(buf, 0, sizeof(buf));
    buf[1] = 13;
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_body_decode(LC_SIG_CHAN_LIST, buf, 67, &m));
    buf[1] = 2;
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_body_decode(LC_SIG_CHAN_LIST, buf, 11, &m)); /* count says 12 bytes */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_body_decode(LC_SIG_CHAN_LIST, buf, 1, &m));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_body_decode(LC_SIG_CHAN_LIST_ACK, buf, 2, &m));
}

/* Golden QR v2 (computed independently in Python, 2026-09-27): key id 1, PKn = 1..32,
 * token id a0..a7, secret b0..bf, +883160655501234, expiry 0x12345678. */
static const char k_qr[] = "opencell:2:AgEAAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyCgoaKjpKWmp7CxsrO0tba3uLm6u7y9vr-"
                           "IMWBlVQEjT3hWNBIAAD44";
/* The same code as v1 (plan 5's golden), and v2 with reserved byte 71 = 1 (CRC fixed up). */
static const char k_qr_v1[] = "opencell:1:AQEAAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyCgoaKjpKWmp7CxsrO0tba3uLm6u7y9vr-"
                              "INgZVUSNPeFY0EoZ3";
static const char k_qr_reserved[] = "opencell:2:AgEAAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyCgoaKjpKWmp7CxsrO0tba3uLm6u7y9vr-"
                                    "IMWBlVQEjT3hWNBIBAA8L";
/* v2 with a CC-1 number of 14 digits (CRC fixed up). */
static const char k_qr_badnum[] = "opencell:2:AgEAAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyCgoaKjpKWmp7CxsrO0tba3uLm6u7y9vr-"
                                  "IMWBlVRI0_3hWNBIAAFxO";

static void example_qr(lc_sig_qr_t *q)
{
    memset(q, 0, sizeof(*q));
    q->key_id = 1;
    for (int i = 0; i < 32; i++) q->pkn[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 8; i++) q->token_id[i] = (uint8_t)(0xa0 + i);
    for (int i = 0; i < 16; i++) q->token_secret[i] = (uint8_t)(0xb0 + i);
    set_number(q->number);
    q->expiry = 0x12345678u;
}

static void test_qr_golden_format_and_parse(void)
{
    lc_sig_qr_t q, back;
    char text[128];
    example_qr(&q);
    TEST_ASSERT_EQUAL_size_t(strlen(k_qr), lc_sig_qr_format(&q, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING(k_qr, text);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_qr_parse(k_qr, strlen(k_qr), &back));
    TEST_ASSERT_EQUAL_MEMORY(&q, &back, sizeof(q));
}

static void test_qr_trims_whitespace_and_rejects_corruption(void)
{
    lc_sig_qr_t q;
    char buf[160];
    snprintf(buf, sizeof(buf), "  %s\r\n", k_qr);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_qr_parse(buf, strlen(buf), &q));
    strcpy(buf, k_qr);
    buf[40] = buf[40] == 'A' ? 'B' : 'A'; /* one character changed: CRC must catch it */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_qr_parse(buf, strlen(buf), &q));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_qr_parse("opencell:2:AAAA", 15, &q));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_qr_parse(k_qr, strlen(k_qr) - 1, &q)); /* truncated */
}

/* numbering v2 §6.1: a v1 code and a v2 code with a non-zero reserved byte are refused. */
static void test_qr_v2_only(void)
{
    lc_sig_qr_t q;
    TEST_ASSERT_EQUAL_size_t(111, strlen(k_qr));
    TEST_ASSERT_EQUAL_size_t(LC_SIG_QR_TEXT, strlen(k_qr));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_qr_parse(k_qr_v1, strlen(k_qr_v1), &q));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_qr_parse(k_qr_reserved, strlen(k_qr_reserved), &q));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_qr_parse(k_qr_badnum, strlen(k_qr_badnum), &q));
    char v1_as_v2[128];
    snprintf(v1_as_v2, sizeof(v1_as_v2), "%s", k_qr_v1);
    v1_as_v2[9] = '2'; /* "opencell:2:" in front of a v1 blob: wrong length */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_qr_parse(v1_as_v2, strlen(v1_as_v2), &q));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_message_roundtrips);
    RUN_TEST(test_decode_rejects_malformed_numbers);
    RUN_TEST(test_golden_bytes);
    RUN_TEST(test_chan_list_golden_bytes);
    RUN_TEST(test_chan_list_limits);
    RUN_TEST(test_qr_golden_format_and_parse);
    RUN_TEST(test_qr_trims_whitespace_and_rejects_corruption);
    RUN_TEST(test_qr_v2_only);
    return UNITY_END();
}
