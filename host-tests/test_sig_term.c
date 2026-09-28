#include "unity.h"

#include <string.h>

#include "lc_sig_crypto.h"
#include "lc_sig_keys.h"
#include "lc_sig_milenage.h"
#include "lc_sig_term.h"

void setUp(void) {}
void tearDown(void) {}

#define TMID 0x76ad0488u

static lc_sig_term_t t;
static lc_sig_ident_t id;
static lc_sig_chan_t net; /* the network's end of the channel */
static uint8_t ul[64][LC_SIG_LINK_MAX], ul_len[64];
static int ul_n, saves, svc_reqs;
static uint8_t svc_cause; /* of the last service request */
static int svc_refuse_n; /* io_svc refuses (-1, no count) this many times, then succeeds */
static uint8_t ev[16][32], ev_len[16];
static int ev_n;

static int io_send(void *c, const uint8_t *p, uint8_t n) { (void)c; memcpy(ul[ul_n], p, n); ul_len[ul_n++] = n; return 0; }
static int io_svc(void *c, uint8_t cause)
{
    (void)c;
    if (svc_refuse_n > 0) {
        svc_refuse_n--;
        return -1; /* e.g. RACH busy: not counted as an ask that went out */
    }
    svc_cause = cause;
    svc_reqs++;
    return 0;
}
static void io_save(void *c, const lc_sig_ident_t *i) { (void)c; (void)i; saves++; }
static void io_event(void *c, const uint8_t *e, uint8_t n) { (void)c; memcpy(ev[ev_n], e, n); ev_len[ev_n++] = n; }
static const lc_sig_term_io_t io = { NULL, io_send, io_svc, io_save, io_event };

/* MILENAGE test set 1 as the subscriber's keys. */
static const uint8_t K[16] = { 0x46, 0x5b, 0x5c, 0xe8, 0xb1, 0x99, 0xb4, 0x9f, 0xaa, 0x5f, 0x0a, 0x2e, 0xe2, 0x38, 0xa6, 0xbc };
static const uint8_t OPC[16] = { 0xcd, 0x63, 0xcb, 0x71, 0x95, 0x4a, 0x9f, 0x4e, 0x48, 0xa5, 0x99, 0x4e, 0x37, 0xa0, 0x2b, 0xaf };
static const uint8_t AMF[2] = { 0x80, 0x00 };

static void boot(int activated)
{
    uint8_t r[32];
    memset(r, 0x42, sizeof(r));
    lc_sig_ident_new(&id, r);
    if (activated) {
        id.activated = 1;
        memcpy(id.k, K, 16);
        memcpy(id.opc, OPC, 16);
        lc_sig_number_to_bcd("+883160655501234", 16, id.number);
    }
    ul_n = ev_n = saves = svc_reqs = svc_refuse_n = 0;
    lc_sig_term_init(&t, &io, &id, TMID, 0);
    lc_sig_chan_init(&net, 1);
    lc_sig_term_link(&t, 1, 1, 0);
}

static int to_net(lc_sig_msg_t *m)
{
    int got = 0;
    for (int i = 0; i < ul_n; i++) {
        if (lc_sig_chan_rx(&net, ul[i], ul_len[i], m, 0) == 1) got++;
    }
    ul_n = 0;
    return got;
}

static void from_net(const lc_sig_msg_t *m, uint64_t now)
{
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&net, m, now));
    const uint8_t *p;
    uint8_t n;
    while (lc_sig_chan_peek(&net, &p, &n) == 0) {
        uint8_t c[LC_SIG_LINK_MAX];
        memcpy(c, p, n);
        lc_sig_chan_pop(&net);
        lc_sig_term_rx(&t, c, n, now);
    }
    lc_sig_term_tick(&t, now);
}

static uint8_t RAND[16];

static lc_sig_msg_t make_auth(uint64_t sqn, int bad_mac, lc_milenage_t *o)
{
    lc_sig_msg_t m;
    uint8_t s[6];
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_AUTH_REQ;
    for (int i = 0; i < 16; i++) RAND[i] = (uint8_t)(0x30 + i);
    lc_sig_sqn_put(s, sqn);
    lc_milenage(K, OPC, RAND, s, AMF, o);
    memcpy(m.u.auth_req.rand, RAND, 16);
    for (int i = 0; i < 6; i++) m.u.auth_req.autn[i] = (uint8_t)(s[i] ^ o->ak[i]);
    memcpy(m.u.auth_req.autn + 6, AMF, 2);
    memcpy(m.u.auth_req.autn + 8, o->mac_a, 8);
    if (bad_mac) m.u.auth_req.autn[15] ^= 1;
    return m;
}

static lc_milenage_t reg_o;

static void register_ok(void)
{
    lc_sig_msg_t m;
    boot(1);
    lc_sig_term_tick(&t, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
    lc_sig_msg_t a = make_auth(1, 0, &reg_o);
    from_net(&a, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AUTH_RSP, m.type);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reg_o.res, m.u.auth_rsp.res, 8);
    uint8_t ki[16], ke[16];
    lc_sig_session_keys(reg_o.ck, reg_o.ik, RAND, TMID, ki, ke);
    lc_sig_sec_key(&net.sec, ki, ke, 1);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_REG_ACK;
    m.u.reg_ack.mode = LC_SIG_MODE_PART15;
    m.u.reg_ack.period_s = 1800;
    memcpy(m.u.reg_ack.number, id.number, LC_SIG_NUMBER_LEN);
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_REGISTERED, ev[ev_n - 1][0]); /* v3: ev, number (8), mode */
    TEST_ASSERT_EQUAL_UINT8(10, ev_len[ev_n - 1]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(id.number, &ev[ev_n - 1][1], LC_SIG_NUMBER_LEN);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_MODE_PART15, ev[ev_n - 1][9]);
    ev_n = 0;
}

static void test_ident_pack_roundtrip_and_new(void)
{
    uint8_t r[32], blob[LC_SIG_IDENT_BLOB], pub[32];
    lc_sig_ident_t a, b;
    /* numbering v2 spec §4.1: +883160655501234 -> 88 31 60 65 55 01 23 4F */
    static const uint8_t expect_number[LC_SIG_NUMBER_LEN] = { 0x88, 0x31, 0x60, 0x65,
                                                               0x55, 0x01, 0x23, 0x4F };
    memset(r, 7, 32);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_ident_new(&a, r));
    lc_sig_x25519_public(r, pub);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pub, a.pk, 32);
    TEST_ASSERT_FALSE(a.activated);
    a.activated = 1;
    a.key_id = 0x1234;
    a.sqn[5] = 9;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+883160655501234", 16, a.number));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect_number, a.number, LC_SIG_NUMBER_LEN);
    TEST_ASSERT_EQUAL_size_t(LC_SIG_IDENT_BLOB, lc_sig_ident_pack(&a, blob));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect_number, blob + 106, LC_SIG_NUMBER_LEN); /* spec §6.2: number at 106..113 */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_ident_unpack(blob, sizeof(blob), &b));
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect_number, b.number, LC_SIG_NUMBER_LEN); /* unpack restores it */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_ident_unpack(blob, sizeof(blob) - 1, &b));
    blob[0] = 1; /* v2 length, v1 version byte */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_ident_unpack(blob, sizeof(blob), &b));
}

/* numbering v2 §6.2: a v1 blob (113 bytes, version 1) is recognised, so the
 * firmware can start over with a fresh identity; nothing else is. */
