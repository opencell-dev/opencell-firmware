/* The home side of activation and authentication (lc_sig_hss), checked the
 * way the terminal checks it: MILENAGE on the terminal's keys for vectors and
 * AUTS, the terminal's own key derivation for ACT_ACK. */
#include "unity.h"

#include <string.h>

#include "lc_sig_crypto.h"
#include "lc_sig_hss.h"
#include "lc_sig_keys.h"
#include "lc_sig_milenage.h"
#include "lc_sig_term.h"

void setUp(void) {}
void tearDown(void) {}

#define TMID  0x76ad0488u
#define TMID2 0x11223344u
#define NOW   1790000000u

static const uint8_t K[16] = { 0x46, 0x5b, 0x5c, 0xe8, 0xb1, 0x99, 0xb4, 0x9f,
                               0xaa, 0x5f, 0x0a, 0x2e, 0xe2, 0x38, 0xa6, 0xbc };
static const uint8_t OPC[16] = { 0xcd, 0x63, 0xcb, 0x71, 0x95, 0x4a, 0x9f, 0x4e,
                                 0x48, 0xa5, 0x99, 0x4e, 0x37, 0xa0, 0x2b, 0xaf };

/* What lc_sig_term does with AUTH_REQ: 0 and the SQN if MAC-A verifies. */
static int terminal_check(const lc_sig_av_t *av, uint8_t sqn[6], lc_milenage_t *o)
{
    static const uint8_t zero[6] = { 0 }, amf[2] = { 0x80, 0x00 };
    if (lc_milenage(K, OPC, av->rand, zero, amf, o) != 0) return -1;
    for (int i = 0; i < 6; i++) sqn[i] = (uint8_t)(av->autn[i] ^ o->ak[i]);
    if (lc_milenage(K, OPC, av->rand, sqn, av->autn + 6, o) != 0) return -1;
    return lc_sig_ct_equal(o->mac_a, av->autn + 8, 8) ? 0 : -1;
}

/* What lc_sig_term sends as AUTS for its own SQN sqn_ms. */
static void terminal_auts(const uint8_t rand[16], const uint8_t sqn_ms[6], uint8_t auts[14])
{
    static const uint8_t amf0[2] = { 0, 0 };
    lc_milenage_t o;
    lc_milenage(K, OPC, rand, sqn_ms, amf0, &o);
    for (int i = 0; i < 6; i++) auts[i] = (uint8_t)(sqn_ms[i] ^ o.ak_s[i]);
    memcpy(auts + 6, o.mac_s, 8);
}

static void test_av_make_passes_the_terminal_check(void)
{
    uint8_t sqn[6], rand[16], got[6];
    lc_sig_av_t av;
    lc_milenage_t o;
    lc_sig_sqn_put(sqn, 42);
    memset(rand, 0x5a, 16);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_av_make(K, OPC, sqn, rand, &av));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rand, av.rand, 16);
    TEST_ASSERT_EQUAL_INT(0, terminal_check(&av, got, &o));
    TEST_ASSERT_EQUAL_UINT64(42, lc_sig_sqn_get(got));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(o.res, av.xres, 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(o.ck, av.ck, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(o.ik, av.ik, 16);
    av.autn[15] ^= 1; /* a forged MAC-A */
    TEST_ASSERT_EQUAL_INT(-1, terminal_check(&av, got, &o));
}

static void test_auts_gives_the_terminal_sqn_and_refuses_forgeries(void)
{
    uint8_t rand[16], ms[6], auts[14], got[6];
    memset(rand, 0x33, 16);
    lc_sig_sqn_put(ms, 500);
    terminal_auts(rand, ms, auts);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_av_auts(K, OPC, rand, auts, got));
    TEST_ASSERT_EQUAL_UINT64(500, lc_sig_sqn_get(got));
    auts[13] ^= 1;
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_av_auts(K, OPC, rand, auts, got));
    auts[13] ^= 1;
    rand[0] ^= 1; /* AUTS for another challenge */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_av_auts(K, OPC, rand, auts, got));
}

/* A network key pair, a terminal key pair, a token, and the ACT_REQ fields. */
static uint8_t SKN[32], PKN[32], TOK[8], SECRET[16], NUM[LC_SIG_NUMBER_LEN];
static lc_sig_ident_t ID;

static void act_world(void)
{
    uint8_t r[32];
    memset(SKN, 0x11, 32);
    lc_sig_x25519_public(SKN, PKN);
    memset(r, 0x42, 32);
    lc_sig_ident_new(&ID, r);
    memset(TOK, 0xa0, 8);
    memset(SECRET, 0xb0, 16);
    lc_sig_number_to_bcd("+883160655501234", 16, NUM);
}

