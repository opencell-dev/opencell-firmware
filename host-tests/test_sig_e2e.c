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
static lc_sig_sub_t *by_number(void *c, const uint8_t num[LC_SIG_NUMBER_LEN])
{
    (void)c;
    for (int i = 0; i < nsubs; i++) if (memcmp(subs[i].number, num, LC_SIG_NUMBER_LEN) == 0) return &subs[i];
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
static int dl_drop_all_sig; /* while set, every DL signalling fragment is lost */
static int net_send(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    (void)tmid;
    if (dl_drop_all_sig && (p[0] & 0xF0u) == LC_SIG_KIND_SIG) return 0;
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
static int alert_now; /* like lcb_net: the far end rings at once when an MO call is set up */
static void net_call(void *c, const lc_sig_net_call_ev_t *e)
{
    (void)c;
    calls[ncalls++ % 16] = *e;
    if (alert_now && e->what == LC_SIG_NET_MO) lc_sig_net_peer_alert(&N, e->call_id, now);
}
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
    if (e[0] == LC_SIG_EV_REGISTERED) reg_mode = e[1 + LC_SIG_NUMBER_LEN];
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
    dl_drop_all_sig = 0;
    alert_now = 0;
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
    memcpy(QR.number, s->number, LC_SIG_NUMBER_LEN);
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

/* Build and deliver an AUTH_FAIL exactly as a terminal would (prot 0, cause 1
 * needs no AUTS): used to make the network's own pending AUTH_REQ get a
 * reply right when the subscriber has gone unbound (fix round 2, Review
 * Focus 1c). */
static void inject_forged_auth_fail(uint32_t tmid, uint8_t seq, uint8_t cause, uint64_t at)
{
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_AUTH_FAIL;
    m.u.auth_fail.cause = cause;
    lc_sig_sec_t sec;
    lc_sig_sec_init(&sec, 0);
    uint8_t buf[LC_SIG_MAX_MSG];
    size_t n = lc_sig_seal(&sec, &m, buf, sizeof(buf));
    uint8_t frag[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], flen[LC_SIG_MAX_FRAGS];
    uint8_t nf = lc_sig_fragment(buf, n, seq, frag, flen);
    for (uint8_t i = 0; i < nf; i++) lc_sig_net_rx(&N, tmid, frag[i], flen[i], at);
}

/* Deliver an AUTH_RSP carrying `res` exactly as a terminal would (prot 0). */
static void inject_forged_auth_rsp(uint32_t tmid, uint8_t seq, const uint8_t res[8], uint64_t at)
{
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_AUTH_RSP;
    memcpy(m.u.auth_rsp.res, res, 8);
    lc_sig_sec_t sec;
    lc_sig_sec_init(&sec, 0);
    uint8_t buf[LC_SIG_MAX_MSG];
    size_t n = lc_sig_seal(&sec, &m, buf, sizeof(buf));
    uint8_t frag[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], flen[LC_SIG_MAX_FRAGS];
    uint8_t nf = lc_sig_fragment(buf, n, seq, frag, flen);
    for (uint8_t i = 0; i < nf; i++) lc_sig_net_rx(&N, tmid, frag[i], flen[i], at);
}

/* The network's session struct for TMID (direct access: same codebase, plain
 * struct - lets tests drive the channel-level retry/expiry state precisely
 * rather than guessing at millisecond timings). */
static lc_sig_net_sess_t *net_sess(uint32_t tmid)
{
    for (unsigned i = 0; i < LC_SIG_NET_TERMS; i++) {
        if (N.s[i].used && N.s[i].tmid == tmid) return &N.s[i];
    }
    return NULL;
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
    uint8_t caller[LC_SIG_NUMBER_LEN];
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

/* Fix round 2, Review Focus 1a: a forged REG_REQ while a vector is pending
 * must not accelerate some OTHER message's retries (here, an MT call's own
 * unanswered SETUP_IND). The call must survive well within its normal
 * ~4 s (1 s x 3 retries) give-up window. */
static void test_forged_reg_req_does_not_accelerate_unrelated_pending_request(void)
{
    registered_world(LC_SIG_MODE_PART15);
    lc_sig_net_link(&N, TMID, 1, now); /* an idle channel may have been released by now: re-grant it */
    uint8_t caller[LC_SIG_NUMBER_LEN];
    uint32_t cid;
    lc_sig_number_to_bcd("+8836065550100", 14, caller);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_call_in(&N, subs[0].number, caller, now, &cid));
    /* the terminal never sees or answers the SETUP_IND: only the network is
     * driven directly, so nothing pops dlq to it */
    nevs = 0;
    ncalls = 0;
    uint8_t seq = 210;
    for (int i = 0; i < 16; i++) { /* 16 x 120 ms = 1.92 s: well under ~4 s */
        now += FRAME;
        lc_sig_net_tick(&N, now);
        inject_forged_reg_req(seq++, now);
    }
    TEST_ASSERT_EQUAL_INT(0, ncalls); /* no ENDED event: the call is still alive */
}

/* Fix round 2, Review Focus 1b: once an AUTH_REQ's own retries genuinely
 * expire unanswered, a fresh REG_REQ (new seq) must still draw and send a
 * new authentication vector - not be wedged by the old one. */
static void test_reg_req_after_auth_req_expiry_gets_fresh_auth_req(void)
{
    registered_world(LC_SIG_MODE_PART15);
    lc_sig_net_link(&N, TMID, 1, now); /* an idle channel may have been released by now: re-grant it,
                                           so the retry/expiry clock actually runs */
    memset(&dlq, 0, sizeof(dlq));
    uint8_t sqn0[6];
    memcpy(sqn0, subs[0].sqn, 6);

    inject_forged_reg_req(230, now); /* draws vector #1, sends AUTH_REQ #1 */
    uint8_t sqn1[6];
    memcpy(sqn1, subs[0].sqn, 6);
    TEST_ASSERT_FALSE(memcmp(sqn0, sqn1, 6) == 0);

    /* let it retry (1 s apart) and fully expire, unanswered: nothing pops
     * dlq, so nothing else clears auth_pending here */
    for (int i = 0; i < 40; i++) { /* 40 x 120 ms = 4.8 s > the ~4 s window */
        now += FRAME;
        lc_sig_net_tick(&N, now);
    }

    memset(&dlq, 0, sizeof(dlq)); /* discard the expired retries */
    inject_forged_reg_req(231, now); /* a fresh REG_REQ: auth_pending is clear by now */
    uint8_t sqn2[6];
    memcpy(sqn2, subs[0].sqn, 6);
    TEST_ASSERT_FALSE(memcmp(sqn1, sqn2, 6) == 0); /* a fresh vector was drawn */

    uint8_t p[LC_SIG_LINK_MAX], n;
    TEST_ASSERT_EQUAL_INT(0, qpop(&dlq, p, &n)); /* the network actually sent it */
    TEST_ASSERT_TRUE((p[0] & 0xF0u) == LC_SIG_KIND_SIG);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_AUTH_REQ, p[2]);
    TEST_ASSERT_TRUE(lc_sig_net_registered(&N, TMID));
}

/* Fix round 2, Review Focus 1c: an AUTH_FAIL that answers the network's own
 * pending AUTH_REQ right as the subscriber goes unbound (sub == NULL) must
 * not wedge auth_pending forever. Once the subscriber is back, a fresh
 * REG_REQ (new seq, so it can't be answered by the channel's own dedup
 * resend of a stale cached reply) must draw and send a real new vector -
 * i.e. registration can actually complete from there. */
static void test_auth_fail_with_unbound_subscriber_does_not_wedge_registration(void)
{
    registered_world(LC_SIG_MODE_PART15);
    lc_sig_net_link(&N, TMID, 1, now); /* an idle channel may have been released by now: re-grant it */
    memset(&dlq, 0, sizeof(dlq));
    uint8_t sqn0[6];
    memcpy(sqn0, subs[0].sqn, 6);

    inject_forged_reg_req(240, now); /* draws vector #1: auth_pending = 1 */
    uint8_t sqn_mid[6];
    memcpy(sqn_mid, subs[0].sqn, 6);
    TEST_ASSERT_FALSE(memcmp(sqn0, sqn_mid, 6) == 0); /* sanity: vector #1 was drawn */
    TEST_ASSERT_TRUE(subs[0].activated);
    subs[0].activated = 0; /* the subscriber vanishes mid-negotiation (e.g. a concurrent deactivation) */
    inject_forged_auth_fail(TMID, 241, 1, now); /* answers the pending AUTH_REQ; sub == NULL now */

    subs[0].activated = 1; /* the subscriber is back (e.g. reactivated) */
    memset(&dlq, 0, sizeof(dlq));
    inject_forged_reg_req(242, now); /* a fresh REG_REQ, new seq: must not be wedged */
    uint8_t sqn1[6];
    memcpy(sqn1, subs[0].sqn, 6);
    TEST_ASSERT_FALSE(memcmp(sqn_mid, sqn1, 6) == 0); /* a SECOND fresh vector was drawn, not silently dropped */

    uint8_t p[LC_SIG_LINK_MAX], n;
    TEST_ASSERT_EQUAL_INT(0, qpop(&dlq, p, &n)); /* the network actually sent it */
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_AUTH_REQ, p[2]);
    TEST_ASSERT_TRUE(lc_sig_net_registered(&N, TMID)); /* an unauthenticated exchange never deregistered it */
}