static void test_ident_v1_blob_is_old(void)
{
    uint8_t v1[LC_SIG_IDENT_BLOB_V1], v2[LC_SIG_IDENT_BLOB];
    lc_sig_ident_t b;
    memset(v1, 0x5a, sizeof(v1));
    v1[0] = 1;
    memset(&b, 0x77, sizeof(b));
    TEST_ASSERT_EQUAL_INT(LC_SIG_IDENT_OLD, lc_sig_ident_unpack(v1, sizeof(v1), &b));
    TEST_ASSERT_EQUAL_HEX8(0x77, ((uint8_t *)&b)[0]); /* untouched */
    v1[0] = 2; /* v1 length, v2 version byte */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_ident_unpack(v1, sizeof(v1), &b));
    memset(v2, 0, sizeof(v2));
    v2[0] = 3; /* a later version */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_ident_unpack(v2, sizeof(v2), &b));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_ident_unpack(v2, 0, &b));
}

static void test_commands_in_wrong_state_refused(void)
{
    boot(0);
    static const uint8_t dial[] = "\x02+883160655501234";
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&t, dial, sizeof(dial) - 1, 0));
    uint8_t c = LC_SIG_CMD_ANSWER;
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&t, &c, 1, 0));
    c = LC_SIG_CMD_REJECT;
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&t, &c, 1, 0));
    c = LC_SIG_CMD_HANGUP;
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&t, &c, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_BAD_LEN, lc_sig_term_command(&t, &c, 0, 0));
    static const uint8_t deact_bad[2] = { LC_SIG_CMD_DEACTIVATE, 0x00 };
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_BAD_ARG, lc_sig_term_command(&t, deact_bad, 2, 0));
    static const uint8_t act_bad[] = "\x01opencell:1:nope";
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_BAD_ARG, lc_sig_term_command(&t, act_bad, sizeof(act_bad) - 1, 0));
    c = 0x7F;
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_BAD_ARG, lc_sig_term_command(&t, &c, 1, 0));
    static const uint8_t answer_extra[2] = { LC_SIG_CMD_ANSWER, 1 };
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_BAD_LEN, lc_sig_term_command(&t, answer_extra, 2, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_NOT_ACTIVATED, lc_sig_term_state(&t));
    TEST_ASSERT_EQUAL_INT(0, ul_n);

    register_ok();
    c = LC_SIG_CMD_ANSWER;
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&t, &c, 1, 0));
    c = LC_SIG_CMD_HANGUP;
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&t, &c, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, dial, sizeof(dial) - 1, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&t, dial, sizeof(dial) - 1, 0)); /* one call */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_CALLING, lc_sig_term_state(&t));
}

/* numbering v2 §5.3: DIAL takes any dialled form, completed from the
 * terminal's own number (+883 1 606 555 01234); CALL_SETUP carries the full form. */
static void dial_expect(const char *dialled, const char *full)
{
    uint8_t cmd[1 + 32], want[LC_SIG_NUMBER_LEN];
    size_t n = strlen(dialled);
    cmd[0] = LC_SIG_CMD_DIAL;
    memcpy(cmd + 1, dialled, n);
    register_ok();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, lc_sig_term_command(&t, cmd, 1 + n, 0), dialled);
    lc_sig_term_tick(&t, 0);
    lc_sig_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CALL_SETUP, m.type);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd(full, strlen(full), want));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, m.u.call_setup.called, LC_SIG_NUMBER_LEN, dialled);
}

static void test_dial_normalizes_short_forms(void)
{
    dial_expect("606-555-0100", "+883160655500100");
    dial_expect("1 606 555 1235", "+883160655501235");
    dial_expect("606 555 01235", "+883160655501235");
    dial_expect("+883160655501235", "+883160655501235");
    TEST_ASSERT_EQUAL_size_t(LC_SIG_DIAL_MAX, strlen("+883 (1) 606-555-01235  "));
    dial_expect("+883 (1) 606-555-01235  ", "+883160655501235"); /* the longest DIAL */
}

static void test_dial_refuses_what_is_not_a_number(void)
{
    static const char *bad[] = { "911", "112", "555-1235", "+1 606 555 1234", "606-555-O1235" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t cmd[1 + 32];
        size_t n = strlen(bad[i]);
        cmd[0] = LC_SIG_CMD_DIAL;
        memcpy(cmd + 1, bad[i], n);
        register_ok();
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(LC_SIG_ATT_BAD_ARG, lc_sig_term_command(&t, cmd, 1 + n, 0), bad[i]);
        lc_sig_term_tick(&t, 0);
        TEST_ASSERT_EQUAL_INT(0, ul_n); /* nothing sent */
        TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
    }
    static const char too_long[] = "\x02+883 (1) 606-555-01235   "; /* 25 bytes of argument */
    register_ok();
    TEST_ASSERT_EQUAL_size_t(1 + 25, sizeof(too_long) - 1);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_BAD_LEN,
                            lc_sig_term_command(&t, (const uint8_t *)too_long, sizeof(too_long) - 1, 0));
}

static void test_activation_request_and_ack(void)
{
    uint8_t skn[32], pkn[32], k[16], opc[16];
    memset(skn, 0x11, 32);
    lc_sig_x25519_public(skn, pkn);
    lc_sig_qr_t q;
    memset(&q, 0, sizeof(q));
    q.key_id = 1;
    memcpy(q.pkn, pkn, 32);
    memset(q.token_id, 0xa0, 8);
    memset(q.token_secret, 0xb0, 16);
    lc_sig_number_to_bcd("+883160655501234", 16, q.number);
    uint8_t cmd[1 + LC_SIG_QR_TEXT + 1];
    cmd[0] = LC_SIG_CMD_ACTIVATE;
    size_t n = lc_sig_qr_format(&q, (char *)cmd + 1, sizeof(cmd) - 1);
    boot(0);
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, cmd, 1 + n, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_ACTIVATING, lc_sig_term_state(&t));
    lc_sig_term_tick(&t, 0);
    lc_sig_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ACT_REQ, m.type);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(id.pk, m.u.act_req.pkt, 32);
    uint8_t tag[8];
    lc_sig_act_tag(q.token_secret, TMID, id.pk, q.token_id, tag);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tag, m.u.act_req.tag, 8);

    TEST_ASSERT_EQUAL_INT(0, lc_sig_act_keys(skn, id.pk, TMID, q.token_id, k, opc)); /* the network's view */
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_ACT_ACK;
    memcpy(m.u.act_ack.number, q.number, LC_SIG_NUMBER_LEN);
    lc_sig_act_confirm(k, TMID, q.token_id, m.u.act_ack.confirm);
    from_net(&m, 0);
    TEST_ASSERT_TRUE(id.activated);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k, id.k, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(opc, id.opc, 16);
    TEST_ASSERT_TRUE(saves >= 1);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_ACTIVATED, ev[0][0]);
    TEST_ASSERT_EQUAL_UINT8(9, ev_len[0]); /* v3: ev, number (8) */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(q.number, &ev[0][1], LC_SIG_NUMBER_LEN);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m)); /* registration follows at once */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
}

/* The firmware's split ACTIVATE: keys derived by prepare (no lc_sig_term_t
 * needed), then activate; a low-order network key is refused up front. */
static void test_activation_prepared_outside_the_lock(void)
{
    uint8_t skn[32], k[16], opc[16];
    memset(skn, 0x11, 32);
    lc_sig_qr_t q;
    memset(&q, 0, sizeof(q));
    q.key_id = 1;
    lc_sig_x25519_public(skn, q.pkn);
    memset(q.token_id, 0xa0, 8);
    lc_sig_number_to_bcd("+883160655501234", 16, q.number); /* the QR parser checks it */
    char text[LC_SIG_QR_TEXT + 1];
    size_t n = lc_sig_qr_format(&q, text, sizeof(text));
    boot(0);
    lc_sig_act_prep_t p;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_act_prepare(&id, TMID, (const uint8_t *)text, n, &p));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_act_keys(skn, id.pk, TMID, q.token_id, k, opc)); /* the network's view */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k, p.k, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(opc, p.opc, 16);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_NOT_ACTIVATED, lc_sig_term_state(&t)); /* prepare changes nothing */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_activate(&t, &p, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_ACTIVATING, lc_sig_term_state(&t));
    TEST_ASSERT_EQUAL_INT(LC_SIG_ATT_NOT_NOW, lc_sig_term_activate(&t, &p, 0));

    memset(q.pkn, 0, 32); /* a low-order point */
    n = lc_sig_qr_format(&q, text, sizeof(text));
    TEST_ASSERT_EQUAL_INT(LC_SIG_ATT_BAD_ARG, lc_sig_term_act_prepare(&id, TMID, (const uint8_t *)text, n, &p));
    TEST_ASSERT_EQUAL_INT(LC_SIG_ATT_BAD_LEN, lc_sig_term_act_prepare(&id, TMID, (const uint8_t *)text, 0, &p));
}

