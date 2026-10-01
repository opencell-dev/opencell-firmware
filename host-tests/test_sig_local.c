/* Terminal-to-terminal calls: two terminal roles and the network role over a
 * fake link (one UL and one DL payload per 120 ms frame each, always granted).
 * The network switches the call itself and relays app data between the legs,
 * as ocbench net does. */
#include "unity.h"

#include <string.h>

#include "oc_sig_crypto.h"
#include "oc_sig_keys.h"
#include "oc_sig_net.h"
#include "oc_sig_term.h"
#include "sig_fake_core.h"

void setUp(void) {}
void tearDown(void) {}

#define FRAME 120000u

typedef struct { uint8_t p[32][OC_SIG_LINK_MAX], n[32]; int head, count; } q_t;

typedef struct {
    uint32_t       tmid;
    oc_sig_term_t  t;
    oc_sig_ident_t id;
    q_t            ul, dl;
    uint8_t        ev[32][16];
    int            nev;
    uint8_t        app[OC_SIG_APP_MAX], app_n;
    uint8_t        air[OC_SIG_LINK_MAX], air_n; /* the last app data frame it sent, as on the air */
} term_t;

static term_t A, B;
static oc_sig_net_t N;
static oc_sig_sub_t subs[2];
static int nsubs = 2;
static oc_sig_net_call_ev_t calls[16];
static int ncalls;
static uint64_t now;
static uint64_t clock_now(void) { return now; }

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

/* network io: a two-subscriber fake core */
static int n_send(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    term_t *t = by_tmid_t(tmid);
    return t != NULL ? qpush(&t->dl, p, n) : -1;
}
/* A call the network can't switch itself goes to the far end, where no
 * number is reachable in this test: the far end releases it at once. */
static void n_call(void *c, const oc_sig_net_call_ev_t *e)
{
    (void)c;
    calls[ncalls++ % 16] = *e;
    if (e->what == OC_SIG_NET_MO) oc_sig_net_peer_release(&N, e->call_id, OC_SIG_CAUSE_UNREACHABLE, now);
}
static const oc_sig_net_io_t net_io = { NULL, fc_act_req, fc_av_req, fc_resync_req, NULL, NULL, n_send,
                                        NULL, n_call, NULL };

/* terminal io (ctx = term_t) */
static int t_send(void *c, const uint8_t *p, uint8_t n) { return qpush(&((term_t *)c)->ul, p, n); }
static int t_svc(void *c, uint8_t cause) { (void)c; (void)cause; return 0; }
static void t_event(void *c, const uint8_t *e, uint8_t n)
{
    term_t *t = c;
    memcpy(t->ev[t->nev % 32], e, n);
    t->nev++;
}

static void make(term_t *t, oc_sig_sub_t *s, uint32_t tmid, const char *number, uint8_t kbyte)
{
    memset(t, 0, sizeof(*t));
    t->tmid = tmid;
    uint8_t r[32];
    memset(r, kbyte, 32);
    oc_sig_ident_new(&t->id, r);
    t->id.activated = 1;
    t->id.key_id = 1;
    memset(t->id.k, kbyte, 16);
    memset(t->id.opc, (uint8_t)(kbyte + 1u), 16);
    oc_sig_number_to_bcd(number, strlen(number), t->id.number);
    memset(s, 0, sizeof(*s));
    memcpy(s->number, t->id.number, OC_SIG_NUMBER_LEN);
    memcpy(s->k, t->id.k, 16);
    memcpy(s->opc, t->id.opc, 16);
    s->tmid = tmid;
    s->activated = 1;
    s->token_used = 1;
    const oc_sig_term_io_t io = { t, t_send, t_svc, NULL, t_event };
    oc_sig_term_init(&t->t, &io, &t->id, tmid, 0);
}