/* Fix round 3, Review Focus 1b (the exhausted window): a REG_REQ that lands
 * exactly when the old AUTH_REQ's retries are exhausted but it hasn't
 * formally expired yet draws a fresh vector #2, which gets stuck in the
 * outq (the channel is still busy with #1). When #1 then genuinely expires,
 * the expiry handler must not wipe auth_pending out from under #2 - once #2
 * is flushed out and the terminal answers it correctly, that AUTH_RSP must
 * be accepted (REG_ACK queued, reg_until advanced), not dropped. */
static void test_reg_req_in_exhausted_window_still_accepts_correct_auth_rsp(void)
{
    registered_world(LC_SIG_MODE_PART15);
    lc_sig_net_link(&N, TMID, 1, now);
    memset(&dlq, 0, sizeof(dlq));

    inject_forged_reg_req(250, now); /* draws vector #1: auth_pending = 1, AUTH_REQ #1 sent */
    lc_sig_net_sess_t *sess = net_sess(TMID);
    TEST_ASSERT_NOT_NULL(sess);

    /* tick until AUTH_REQ #1's retries are exhausted but it hasn't expired
     * yet: pend_tries == LC_SIG_RETX_MAX and ch.pend still set */
    int i;
    for (i = 0; i < 100 && !(sess->ch.pend && sess->ch.pend_tries >= LC_SIG_RETX_MAX); i++) {
        now += FRAME;
        lc_sig_net_tick(&N, now);
    }
    TEST_ASSERT_TRUE(sess->ch.pend);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_RETX_MAX, sess->ch.pend_tries);

    /* a REG_REQ now: branch 3 (retries exhausted) fires - draws vector #2,
     * stuck in outq because the channel is still busy with #1 */
    memset(&dlq, 0, sizeof(dlq));
    inject_forged_reg_req(251, now);

    /* tick past AUTH_REQ #1's actual expiry: it clears, and #2 gets flushed */
    for (i = 0; i < 20; i++) {
        now += FRAME;
        lc_sig_net_tick(&N, now);
    }
    uint8_t p[LC_SIG_LINK_MAX], n;
    TEST_ASSERT_EQUAL_INT(0, qpop(&dlq, p, &n)); /* AUTH_REQ #2 actually went out */
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_AUTH_REQ, p[2]);

    /* the terminal answers vector #2's real challenge correctly */
    uint64_t reg_until_before = sess->reg_until;
    memset(&dlq, 0, sizeof(dlq));
    inject_forged_auth_rsp(TMID, 60, sess->p_xres, now);

    TEST_ASSERT_TRUE(sess->reg_until > reg_until_before); /* accepted: reg_until advanced */
    uint8_t p2[LC_SIG_LINK_MAX], n2;
    TEST_ASSERT_EQUAL_INT(0, qpop(&dlq, p2, &n2)); /* REG_ACK queued and sent */
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_REG_ACK, p2[2]);
}