static void test_bad_confirm_fails_activation(void)
{
    lc_sig_qr_t q;
    memset(&q, 0, sizeof(q));
    uint8_t skn[32];
    memset(skn, 0x11, 32);
    lc_sig_x25519_public(skn, q.pkn);
    lc_sig_number_to_bcd("+883160655501234", 16, q.number);
    uint8_t cmd[1 + LC_SIG_QR_TEXT + 1];
    cmd[0] = LC_SIG_CMD_ACTIVATE;
    size_t n = lc_sig_qr_format(&q, (char *)cmd + 1, sizeof(cmd) - 1);
    boot(0);
    lc_sig_term_command(&t, cmd, 1 + n, 0);
    lc_sig_term_tick(&t, 0);
    lc_sig_msg_t m;
    to_net(&m);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_ACT_ACK; /* confirm all zero: wrong */
    lc_sig_number_to_bcd("+883160655501234", 16, m.u.act_ack.number);
    from_net(&m, 0);
    TEST_ASSERT_FALSE(id.activated);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_ACT_FAILED, ev[0][0]);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_BAD_CONFIRM, ev[0][1]);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_NOT_ACTIVATED, lc_sig_term_state(&t));
}

static void test_bad_network_mac_reported(void)
{
    boot(1);
    lc_sig_term_tick(&t, 0);
    lc_sig_msg_t m;
    to_net(&m);
    lc_milenage_t o;
    lc_sig_msg_t a = make_auth(1, 1, &o);
    from_net(&a, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AUTH_FAIL, m.type);
    TEST_ASSERT_EQUAL_UINT8(1, m.u.auth_fail.cause);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_REG_FAILED, ev[0][0]);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_REG_NET_AUTH, ev[0][1]);
    TEST_ASSERT_EQUAL_UINT8(0, id.sqn[5]); /* SQN untouched */
}

static void test_sqn_out_of_range_sends_auts(void)
{
    boot(1);
    lc_sig_sqn_put(id.sqn, 100);
    lc_sig_term_tick(&t, 0);
    lc_sig_msg_t m;
    to_net(&m);
    lc_milenage_t o, o2;
    lc_sig_msg_t a = make_auth(50, 0, &o);
    from_net(&a, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AUTH_FAIL, m.type);
    TEST_ASSERT_EQUAL_UINT8(2, m.u.auth_fail.cause);
    static const uint8_t zero[6] = { 0 }, amf0[2] = { 0, 0 };
    lc_milenage(K, OPC, RAND, zero, amf0, &o2); /* AK* doesn't depend on SQN */
    uint8_t ms[6];
    for (int i = 0; i < 6; i++) ms[i] = (uint8_t)(m.u.auth_fail.auts[i] ^ o2.ak_s[i]);
    TEST_ASSERT_EQUAL_UINT64(100, lc_sig_sqn_get(ms));
    lc_milenage(K, OPC, RAND, ms, amf0, &o2);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(o2.mac_s, m.u.auth_fail.auts + 6, 8);
}

static void test_outgoing_call_flow(void)
{
    register_ok();
    static const uint8_t dial[] = "\x02+883160655500100";
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, dial, sizeof(dial) - 1, 0));
    lc_sig_term_tick(&t, 0);
    lc_sig_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CALL_SETUP, m.type);
    uint8_t ref = m.u.call_setup.ref;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CALL_PROC;
    m.u.call_proc.ref = ref;
    m.u.call_proc.call_id = 90;
    from_net(&m, 0);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_ALERTING;
    m.u.call.call_id = 90;
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_RINGING, ev[0][0]);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CONNECT;
    m.u.connect.call_id = 90;
    m.u.connect.codec = 1;
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CONNECT_ACK, m.type);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&t));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_CONNECTED, ev[1][0]);
    uint8_t c = LC_SIG_CMD_HANGUP;
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, &c, 1, 0));
    lc_sig_term_tick(&t, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_RELEASE, m.type);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_NORMAL, m.u.release.cause);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_RELEASE_COMPLETE;
    m.u.call.call_id = 90;
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_ENDED, ev[2][0]);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
}

static void test_incoming_call_answer_voice_and_release(void)
{
    register_ok();
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_SETUP_IND;
    m.u.setup_ind.call_id = 77;
    lc_sig_number_to_bcd("+883160655500100", 16, m.u.setup_ind.caller);
    uint8_t caller[LC_SIG_NUMBER_LEN];
    memcpy(caller, m.u.setup_ind.caller, sizeof(caller));
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_INCOMING, ev[0][0]);
    TEST_ASSERT_EQUAL_UINT8(13, ev_len[0]); /* v3: ev, call id (4), number (8) */
    TEST_ASSERT_EQUAL_UINT32(77, lc_sig_get32(&ev[0][1]));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(caller, &ev[0][5], LC_SIG_NUMBER_LEN);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ALERTING, m.type);
    uint8_t c = LC_SIG_CMD_ANSWER;
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, &c, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&t, &c, 1, 0)); /* already answered */
    lc_sig_term_tick(&t, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CONNECT, m.type);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CONNECT_ACK;
    m.u.call.call_id = 77;
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&t));

    /* A DL voice frame as the network encrypts it (Part 15), frame counter 0. */
    uint8_t kv[16], nonce[14] = { 0 }, frame[LC_SIG_LINK_MAX], out[LC_SIG_APP_MAX], outn;
    lc_sig_voice_key(reg_o.ck, reg_o.ik, RAND, TMID, 77, kv);
    nonce[0] = 1;
    lc_sig_put32(nonce + 1, 77);
    frame[0] = LC_SIG_KIND_DATA;
    frame[1] = 0;
    memcpy(frame + 2, "HELLO", 5);
    lc_sig_aes128_ctr(kv, nonce, frame + 2, 5);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_in(&t, frame, 7, out, &outn));
    TEST_ASSERT_EQUAL_UINT8(5, outn);
    TEST_ASSERT_EQUAL_MEMORY("HELLO", out, 5);
    uint8_t up[LC_SIG_LINK_MAX], upn;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_out(&t, (const uint8_t *)"HI", 2, up, &upn));
    TEST_ASSERT_EQUAL_UINT8(4, upn);
    TEST_ASSERT_FALSE(memcmp(up + 2, "HI", 2) == 0); /* encrypted on air */
    uint8_t big[19] = { 0 };
    TEST_ASSERT_EQUAL_INT(LC_SIG_ATT_BAD_LEN, lc_sig_term_data_out(&t, big, 19, up, &upn));

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_SETUP_IND; /* a second call while busy */
    m.u.setup_ind.call_id = 78;
    lc_sig_number_to_bcd("+883160655501235", 16, m.u.setup_ind.caller);
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_RELEASE, m.type);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_BUSY, m.u.release.cause);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&t));
    lc_sig_chan_reset(&net); /* the network gives up on call 78 */

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_RELEASE;
    m.u.release.call_id = 77;
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_RELEASE_COMPLETE, m.type);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
}

