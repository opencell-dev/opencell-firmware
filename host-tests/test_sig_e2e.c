/* Terminal and network roles over a fake link: one UL and one DL payload
 * per 120 ms frame, optional loss, channel grants that follow the network's
 * requests after 3 frames (like a page and a grant). */
#include "unity.h"

#include <string.h>

#include "lc_sig_crypto.h"
#include "lc_sig_keys.h"
#include "lc_sig_net.h"
#include "lc_sig_term.h"

void setUp(void) {}
void tearDown(void) {}

#define TMID  0x76ad0488u
#define TMID2 0x11223344u
#define FRAME 120000u

static lc_sig_net_t N;
static lc_sig_term_t T;
static lc_sig_ident_t ID;
static lc_sig_sub_t subs[4];
static int nsubs, saves;
static uint64_t now;
static uint32_t unix_s = 1790000000u;
static uint32_t rng = 1;
static unsigned loss_pct;

/* fake air: queues of link payloads */
typedef struct { uint8_t p[32][LC_SIG_LINK_MAX], n[32]; int head, count; } q_t;
static q_t ulq, dlq;
static int granted, grant_pending;
static uint64_t grant_at;

static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static int lost(void) { return loss_pct != 0 && rnd() % 100u < loss_pct; }
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

/* HSS */
static lc_sig_sub_t *by_token(void *c, const uint8_t tok[8])
{
    (void)c;
    for (int i = 0; i < nsubs; i++) if (memcmp(subs[i].token_id, tok, 8) == 0) return &subs[i];
    return NULL;
}
static lc_sig_sub_t *by_tmid(void *c, uint32_t tmid)
{
    (void)c;
    for (int i = 0; i < nsubs; i++) if (subs[i].activated && subs[i].tmid == tmid) return &subs[i];
    return NULL;
}
static lc_sig_sub_t *by_number(void *c, const uint8_t num[7])
{
    (void)c;
    for (int i = 0; i < nsubs; i++) if (memcmp(subs[i].number, num, 7) == 0) return &subs[i];
    return NULL;
}
static void unbind(void *c, uint32_t tmid)
{
    (void)c;
    for (int i = 0; i < nsubs; i++) if (subs[i].tmid == tmid) { subs[i].tmid = 0; subs[i].activated = 0; }
}
static void hss_save(void *c) { (void)c; saves++; }
/* Test-only hook for Review Focus 4: while set, drop every fragment of the
 * next DL signalling message (clearing itself once the fragment carrying the
 * last-fragment bit has been dropped), to deterministically lose exactly one
 * whole message rather than a random one. */
static int dl_drop_msg;
static int net_send(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    (void)tmid;
    if (dl_drop_msg && (p[0] & 0xF0u) == LC_SIG_KIND_SIG) {
        if (p[0] & 0x02u) dl_drop_msg = 0; /* that was the last fragment of the message */
        return 0; /* the network sent it; the air dropped it */
    }
    return qpush(&dlq, p, n);
}
static void net_channel(void *c, uint32_t tmid, int on)
{
    (void)c;
    (void)tmid;
    if (on && !granted && !grant_pending) { grant_pending = 1; grant_at = now + 3u * FRAME; }
    if (!on) { granted = 0; grant_pending = 0; }
}
static lc_sig_net_call_ev_t calls[16];
static int ncalls;
static void net_call(void *c, const lc_sig_net_call_ev_t *e) { (void)c; calls[ncalls++ % 16] = *e; }
static void net_random(void *c, uint8_t *out, size_t n) { (void)c; for (size_t i = 0; i < n; i++) out[i] = (uint8_t)rnd(); }
static uint32_t net_unix(void *c) { (void)c; return unix_s; }
static const lc_sig_net_io_t net_io = { NULL, by_token, by_tmid, by_number, unbind, hss_save, net_send,
                                        net_channel, net_call, net_random, net_unix, NULL };