static lc_sig_act_token_t fresh_token(void)
{
    lc_sig_act_token_t t;
    memset(&t, 0, sizeof(t));
    t.known = 1;
    t.expiry = NOW + 3600u;
    memcpy(t.secret, SECRET, 16);
    return t;
}

static int answer(const lc_sig_act_token_t *t, uint32_t tmid, const uint8_t pk[32], lc_sig_msg_t *out,
                  uint8_t k[16], uint8_t opc[16])
{
    uint8_t tag[8];
    lc_sig_act_tag(SECRET, tmid, pk, TOK, tag);
    return lc_sig_act_answer(t, SKN, NOW, tmid, TOK, pk, tag, NUM, out, k, opc);
}

static void test_act_fresh_ack_confirms_with_the_terminal_keys(void)
{
    act_world();
    lc_sig_act_token_t t = fresh_token();
    lc_sig_msg_t m;
    uint8_t k[16], opc[16], tk[16], topc[16], conf[8];
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_FRESH, answer(&t, TMID, ID.pk, &m, k, opc));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_ACK, m.type);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(NUM, m.u.act_ack.number, LC_SIG_NUMBER_LEN);
    /* the terminal derives the same K and OPc from its side of the exchange */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_act_keys(ID.sk, PKN, TMID, TOK, tk, topc));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tk, k, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(topc, opc, 16);
    lc_sig_act_confirm(tk, TMID, TOK, conf);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(conf, m.u.act_ack.confirm, 8);
}

static void test_act_refusals(void)
{
    act_world();
    lc_sig_msg_t m;
    uint8_t k[16], opc[16], want[8];
    lc_sig_act_token_t t;

    memset(&t, 0, sizeof(t)); /* unknown: no secret, zero tag */
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_REFUSED, answer(&t, TMID, ID.pk, &m, k, opc));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_NAK, m.type);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_UNKNOWN, m.u.act_nak.reason);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, m.u.act_nak.tag, 8);

    t = fresh_token();
    t.expiry = NOW - 1u;
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_REFUSED, answer(&t, TMID, ID.pk, &m, k, opc));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_EXPIRED, m.u.act_nak.reason);
    lc_sig_act_nak_tag(SECRET, TMID, TOK, LC_SIG_ACT_EXPIRED, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m.u.act_nak.tag, 8);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, k, 16); /* no keys leave a refusal */

    t = fresh_token();
    uint8_t tag[8];
    lc_sig_act_tag(SECRET, TMID, ID.pk, TOK, tag);
    tag[0] ^= 1; /* someone guessing without the real QR */
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_REFUSED, lc_sig_act_answer(&t, SKN, NOW, TMID, TOK, ID.pk, tag, NUM, &m, k, opc));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_BAD_TAG, m.u.act_nak.reason);

    t = fresh_token(); /* used, and bound to another terminal */
    t.used = 1;
    t.bound_tmid = TMID2;
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_REFUSED, answer(&t, TMID, ID.pk, &m, k, opc));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_USED, m.u.act_nak.reason);
}

/* Final review I2 (plan 5): the same terminal and key pair asking again gets
 * the same ACK; the same TMID with another key pair is refused as used. */
static void test_act_again_only_for_the_same_key_pair(void)
{
    act_world();
    lc_sig_act_token_t t = fresh_token();
    lc_sig_msg_t first, m;
    uint8_t k[16], opc[16], k2[16], opc2[16];
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_FRESH, answer(&t, TMID, ID.pk, &first, k, opc));
    t.used = 1;
    t.bound_tmid = TMID;
    memcpy(t.bound_k, k, 16);
    t.expiry = NOW - 1u; /* expiry no longer matters once used */
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_AGAIN, answer(&t, TMID, ID.pk, &m, k2, opc2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&first, &m, sizeof(m));

    lc_sig_ident_t other;
    uint8_t r[32];
    memset(r, 0x99, 32);
    lc_sig_ident_new(&other, r);
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_REFUSED, answer(&t, TMID, other.pk, &m, k2, opc2));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_USED, m.u.act_nak.reason);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, k2, 16);   /* a refusal never hands out keys */
    TEST_ASSERT_EACH_EQUAL_HEX8(0, opc2, 16);
}