/* Fix round 3, Review Focus 1b (regression guard): the acceleration branch
 * (retries left) must still just resend the very same message at once, not
 * draw a new vector. */
static void test_reg_req_with_retries_left_forces_resend_not_new_vector(void)
{
    registered_world(LC_SIG_MODE_PART15);
    lc_sig_net_link(&N, TMID, 1, now);
    memset(&dlq, 0, sizeof(dlq));

    inject_forged_reg_req(70, now); /* draws a vector: auth_pending = 1, AUTH_REQ sent, tries = 0 */
    lc_sig_net_sess_t *sess = net_sess(TMID);
    TEST_ASSERT_NOT_NULL(sess);
    TEST_ASSERT_TRUE(sess->ch.pend);
    uint8_t tries_before = sess->ch.pend_tries;
    uint8_t sqn_before[6];
    memcpy(sqn_before, subs[0].sqn, 6);
    int saves_before = saves;

    memset(&dlq, 0, sizeof(dlq)); /* discard AUTH_REQ #1's original send */
    inject_forged_reg_req(71, now); /* retries left: must resend at once, not draw a new vector */
    lc_sig_net_tick(&N, now);        /* pend_due was set to `now`: this fires the resend right away */

    TEST_ASSERT_EQUAL_UINT8((uint8_t)(tries_before + 1), sess->ch.pend_tries);
    uint8_t p[LC_SIG_LINK_MAX], n;
    TEST_ASSERT_EQUAL_INT(0, qpop(&dlq, p, &n)); /* the very same AUTH_REQ, retransmitted */
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_AUTH_REQ, p[2]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sqn_before, subs[0].sqn, 6); /* no new vector */
    TEST_ASSERT_EQUAL_INT(saves_before, saves);               /* no extra HSS save */
}