/* terminal io */
static int term_send(void *c, const uint8_t *p, uint8_t n) { (void)c; return qpush(&ulq, p, n); }
static int term_svc(void *c, uint8_t cause)
{
    (void)c;
    if (!lost()) lc_sig_net_service_req(&N, TMID, cause, now);
    return 0;
}
static void term_save(void *c, const lc_sig_ident_t *i) { (void)c; (void)i; }
static uint8_t evs[64][16];
static int nevs;
static uint8_t reg_mode; /* mode byte of the last REGISTERED event */
static void term_event(void *c, const uint8_t *e, uint8_t n)
{
    (void)c;
    memcpy(evs[nevs % 64], e, n);
    nevs++;
    if (e[0] == LC_SIG_EV_REGISTERED) reg_mode = e[8];
}
static const lc_sig_term_io_t term_io = { NULL, term_send, term_svc, term_save, term_event };

static uint8_t SKN[32];
static lc_sig_qr_t QR;

static void world(uint8_t mode, uint16_t period_s)
{
    memset(&ulq, 0, sizeof(ulq));
    memset(&dlq, 0, sizeof(dlq));
    memset(subs, 0, sizeof(subs));
    nsubs = saves = ncalls = nevs = 0;
    memset(evs, 0, sizeof(evs));
    reg_mode = 0;
    now = 0;
    loss_pct = 0;
    dl_drop_msg = 0;
    granted = 1; /* the cell grants on attach */
    grant_pending = 0;
    memset(SKN, 0x11, 32);
    lc_sig_net_cfg_t cfg = { 1, { 0 }, mode, period_s };
    memcpy(cfg.sk, SKN, 32);
    lc_sig_net_init(&N, &net_io, &cfg);
    /* one subscriber with a fresh token, and its QR */
    lc_sig_sub_t *s = &subs[nsubs++];
    lc_sig_number_to_bcd("+8836065551234", 14, s->number);
    memset(s->token_id, 0xa0, 8);
    memset(s->token_secret, 0xb0, 16);
    s->token_expiry = unix_s + 3600u;
    memset(&QR, 0, sizeof(QR));
    QR.key_id = 1;
    lc_sig_x25519_public(SKN, QR.pkn);
    memcpy(QR.token_id, s->token_id, 8);
    memcpy(QR.token_secret, s->token_secret, 16);
    memcpy(QR.number, s->number, 7);
    uint8_t r[32];
    memset(r, 0x42, 32);
    lc_sig_ident_new(&ID, r);
    lc_sig_term_init(&T, &term_io, &ID, TMID, 0);
}

static void frame(void)
{
    uint8_t p[LC_SIG_LINK_MAX], n;
    now += FRAME;
    if (grant_pending && now >= grant_at) { granted = 1; grant_pending = 0; }
    lc_sig_term_link(&T, 1, granted, now);
    lc_sig_net_link(&N, TMID, granted, now);
    lc_sig_term_tick(&T, now);
    lc_sig_net_tick(&N, now);
    if (granted) {
        if (qpop(&ulq, p, &n) == 0 && !lost()) {
            if ((p[0] & 0xF0u) == LC_SIG_KIND_SIG) lc_sig_net_rx(&N, TMID, p, n, now);
        }
        if (!lost()) lc_sig_net_heard(&N, TMID, now); /* the UL slot was heard */
        if (qpop(&dlq, p, &n) == 0 && !lost()) {
            if ((p[0] & 0xF0u) == LC_SIG_KIND_SIG) lc_sig_term_rx(&T, p, n, now);
        }
    }
}

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += FRAME / 1000u) frame();
}

static int has_event(uint8_t code)
{
    for (int i = 0; i < nevs && i < 64; i++) if (evs[i][0] == code) return 1;
    return 0;
}

static void command(const char *s, size_t n)
{
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&T, (const uint8_t *)s, n, now));
}

static void activate(void)
{
    uint8_t cmd[1 + LC_SIG_QR_TEXT + 1];
    cmd[0] = LC_SIG_CMD_ACTIVATE;
    size_t n = lc_sig_qr_format(&QR, (char *)cmd + 1, sizeof(cmd) - 1);
    TEST_ASSERT_EQUAL_UINT8(0, lc_sig_term_command(&T, cmd, 1 + n, now));
}

