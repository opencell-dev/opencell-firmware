/* Terminal-to-terminal calls: two terminal roles and the network role over a
 * fake link (one UL and one DL payload per 120 ms frame each, always granted).
 * The network switches the call itself and relays app data between the legs,
 * as lcbench net does. */
#include "unity.h"

#include <string.h>

#include "lc_sig_crypto.h"
#include "lc_sig_keys.h"
#include "lc_sig_net.h"
#include "lc_sig_term.h"

void setUp(void) {}
void tearDown(void) {}

#define FRAME 120000u

typedef struct { uint8_t p[32][LC_SIG_LINK_MAX], n[32]; int head, count; } q_t;

typedef struct {
    uint32_t       tmid;
    lc_sig_term_t  t;
    lc_sig_ident_t id;
    q_t            ul, dl;
    uint8_t        ev[32][16];
    int            nev;
    uint8_t        app[LC_SIG_APP_MAX], app_n;
    uint8_t        air[LC_SIG_LINK_MAX], air_n; /* the last app data frame it sent, as on the air */
} term_t;

static term_t A, B;
static lc_sig_net_t N;
static lc_sig_sub_t subs[2];
static lc_sig_net_call_ev_t calls[16];
static int ncalls;
static uint64_t now;
static uint32_t rng = 7;

static int qpush(q_t *q, const uint8_t *p, uint8_t n)
{
    if (q->count == 32) return -1;
    int i = (q->head + q->count++) % 32;
    memcpy(q->p[i], p, n);
    q->n[i] = n;
    return 0;
}
static int qpop(q_t *q, uint8_t *p, uint8_t *n)
{
    if (q->count == 0) return -1;
    memcpy(p, q->p[q->head], q->n[q->head]);
    *n = q->n[q->head];
    q->head = (q->head + 1) % 32;
    q->count--;
    return 0;
}
static term_t *by_tmid_t(uint32_t tmid) { return tmid == A.tmid ? &A : tmid == B.tmid ? &B : NULL; }

/* network io: a two-subscriber HSS */
static lc_sig_sub_t *h_token(void *c, const uint8_t t[8])
{
    (void)c;
    for (int i = 0; i < 2; i++) if (memcmp(subs[i].token_id, t, 8) == 0) return &subs[i];
    return NULL;
}
static lc_sig_sub_t *h_tmid(void *c, uint32_t tmid)
{
    (void)c;
    for (int i = 0; i < 2; i++) if (subs[i].activated && subs[i].tmid == tmid) return &subs[i];
    return NULL;
}
static lc_sig_sub_t *h_number(void *c, const uint8_t num[7])
{
    (void)c;
    for (int i = 0; i < 2; i++) if (memcmp(subs[i].number, num, 7) == 0) return &subs[i];
    return NULL;
}
static int n_send(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    term_t *t = by_tmid_t(tmid);
    return t != NULL ? qpush(&t->dl, p, n) : -1;
}
static void n_call(void *c, const lc_sig_net_call_ev_t *e) { (void)c; calls[ncalls++ % 16] = *e; }
static void n_random(void *c, uint8_t *o, size_t n) { (void)c; for (size_t i = 0; i < n; i++) o[i] = (uint8_t)((rng = rng * 1103515245u + 12345u) >> 16); }
static uint32_t n_unix(void *c) { (void)c; return 1790000000u; }
static const lc_sig_net_io_t net_io = { NULL, h_token, h_tmid, h_number, NULL, NULL, n_send,
                                        NULL, n_call, n_random, n_unix, NULL };

/* terminal io (ctx = term_t) */
static int t_send(void *c, const uint8_t *p, uint8_t n) { return qpush(&((term_t *)c)->ul, p, n); }
static int t_svc(void *c, uint8_t cause) { (void)c; (void)cause; return 0; }
static void t_event(void *c, const uint8_t *e, uint8_t n)
{
    term_t *t = c;
    memcpy(t->ev[t->nev % 32], e, n);
    t->nev++;
}

static void make(term_t *t, lc_sig_sub_t *s, uint32_t tmid, const char *number, uint8_t kbyte)
{
    memset(t, 0, sizeof(*t));
    t->tmid = tmid;
    uint8_t r[32];
    memset(r, kbyte, 32);
    lc_sig_ident_new(&t->id, r);
    t->id.activated = 1;
    t->id.key_id = 1;
    memset(t->id.k, kbyte, 16);
    memset(t->id.opc, (uint8_t)(kbyte + 1u), 16);
    lc_sig_number_to_bcd(number, strlen(number), t->id.number);
    memset(s, 0, sizeof(*s));
    memcpy(s->number, t->id.number, 7);
    memcpy(s->k, t->id.k, 16);
    memcpy(s->opc, t->id.opc, 16);
    s->tmid = tmid;
    s->activated = 1;
    s->token_used = 1;
    const lc_sig_term_io_t io = { t, t_send, t_svc, NULL, t_event };
    lc_sig_term_init(&t->t, &io, &t->id, tmid, 0);
}