static void test_deactivate_wipes(void)
{
    register_ok();
    static const uint8_t d[2] = { LC_SIG_CMD_DEACTIVATE, 0xA5 };
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, d, 2, 0));
    static const uint8_t zero[16] = { 0 };
    TEST_ASSERT_FALSE(id.activated);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, id.k, 16);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_DEACTIVATED, ev[0][0]);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_NOT_ACTIVATED, lc_sig_term_state(&t));
}

/* A registered terminal that loses the cell and attaches again registers
 * again; a channel release (still attached) doesn't. */
static void test_reattach_registers_again(void)
{
    boot(1);
    register_ok();
    lc_sig_term_link(&t, 1, 0, 1000); /* channel released: still attached */
    lc_sig_term_tick(&t, 1000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
    lc_sig_term_link(&t, 0, 0, 2000); /* cell lost */
    lc_sig_term_tick(&t, 2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
    lc_sig_term_link(&t, 1, 1, 3000); /* attached and granted again */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t));
    lc_sig_term_tick(&t, 3000);
    lc_sig_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
}

/* The cell's beacon says Part 97 but REG_ACK said Part 15: register again. */
static void test_cell_mode_change_registers_again(void)
{
    boot(1);
    register_ok(); /* REG_ACK mode Part 15 */
    lc_sig_term_cell_mode(&t, LC_SIG_MODE_PART15, 1000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
    lc_sig_term_cell_mode(&t, LC_SIG_MODE_PART97, 2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t));
    lc_sig_term_tick(&t, 2000);
    lc_sig_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
}

static void test_asks_for_channel_when_not_granted(void)
{
    boot(1);
    lc_sig_term_link(&t, 1, 0, 0); /* attached, no grant */
    lc_sig_term_tick(&t, 0);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    lc_sig_term_tick(&t, 500000);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs); /* not more than every 2 s */
    lc_sig_term_tick(&t, 2100000);
    TEST_ASSERT_EQUAL_INT(2, svc_reqs);
    TEST_ASSERT_EQUAL_INT(0, ul_n);
}

/* Fix round 1, defect 1(a): once a request is handed to the channel (it has
 * left out_count and is waiting for its answer) and the grant goes away
 * before an answer arrives, flush() must keep asking for a channel -
 * out_count alone missed this case. */
static void test_asks_for_channel_while_awaiting_reply(void)
{
    boot(1);
    lc_sig_term_tick(&t, 0); /* REG_REQ queued and sent while still granted */
    lc_sig_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, to_net(&m)); /* drain the uplink; the network never answers */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
    lc_sig_term_link(&t, 1, 0, 0); /* grant lost, still attached */
    svc_reqs = 0;
    lc_sig_term_tick(&t, 0);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    lc_sig_term_tick(&t, 500000);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs); /* not more than every 2 s */
    lc_sig_term_tick(&t, 2100000);
    TEST_ASSERT_EQUAL_INT(2, svc_reqs);
}

/* Fix round 1, defect 1(b): the channel's own retransmit clock freezes while
 * ungranted (lc_sig_chan_tick just pushes pend_due forward), so without a
 * supervision deadline REGISTERING would hang forever asking for a channel
 * that never gets answered. 30 s with no answer gives up and backs off. */
static void test_registration_supervision_timeout(void)
{
    boot(1);
    lc_sig_term_tick(&t, 0); /* REG_REQ sent while granted */
    lc_sig_msg_t m;
    to_net(&m); /* the network never answers */
    lc_sig_term_link(&t, 1, 0, 0); /* grant lost right away and never comes back */
    lc_sig_term_tick(&t, 30000000ull);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_REG_FAILED, ev[0][0]);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_REG_TIMEOUT, ev[0][1]);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t)); /* retries after the backoff */
}

/* Same defect, for ACTIVATING: 30 s with no ACT_ACK/ACT_NAK gives up, and a
 * fresh ACTIVATE is then accepted. */
static void test_activation_supervision_timeout(void)
{
    uint8_t skn[32], pkn[32];
    memset(skn, 0x11, 32);
    lc_sig_x25519_public(skn, pkn);
    lc_sig_qr_t q;
    memset(&q, 0, sizeof(q));
    q.key_id = 1;
    memcpy(q.pkn, pkn, 32);
    memset(q.token_id, 0xa0, 8);
    memset(q.token_secret, 0xb0, 16);
    lc_sig_number_to_bcd("+883160655501234", 16, q.number);
    uint8_t cmd[1 + LC_SIG_QR_TEXT + 1];
    cmd[0] = LC_SIG_CMD_ACTIVATE;
    size_t n = lc_sig_qr_format(&q, (char *)cmd + 1, sizeof(cmd) - 1);
    boot(0);
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, cmd, 1 + n, 0));
    lc_sig_term_tick(&t, 0); /* ACT_REQ sent while granted */
    lc_sig_msg_t m;
    to_net(&m); /* the network never answers */
    lc_sig_term_link(&t, 1, 0, 0); /* grant lost right away and never comes back */
    lc_sig_term_tick(&t, 30000000ull);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_ACT_FAILED, ev[0][0]);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_TIMEOUT, ev[0][1]);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_NOT_ACTIVATED, lc_sig_term_state(&t));
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, cmd, 1 + n, 0)); /* a new ACTIVATE is accepted */
}

/* Fix round 1, defect 2: REG_REJ travels unauthenticated (prot 0), so a
 * forged one must not be able to clear the terminal's activation on its own
 * word; every cause is just reported and backed off from. */
static void test_reg_rej_not_activated_does_not_deactivate(void)
{
    boot(1);
    lc_sig_term_tick(&t, 0); /* REG_REQ sent */
    lc_sig_msg_t m;
    to_net(&m);
    lc_sig_msg_t rej;
    memset(&rej, 0, sizeof(rej));
    rej.type = LC_SIG_REG_REJ;
    rej.u.reg_rej.cause = LC_SIG_REG_NOT_ACTIVATED;
    from_net(&rej, 0);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_REG_FAILED, ev[0][0]);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_REG_NOT_ACTIVATED, ev[0][1]);
    TEST_ASSERT_TRUE(id.activated);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(K, id.k, 16);
    TEST_ASSERT_EQUAL_INT(0, saves);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t));
}

/* Fix round 1, defect 3: a duplicate DL voice frame must not desynchronise
 * the frame counter for the rest of the call. */
static void test_duplicate_voice_frame_rejected(void)
{
    register_ok();
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_SETUP_IND;
    m.u.setup_ind.call_id = 77;
    lc_sig_number_to_bcd("+883160655500100", 16, m.u.setup_ind.caller);
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m)); /* ALERTING */
    uint8_t c = LC_SIG_CMD_ANSWER;
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, &c, 1, 0));
    lc_sig_term_tick(&t, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m)); /* CONNECT */
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CONNECT_ACK;
    m.u.call.call_id = 77;
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&t));

    uint8_t kv[16], nonce[14] = { 0 }, frame[LC_SIG_LINK_MAX], dup[LC_SIG_LINK_MAX], out[LC_SIG_APP_MAX], outn;
    lc_sig_voice_key(reg_o.ck, reg_o.ik, RAND, TMID, 77, kv);
    nonce[0] = 1;
    lc_sig_put32(nonce + 1, 77);

    lc_sig_put32(nonce + 5, 0); /* frame counter 0 */
    frame[0] = LC_SIG_KIND_DATA;
    frame[1] = 0;
    memcpy(frame + 2, "HELLO", 5);
    lc_sig_aes128_ctr(kv, nonce, frame + 2, 5);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_in(&t, frame, 7, out, &outn));
    TEST_ASSERT_EQUAL_MEMORY("HELLO", out, 5);

    memcpy(dup, frame, 7); /* the same frame, resent: must be rejected */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_term_data_in(&t, dup, 7, out, &outn));

    lc_sig_put32(nonce + 5, 1); /* frame counter 1: must still decrypt correctly */
    frame[1] = 1;
    memcpy(frame + 2, "WORLD", 5);
    lc_sig_aes128_ctr(kv, nonce, frame + 2, 5);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_in(&t, frame, 7, out, &outn));
    TEST_ASSERT_EQUAL_MEMORY("WORLD", out, 5);
}