/* Final review C1: lcb_net alerts as soon as the MO call is set up, so
 * CALL_PROC and ALERTING leave back to back. With CALL_PROC lost, the
 * retransmitted CALL_SETUP must be answered with CALL_PROC (not ALERTING,
 * the last message sent), and the call must connect. */
static void test_lost_call_proc_with_immediate_alert_still_connects(void)
{
    registered_world(LC_SIG_MODE_PART15);
    alert_now = 1;
    command("\x02+8836065550100", 15);
    dl_drop_msg = 1; /* the next DL signalling message is CALL_PROC */
    run_ms(3000);
    TEST_ASSERT_EQUAL_INT(0, dl_drop_msg); /* the drop fired */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_NET_MO, calls[0].what);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_peer_answer(&N, calls[0].call_id, now));
    run_ms(3000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&T));
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_ENDED));
}

/* A connected call to the far end; returns its call id. */
static uint32_t connected_mo_call(void)
{
    command("\x02+8836065550100", 15);
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_NET_MO, calls[ncalls - 1].what);
    uint32_t cid = calls[ncalls - 1].call_id;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_peer_answer(&N, cid, now));
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&T));
    return cid;
}

static int net_ended(uint32_t cid)
{
    for (int i = 0; i < ncalls && i < 16; i++) {
        if (calls[i].what == LC_SIG_NET_ENDED && calls[i].call_id == cid) return 1;
    }
    return 0;
}

/* Final review C2: the terminal reboots mid-call and registers again. The
 * network's leg must not outlive it (the empty UL frames keep "heard"
 * fresh): registration ends it, and a new call works. */
static void test_reboot_mid_call_ends_network_leg_and_new_call_works(void)
{
    registered_world(LC_SIG_MODE_PART15);
    uint32_t cid = connected_mo_call();
    lc_sig_term_init(&T, &term_io, &ID, TMID, now); /* reboot: same identity, same TMID */
    nevs = 0;
    run_ms(10000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    TEST_ASSERT_TRUE(net_ended(cid));
    uint32_t cid2 = connected_mo_call();
    TEST_ASSERT_NOT_EQUAL(cid, cid2);
}

/* Final review C2: re-activating the TMID that holds a call (a rebooted
 * terminal scanning a new QR) ends that call in the network. */
static void test_reactivation_on_same_tmid_ends_its_call(void)
{
    registered_world(LC_SIG_MODE_PART15);
    uint32_t cid = connected_mo_call();
    memset(subs[0].token_id, 0xc0, 8);
    memset(subs[0].token_secret, 0xd0, 16);
    subs[0].token_used = 0;
    subs[0].token_expiry = unix_s + 3600u;
    lc_sig_qr_t qr2 = QR;
    memcpy(qr2.token_id, subs[0].token_id, 8);
    memcpy(qr2.token_secret, subs[0].token_secret, 16);
    activate_direct(TMID, &qr2, now);
    TEST_ASSERT_EQUAL_UINT32(TMID, subs[0].tmid);
    TEST_ASSERT_TRUE(net_ended(cid));
}

/* Final review C2: DEACTIVATE in a call is refused, like ACTIVATE. */
static void test_deactivate_mid_call_refused(void)
{
    registered_world(LC_SIG_MODE_PART15);
    connected_mo_call();
    const uint8_t cmd[2] = { LC_SIG_CMD_DEACTIVATE, 0xA5 };
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ATT_NOT_NOW, lc_sig_term_command(&T, cmd, 2, now));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&T));
    TEST_ASSERT_TRUE(ID.activated);
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_DEACTIVATED));
}