static void world(uint8_t mode)
{
    now = 0;
    ncalls = 0;
    uint8_t sk[32];
    memset(sk, 0x11, 32);
    oc_sig_net_cfg_t cfg = { mode, 1800 };
    oc_sig_net_init(&N, &net_io, &cfg);
    fc_init(&N, subs, &nsubs, sk, 1790000000u, clock_now);
    make(&A, &subs[0], 0x76ad0488u, "+883160655501234", 0x21);
    make(&B, &subs[1], 0x76ae1ae8u, "+883160655501235", 0x31);
}

static void frame(void)
{
    term_t *ts[2] = { &A, &B };
    uint8_t p[OC_SIG_LINK_MAX], n;
    now += FRAME;
    for (int i = 0; i < 2; i++) {
        oc_sig_term_link(&ts[i]->t, 1, 1, now);
        oc_sig_net_link(&N, ts[i]->tmid, 1, now);
        oc_sig_term_tick(&ts[i]->t, now);
    }
    oc_sig_net_tick(&N, now);
    for (int i = 0; i < 2; i++) {
        term_t *t = ts[i];
        oc_sig_net_heard(&N, t->tmid, now);
        if (qpop(&t->ul, p, &n) == 0) {
            if ((p[0] & 0xF0u) == OC_SIG_KIND_SIG) {
                oc_sig_net_rx(&N, t->tmid, p, n, now);
            } else if (p[0] == OC_SIG_KIND_DATA) { /* as ocb_net: to the other leg, else echo */
                uint8_t d[OC_SIG_APP_MAX], dn, out[OC_SIG_LINK_MAX], on;
                uint32_t to = t->tmid;
                oc_sig_net_local_peer(&N, t->tmid, &to);
                if (oc_sig_net_data_in(&N, t->tmid, p, n, d, &dn) == 0 &&
                    oc_sig_net_data_out(&N, to, d, dn, out, &on) == 0) {
                    qpush(&by_tmid_t(to)->dl, out, on);
                }
            }
        }
        if (qpop(&t->dl, p, &n) == 0) {
            if ((p[0] & 0xF0u) == OC_SIG_KIND_SIG) {
                oc_sig_term_rx(&t->t, p, n, now);
            } else if (p[0] == OC_SIG_KIND_DATA) {
                oc_sig_term_data_in(&t->t, p, n, t->app, &t->app_n);
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
    TEST_ASSERT_EQUAL_UINT8(0, oc_sig_term_command(&t->t, (const uint8_t *)c, n, now));
}

static void app_up(term_t *t, const char *s)
{
    uint8_t out[OC_SIG_LINK_MAX], on;
    TEST_ASSERT_EQUAL_INT(0, oc_sig_term_data_out(&t->t, (const uint8_t *)s, (uint8_t)strlen(s), out, &on));
    memcpy(t->air, out, on);
    t->air_n = on;
    qpush(&t->ul, out, on);
}

static void both_registered(void)
{
    run_ms(5000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&B.t));
}

static void a_calls_b(void)
{
    static const char dial[] = "\x02" "606-555-1235"; /* in-country, the subscriber's 0 left out */
    cmd(&A, dial, sizeof(dial) - 1);
    run_ms(3000);
    const uint8_t *in = event(&B, OC_SIG_EV_INCOMING);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(A.id.number, in + 5, OC_SIG_NUMBER_LEN); /* caller id is A's number */
    TEST_ASSERT_NOT_NULL(event(&A, OC_SIG_EV_RINGING));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_RINGING_OUT, oc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_RINGING_IN, oc_sig_term_state(&B.t));
}

static void test_local_call_connects_and_carries_data_part15(void)
{
    world(OC_SIG_MODE_PART15);
    both_registered();
    a_calls_b();
    TEST_ASSERT_EQUAL_INT(OC_SIG_NET_LOCAL, calls[0].what);
    TEST_ASSERT_EQUAL_HEX32(B.tmid, calls[0].peer_tmid);
    uint8_t c = OC_SIG_CMD_ANSWER;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_IN_CALL, oc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_IN_CALL, oc_sig_term_state(&B.t));
    uint32_t peer = 0;
    TEST_ASSERT_EQUAL_INT(1, oc_sig_net_local_peer(&N, A.tmid, &peer));
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

    c = OC_SIG_CMD_HANGUP;
    cmd(&A, (const char *)&c, 1);
    run_ms(3000);
    const uint8_t *end = event(&B, OC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_CAUSE_NORMAL, end[5]);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&B.t));
    TEST_ASSERT_EQUAL_INT(0, oc_sig_net_local_peer(&N, A.tmid, &peer));
}

