#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "lc_sig.h"
#include "lc_sig_msg.h"
#include "lc_sig_qr.h"

void setUp(void) {}
void tearDown(void) {}

static void test_number_roundtrip_and_rules(void)
{
    uint8_t bcd[7];
    char text[16];
    static const uint8_t want[7] = { 0x88, 0x36, 0x06, 0x55, 0x51, 0x23, 0x4F };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+8836065551234", 14, bcd));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, bcd, 7);
    lc_sig_number_to_text(bcd, text);
    TEST_ASSERT_EQUAL_STRING("+8836065551234", text);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("8836065551234", 13, bcd)); /* '+' optional */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+1606555123", 11, bcd));   /* not +883 */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+88360655512345", 15, bcd)); /* 14 digits */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+883606555x234", 14, bcd));
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
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_ACT_ACK; m.u.act_ack.confirm[3] = 9; roundtrip(&m, 15);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_ACT_NAK; m.u.act_nak.reason = 2; roundtrip(&m, 9);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_REG_REQ; m.u.reg_req.caps = 1; roundtrip(&m, 4);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_AUTH_REQ; m.u.auth_req.rand[0] = 1; roundtrip(&m, 32);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_AUTH_RSP; m.u.auth_rsp.res[7] = 5; roundtrip(&m, 8);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_AUTH_FAIL; m.u.auth_fail.cause = 1; roundtrip(&m, 1);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_AUTH_FAIL; m.u.auth_fail.cause = 2; m.u.auth_fail.auts[13] = 3;
    roundtrip(&m, 15);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_REG_ACK; m.u.reg_ack.mode = 1; m.u.reg_ack.period_s = 1800;
    roundtrip(&m, 10);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_REG_REJ; m.u.reg_rej.cause = 1; roundtrip(&m, 1);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_CALL_SETUP; m.u.call_setup.ref = 3; roundtrip(&m, 9);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_CALL_PROC; m.u.call_proc.call_id = 0xdeadbeef; roundtrip(&m, 5);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_ALERTING; m.u.call.call_id = 42; roundtrip(&m, 4);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_CONNECT; m.u.connect.codec = 1; roundtrip(&m, 5);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_CONNECT_ACK; m.u.call.call_id = 7; roundtrip(&m, 4);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_SETUP_IND; m.u.setup_ind.caller[6] = 0x4F; roundtrip(&m, 12);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_RELEASE; m.u.release.cause = LC_SIG_CAUSE_BUSY; roundtrip(&m, 5);
    memset(&m, 0, sizeof(m)); m.type = LC_SIG_RELEASE_COMPLETE; roundtrip(&m, 4);
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_body_decode(0x7F, NULL, 0, &m)); /* unknown type */
}

static void test_golden_bytes(void)
{
    lc_sig_msg_t m;
    uint8_t buf[16];
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CALL_SETUP;
    m.u.call_setup.ref = 7;
    lc_sig_number_to_bcd("+8836065551234", 14, m.u.call_setup.called);
    m.u.call_setup.codec_caps = 1;
    static const uint8_t setup[9] = { 0x07, 0x88, 0x36, 0x06, 0x55, 0x51, 0x23, 0x4F, 0x01 };
    TEST_ASSERT_EQUAL_size_t(9, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(setup, buf, 9);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_RELEASE;
    m.u.release.call_id = 0x01020304;
    m.u.release.cause = LC_SIG_CAUSE_BUSY;
    static const uint8_t rel[5] = { 0x01, 0x02, 0x03, 0x04, 0x02 };
    TEST_ASSERT_EQUAL_size_t(5, lc_sig_body_encode(&m, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rel, buf, 5);
}

/* Golden QR (computed independently in Python, 2026-09-26): key id 1, PKn = 1..32,
 * token id a0..a7, secret b0..bf, +8836065551234, expiry 0x12345678. */
static const char k_qr[] = "opencell:1:AQEAAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyCgoaKjpKWmp7CxsrO0tba3uLm6u7y9vr-"
                           "INgZVUSNPeFY0EoZ3";

static void example_qr(lc_sig_qr_t *q)
{
    memset(q, 0, sizeof(*q));
    q->key_id = 1;
    for (int i = 0; i < 32; i++) q->pkn[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 8; i++) q->token_id[i] = (uint8_t)(0xa0 + i);
    for (int i = 0; i < 16; i++) q->token_secret[i] = (uint8_t)(0xb0 + i);
    lc_sig_number_to_bcd("+8836065551234", 14, q->number);
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_number_roundtrip_and_rules);
    RUN_TEST(test_every_message_roundtrips);
    RUN_TEST(test_golden_bytes);
    RUN_TEST(test_qr_golden_format_and_parse);
    RUN_TEST(test_qr_trims_whitespace_and_rejects_corruption);
    return UNITY_END();
}
