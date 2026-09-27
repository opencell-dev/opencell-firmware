#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "lc_sig_crypto.h"
#include "lc_sig_frag.h"
#include "lc_sig_keys.h"
#include "lc_sig_prot.h"

void setUp(void) {}
void tearDown(void) {}

static void hex(const char *s, uint8_t *out)
{
    for (size_t i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(&s[2 * i], "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_fragment_and_reassemble_any_order_of_duplicates(void)
{
    uint8_t msg[51], out[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], olen[LC_SIG_MAX_FRAGS], back[LC_SIG_MAX_MSG], seq;
    size_t len;
    for (int i = 0; i < 51; i++) msg[i] = (uint8_t)(i * 3);
    uint8_t n = lc_sig_fragment(msg, sizeof(msg), 9, out, olen);
    TEST_ASSERT_EQUAL_UINT8(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x10, out[0][0]);          /* frag 0 */
    TEST_ASSERT_EQUAL_HEX8(0x10 | 2 << 2 | 1 << 1, out[2][0]); /* frag 2, last */
    TEST_ASSERT_EQUAL_UINT8(20, olen[0]);
    TEST_ASSERT_EQUAL_UINT8(2 + 15, olen[2]);
    lc_sig_reasm_t r;
    lc_sig_reasm_init(&r);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_reasm_push(&r, out[2], olen[2], back, &len, &seq));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_reasm_push(&r, out[0], olen[0], back, &len, &seq));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_reasm_push(&r, out[0], olen[0], back, &len, &seq)); /* duplicate */
    TEST_ASSERT_EQUAL_INT(1, lc_sig_reasm_push(&r, out[1], olen[1], back, &len, &seq));
    TEST_ASSERT_EQUAL_size_t(51, len);
    TEST_ASSERT_EQUAL_UINT8(9, seq);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, back, 51);
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_fragment(msg, 73, 0, out, olen)); /* too long */
    uint8_t bad[3] = { 0x80, 0, 0 };
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_reasm_push(&r, bad, 3, back, &len, &seq));
}

static void keyed_pair(lc_sig_sec_t *term, lc_sig_sec_t *net, int encrypt)
{
    uint8_t ki[16], ke[16];
    for (int i = 0; i < 16; i++) { ki[i] = (uint8_t)i; ke[i] = (uint8_t)(0x80 + i); }
    lc_sig_sec_init(term, 0);
    lc_sig_sec_init(net, 1);
    lc_sig_sec_key(term, ki, ke, encrypt);
    lc_sig_sec_key(net, ki, ke, encrypt);
}

static void test_preauth_messages_travel_clear(void)
{
    lc_sig_sec_t t, n;
    lc_sig_sec_init(&t, 0);
    lc_sig_sec_init(&n, 1);
    lc_sig_msg_t m, back;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_REG_REQ;
    m.u.reg_req.caps = 1;
    uint8_t buf[80];
    size_t len = lc_sig_seal(&t, &m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(3 + 4, len);
    TEST_ASSERT_EQUAL_HEX8(0, buf[1]); /* prot 0 */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_open(&n, buf, len, &back));
    TEST_ASSERT_EQUAL_UINT8(1, back.u.reg_req.caps);
    m.type = LC_SIG_CALL_SETUP; /* needs keys */
    TEST_ASSERT_EQUAL_size_t(0, lc_sig_seal(&t, &m, buf, sizeof(buf)));
}

static void test_part15_encrypts_and_macs(void)
{
    lc_sig_sec_t t, n;
    keyed_pair(&t, &n, 1);
    lc_sig_msg_t m, back;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CALL_SETUP;
    m.u.call_setup.ref = 5;
    lc_sig_number_to_bcd("+883160655501234", 16, m.u.call_setup.called);
    uint8_t buf[80];
    size_t len = lc_sig_seal(&t, &m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(3 + 10 + 4, len);
    TEST_ASSERT_EQUAL_HEX8(2, buf[1]);
    TEST_ASSERT_NOT_EQUAL(0x88, buf[4]); /* number is not in clear */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_open(&n, buf, len, &back));
    TEST_ASSERT_EQUAL_MEMORY(&m, &back, sizeof(m));
    buf[5] ^= 1; /* tamper */
    lc_sig_sec_t n2;
    keyed_pair(&t, &n2, 1);
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_open(&n2, buf, len, &back));
}

static void test_part97_integrity_only_and_no_downgrade(void)
{
    lc_sig_sec_t t, n;
    keyed_pair(&t, &n, 0);
    lc_sig_msg_t m, back;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_RELEASE;
    m.u.release.call_id = 0x11223344;
    uint8_t buf[80];
    size_t len = lc_sig_seal(&t, &m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_HEX8(1, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x11, buf[3]); /* body in clear */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_open(&n, buf, len, &back));
    lc_sig_sec_t p15;
    lc_sig_sec_init(&p15, 1);
    uint8_t ki[16], ke[16];
    for (int i = 0; i < 16; i++) { ki[i] = (uint8_t)i; ke[i] = (uint8_t)(0x80 + i); }
    lc_sig_sec_key(&p15, ki, ke, 1); /* a Part 15 receiver refuses prot 1 */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_open(&p15, buf, len, &back));
}