/* Media gate (core-test-services spec §14 F1): while a local call rings,
 * neither terminal sends app data, and a data frame on either leg's uplink
 * (an old terminal's clear frame, or a forged one) is not relayed: the
 * network sends nothing on a leg before it is active. */
static void test_local_call_no_data_before_connected_part15(void)
{
    world(OC_SIG_MODE_PART15);
    both_registered();
    a_calls_b();
    uint8_t out[OC_SIG_LINK_MAX], on;
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_NOT_NOW, oc_sig_term_data_out(&A.t, (const uint8_t *)"EARLY-A", 7, out, &on));
    TEST_ASSERT_EQUAL_INT(OC_SIG_ATT_NOT_NOW, oc_sig_term_data_out(&B.t, (const uint8_t *)"EARLY-B", 7, out, &on));
    static const uint8_t clear[] = { OC_SIG_KIND_DATA, 0, 'E', 'A', 'R', 'L', 'Y' };
    qpush(&A.ul, clear, sizeof(clear));
    qpush(&B.ul, clear, sizeof(clear));
    TEST_ASSERT_EQUAL_INT(-1, oc_sig_net_data_out(&N, A.tmid, (const uint8_t *)"EARLY", 5, out, &on));
    TEST_ASSERT_EQUAL_INT(-1, oc_sig_net_data_out(&N, B.tmid, (const uint8_t *)"EARLY", 5, out, &on));
    run_ms(500);
    TEST_ASSERT_EQUAL_UINT8(0, A.app_n);
    TEST_ASSERT_EQUAL_UINT8(0, B.app_n);

    uint8_t c = OC_SIG_CMD_ANSWER;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_IN_CALL, oc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_IN_CALL, oc_sig_term_state(&B.t));
    app_up(&A, "AFTER");
    run_ms(500);
    TEST_ASSERT_EQUAL_UINT8(5, B.app_n);
    TEST_ASSERT_EQUAL_MEMORY("AFTER", B.app, 5);
}

static void test_local_call_rejected_busy_unreachable(void)
{
    world(OC_SIG_MODE_PART15);
    both_registered();
    a_calls_b();
    uint8_t c = OC_SIG_CMD_REJECT;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    const uint8_t *end = event(&A, OC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_CAUSE_REJECTED, end[5]);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&A.t));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&B.t));

    A.nev = 0;
    static const char self[] = "\x02+883160655501234"; /* A's own number: busy */
    cmd(&A, self, sizeof(self) - 1);
    run_ms(3000);
    end = event(&A, OC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_CAUSE_BUSY, end[5]);

    A.nev = B.nev = 0;
    oc_sig_net_drop(&N, B.tmid, OC_SIG_CAUSE_NET_FAILURE, now); /* the core cancelled B here: unreachable */
    static const char dial[] = "\x02" "+883-1-606-555-01235";
    cmd(&A, dial, sizeof(dial) - 1);
    run_ms(3000);
    end = event(&A, OC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_CAUSE_UNREACHABLE, end[5]);
    TEST_ASSERT_NULL(event(&B, OC_SIG_EV_INCOMING));
}

static void test_local_call_part97_in_clear(void)
{
    world(OC_SIG_MODE_PART97);
    both_registered();
    a_calls_b();
    uint8_t c = OC_SIG_CMD_ANSWER;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    app_up(&A, "CLEAR");
    run_ms(500);
    TEST_ASSERT_EQUAL_MEMORY("CLEAR", A.air + 2, 5); /* Part 97: in the clear */
    TEST_ASSERT_EQUAL_MEMORY("CLEAR", B.app, 5);
    c = OC_SIG_CMD_HANGUP;
    cmd(&B, (const char *)&c, 1); /* the callee hangs up this time */
    run_ms(3000);
    TEST_ASSERT_NOT_NULL(event(&A, OC_SIG_EV_ENDED));
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&A.t));
}