/* TMID 0 names no terminal: refused before any token or tag check. */
static void test_act_refuses_tmid_zero(void)
{
    act_world();
    lc_sig_act_token_t t = fresh_token();
    lc_sig_msg_t m;
    uint8_t k[16], opc[16];
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_REFUSED, answer(&t, 0, ID.pk, &m, k, opc));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_NAK, m.type);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_BAD_TAG, m.u.act_nak.reason);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, k, 16);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, opc, 16);
}

static void flat_act(lc_sig_sub_t *subs, unsigned n, uint32_t tmid, const uint8_t pk[32], lc_sig_msg_t *m,
                     uint32_t drop[2], unsigned *nd, int want)
{
    uint8_t tag[8];
    lc_sig_act_tag(SECRET, tmid, pk, TOK, tag);
    TEST_ASSERT_EQUAL_INT(want, lc_sig_flat_act(subs, n, SKN, NOW, tmid, TOK, pk, tag, m, drop, nd));
}

/* Re-activation: the subscriber's old terminal, and whatever the new TMID
 * was bound to, are dropped; the old binding of the TMID is cleared. */
static void test_flat_act_binds_and_names_the_terminals_to_drop(void)
{
    act_world();
    lc_sig_sub_t subs[2];
    lc_sig_msg_t m;
    uint32_t drop[2];
    unsigned nd;
    memset(subs, 0, sizeof(subs));
    memcpy(subs[0].number, NUM, LC_SIG_NUMBER_LEN);
    memcpy(subs[0].token_id, TOK, 8);
    memcpy(subs[0].token_secret, SECRET, 16);
    subs[0].token_expiry = NOW + 60u;
    lc_sig_number_to_bcd("+883160655501235", 16, subs[1].number); /* bound to TMID2 */
    subs[1].tmid = TMID2;
    subs[1].activated = 1;

    flat_act(subs, 2, TMID, ID.pk, &m, drop, &nd, LC_SIG_ACT_FRESH);
    TEST_ASSERT_EQUAL_UINT(0, nd); /* a first activation replaces nobody */
    TEST_ASSERT_TRUE(subs[0].activated && subs[0].token_used);
    TEST_ASSERT_EQUAL_HEX32(TMID, subs[0].tmid);
    TEST_ASSERT_EQUAL_UINT64(0, lc_sig_sqn_get(subs[0].sqn));

    flat_act(subs, 2, TMID, ID.pk, &m, drop, &nd, LC_SIG_ACT_AGAIN); /* the ACK was lost */
    TEST_ASSERT_EQUAL_UINT(0, nd);

    /* a new token for number 0, presented from TMID2 (bound to number 1) */
    memset(subs[0].token_id, 0xc0, 8);
    memcpy(TOK, subs[0].token_id, 8);
    subs[0].token_used = 0;
    flat_act(subs, 2, TMID2, ID.pk, &m, drop, &nd, LC_SIG_ACT_FRESH);
    TEST_ASSERT_EQUAL_UINT(2, nd);
    TEST_ASSERT_EQUAL_HEX32(TMID, drop[0]);  /* number 0's old terminal */
    TEST_ASSERT_EQUAL_HEX32(TMID2, drop[1]); /* TMID2's old binding (number 1) */
    TEST_ASSERT_EQUAL_HEX32(TMID2, subs[0].tmid);
    TEST_ASSERT_FALSE(subs[1].activated);
    TEST_ASSERT_EQUAL_HEX32(0, subs[1].tmid);
}

/* An all-zero token_id (an unprovisioned record's default) must never match
 * a request, even one that also presents an all-zero token_id. */
static void test_flat_act_all_zero_token_id_never_matches(void)
{
    act_world();
    lc_sig_sub_t subs[2]; /* both unprovisioned: token_id all zero */
    lc_sig_msg_t m;
    uint32_t drop[2];
    unsigned nd;
    uint8_t zero_token[8] = { 0 }, tag[8];
    memset(subs, 0, sizeof(subs));

    lc_sig_act_tag(SECRET, TMID, ID.pk, zero_token, tag); /* guessing the all-zero token */
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_REFUSED,
                          lc_sig_flat_act(subs, 2, SKN, NOW, TMID, zero_token, ID.pk, tag, &m, drop, &nd));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_UNKNOWN, m.u.act_nak.reason);
    TEST_ASSERT_FALSE(subs[0].activated);
    TEST_ASSERT_FALSE(subs[1].activated);
}

/* A refused ACT_REQ must not touch the subscriber record: no partial
 * activation, no stashed keys, whichever refusal reason fires. */