/* Build and deliver a REG_REQ exactly as a terminal would (prot 0, no keys
 * needed): REG_REQ travels in the clear, so anyone can send one for a TMID
 * they don't own (Review Focus 1). */
static void inject_forged_reg_req(uint8_t seq, uint64_t at)
{
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_REG_REQ;
    m.u.reg_req.sw_version[0] = 0;
    m.u.reg_req.sw_version[1] = 5;
    m.u.reg_req.sw_version[2] = 0;
    m.u.reg_req.caps = 1;
    lc_sig_sec_t sec;
    lc_sig_sec_init(&sec, 0);
    uint8_t buf[LC_SIG_MAX_MSG];
    size_t n = lc_sig_seal(&sec, &m, buf, sizeof(buf));
    uint8_t frag[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], flen[LC_SIG_MAX_FRAGS];
    uint8_t nf = lc_sig_fragment(buf, n, seq, frag, flen);
    for (uint8_t i = 0; i < nf; i++) lc_sig_net_rx(&N, TMID, frag[i], flen[i], at);
}

/* Build and deliver an ACT_REQ exactly as a terminal activating on `tmid`
 * would, from a key pair of its own (Review Focus 2: re-activation on a
 * second terminal). */
static void activate_direct(uint32_t tmid, const lc_sig_qr_t *qr, uint64_t at)
{
    lc_sig_ident_t idb;
    uint8_t r[32];
    memset(r, 0x99, 32);
    lc_sig_ident_new(&idb, r);
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_ACT_REQ;
    memcpy(m.u.act_req.token_id, qr->token_id, 8);
    memcpy(m.u.act_req.pkt, idb.pk, 32);
    lc_sig_act_tag(qr->token_secret, tmid, idb.pk, qr->token_id, m.u.act_req.tag);
    lc_sig_sec_t sec;
    lc_sig_sec_init(&sec, 0);
    uint8_t buf[LC_SIG_MAX_MSG];
    size_t n = lc_sig_seal(&sec, &m, buf, sizeof(buf));
    uint8_t frag[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], flen[LC_SIG_MAX_FRAGS];
    uint8_t nf = lc_sig_fragment(buf, n, 0, frag, flen);
    for (uint8_t i = 0; i < nf; i++) lc_sig_net_rx(&N, tmid, frag[i], flen[i], at);
}

static void test_activation_then_registration_part15(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    activate();
    run_ms(10000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ACTIVATED));
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_TRUE(subs[0].activated);
    TEST_ASSERT_EQUAL_UINT32(TMID, subs[0].tmid);
    TEST_ASSERT_TRUE(subs[0].token_used);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(subs[0].k, ID.k, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(subs[0].sqn, ID.sqn, 6);
    TEST_ASSERT_TRUE(lc_sig_net_registered(&N, TMID));
    TEST_ASSERT_EQUAL_INT(1, T.ch.sec.encrypt);
}

static void test_token_reuse_expired_unknown_tampered(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    subs[0].token_used = 1;
    activate();
    run_ms(8000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ACT_FAILED));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_USED, evs[0][1]);

    world(LC_SIG_MODE_PART15, 1800);
    subs[0].token_expiry = unix_s - 1u;
    activate();
    run_ms(8000);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_EXPIRED, evs[0][1]);

    world(LC_SIG_MODE_PART15, 1800);
    QR.token_id[0] ^= 1; /* no such token */
    activate();
    run_ms(8000);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_UNKNOWN, evs[0][1]);

    world(LC_SIG_MODE_PART15, 1800);
    QR.token_secret[0] ^= 1; /* someone guessing without the real QR */
    activate();
    run_ms(8000);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_BAD_TAG, evs[0][1]);
    TEST_ASSERT_FALSE(subs[0].activated);
    TEST_ASSERT_FALSE(subs[0].token_used);
}