static void world(uint8_t mode)
{
    now = 0;
    ncalls = 0;
    uint8_t sk[32];
    memset(sk, 0x11, 32);
    lc_sig_net_cfg_t cfg = { 1, { 0 }, mode, 1800 };
    memcpy(cfg.sk, sk, 32);
    lc_sig_net_init(&N, &net_io, &cfg);
    make(&A, &subs[0], 0x76ad0488u, "+8836065551234", 0x21);
    make(&B, &subs[1], 0x76ae1ae8u, "+8836065551235", 0x31);
}

static void frame(void)
{
    term_t *ts[2] = { &A, &B };
    uint8_t p[LC_SIG_LINK_MAX], n;
    now += FRAME;
    for (int i = 0; i < 2; i++) {
        lc_sig_term_link(&ts[i]->t, 1, 1, now);
        lc_sig_net_link(&N, ts[i]->tmid, 1, now);
        lc_sig_term_tick(&ts[i]->t, now);
    }
    lc_sig_net_tick(&N, now);
    for (int i = 0; i < 2; i++) {
        term_t *t = ts[i];
        lc_sig_net_heard(&N, t->tmid, now);
        if (qpop(&t->ul, p, &n) == 0) {
            if ((p[0] & 0xF0u) == LC_SIG_KIND_SIG) {
                lc_sig_net_rx(&N, t->tmid, p, n, now);
            } else if (p[0] == LC_SIG_KIND_DATA) { /* as lcb_net: to the other leg, else echo */
                uint8_t d[LC_SIG_APP_MAX], dn, out[LC_SIG_LINK_MAX], on;
                uint32_t to = t->tmid;
                lc_sig_net_local_peer(&N, t->tmid, &to);
                if (lc_sig_net_data_in(&N, t->tmid, p, n, d, &dn) == 0 &&
                    lc_sig_net_data_out(&N, to, d, dn, out, &on) == 0) {
                    qpush(&by_tmid_t(to)->dl, out, on);
                }
            }
        }
        if (qpop(&t->dl, p, &n) == 0) {
            if ((p[0] & 0xF0u) == LC_SIG_KIND_SIG) {
                lc_sig_term_rx(&t->t, p, n, now);
            } else if (p[0] == LC_SIG_KIND_DATA) {
                lc_sig_term_data_in(&t->t, p, n, t->app, &t->app_n);
            }
        }
    }
}

static void run_ms(uint32_t ms) { for (uint32_t t = 0; t < ms; t += FRAME / 1000u) frame(); }

static const uint8_t *event(const term_t *t, uint8_t code)
{
    for (int i = t->nev - 1; i >= 0 && i >= t->nev - 32; i--) if (t->ev[i % 32][0] == code) return t->ev[i % 32];
    return NULL;
}

static void cmd(term_t *t, const char *c, size_t n)
{
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&t->t, (const uint8_t *)c, n, now));
}

static void app_up(term_t *t, const char *s)
{
    uint8_t out[LC_SIG_LINK_MAX], on;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_out(&t->t, (const uint8_t *)s, (uint8_t)strlen(s), out, &on));
    memcpy(t->air, out, on);
    t->air_n = on;
    qpush(&t->ul, out, on);
}

static void both_registered(void)
{
    run_ms(5000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&B.t));
}

static void a_calls_b(void)
{
    static const char dial[] = "\x02+8836065551235";
    cmd(&A, dial, sizeof(dial) - 1);
    run_ms(3000);
    const uint8_t *in = event(&B, LC_SIG_EV_INCOMING);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(A.id.number, in + 5, 7); /* caller id is A's number */
    TEST_ASSERT_NOT_NULL(event(&A, LC_SIG_EV_RINGING));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_RINGING_OUT, lc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_RINGING_IN, lc_sig_term_state(&B.t));
}

static void test_local_call_connects_and_carries_data_part15(void)
{
    world(LC_SIG_MODE_PART15);
    both_registered();
    a_calls_b();
    TEST_ASSERT_EQUAL_INT(LC_SIG_NET_LOCAL, calls[0].what);
    TEST_ASSERT_EQUAL_HEX32(B.tmid, calls[0].peer_tmid);
    uint8_t c = LC_SIG_CMD_ANSWER;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&B.t));
    uint32_t peer = 0;
    TEST_ASSERT_EQUAL_INT(1, lc_sig_net_local_peer(&N, A.tmid, &peer));
    TEST_ASSERT_EQUAL_HEX32(B.tmid, peer);

    app_up(&A, "HELLO FROM A");
    run_ms(500);
    TEST_ASSERT_EQUAL_UINT8(12, B.app_n);
    TEST_ASSERT_EQUAL_MEMORY("HELLO FROM A", B.app, 12);
    TEST_ASSERT_TRUE(memcmp(A.air + 2, "HELLO FROM A", 12) != 0); /* encrypted on the air */
    app_up(&B, "HI");
    run_ms(500);
    TEST_ASSERT_EQUAL_UINT8(2, A.app_n);
    TEST_ASSERT_EQUAL_MEMORY("HI", A.app, 2);

    c = LC_SIG_CMD_HANGUP;
    cmd(&A, (const char *)&c, 1);
    run_ms(3000);
    const uint8_t *end = event(&B, LC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_NORMAL, end[5]);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&B.t));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_local_peer(&N, A.tmid, &peer));
}