/* Final review I2: every copy of ACT_ACK is lost and the terminal gives up,
 * though the network bound it. Scanning the same QR again must work (the
 * network answers ACT_ACK again for the same terminal and key); another
 * terminal presenting the used token is still refused (reason 2). */
static void test_lost_act_ack_same_qr_retry_succeeds(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    dl_drop_all_sig = 1;
    activate();
    run_ms(8000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ACT_FAILED));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_TIMEOUT, evs[0][1]);
    TEST_ASSERT_TRUE(subs[0].token_used); /* the network did bind it */
    TEST_ASSERT_EQUAL_UINT32(TMID, subs[0].tmid);
    uint8_t k0[16];
    memcpy(k0, subs[0].k, 16);

    dl_drop_all_sig = 0;
    memset(&dlq, 0, sizeof(dlq));
    activate_direct(TMID2, &QR, now); /* someone else with the used QR */
    uint8_t p[LC_SIG_LINK_MAX], n;
    TEST_ASSERT_EQUAL_INT(0, qpop(&dlq, p, &n));
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_NAK, p[2]);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_ACT_USED, p[5]);
    TEST_ASSERT_EQUAL_UINT32(TMID, subs[0].tmid);
    memset(&dlq, 0, sizeof(dlq));

    nevs = 0;
    activate(); /* the same QR, on the same terminal */
    run_ms(10000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ACTIVATED));
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k0, subs[0].k, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(subs[0].k, ID.k, 16);
}

extern int lc_sig_test_fail_aes; /* crypto_openssl.c: fault injection */

/* Final review I5: if the voice cipher fails, Part 15 app data must not go
 * out in the clear (and the frame counter must not move); a frame that
 * can't be decrypted is dropped. */
static void test_voice_crypto_failure_fails_closed(void)
{
    registered_world(LC_SIG_MODE_PART15);
    connected_mo_call();
    uint8_t up[LC_SIG_LINK_MAX], upn, dn[LC_SIG_LINK_MAX], dnn, out[LC_SIG_APP_MAX], on;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_out(&T, (const uint8_t *)"VOICE-UP", 8, up, &upn));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_data_out(&N, TMID, (const uint8_t *)"VOICE-DN", 8, dn, &dnn));
    uint32_t t_tx = T.d_tx, t_rx = T.d_rx_next;
    uint32_t n_tx = net_sess(TMID)->d_tx, n_rx = net_sess(TMID)->d_rx_next;

    lc_sig_test_fail_aes = 1;
    uint8_t air[LC_SIG_LINK_MAX], an = 0;
    memset(air, 0, sizeof(air));
    TEST_ASSERT_NOT_EQUAL(0, lc_sig_term_data_out(&T, (const uint8_t *)"SECRET-1", 8, air, &an));
    TEST_ASSERT_FALSE(memcmp(air + 2, "SECRET-1", 8) == 0);
    TEST_ASSERT_EQUAL_UINT32(t_tx, T.d_tx);
    memset(air, 0, sizeof(air));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_net_data_out(&N, TMID, (const uint8_t *)"SECRET-2", 8, air, &an));
    TEST_ASSERT_FALSE(memcmp(air + 2, "SECRET-2", 8) == 0);
    TEST_ASSERT_EQUAL_UINT32(n_tx, net_sess(TMID)->d_tx);
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_net_data_in(&N, TMID, up, upn, out, &on));
    TEST_ASSERT_EQUAL_UINT32(n_rx, net_sess(TMID)->d_rx_next);
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_term_data_in(&T, dn, dnn, out, &on));
    TEST_ASSERT_EQUAL_UINT32(t_rx, T.d_rx_next);
    lc_sig_test_fail_aes = 0;

    /* the same frames still decrypt once the cipher works again */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_data_in(&N, TMID, up, upn, out, &on));
    TEST_ASSERT_EQUAL_MEMORY("VOICE-UP", out, 8);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_data_in(&T, dn, dnn, out, &on));
    TEST_ASSERT_EQUAL_MEMORY("VOICE-DN", out, 8);
}