/* Final review I1: a registration that times out (no answer: coverage lost)
 * is not a refusal, so it retries every 30 s instead of backing off towards
 * 10 min. */
static void test_reg_timeout_does_not_escalate_backoff(void)
{
    boot(1);
    lc_sig_term_link(&t, 1, 0, 0); /* attached, never granted */
    uint64_t now = 0;
    for (int i = 0; i < 4; i++) {
        lc_sig_term_tick(&t, now); /* REG_REQ queued (waits for a channel) */
        TEST_ASSERT_TRUE(t.reg_sent);
        now += 30000000ull;
        lc_sig_term_tick(&t, now); /* supervision: REG_FAILED(timeout) */
        TEST_ASSERT_FALSE(t.reg_sent);
        TEST_ASSERT_EQUAL_HEX8(LC_SIG_REG_TIMEOUT, ev[ev_n - 1][1]);
        TEST_ASSERT_EQUAL_UINT64(now + 30000000ull, t.reg_retry_at);
        now = t.reg_retry_at;
    }
}

/* ...while a real refusal still backs off further each time. */
static void test_reg_rejections_still_escalate_backoff(void)
{
    boot(1);
    lc_sig_msg_t m, rej;
    memset(&rej, 0, sizeof(rej));
    rej.type = LC_SIG_REG_REJ;
    rej.u.reg_rej.cause = LC_SIG_REG_AUTH_FAILED;
    lc_sig_term_tick(&t, 0);
    to_net(&m);
    from_net(&rej, 0);
    TEST_ASSERT_EQUAL_UINT64(30000000ull, t.reg_retry_at);
    lc_sig_term_tick(&t, 30000000ull);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
    from_net(&rej, 30000000ull);
    TEST_ASSERT_EQUAL_UINT64(90000000ull, t.reg_retry_at); /* 60 s this time */
}

/* Final review I1: back in coverage while waiting to retry a registration:
 * retry at once instead of sitting out the backoff (refusing DIAL). */
static void test_reattach_while_registering_retries_at_once(void)
{
    boot(1);
    lc_sig_term_link(&t, 1, 0, 0);
    lc_sig_term_tick(&t, 0);
    lc_sig_term_tick(&t, 30000000ull); /* timed out: next try at 60 s */
    TEST_ASSERT_FALSE(t.reg_sent);
    lc_sig_term_link(&t, 0, 0, 31000000ull); /* coverage lost */
    lc_sig_term_tick(&t, 31000000ull);
    lc_sig_term_link(&t, 1, 1, 32000000ull); /* and back, with a grant */
    ul_n = 0;
    lc_sig_term_tick(&t, 32000000ull);
    lc_sig_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
}

/* Final review P1 (spec §3.2 "DEACTIVATE wipes the keys"): the RAM copies
 * go too - the pending activation's K/OPc and QR (token secret), the
 * registration's CK/IK/RAND, the last call's voice key, the session keys. */
static void test_deactivate_wipes_ram_key_copies(void)
{
    register_ok(); /* ck/ik/rand and the channel's session keys are live */
    memset(t.act_k, 0x5a, 16);  /* as an activation leaves them */
    memset(t.act_opc, 0x5a, 16);
    memset(t.qr.token_secret, 0x5a, 16);
    memset(t.qr.token_id, 0x5a, 8);
    memset(t.k_voice, 0x5a, 16); /* as a call leaves it */
    static const uint8_t d[2] = { LC_SIG_CMD_DEACTIVATE, 0xA5 };
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, d, 2, 0));
    static const uint8_t zero[16] = { 0 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.act_k, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.act_opc, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.qr.token_secret, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.qr.token_id, 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.ck, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.ik, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.rand, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.k_voice, 16);
    TEST_ASSERT_FALSE(t.ch.sec.keyed);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.ch.sec.k_int, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, t.ch.sec.k_enc, 16);
    TEST_ASSERT_EQUAL_UINT8(0, t.reg_mode);
}

/* Final review P2: the network's RELEASE(busy) to a CALL_SETUP carries call
 * id 0 (no call id was given): the calling terminal takes it as its own. */
static lc_sig_msg_t chan_list_msg(uint8_t ver)
{
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CHAN_LIST;
    m.u.chan_list.ver = ver;
    m.u.chan_list.count = 2;
    m.u.chan_list.freq_hz[0] = 917250000u;
    m.u.chan_list.freq_hz[1] = 907250000u;
    m.u.chan_list.flags[1] = LC_SIG_CHAN_FIXED;
    return m;
}

/* Channel-list spec §7: CHAN_LIST is acknowledged with its version and handed
 * over once; a retransmission gets the same ACK and is not handed over again. */
static void test_chan_list_acked_and_handed_over_once(void)
{
    register_ok();
    lc_sig_msg_t m = chan_list_msg(5), r;
    lc_sig_chan_list_t got;
    from_net(&m, 1000);
    TEST_ASSERT_EQUAL_UINT8(5, t.list_ver);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&t, &got));
    TEST_ASSERT_EQUAL_UINT8(2, got.count);
    TEST_ASSERT_EQUAL_UINT32(907250000u, got.freq_hz[1]);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_CHAN_FIXED, got.flags[1]);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_chan_list(&t, &got));

    ul_n = 0; /* the ACK is lost on the air: the network sends CHAN_LIST again */
    lc_sig_chan_tick(&net, 1000 + LC_SIG_RETX_US, 1, NULL);
    const uint8_t *p;
    uint8_t n;
    while (lc_sig_chan_peek(&net, &p, &n) == 0) {
        uint8_t c[LC_SIG_LINK_MAX];
        memcpy(c, p, n);
        lc_sig_chan_pop(&net);
        lc_sig_term_rx(&t, c, n, 1000 + LC_SIG_RETX_US);
    }
    TEST_ASSERT_EQUAL_INT(1, to_net(&r)); /* the same ACK */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CHAN_LIST_ACK, r.type);
    TEST_ASSERT_EQUAL_UINT8(5, r.u.chan_list_ack.ver);
    TEST_ASSERT_FALSE(net.pend);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_chan_list(&t, &got)); /* not handed over twice */
}

/* Controller ruling B: a CHAN_LIST arriving before REG_ACK (still
 * REGISTERING) is ignored outright - no ACK, no hand-over - so a list can
 * never be applied before REG_ACK has set reg_mode. Registration still
 * completes normally afterwards. */
static void test_chan_list_ignored_while_registering(void)
{
    lc_sig_msg_t m;
    boot(1);
    lc_sig_term_tick(&t, 0);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
    lc_sig_msg_t a = make_auth(1, 0, &reg_o);
    from_net(&a, 0); /* the terminal computes and sends AUTH_RSP; its chan is now keyed */
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AUTH_RSP, m.type);
    uint8_t ki[16], ke[16];
    lc_sig_session_keys(reg_o.ck, reg_o.ik, RAND, TMID, ki, ke);
    lc_sig_sec_key(&net.sec, ki, ke, 1);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t)); /* REG_ACK hasn't arrived yet */

    lc_sig_msg_t cl = chan_list_msg(9);
    from_net(&cl, 0);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t));
    TEST_ASSERT_EQUAL_INT(0, to_net(&m)); /* no CHAN_LIST_ACK: it was ignored */
    lc_sig_chan_list_t got;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_chan_list(&t, &got)); /* not handed over */
    TEST_ASSERT_EQUAL_UINT8(0, t.list_ver);                    /* untouched */

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_REG_ACK;
    m.u.reg_ack.mode = LC_SIG_MODE_PART15;
    m.u.reg_ack.period_s = 1800;
    memcpy(m.u.reg_ack.number, id.number, LC_SIG_NUMBER_LEN);
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t)); /* registers normally afterwards */
}