/* Interaction with re-activation (Task 7): A's subscriber activates on a third
 * terminal mid-call. The network ends A's leg (NET_FAILURE, as for any call of
 * a terminal that loses its subscriber) and must release B's leg with the same
 * cause: B isn't left in a call with nobody, and nothing is relayed any more. */
static void test_reactivation_mid_local_call_ends_both_legs(void)
{
    const uint32_t tmid_c = 0x11223344u;
    world(OC_SIG_MODE_PART15);
    both_registered();
    a_calls_b();
    uint8_t c = OC_SIG_CMD_ANSWER;
    cmd(&B, (const char *)&c, 1);
    run_ms(3000);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_IN_CALL, oc_sig_term_state(&B.t));

    /* the operator re-issues A's number; a third terminal activates with it */
    memset(subs[0].token_id, 0xc0, 8);
    memset(subs[0].token_secret, 0xd0, 16);
    subs[0].token_used = 0;
    subs[0].token_expiry = 1790000000u + 3600u;
    oc_sig_ident_t idc;
    uint8_t r[32];
    memset(r, 0x99, 32);
    oc_sig_ident_new(&idc, r);
    oc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = OC_SIG_ACT_REQ;
    memcpy(m.u.act_req.token_id, subs[0].token_id, 8);
    memcpy(m.u.act_req.pkt, idc.pk, 32);
    oc_sig_act_tag(subs[0].token_secret, tmid_c, idc.pk, subs[0].token_id, m.u.act_req.tag);
    oc_sig_sec_t sec;
    oc_sig_sec_init(&sec, 0);
    uint8_t buf[OC_SIG_MAX_MSG];
    size_t len = oc_sig_seal(&sec, &m, buf, sizeof(buf));
    uint8_t frag[OC_SIG_MAX_FRAGS][OC_SIG_LINK_MAX], flen[OC_SIG_MAX_FRAGS];
    uint8_t nf = oc_sig_fragment(buf, len, 0, frag, flen);
    for (uint8_t i = 0; i < nf; i++) oc_sig_net_rx(&N, tmid_c, frag[i], flen[i], now);
    TEST_ASSERT_EQUAL_HEX32(tmid_c, subs[0].tmid);

    uint32_t peer = 0;
    TEST_ASSERT_EQUAL_INT(0, oc_sig_net_local_peer(&N, A.tmid, &peer));
    TEST_ASSERT_EQUAL_INT(0, oc_sig_net_local_peer(&N, B.tmid, &peer));
    run_ms(3000);
    const uint8_t *end = event(&B, OC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_CAUSE_NET_FAILURE, end[5]);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&B.t));
    /* final review P3: A's (old) terminal is told too, not left IN_CALL */
    const uint8_t *end_a = event(&A, OC_SIG_EV_ENDED);
    TEST_ASSERT_NOT_NULL(end_a);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_CAUSE_NET_FAILURE, end_a[5]);
    TEST_ASSERT_EQUAL_UINT8(OC_SIG_ST_REGISTERED, oc_sig_term_state(&A.t));
    int ended = 0; /* the switch saw both legs end: 1 = A's, 2 = B's */
    for (int i = 0; i < ncalls && i < 16; i++) {
        if (calls[i].what == OC_SIG_NET_ENDED && calls[i].cause == OC_SIG_CAUSE_NET_FAILURE) {
            ended |= calls[i].tmid == A.tmid ? 1 : calls[i].tmid == B.tmid ? 2 : 0;
        }
    }
    TEST_ASSERT_EQUAL_INT(3, ended);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_local_call_connects_and_carries_data_part15);
    RUN_TEST(test_local_call_no_data_before_connected_part15);
    RUN_TEST(test_local_call_rejected_busy_unreachable);
    RUN_TEST(test_local_call_part97_in_clear);
    RUN_TEST(test_reactivation_mid_local_call_ends_both_legs);
    return UNITY_END();
}
