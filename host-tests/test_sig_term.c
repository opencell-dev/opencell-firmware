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
static uint8_t ev[16][32], ev_len[16];
static int ev_n;

static int io_send(void *c, const uint8_t *p, uint8_t n) { (void)c; memcpy(ul[ul_n], p, n); ul_len[ul_n++] = n; return 0; }
static int io_svc(void *c, uint8_t cause) { (void)c; (void)cause; svc_reqs++; return 0; }
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
        lc_sig_number_to_bcd("+8836065551234", 14, id.number);
    }
    ul_n = ev_n = saves = svc_reqs = 0;
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
    memset(r, 7, 32);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_ident_new(&a, r));
    lc_sig_x25519_public(r, pub);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pub, a.pk, 32);
    TEST_ASSERT_FALSE(a.activated);
    a.activated = 1;
    a.key_id = 0x1234;
    a.sqn[5] = 9;
    TEST_ASSERT_EQUAL_size_t(LC_SIG_IDENT_BLOB, lc_sig_ident_pack(&a, blob));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_ident_unpack(blob, sizeof(blob), &b));
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_ident_unpack(blob, sizeof(blob) - 1, &b));
    blob[0] = 1; /* v2 length, v1 version byte */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_ident_unpack(blob, sizeof(blob), &b));
}

static void test_commands_in_wrong_state_refused(void)
{
    boot(0);
    static const uint8_t dial[] = "\x02+8836065551234";
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
    lc_sig_number_to_bcd("+8836065551234", 14, q.number);
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
    static const uint8_t dial[] = "\x02+8836065550100";
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
    lc_sig_number_to_bcd("+8836065550100", 14, m.u.setup_ind.caller);
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
static void test_release_call_id_0_while_calling_ends_busy(void)
{
    register_ok();
    static const uint8_t dial[] = "\x02+8836065550100";
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ident_pack_roundtrip_and_new);
    RUN_TEST(test_activation_prepared_outside_the_lock);
    RUN_TEST(test_commands_in_wrong_state_refused);
    RUN_TEST(test_activation_request_and_ack);
    RUN_TEST(test_bad_confirm_fails_activation);
    RUN_TEST(test_bad_network_mac_reported);
    RUN_TEST(test_sqn_out_of_range_sends_auts);
    RUN_TEST(test_outgoing_call_flow);
    RUN_TEST(test_incoming_call_answer_voice_and_release);
    RUN_TEST(test_deactivate_wipes);
    RUN_TEST(test_deactivate_wipes_ram_key_copies);
    RUN_TEST(test_release_call_id_0_while_calling_ends_busy);
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
    return UNITY_END();
}