/* A beacon cfg_ver other than the list's version asks for the list with
 * cause 4, only registered, attached and ungranted, at most every 30 s. */
static void test_cfg_ver_change_asks_for_the_list(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_cell_cfg(&t, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, svc_reqs); /* granted: the push after REG_ACK covers it */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 0, 1000); /* same version */
    TEST_ASSERT_EQUAL_INT(0, svc_reqs);
    lc_sig_term_cell_cfg(&t, 1, 1000);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_SVC_CONFIG, svc_cause);
    lc_sig_term_cell_cfg(&t, 1, 29000000u);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs); /* before the 30 s retry: no repeat */
    /* Review fix: past the retry timer too, so only the list-ver gate itself
     * (not the timer, which would vacuously pass either way) is what stops
     * it here. */
    t.list_ver = 5; /* mod 4 == 1: up to date */
    lc_sig_term_cell_cfg(&t, 1, 31000000u);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));

    boot(1); /* not registered yet */
    lc_sig_term_link(&t, 1, 0, 0);
    lc_sig_term_cell_cfg(&t, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, svc_reqs);
}

/* Task 9 fix: an ask still unanswered when its own 30 s retry comes due
 * re-registers instead of repeating itself - the bench case (the cell was
 * restarted within ~1 s, so the terminal never lost sync and still believes
 * it is REGISTERED, but the new network has no session for it; only a
 * REGISTERED session holds the keys CHAN_LIST needs, so a cause-4 ask into a
 * network with no session for us gets no answer at all, and without this fix
 * cell_cfg would repeat the same ask every 30 s forever). A registration's
 * REG_ACK is followed by the network's CHAN_LIST push anyway, so the list
 * still arrives - just through that path instead. */
static void test_cfg_ask_unanswered_reregisters_at_the_retry_time(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 1, 1000); /* mismatch: list_ver 0 vs cfg_ver 1 */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_SVC_CONFIG, svc_cause);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));

    /* no CHAN_LIST arrives before the 30 s retry */
    lc_sig_term_cell_cfg(&t, 1, 31000000u);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs); /* not a second SERVICE_REQ 4 */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t)); /* re-registers instead */
}

/* Review fix, point 1: only ONE re-registration per cfg_ver. If the cell that
 * answers the fresh registration still can't satisfy this exact cfg_ver (its
 * own list genuinely doesn't cover it - not a lost session), further
 * unanswered cycles fall back to plain SERVICE_REQ 4 asks with increasing
 * backoff (30 s, 60, 120 ...) instead of re-registering forever. */
static void test_cfg_ask_unanswered_after_reregistration_falls_back_to_backoff(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 1, 1000); /* mismatch: list_ver 0 vs cfg_ver 1 */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);

    lc_sig_term_cell_cfg(&t, 1, 31000000u); /* unanswered at the retry: re-registers once */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t));

    /* the new registration completes, but the network's own list still
     * doesn't cover cfg_ver 1 (list_ver stays 0) - simulated directly here,
     * as elsewhere in this file, rather than re-running the full AKA. */
    t.state = LC_SIG_ST_REGISTERED;

    lc_sig_term_cell_cfg(&t, 1, 61000000u); /* the cfg_retry_at the re-register path set */
    TEST_ASSERT_EQUAL_INT(2, svc_reqs); /* a plain ask, not a second re-registration */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_SVC_CONFIG, svc_cause);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));

    lc_sig_term_cell_cfg(&t, 1, 90000000u); /* under 30 s later: the backoff isn't due yet */
    TEST_ASSERT_EQUAL_INT(2, svc_reqs);
    lc_sig_term_cell_cfg(&t, 1, 91000000u); /* 30 s after that ask: the backoff's first step */
    TEST_ASSERT_EQUAL_INT(3, svc_reqs);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t)); /* never re-registers again */
}

/* Re-review, point 1: the one-re-registration record must not survive past
 * the mismatch it was tracking. Without clearing it once the list catches up
 * (here: the re-register's own post-REG_ACK push takes list v1, matching
 * cfg_ver 1), a LATER, unrelated session loss that happens to show the same
 * 2-bit cfg_ver value again would be mistaken for the same already-tried
 * question - falling straight into a backoff-ask loop that a genuinely lost
 * session can never answer, recovering only at the next periodic
 * re-registration, up to 30 min later - instead of getting its own fresh
 * re-registration at 30 s. */
static void test_cfg_rereg_record_forgotten_once_the_list_catches_up(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 1, 1000); /* mismatch: list_ver 0 vs cfg_ver 1 */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);

    lc_sig_term_cell_cfg(&t, 1, 31000000u); /* unanswered at the retry: re-registers once */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t));
    TEST_ASSERT_TRUE(t.cfg_reregistered);

    /* the new registration completes, and the network's automatic
     * post-REG_ACK push takes list v1, resolving the mismatch. */
    t.state = LC_SIG_ST_REGISTERED;
    lc_sig_term_link(&t, 1, 1, 31000000u); /* granted for the push */
    lc_sig_msg_t m = chan_list_msg(1), r;
    lc_sig_chan_list_t got;
    from_net(&m, 31000000u);
    TEST_ASSERT_EQUAL_INT(1, to_net(&r)); /* the ACK */
    TEST_ASSERT_EQUAL_UINT8(1, t.list_ver);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&t, &got));
    TEST_ASSERT_FALSE(t.cfg_reregistered); /* forgotten now that the list caught up */

    t.list_ver = 4; /* the list moves on with ordinary traffic (v2, v3, v4) */

    /* an unrelated session loss the terminal never notices (attached and
     * REGISTERED throughout, as in the original bench bug), and the beacon's
     * 2-bit cfg_ver comes back around to 1 - the SAME value as before,
     * against a list_ver (4) that doesn't match it. */
    lc_sig_term_link(&t, 1, 0, 62000000u);
    lc_sig_term_cell_cfg(&t, 1, 62000000u); /* mismatch: list_ver 4 vs cfg_ver 1 */
    TEST_ASSERT_EQUAL_INT(2, svc_reqs);
    lc_sig_term_cell_cfg(&t, 1, 92000000u); /* unanswered at its own 30 s retry */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t)); /* re-registers again... */
    TEST_ASSERT_EQUAL_INT(2, svc_reqs);                                   /* ...not a third plain ask */
}

/* Re-review, point 2: the fallback backoff must only advance on an ask that
 * actually went out. io.service_req can refuse (e.g. RACH busy) while
 * cell_cfg itself is re-run every ~100 ms (lc_term_sig_step); doubling the
 * backoff on every refusal would race it to 600 s before a single ask ever
 * left. */
static void test_backoff_only_advances_on_asks_that_go_out(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 1, 1000); /* mismatch: list_ver 0 vs cfg_ver 1 */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);

    lc_sig_term_cell_cfg(&t, 1, 31000000u); /* unanswered at the retry: re-registers once */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t));
    t.state = LC_SIG_ST_REGISTERED; /* the new registration completes; still mismatched */

    lc_sig_term_cell_cfg(&t, 1, 61000000u); /* the fallback's first ask */
    TEST_ASSERT_EQUAL_INT(2, svc_reqs);
    TEST_ASSERT_EQUAL_UINT32(30u, t.cfg_backoff_s);

    /* its next ask is refused five times in a row (RACH busy) before it
     * finally goes out - cell_cfg is retried every attempt, as
     * lc_term_sig_step would every ~100 ms while ungranted. */
    svc_refuse_n = 5;
    for (int i = 0; i < 5; i++) {
        lc_sig_term_cell_cfg(&t, 1, 91000000u + (uint64_t)i * 100000u);
        TEST_ASSERT_EQUAL_INT(2, svc_reqs); /* still refused: no successful ask yet */
    }
    TEST_ASSERT_EQUAL_UINT32(30u, t.cfg_backoff_s); /* not advanced by the refusals */
    uint64_t now = 91000000u + 5u * 100000u;
    lc_sig_term_cell_cfg(&t, 1, now); /* the 6th attempt gets through */
    TEST_ASSERT_EQUAL_INT(3, svc_reqs);
    TEST_ASSERT_EQUAL_UINT32(60u, t.cfg_backoff_s); /* the backoff's next step, not 600 s */
    TEST_ASSERT_EQUAL_UINT64(now + 60000000u, t.cfg_retry_at);
}