static void registered_world(uint8_t mode)
{
    world(mode, 1800);
    activate();
    run_ms(10000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    nevs = 0;
    ncalls = 0;
}

static void test_mo_call_answered_voice_and_hangup(void)
{
    registered_world(LC_SIG_MODE_PART15);
    command("\x02+8836065550100", 15);
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_NET_MO, calls[0].what);
    uint32_t cid = calls[0].call_id;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_peer_alert(&N, cid, now));
    run_ms(1000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_RINGING_OUT, lc_sig_term_state(&T));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_peer_answer(&N, cid, now));
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&T));
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_CONNECTED));

    /* voice both ways, encrypted on air */
    uint8_t air[LC_SIG_LINK_MAX], an, out[LC_SIG_APP_MAX], on;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_out(&T, (const uint8_t *)"VOICE-UP", 8, air, &an));
    TEST_ASSERT_FALSE(memcmp(air + 2, "VOICE-UP", 8) == 0);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_data_in(&N, TMID, air, an, out, &on));
    TEST_ASSERT_EQUAL_MEMORY("VOICE-UP", out, 8);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_data_out(&N, TMID, (const uint8_t *)"VOICE-DN", 8, air, &an));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_in(&T, air, an, out, &on));
    TEST_ASSERT_EQUAL_MEMORY("VOICE-DN", out, 8);

    command("\x05", 1); /* HANGUP */
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_NET_ENDED, calls[ncalls - 1].what);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_NORMAL, calls[ncalls - 1].cause);
}

static void test_mt_call_answer_then_reject(void)
{
    registered_world(LC_SIG_MODE_PART15);
    uint8_t caller[7];
    uint32_t cid;
    lc_sig_number_to_bcd("+8836065550100", 14, caller);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_call_in(&N, subs[0].number, caller, now, &cid));
    run_ms(2000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_INCOMING));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_RINGING_IN, lc_sig_term_state(&T));
    command("\x03", 1); /* ANSWER */
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&T));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_NET_ANSWERED, calls[ncalls - 1].what);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_peer_release(&N, cid, LC_SIG_CAUSE_NORMAL, now)); /* the far end hangs up */
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ENDED));

    nevs = 0;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_call_in(&N, subs[0].number, caller, now, &cid));
    run_ms(2000);
    command("\x04", 1); /* REJECT */
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_NET_ENDED, calls[ncalls - 1].what);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CAUSE_REJECTED, calls[ncalls - 1].cause);
}

static void test_part97_integrity_only(void)
{
    registered_world(LC_SIG_MODE_PART97);
    TEST_ASSERT_EQUAL_INT(0, T.ch.sec.encrypt);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_MODE_PART97, reg_mode);
    granted = 1; /* the idle channel was released: give it back so the setup goes out at once */
    lc_sig_term_link(&T, 1, 1, now);
    lc_sig_net_link(&N, TMID, 1, now);
    command("\x02+8836065550100", 15);
    uint8_t first[LC_SIG_LINK_MAX], n;
    lc_sig_term_tick(&T, now);
    TEST_ASSERT_EQUAL_INT(0, qpop(&ulq, first, &n));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_CALL_SETUP, first[2]);
    TEST_ASSERT_EQUAL_HEX8(1, first[3]);    /* prot 1 */
    TEST_ASSERT_EQUAL_HEX8(0x88, first[6]); /* the called number is readable */
}

static void test_sqn_resync(void)
{
    registered_world(LC_SIG_MODE_PART15);
    /* The terminal rebooted after the network lost state: the HSS is behind. */
    lc_sig_sqn_put(subs[0].sqn, 0);
    lc_sig_sqn_put(ID.sqn, 500);
    lc_sig_term_init(&T, &term_io, &ID, TMID, now);
    nevs = 0;
    run_ms(10000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    TEST_ASSERT_EQUAL_UINT64(501, lc_sig_sqn_get(ID.sqn));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(subs[0].sqn, ID.sqn, 6);
}

static void test_lossy_link_still_registers_and_calls(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    loss_pct = 20;
    activate();
    run_ms(60000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    /* a request answered from the cache must not have advanced the network's SQN */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(subs[0].sqn, ID.sqn, 6);
    command("\x02+8836065550100", 15);
    run_ms(10000);
    TEST_ASSERT_TRUE(ncalls >= 1);
    /* the run is deterministic (fixed seed, fixed loss pattern): exactly one
     * call was set up, not a duplicate from a retransmitted CALL_SETUP */
    TEST_ASSERT_EQUAL_UINT32(1, N.next_call_id);
    TEST_ASSERT_EQUAL_INT(1, ncalls);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_peer_answer(&N, calls[0].call_id, now));
    run_ms(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&T));
}