static void test_flat_act_refusal_leaves_the_record_unchanged(void)
{
    act_world();
    lc_sig_sub_t subs[1];
    lc_sig_msg_t m;
    uint32_t drop[2];
    unsigned nd;
    memset(subs, 0, sizeof(subs));
    memcpy(subs[0].number, NUM, LC_SIG_NUMBER_LEN);
    memcpy(subs[0].token_id, TOK, 8);
    memcpy(subs[0].token_secret, SECRET, 16);
    subs[0].token_expiry = NOW - 1u; /* already expired */

    uint8_t tag[8];
    lc_sig_act_tag(SECRET, TMID, ID.pk, TOK, tag);
    TEST_ASSERT_EQUAL_INT(LC_SIG_ACT_REFUSED,
                          lc_sig_flat_act(subs, 1, SKN, NOW, TMID, TOK, ID.pk, tag, &m, drop, &nd));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_EXPIRED, m.u.act_nak.reason);
    TEST_ASSERT_FALSE(subs[0].activated);
    TEST_ASSERT_FALSE(subs[0].token_used);
    TEST_ASSERT_EQUAL_HEX32(0, subs[0].tmid);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, subs[0].k, 16);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, subs[0].opc, 16);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, subs[0].sqn, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(NUM, subs[0].number, LC_SIG_NUMBER_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(TOK, subs[0].token_id, 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(SECRET, subs[0].token_secret, 16);
    TEST_ASSERT_EQUAL_UINT32(NOW - 1u, subs[0].token_expiry);
}

static void test_flat_av_steps_sqn_and_resync_takes_the_terminal_sqn(void)
{
    lc_sig_sub_t sub;
    lc_sig_av_t av;
    lc_milenage_t o;
    uint8_t rand[16], num[LC_SIG_NUMBER_LEN], got[6], ms[6], auts[14];
    memset(&sub, 0, sizeof(sub));
    lc_sig_number_to_bcd("+883160655501234", 16, sub.number);
    memcpy(sub.k, K, 16);
    memcpy(sub.opc, OPC, 16);
    sub.tmid = TMID;
    sub.activated = 1;
    lc_sig_sqn_put(sub.sqn, 7);
    memset(rand, 0x21, 16);

    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AV_NOT_ACTIVATED, lc_sig_flat_av(&sub, 1, TMID2, rand, num, &av));
    uint8_t no_auts[14] = { 0 };
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AV_NOT_ACTIVATED,
                            lc_sig_flat_resync(&sub, 1, TMID2, rand, no_auts, rand, num, &av));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AV_OK, lc_sig_flat_av(&sub, 1, TMID, rand, num, &av));
    TEST_ASSERT_EQUAL_UINT64(8, lc_sig_sqn_get(sub.sqn));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sub.number, num, LC_SIG_NUMBER_LEN);
    TEST_ASSERT_EQUAL_INT(0, terminal_check(&av, got, &o));
    TEST_ASSERT_EQUAL_UINT64(8, lc_sig_sqn_get(got));

    lc_sig_sqn_put(ms, 900); /* the terminal is ahead: it answers with AUTS */
    terminal_auts(av.rand, ms, auts);
    uint8_t fresh[16];
    memset(fresh, 0x77, 16);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AV_OK, lc_sig_flat_resync(&sub, 1, TMID, av.rand, auts, fresh, num, &av));
    TEST_ASSERT_EQUAL_UINT64(901, lc_sig_sqn_get(sub.sqn));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fresh, av.rand, 16);
    TEST_ASSERT_EQUAL_INT(0, terminal_check(&av, got, &o));
    TEST_ASSERT_EQUAL_UINT64(901, lc_sig_sqn_get(got));

    auts[6] ^= 1;
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AV_AUTH_FAILED, lc_sig_flat_resync(&sub, 1, TMID, rand, auts, fresh, num, &av));
    TEST_ASSERT_EQUAL_UINT64(901, lc_sig_sqn_get(sub.sqn)); /* untouched */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_av_make_passes_the_terminal_check);
    RUN_TEST(test_auts_gives_the_terminal_sqn_and_refuses_forgeries);
    RUN_TEST(test_act_fresh_ack_confirms_with_the_terminal_keys);
    RUN_TEST(test_act_refusals);
    RUN_TEST(test_act_again_only_for_the_same_key_pair);
    RUN_TEST(test_act_refuses_tmid_zero);
    RUN_TEST(test_flat_act_binds_and_names_the_terminals_to_drop);
    RUN_TEST(test_flat_act_all_zero_token_id_never_matches);
    RUN_TEST(test_flat_act_refusal_leaves_the_record_unchanged);
    RUN_TEST(test_flat_av_steps_sqn_and_resync_takes_the_terminal_sqn);
    return UNITY_END();
}