static void test_counter_wraps_and_rejects_replay(void)
{
    lc_sig_sec_t t, n;
    keyed_pair(&t, &n, 1);
    lc_sig_msg_t m, back;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_ALERTING;
    uint8_t buf[80], old[80];
    size_t old_len = 0;
    for (uint32_t i = 0; i < 300; i++) {
        m.u.call.call_id = i;
        size_t len = lc_sig_seal(&t, &m, buf, sizeof(buf));
        if (i == 5) { memcpy(old, buf, len); old_len = len; }
        TEST_ASSERT_EQUAL_INT(0, lc_sig_open(&n, buf, len, &back));
        TEST_ASSERT_EQUAL_UINT32(i, back.u.call.call_id);
    }
    TEST_ASSERT_EQUAL_UINT32(300, n.rx_next);
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_open(&n, old, old_len, &back)); /* replay */
}

static void test_activation_keys_agree_and_tags_bind(void)
{
    uint8_t a[32], b[32], apub[32], bpub[32], ka[16], oa[16], kb[16], ob[16], tok[8], sec[16];
    uint8_t tag1[8], tag2[8], c1[8], c2[8];
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
    lc_sig_x25519_public(a, apub);
    lc_sig_x25519_public(b, bpub);
    memset(tok, 0xa5, 8);
    memset(sec, 0x5a, 16);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_act_keys(a, bpub, 0x76ad0488u, tok, ka, oa));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_act_keys(b, apub, 0x76ad0488u, tok, kb, ob));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ka, kb, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(oa, ob, 16);
    TEST_ASSERT_FALSE(memcmp(ka, oa, 16) == 0);
    lc_sig_act_tag(sec, 0x76ad0488u, apub, tok, tag1);
    lc_sig_act_tag(sec, 0x76ad0489u, apub, tok, tag2); /* another TMID: another tag */
    TEST_ASSERT_FALSE(memcmp(tag1, tag2, 8) == 0);
    lc_sig_act_confirm(ka, 0x76ad0488u, tok, c1);
    lc_sig_act_confirm(kb, 0x76ad0488u, tok, c2);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(c1, c2, 8);
    TEST_ASSERT_TRUE(lc_sig_ct_equal(c1, c2, 8));
}

/* numbering v2 §4.2: with 8-byte numbers ACT_ACK takes 2 fragments, REG_ACK
 * exactly fills 1, CALL_SETUP fits 1 and SETUP_IND stays at 2. */
static void test_numbered_messages_fragment_counts(void)
{
    lc_sig_sec_t t, n;
    keyed_pair(&t, &n, 1);
    uint8_t num[LC_SIG_NUMBER_LEN], buf[80], out[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], olen[LC_SIG_MAX_FRAGS];
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+883160655501234", 16, num));
    static const struct { uint8_t type; size_t len; uint8_t frags; } k[] = {
        { LC_SIG_ACT_ACK, 3 + 16, 2 },        /* prot 0: no MAC */
        { LC_SIG_REG_ACK, 3 + 11 + 4, 1 },
        { LC_SIG_CALL_SETUP, 3 + 10 + 4, 1 },
        { LC_SIG_SETUP_IND, 3 + 13 + 4, 2 },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        lc_sig_msg_t m;
        memset(&m, 0, sizeof(m));
        m.type = k[i].type;
        uint8_t *field = k[i].type == LC_SIG_ACT_ACK   ? m.u.act_ack.number
                         : k[i].type == LC_SIG_REG_ACK ? m.u.reg_ack.number
                         : k[i].type == LC_SIG_CALL_SETUP ? m.u.call_setup.called
                                                         : m.u.setup_ind.caller;
        memcpy(field, num, sizeof(num));
        lc_sig_sec_t *s = k[i].type == LC_SIG_CALL_SETUP ? &t : &n; /* CALL_SETUP goes up, the rest down */
        size_t len = lc_sig_seal(s, &m, buf, sizeof(buf));
        TEST_ASSERT_EQUAL_size_t(k[i].len, len);
        TEST_ASSERT_EQUAL_UINT8(k[i].frags, lc_sig_fragment(buf, len, 0, out, olen));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fragment_and_reassemble_any_order_of_duplicates);
    RUN_TEST(test_preauth_messages_travel_clear);
    RUN_TEST(test_part15_encrypts_and_macs);
    RUN_TEST(test_part97_integrity_only_and_no_downgrade);
    RUN_TEST(test_counter_wraps_and_rejects_replay);
    RUN_TEST(test_activation_keys_agree_and_tags_bind);
    RUN_TEST(test_numbered_messages_fragment_counts);
    return UNITY_END();
}