static void test_reregisters_after_period_and_channel_is_released(void)
{
    world(LC_SIG_MODE_PART15, 60);
    activate();
    run_ms(10000);
    TEST_ASSERT_EQUAL_INT(0, granted); /* idle: the network released the channel */
    nevs = 0;
    run_ms(60000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED)); /* asked for a channel and registered again */
    TEST_ASSERT_EQUAL_UINT64(2, lc_sig_sqn_get(ID.sqn));
}

static void test_data_in_duplicate_frame_rejected_without_moving_counter(void)
{
    registered_world(LC_SIG_MODE_PART15);
    command("\x02+8836065550100", 15);
    run_ms(2000);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_peer_answer(&N, calls[0].call_id, now));
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&T));

    uint8_t air[LC_SIG_LINK_MAX], an, out[LC_SIG_APP_MAX], on;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_out(&T, (const uint8_t *)"VOICE-01", 8, air, &an));
    uint8_t air_copy[LC_SIG_LINK_MAX];
    memcpy(air_copy, air, an);
    uint8_t an_copy = an;

    /* genuine frame decrypts fine */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_data_in(&N, TMID, air, an, out, &on));
    TEST_ASSERT_EQUAL_MEMORY("VOICE-01", out, 8);

    /* a duplicate of the same frame is rejected and must not move d_rx_next */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_net_data_in(&N, TMID, air_copy, an_copy, out, &on));

    /* the next genuine frame still decrypts correctly */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_out(&T, (const uint8_t *)"VOICE-02", 8, air, &an));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_data_in(&N, TMID, air, an, out, &on));
    TEST_ASSERT_EQUAL_MEMORY("VOICE-02", out, 8);
}

/* Review Focus 1: REG_REQ travels unauthenticated (prot 0). A forged one for
 * a registered TMID must not knock it off the network, and a second forged
 * one arriving while the network's own (unanswered) challenge is still
 * pending must not draw a second vector. Normal service must still work
 * afterwards. */
static void test_forged_reg_req_cannot_deregister_or_replay_auth(void)
{
    registered_world(LC_SIG_MODE_PART15);
    TEST_ASSERT_TRUE(lc_sig_net_registered(&N, TMID));
    uint8_t sqn0[6];
    memcpy(sqn0, subs[0].sqn, 6);

    inject_forged_reg_req(200, now);
    TEST_ASSERT_TRUE(lc_sig_net_registered(&N, TMID)); /* still registered: no AUTH_RSP has confirmed anything yet */
    uint8_t sqn1[6];
    memcpy(sqn1, subs[0].sqn, 6);
    TEST_ASSERT_FALSE(memcmp(sqn0, sqn1, 6) == 0); /* the network can't tell forged from real: it drew a vector */

    inject_forged_reg_req(201, now); /* a second forged REG_REQ while that vector is still pending */
    TEST_ASSERT_TRUE(lc_sig_net_registered(&N, TMID));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sqn1, subs[0].sqn, 6); /* no second vector, no second SQN advance */

    /* let the unanswered challenge time out, then prove a call still works */
    run_ms(6000);
    nevs = 0;
    ncalls = 0;
    command("\x02+8836065550100", 15);
    run_ms(3000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_NET_MO, calls[0].what);
}

/* Review Focus 2: activating the same subscriber on a second terminal must
 * cut the old terminal's session off, and the old terminal must no longer be
 * able to place calls. */