static void test_local_call_rejected_busy_unreachable(void)
{
    world(LC_SIG_MODE_PART15);
    both_registered();
    a_calls_b();
    uint8_t c = LC_SIG_CMD_REJECT;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    const uint8_t *end = event(&A, LC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_REJECTED, end[5]);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&B.t));

    A.nev = 0;
    static const char self[] = "\x02+8836065551234"; /* A's own number: busy */
    cmd(&A, self, sizeof(self) - 1);
    run_ms(3000);
    end = event(&A, LC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_BUSY, end[5]);

    A.nev = B.nev = 0;
    subs[1].activated = 0; /* B's subscription no longer bound: unreachable */
    static const char dial[] = "\x02+8836065551235";
    cmd(&A, dial, sizeof(dial) - 1);
    run_ms(3000);
    end = event(&A, LC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_UNREACHABLE, end[5]);
    TEST_ASSERT_NULL(event(&B, LC_SIG_EV_INCOMING));
}

static void test_local_call_part97_in_clear(void)
{
    world(LC_SIG_MODE_PART97);
    both_registered();
    a_calls_b();
    uint8_t c = LC_SIG_CMD_ANSWER;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    app_up(&A, "CLEAR");
    run_ms(500);
    TEST_ASSERT_EQUAL_MEMORY("CLEAR", A.air + 2, 5); /* Part 97: in the clear */
    TEST_ASSERT_EQUAL_MEMORY("CLEAR", B.app, 5);
    c = LC_SIG_CMD_HANGUP;
    cmd(&B, (const char *)&c, 1); /* the callee hangs up this time */
    run_ms(3000);
    TEST_ASSERT_NOT_NULL(event(&A, LC_SIG_EV_ENDED));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&A.t));
}

/* Interaction with re-activation (Task 7): A's subscriber activates on a third
 * terminal mid-call. The network ends A's leg (NET_FAILURE, as for any call of
 * a terminal that loses its subscriber) and must release B's leg with the same
 * cause: B isn't left in a call with nobody, and nothing is relayed any more. */
static void test_reactivation_mid_local_call_ends_both_legs(void)
{
    const uint32_t tmid_c = 0x11223344u;
    world(LC_SIG_MODE_PART15);
    both_registered();
    a_calls_b();
    uint8_t c = LC_SIG_CMD_ANSWER;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&B.t));

    /* the operator re-issues A's number; a third terminal activates with it */
    memset(subs[0].token_id, 0xc0, 8);
    memset(subs[0].token_secret, 0xd0, 16);
    subs[0].token_used = 0;
    subs[0].token_expiry = 1790000000u + 3600u;
    lc_sig_ident_t idc;
    uint8_t r[32];
    memset(r, 0x99, 32);
    lc_sig_ident_new(&idc, r);
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_ACT_REQ;
    memcpy(m.u.act_req.token_id, subs[0].token_id, 8);
    memcpy(m.u.act_req.pkt, idc.pk, 32);
    lc_sig_act_tag(subs[0].token_secret, tmid_c, idc.pk, subs[0].token_id, m.u.act_req.tag);
    lc_sig_sec_t sec;
    lc_sig_sec_init(&sec, 0);
    uint8_t buf[LC_SIG_MAX_MSG];
    size_t len = lc_sig_seal(&sec, &m, buf, sizeof(buf));
    uint8_t frag[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], flen[LC_SIG_MAX_FRAGS];
    uint8_t nf = lc_sig_fragment(buf, len, 0, frag, flen);
    for (uint8_t i = 0; i < nf; i++) lc_sig_net_rx(&N, tmid_c, frag[i], flen[i], now);
    TEST_ASSERT_EQUAL_HEX32(tmid_c, subs[0].tmid);

    uint32_t peer = 0;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_local_peer(&N, A.tmid, &peer));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_local_peer(&N, B.tmid, &peer));
    run_ms(3000);
    const uint8_t *end = event(&B, LC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_NET_FAILURE, end[5]);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&B.t));
    int ended = 0; /* the switch saw both legs end: 1 = A's, 2 = B's */
    for (int i = 0; i < ncalls && i < 16; i++) {
        if (calls[i].what == LC_SIG_NET_ENDED && calls[i].cause == LC_SIG_CAUSE_NET_FAILURE) {
            ended |= calls[i].tmid == A.tmid ? 1 : calls[i].tmid == B.tmid ? 2 : 0;
        }
    }
    TEST_ASSERT_EQUAL_INT(3, ended);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_local_call_connects_and_carries_data_part15);
    RUN_TEST(test_local_call_rejected_busy_unreachable);
    RUN_TEST(test_local_call_part97_in_clear);
    RUN_TEST(test_reactivation_mid_local_call_ends_both_legs);
    return UNITY_END();
}