/* Same, but a CHAN_LIST answers in time: cfg_answered stops both a repeat ask
 * (M1, already covered) and the new re-registration - an answered ask must
 * never trigger it. */
static void test_cfg_ask_answered_in_time_does_not_reregister(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 1, 1000); /* mismatch: list_ver 0 vs cfg_ver 1 */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    lc_sig_term_link(&t, 1, 1, 1000); /* the network grants a channel for the request */

    lc_sig_msg_t m = chan_list_msg(1), r; /* 1 & 3 == 1: matches cfg_ver 1 */
    lc_sig_chan_list_t got;
    from_net(&m, 2000);
    TEST_ASSERT_EQUAL_INT(1, to_net(&r)); /* the ACK */
    TEST_ASSERT_EQUAL_UINT8(1, t.list_ver);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&t, &got));
    TEST_ASSERT_TRUE(t.cfg_answered);

    lc_sig_term_link(&t, 1, 0, 31000000u); /* idle again, past the 30 s retry */
    lc_sig_term_cell_cfg(&t, 1, 31000000u);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);                                   /* no repeat ask */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t)); /* and no re-registration */
}

/* Review fix, point 2: M1 is scoped to the list version held at the time of
 * the answer. An unrelated CHAN_LIST push (no ask outstanding at the time)
 * later moves list_ver on without disturbing that record; when cfg_ver comes
 * back to the value that was once answered, the answer no longer applies to
 * the CURRENT list_ver, so cell_cfg must ask again. */
static void test_m1_does_not_survive_an_unrelated_list_version_change(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 1, 1000); /* mismatch: list_ver 0 vs cfg_ver 1 */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    lc_sig_term_link(&t, 1, 1, 1000); /* the network grants a channel for the request */

    lc_sig_msg_t m1 = chan_list_msg(1), r; /* 1 & 3 == 1: matches cfg_ver 1 */
    lc_sig_chan_list_t got;
    from_net(&m1, 2000);
    TEST_ASSERT_EQUAL_INT(1, to_net(&r)); /* the ACK */
    TEST_ASSERT_EQUAL_UINT8(1, t.list_ver);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&t, &got));
    TEST_ASSERT_TRUE(t.cfg_answered);

    /* an unsolicited push (no ask outstanding: cfg_asked is already answered)
     * moves the list on, to a version that still mismatches cfg_ver 1. */
    lc_sig_msg_t m2 = chan_list_msg(6); /* 6 & 3 == 2: mismatches cfg_ver 1 */
    from_net(&m2, 3000);
    TEST_ASSERT_EQUAL_UINT8(6, t.list_ver);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&t, &got));

    /* the beacon returns to cfg_ver 1, the same value M1 once recorded, but
     * against a list_ver that has since moved: it must ask again - once past
     * the first ask's own 30 s retry timer (independent of this fix). */
    lc_sig_term_link(&t, 1, 0, 31000000u);
    lc_sig_term_cell_cfg(&t, 1, 31000000u);
    TEST_ASSERT_EQUAL_INT(2, svc_reqs);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_SVC_CONFIG, svc_cause);
}

/* Granted, or mid-call: cell_cfg's existing early return keeps this path from
 * ever re-registering out from under an active call, even with an unanswered
 * ask outstanding and its retry time already past. */
static void test_cfg_ask_unanswered_does_not_reregister_when_granted_or_in_call(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 1, 1000); /* mismatch: asks while ungranted */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    lc_sig_term_link(&t, 1, 1, 1000); /* granted again before any answer arrives */

    lc_sig_term_cell_cfg(&t, 1, 31000000u); /* past the retry time, but granted now */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t)); /* no re-registration */

    /* mid-call: not REGISTERED at all, so the state check alone already
     * blocks it - confirm that still holds with an unanswered ask pending. */
    t.state = LC_SIG_ST_IN_CALL;
    lc_sig_term_cell_cfg(&t, 1, 62000000u);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&t));
}

/* Fix round 1, M1: once a CHAN_LIST has answered a given cfg_ver, cell_cfg
 * does not ask again for that exact cfg_ver - only a different one reopens
 * the question. Without this, a persistent mismatch (the beacon's cfg_ver
 * never actually clearing against the list version the network hands back)
 * would ask again forever, every 30 s. */
static void test_persistent_cfg_mismatch_stops_asking_after_a_list_answers_it(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000);
    lc_sig_term_cell_cfg(&t, 1, 1000); /* mismatch: list_ver 0 vs cfg_ver 1 */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    lc_sig_term_link(&t, 1, 1, 1000); /* the network grants a channel for the request */

    lc_sig_msg_t m = chan_list_msg(6), r; /* 6 & 3 == 2: still mismatches cfg_ver 1 */
    lc_sig_chan_list_t got;
    from_net(&m, 1000);
    TEST_ASSERT_EQUAL_INT(1, to_net(&r)); /* the ACK */
    TEST_ASSERT_EQUAL_UINT8(6, t.list_ver);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&t, &got));

    lc_sig_term_link(&t, 1, 0, 31000000u); /* idle again */
    lc_sig_term_cell_cfg(&t, 1, 31000000u); /* past the 30 s retry timer, same cfg_ver: already answered */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs);
    lc_sig_term_cell_cfg(&t, 3, 31000000u); /* a different cfg_ver: still mismatches list_ver 6, so it asks */
    TEST_ASSERT_EQUAL_INT(2, svc_reqs);
}

/* Controller ruling C4: the cause-4 request needs the link both ungranted
 * AND attached (RACH UPPER is IDLE-only, and IDLE requires attachment); a
 * mismatch alone must not be enough. Kept attached throughout except for the
 * one call under test: toggling attached back on while REGISTERED starts a
 * fresh registration (spec §4.3, unrelated to this gate) and would mask it. */
static void test_cell_cfg_needs_an_attached_link(void)
{
    register_ok(); /* list_ver 0, granted */
    lc_sig_term_link(&t, 1, 0, 1000); /* ungranted, still attached */
    lc_sig_term_cell_cfg(&t, 1, 1000);
    TEST_ASSERT_EQUAL_INT(1, svc_reqs); /* attached: the gate lets it ask */
    lc_sig_term_link(&t, 0, 0, 31000000u); /* now detached too */
    lc_sig_term_cell_cfg(&t, 1, 31000000u); /* same mismatch, past the retry timer, but detached */
    TEST_ASSERT_EQUAL_INT(1, svc_reqs); /* no new ask: detached blocks it */
}

static void test_deactivate_forgets_the_list_version(void)
{
    register_ok();
    lc_sig_msg_t m = chan_list_msg(6), r;
    from_net(&m, 1000);
    to_net(&r);
    TEST_ASSERT_EQUAL_UINT8(6, t.list_ver);
    static const uint8_t deact[2] = { LC_SIG_CMD_DEACTIVATE, 0xA5 };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_command(&t, deact, 2, 2000));
    TEST_ASSERT_EQUAL_UINT8(0, t.list_ver);
    lc_sig_chan_list_t got;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_chan_list(&t, &got));
    static const uint8_t scan[1] = { LC_SIG_CMD_SCAN }; /* lc_term_gatt's, not lc_sig's */
    TEST_ASSERT_EQUAL_INT(LC_SIG_ATT_BAD_ARG, lc_sig_term_command(&t, scan, 1, 2000));
}