static void test_reactivation_deregisters_old_terminal(void)
{
    registered_world(LC_SIG_MODE_PART15);
    TEST_ASSERT_TRUE(lc_sig_net_registered(&N, TMID));

    /* the operator re-issues a token for the subscriber to activate elsewhere */
    memset(subs[0].token_id, 0xc0, 8);
    memset(subs[0].token_secret, 0xd0, 16);
    subs[0].token_used = 0;
    subs[0].token_expiry = unix_s + 3600u;
    lc_sig_qr_t qr2 = QR;
    memcpy(qr2.token_id, subs[0].token_id, 8);
    memcpy(qr2.token_secret, subs[0].token_secret, 16);

    activate_direct(TMID2, &qr2, now);
    TEST_ASSERT_TRUE(subs[0].activated);
    TEST_ASSERT_EQUAL_UINT32(TMID2, subs[0].tmid);
    TEST_ASSERT_FALSE(lc_sig_net_registered(&N, TMID)); /* the old terminal is cut off */

    nevs = 0;
    ncalls = 0;
    command("\x02+8836065550100", 15); /* the old terminal tries to dial */
    run_ms(3000);
    TEST_ASSERT_EQUAL_INT(0, ncalls);   /* refused: no MO event reached the switch */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T)); /* released back, not left hanging */
}

/* Review Focus 3: the session table (LC_SIG_NET_TERMS slots) must not fill
 * permanently with idle terminals that were merely heard once; a 5th,
 * genuine terminal must still be able to get a session. */
static void test_session_table_reclaims_lru_idle_slot(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    uint8_t dummy[1] = { 0x00 };
    for (uint32_t i = 0; i < 4; i++) {
        now += FRAME;
        lc_sig_net_rx(&N, 0xAAAA0000u + i, dummy, 1, now); /* just enough to touch a session */
    }
    activate();
    run_ms(10000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ACTIVATED));
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_TRUE(lc_sig_net_registered(&N, TMID));
}

/* Review Focus 4: drop exactly the network's first AUTH_REQ (every fragment
 * of it) and prove registration still completes by the channel's own
 * retransmit, with exactly one vector used (SQN 1) and the HSS save count
 * that one vector plus one activation produce. */
static void test_drops_first_dl_auth_req_then_recovers(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    activate();
    int armed = 0;
    for (int i = 0; i < 200 && !has_event(LC_SIG_EV_REGISTERED); i++) {
        frame();
        if (!armed && has_event(LC_SIG_EV_ACTIVATED)) {
            dl_drop_msg = 1; /* the very next DL signalling message is AUTH_REQ */
            armed = 1;
        }
    }
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_EQUAL_UINT64(1, lc_sig_sqn_get(ID.sqn));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(subs[0].sqn, ID.sqn, 6);
    /* one HSS save for the activation (on_act_req) plus one for the single
     * authentication vector (new_av, SQN 0 -> 1): the lost AUTH_REQ is
     * recovered by the channel's own retransmit of that same vector, not by
     * drawing a fresh one, so the save count stays 2. */
    TEST_ASSERT_EQUAL_INT(2, saves);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_activation_then_registration_part15);
    RUN_TEST(test_token_reuse_expired_unknown_tampered);
    RUN_TEST(test_mo_call_answered_voice_and_hangup);
    RUN_TEST(test_mt_call_answer_then_reject);
    RUN_TEST(test_part97_integrity_only);
    RUN_TEST(test_sqn_resync);
    RUN_TEST(test_lossy_link_still_registers_and_calls);
    RUN_TEST(test_reregisters_after_period_and_channel_is_released);
    RUN_TEST(test_data_in_duplicate_frame_rejected_without_moving_counter);
    RUN_TEST(test_forged_reg_req_cannot_deregister_or_replay_auth);
    RUN_TEST(test_reactivation_deregisters_old_terminal);
    RUN_TEST(test_session_table_reclaims_lru_idle_slot);
    RUN_TEST(test_drops_first_dl_auth_req_then_recovers);
    return UNITY_END();
}