static int ended_cause(void)
{
    for (int i = 0; i < nevs && i < 64; i++) if (evs[i][0] == LC_SIG_EV_ENDED) return evs[i][5];
    return -1;
}

/* Final review P2: the network answers a CALL_SETUP with RELEASE(busy) while
 * its session has a call (here: an incoming call crossing the terminal's
 * own dial). That RELEASE must carry call id 0, as the unregistered path
 * does, so the calling terminal (which has no call id yet) ends at once
 * with cause busy instead of timing out. */
static void test_busy_release_on_crossing_setup_uses_call_id_0(void)
{
    registered_world(LC_SIG_MODE_PART15);
    uint8_t caller[LC_SIG_NUMBER_LEN];
    uint32_t cid;
    lc_sig_number_to_bcd("+8836065550100", 14, caller);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_call_in(&N, subs[0].number, caller, now, &cid));
    command("\x02+8836065550101", 15); /* the terminal dials at the same moment */
    /* both ends now hold a request in flight (SETUP_IND, CALL_SETUP) and each
     * answer waits behind it; the network's SETUP_IND gives up after ~4.4 s,
     * then its RELEASE(busy) goes out */
    run_ms(6000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ENDED));
    TEST_ASSERT_EQUAL_INT(LC_SIG_CAUSE_BUSY, ended_cause());
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
}

/* Final review P3: the subscriber re-activates on another terminal while the
 * old one is in a call. The old terminal's leg is released with a RELEASE
 * (cause network failure), so the old terminal ends its call too instead of
 * staying IN_CALL with nothing behind it. */
static void test_reactivation_elsewhere_releases_old_terminals_call(void)
{
    registered_world(LC_SIG_MODE_PART15);
    uint32_t cid = connected_mo_call();
    memset(subs[0].token_id, 0xc0, 8);
    memset(subs[0].token_secret, 0xd0, 16);
    subs[0].token_used = 0;
    subs[0].token_expiry = unix_s + 3600u;
    lc_sig_qr_t qr2 = QR;
    memcpy(qr2.token_id, subs[0].token_id, 8);
    memcpy(qr2.token_secret, subs[0].token_secret, 16);
    nevs = 0;
    activate_direct(TMID2, &qr2, now);
    TEST_ASSERT_EQUAL_UINT32(TMID2, subs[0].tmid);
    run_ms(3000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ENDED));
    TEST_ASSERT_EQUAL_INT(LC_SIG_CAUSE_NET_FAILURE, ended_cause());
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_TRUE(net_ended(cid));
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
    RUN_TEST(test_forged_reg_req_does_not_accelerate_unrelated_pending_request);
    RUN_TEST(test_reg_req_after_auth_req_expiry_gets_fresh_auth_req);
    RUN_TEST(test_auth_fail_with_unbound_subscriber_does_not_wedge_registration);
    RUN_TEST(test_reg_req_in_exhausted_window_still_accepts_correct_auth_rsp);
    RUN_TEST(test_reg_req_with_retries_left_forces_resend_not_new_vector);
    RUN_TEST(test_lost_call_proc_with_immediate_alert_still_connects);
    RUN_TEST(test_reboot_mid_call_ends_network_leg_and_new_call_works);
    RUN_TEST(test_reactivation_on_same_tmid_ends_its_call);
    RUN_TEST(test_deactivate_mid_call_refused);
    RUN_TEST(test_lost_act_ack_same_qr_retry_succeeds);
    RUN_TEST(test_voice_crypto_failure_fails_closed);
    RUN_TEST(test_busy_release_on_crossing_setup_uses_call_id_0);
    RUN_TEST(test_reactivation_elsewhere_releases_old_terminals_call);
    return UNITY_END();
}