static void test_release_call_id_0_while_calling_ends_busy(void)
{
    register_ok();
    static const uint8_t dial[] = "\x02+883160655500100";
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t, dial, sizeof(dial) - 1, 0));
    lc_sig_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CALL_SETUP, m.type);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_RELEASE;
    m.u.release.call_id = 0;
    m.u.release.cause = LC_SIG_CAUSE_BUSY;
    from_net(&m, 0);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_ENDED, ev[0][0]);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_CAUSE_BUSY, ev[0][5]);
    TEST_ASSERT_FALSE(lc_sig_chan_busy(&t.ch)); /* RELEASE answered the CALL_SETUP */
}

/* Fix round 2 (Task 8), finding 1: a new registration forgets the previous
 * network's numbering. The network restarts twice: the first new session's
 * AUTH_REQ (seq 0) must not be taken for a repeat of the old network's
 * AUTH_REQ (seq 0, cached with its AUTH_RSP), and after the second restart
 * (before REG_ACK: the AUTH_RSP times out) the next session's AUTH_REQ,
 * seq 0 again and of the same type, must not be dropped as a repeat of the
 * one just received. */
static void test_registration_forgets_the_old_networks_numbering(void)
{
    lc_sig_msg_t m;
    lc_milenage_t o;
    register_ok(); /* AUTH_REQ seq 0, REG_ACK seq 1 */
    lc_sig_chan_init(&net, 1); /* restart 1: a new session, numbered from 0 */
    lc_sig_term_link(&t, 0, 0, 1000);
    lc_sig_term_link(&t, 1, 1, 2000);
    lc_sig_term_tick(&t, 2000);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
    lc_sig_msg_t a = make_auth(2, 0, &o);
    from_net(&a, 2000);
    TEST_ASSERT_EQUAL_UINT64(2, lc_sig_sqn_get(id.sqn)); /* answered, not the old AUTH_RSP resent */
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AUTH_RSP, m.type);

    lc_sig_chan_init(&net, 1); /* restart 2, before REG_ACK */
    ev_n = 0;
    uint64_t now = 2000;
    for (int i = 0; i < 6; i++) {
        now += LC_SIG_RETX_US;
        lc_sig_term_tick(&t, now);
    }
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_EV_REG_FAILED, ev[0][0]); /* the AUTH_RSP went unanswered */
    ul_n = 0;
    now += 30000000u;
    lc_sig_term_tick(&t, now);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
    a = make_auth(3, 0, &o);
    from_net(&a, now);
    TEST_ASSERT_EQUAL_UINT64(3, lc_sig_sqn_get(id.sqn));
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AUTH_RSP, m.type);
}

/* Fix round 2 follow-up: REG_ACK completes a registration only once this
 * attempt's AUTH_RSP went out. A REG_ACK sealed with the previous session's
 * keys (e.g. the network's cached reply, drawn out by a recorded AUTH_RSP
 * played back) arriving after REG_REQ but before AUTH_REQ is ignored, and
 * the terminal still runs a fresh AKA. */
static void test_reg_ack_before_this_attempts_auth_rsp_ignored(void)
{
    lc_sig_msg_t m;
    register_ok(); /* net keyed with this session's keys */
    lc_sig_term_cell_mode(&t, LC_SIG_MODE_PART97, 1000); /* re-register */
    lc_sig_term_tick(&t, 1000);
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, m.type);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_REG_ACK; /* the old keys still open it */
    m.u.reg_ack.mode = LC_SIG_MODE_PART15;
    m.u.reg_ack.period_s = 1800;
    memcpy(m.u.reg_ack.number, id.number, LC_SIG_NUMBER_LEN);
    from_net(&m, 1000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&t));
    TEST_ASSERT_EQUAL_INT(0, ev_n);

    lc_milenage_t o;
    lc_sig_msg_t a = make_auth(2, 0, &o);
    from_net(&a, 1000);
    TEST_ASSERT_EQUAL_UINT64(2, lc_sig_sqn_get(id.sqn)); /* a fresh AKA */
    TEST_ASSERT_EQUAL_INT(1, to_net(&m));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AUTH_RSP, m.type);
    uint8_t ki[16], ke[16];
    lc_sig_session_keys(o.ck, o.ik, RAND, TMID, ki, ke);
    lc_sig_sec_key(&net.sec, ki, ke, 1);
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_REG_ACK;
    m.u.reg_ack.mode = LC_SIG_MODE_PART97;
    m.u.reg_ack.period_s = 1800;
    memcpy(m.u.reg_ack.number, id.number, LC_SIG_NUMBER_LEN);
    from_net(&m, 1000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&t));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ident_pack_roundtrip_and_new);
    RUN_TEST(test_ident_v1_blob_is_old);
    RUN_TEST(test_activation_prepared_outside_the_lock);
    RUN_TEST(test_commands_in_wrong_state_refused);
    RUN_TEST(test_dial_normalizes_short_forms);
    RUN_TEST(test_dial_refuses_what_is_not_a_number);
    RUN_TEST(test_activation_request_and_ack);
    RUN_TEST(test_bad_confirm_fails_activation);
    RUN_TEST(test_bad_network_mac_reported);
    RUN_TEST(test_sqn_out_of_range_sends_auts);
    RUN_TEST(test_outgoing_call_flow);
    RUN_TEST(test_incoming_call_answer_voice_and_release);
    RUN_TEST(test_deactivate_wipes);
    RUN_TEST(test_deactivate_wipes_ram_key_copies);
    RUN_TEST(test_release_call_id_0_while_calling_ends_busy);
    RUN_TEST(test_chan_list_acked_and_handed_over_once);
    RUN_TEST(test_chan_list_ignored_while_registering);
    RUN_TEST(test_cfg_ver_change_asks_for_the_list);
    RUN_TEST(test_cfg_ask_unanswered_reregisters_at_the_retry_time);
    RUN_TEST(test_cfg_ask_unanswered_after_reregistration_falls_back_to_backoff);
    RUN_TEST(test_cfg_rereg_record_forgotten_once_the_list_catches_up);
    RUN_TEST(test_backoff_only_advances_on_asks_that_go_out);
    RUN_TEST(test_cfg_ask_answered_in_time_does_not_reregister);
    RUN_TEST(test_m1_does_not_survive_an_unrelated_list_version_change);
    RUN_TEST(test_cfg_ask_unanswered_does_not_reregister_when_granted_or_in_call);
    RUN_TEST(test_persistent_cfg_mismatch_stops_asking_after_a_list_answers_it);
    RUN_TEST(test_cell_cfg_needs_an_attached_link);
    RUN_TEST(test_deactivate_forgets_the_list_version);
    RUN_TEST(test_asks_for_channel_when_not_granted);
    RUN_TEST(test_cell_mode_change_registers_again);
    RUN_TEST(test_reattach_registers_again);
    RUN_TEST(test_asks_for_channel_while_awaiting_reply);
    RUN_TEST(test_registration_supervision_timeout);
    RUN_TEST(test_activation_supervision_timeout);
    RUN_TEST(test_reg_rej_not_activated_does_not_deactivate);
    RUN_TEST(test_duplicate_voice_frame_rejected);
    RUN_TEST(test_reg_timeout_does_not_escalate_backoff);
    RUN_TEST(test_reg_rejections_still_escalate_backoff);
    RUN_TEST(test_reattach_while_registering_retries_at_once);
    RUN_TEST(test_registration_forgets_the_old_networks_numbering);
    RUN_TEST(test_reg_ack_before_this_attempts_auth_rsp_ignored);
    return UNITY_END();
}
