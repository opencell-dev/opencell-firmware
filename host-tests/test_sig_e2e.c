/* Terminal and network roles over a fake link: one UL and one DL payload
 * per 120 ms frame, optional loss, channel grants that follow the network's
 * requests after 3 frames (like a page and a grant). */
#include "unity.h"

#include <string.h>

#include "lc_sig_crypto.h"
#include "lc_sig_keys.h"
#include "lc_sig_milenage.h"
#include "lc_sig_net.h"
#include "lc_sig_term.h"

void setUp(void) {}
void tearDown(void) {}

#define TMID  0x76ad0488u
#define TMID2 0x11223344u
#define TMID3 0x33445566u /* a second real device, for the session-independence test only */
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
/* TMID3's own downlink (fix round 1, Review Focus 3): kept apart from T's
 * fake air so a session-independence test can drive TMID3 directly (a bare
 * chan, not a full lc_sig_term_t) without disturbing T's traffic. TMID2
 * deliberately keeps sharing dlq with T - other tests rely on that. */
static q_t dlq3;
static int net_send(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    if (dl_drop_all_sig && (p[0] & 0xF0u) == LC_SIG_KIND_SIG) return 0;
    if (dl_drop_msg && (p[0] & 0xF0u) == LC_SIG_KIND_SIG) {
        if (p[0] & 0x02u) dl_drop_msg = 0; /* that was the last fragment of the message */
        return 0; /* the network sent it; the air dropped it */
    }
    if (tmid == TMID3) return qpush(&dlq3, p, n);
    return qpush(&dlq, p, n);
}
static int block_grant; /* while set, the cell grants nothing (the terminal still hears its beacons) */
static void net_channel(void *c, uint32_t tmid, int on)
{
    (void)c;
    (void)tmid;
    if (on && !granted && !grant_pending && !block_grant) { grant_pending = 1; grant_at = now + 3u * FRAME; }
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
static int svc_config; /* service requests with cause 4 (config) */
static int cfg_on;     /* the terminal hears a beacon carrying cfg_ver */
static uint8_t cfg_ver;
static int term_svc(void *c, uint8_t cause)
{
    (void)c;
    if (cause == LC_SIG_SVC_CONFIG) svc_config++;
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

/* UL hooks: drop every fragment of the next UL message of ul_drop_type;
 * keep a copy of the last AUTH_RSP heard on air (single fragment). */
static int ul_drop_type, ul_drop_active;
static uint8_t rec_auth_rsp[LC_SIG_LINK_MAX], rec_auth_rsp_n;
static int ul_heard(const uint8_t *p, uint8_t n)
{
    if ((p[0] & 0xF0u) != LC_SIG_KIND_SIG) return 0;
    if ((p[0] & 0x0Cu) == 0 && p[2] == LC_SIG_AUTH_RSP) {
        memcpy(rec_auth_rsp, p, n);
        rec_auth_rsp_n = n;
    }
    if (ul_drop_type && (p[0] & 0x0Cu) == 0 && p[2] == ul_drop_type) {
        ul_drop_active = 1;
        ul_drop_type = 0;
    }
    if (!ul_drop_active) return 1;
    if (p[0] & 0x02u) ul_drop_active = 0;
    return 0;
}

static void world(uint8_t mode, uint16_t period_s)
{
    memset(&ulq, 0, sizeof(ulq));
    memset(&dlq, 0, sizeof(dlq));
    memset(&dlq3, 0, sizeof(dlq3));
    memset(subs, 0, sizeof(subs));
    nsubs = saves = ncalls = nevs = 0;
    memset(evs, 0, sizeof(evs));
    reg_mode = 0;
    now = 0;
    loss_pct = 0;
    dl_drop_msg = 0;
    dl_drop_all_sig = 0;
    block_grant = 0;
    ul_drop_type = ul_drop_active = 0;
    rec_auth_rsp_n = 0;
    alert_now = 0;
    cfg_on = 0;
    cfg_ver = 0;
    svc_config = 0;
    granted = 1; /* the cell grants on attach */
    grant_pending = 0;
    memset(SKN, 0x11, 32);
    lc_sig_net_cfg_t cfg = { 1, { 0 }, mode, period_s };
    memcpy(cfg.sk, SKN, 32);
    lc_sig_net_init(&N, &net_io, &cfg);
    /* one subscriber with a fresh token, and its QR */
    lc_sig_sub_t *s = &subs[nsubs++];
    lc_sig_number_to_bcd("+883160655501234", 16, s->number);
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
    if (cfg_on) lc_sig_term_cell_cfg(&T, cfg_ver, now);
    lc_sig_term_tick(&T, now);
    lc_sig_net_tick(&N, now);
    if (granted) {
        if (qpop(&ulq, p, &n) == 0 && !lost()) {
            if (ul_heard(p, n)) lc_sig_net_rx(&N, TMID, p, n, now);
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
    m.u.reg_req.sw_version[1] = 6;
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
    command("\x02+883160655500100", 17);
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
    lc_sig_number_to_bcd("+883160655500100", 16, caller);
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
    command("\x02+883160655500100", 17);
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
    command("\x02+883160655500100", 17);
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

/* ---- channel list (spec 2026-09-27-channel-list-design.md §7) ---- */

static lc_sig_chan_list_t make_list(uint8_t ver, uint8_t count, uint8_t first_ch)
{
    lc_sig_chan_list_t l;
    memset(&l, 0, sizeof(l));
    l.ver = ver;
    l.count = count;
    for (uint8_t i = 0; i < count; i++) l.freq_hz[i] = 902250000u + 500000u * (uint32_t)(first_ch + i);
    return l;
}

/* Pushed after REG_ACK, on every registration (here every 60 s). */
static void test_chan_list_pushed_after_every_registration(void)
{
    world(LC_SIG_MODE_PART15, 60);
    lc_sig_chan_list_t l = make_list(3, 2, 30), got;
    l.flags[1] = LC_SIG_CHAN_FIXED;
    lc_sig_net_set_chan_list(&N, &l);
    activate();
    run_ms(10000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&T, &got));
    TEST_ASSERT_EQUAL_MEMORY(&l, &got, sizeof(l));
    TEST_ASSERT_EQUAL_UINT8(3, T.list_ver);
    TEST_ASSERT_FALSE(net_sess(TMID)->ch.pend); /* acknowledged */
    TEST_ASSERT_EQUAL_INT(0, granted);          /* and the idle channel released */
    run_ms(60000);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&T, &got)); /* registered again: pushed again */
    TEST_ASSERT_EQUAL_INT(0, svc_config);
}

/* The cell's list changes: the beacon's cfg_ver no longer matches, the
 * terminal asks with SERVICE_REQ(4) and gets the new list, over a link that
 * loses 20 % of everything. */
static void test_cfg_ver_change_gets_the_new_list_over_a_lossy_link(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    lc_sig_chan_list_t l = make_list(1, 2, 30), got;
    lc_sig_net_set_chan_list(&N, &l);
    activate();
    run_ms(10000);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&T, &got));
    loss_pct = 20;
    l = make_list(2, 1, 40);
    lc_sig_net_set_chan_list(&N, &l);
    cfg_on = 1;
    cfg_ver = 2;
    run_ms(70000);
    TEST_ASSERT_TRUE(svc_config >= 1);
    TEST_ASSERT_EQUAL_UINT8(2, T.list_ver);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&T, &got));
    TEST_ASSERT_EQUAL_UINT8(1, got.count);
    TEST_ASSERT_EQUAL_UINT32(922250000u, got.freq_hz[0]);
    int asked = svc_config;
    run_ms(60000);
    TEST_ASSERT_EQUAL_INT(asked, svc_config); /* up to date: no more requests */
}

/* A network without a list answers a config request with an empty list,
 * version 0, so a terminal holding another network's list stops asking; an
 * unregistered session gets nothing (no keys to protect it). */
static void test_config_request_without_a_list(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    activate();
    run_ms(10000);
    lc_sig_chan_list_t got;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_term_chan_list(&T, &got)); /* no list set: no push */
    T.list_ver = 2; /* from another network */
    cfg_on = 1;
    cfg_ver = 0;
    run_ms(10000);
    TEST_ASSERT_EQUAL_INT(1, svc_config);
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&T, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.ver);
    TEST_ASSERT_EQUAL_UINT8(0, got.count);
    run_ms(60000);
    TEST_ASSERT_EQUAL_INT(1, svc_config);

    lc_sig_net_service_req(&N, TMID2, LC_SIG_SVC_CONFIG, now);
    TEST_ASSERT_NOT_NULL(net_sess(TMID2));
    TEST_ASSERT_EQUAL_UINT8(0, net_sess(TMID2)->out_count);
}

/* Controller ruling C1: Part 97, with a channel list configured (so REG_ACK
 * is immediately followed by a CHAN_LIST push - the crossing that fix round
 * 1's ruling A guards): drop the network's REG_ACK once. Registration must
 * still complete, recovered by the terminal's own AUTH_RSP retransmission
 * (ruling A: the network's chan must still recognise it as a repeat of the
 * request it already answered, despite the terminal's CHAN_LIST_ACK crossing
 * it in between), without ever reporting REG_FAILED. */
static void test_part97_list_survives_a_lost_reg_ack(void)
{
    world(LC_SIG_MODE_PART97, 1800);
    lc_sig_chan_list_t l = make_list(2, 1, 10);
    lc_sig_net_set_chan_list(&N, &l);
    activate();
    int armed = 0;
    for (int i = 0; i < 200 && !has_event(LC_SIG_EV_REGISTERED); i++) {
        frame();
        /* AUTH_RSP just went out (is_request itself): REG_ACK is next. */
        if (!armed && T.ch.pend && T.ch.pend_type == LC_SIG_AUTH_RSP) {
            dl_drop_msg = 1; /* the very next DL signalling message is REG_ACK */
            armed = 1;
        }
    }
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_REG_FAILED));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
}

/* Controller ruling C2: a CHAN_LIST crossing a CALL_SETUP, with the CALL_PROC
 * that answers it lost: the network's cached CALL_PROC (ruling A) must
 * survive the terminal's own, unrelated CHAN_LIST_ACK arriving in between, so
 * the retransmitted CALL_SETUP still gets it back and the call proceeds -
 * not a spurious BUSY release from a CALL_SETUP mistaken for a brand new one. */
static void test_chan_list_crossing_call_setup_survives_lost_call_proc(void)
{
    registered_world(LC_SIG_MODE_PART15);
    lc_sig_chan_list_t l = make_list(4, 1, 25);
    lc_sig_net_set_chan_list(&N, &l);
    granted = 1; /* the idle channel was released: give it back at once */
    lc_sig_term_link(&T, 1, 1, now);
    lc_sig_net_link(&N, TMID, 1, now);
    command("\x02+883160655500100", 17);                     /* CALL_SETUP queued on T */
    lc_sig_net_service_req(&N, TMID, LC_SIG_SVC_CONFIG, now); /* CHAN_LIST crosses it, queued on N */

    /* One frame, driven by hand: CALL_SETUP and CHAN_LIST both go out, then
     * CALL_SETUP is delivered (generating and dropping CALL_PROC) before
     * CHAN_LIST is delivered (so the terminal's CHAN_LIST_ACK, sent right
     * back, is what crosses the network's still-pending CALL_PROC cache). */
    now += FRAME;
    lc_sig_term_link(&T, 1, 1, now);
    lc_sig_net_link(&N, TMID, 1, now);
    lc_sig_term_tick(&T, now); /* sends CALL_SETUP */
    lc_sig_net_tick(&N, now);  /* sends CHAN_LIST */
    dl_drop_msg = 1;           /* CALL_PROC is the network's next DL message: drop it once */
    uint8_t p[LC_SIG_LINK_MAX], n;
    TEST_ASSERT_EQUAL_INT(0, qpop(&ulq, p, &n));
    lc_sig_net_rx(&N, TMID, p, n, now); /* delivers CALL_SETUP; CALL_PROC is generated and dropped */
    lc_sig_net_heard(&N, TMID, now);
    TEST_ASSERT_EQUAL_INT(0, qpop(&dlq, p, &n));
    lc_sig_term_rx(&T, p, n, now); /* delivers CHAN_LIST; the terminal answers with CHAN_LIST_ACK */
    TEST_ASSERT_EQUAL_INT(0, dl_drop_msg); /* the drop fired: that was CALL_PROC's last fragment */

    run_ms(5000); /* CHAN_LIST_ACK reaches the network; the retransmitted CALL_SETUP recovers CALL_PROC */
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_ENDED));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_CALLING, lc_sig_term_state(&T));
    TEST_ASSERT_NOT_EQUAL(0, T.call_id); /* CALL_PROC's resend was received and applied */
}

/* Controller ruling C3 (fix round 1, Review Focus 3): two terminals on one
 * network have genuinely separate session chans. TMID2 is driven directly (a
 * bare chan playing its own terminal's part - its activation path is
 * irrelevant here) straight through its own CHAN_LIST/ACK exchange while
 * TMID's own CHAN_LIST push is still queued and unacknowledged, proving one
 * session's ACK never touches the other's pending state. */
static void test_two_terminals_ack_does_not_clear_the_others_pending_push(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    static const uint8_t K2[16] = { 0x46, 0x5b, 0x5c, 0xe8, 0xb1, 0x99, 0xb4, 0x9f,
                                     0xaa, 0x5f, 0x0a, 0x2e, 0xe2, 0x38, 0xa6, 0xbc };
    static const uint8_t OPC2[16] = { 0xcd, 0x63, 0xcb, 0x71, 0x95, 0x4a, 0x9f, 0x4e,
                                       0x48, 0xa5, 0x99, 0x4e, 0x37, 0xa0, 0x2b, 0xaf };
    static const uint8_t amf0[2] = { 0x80, 0x00 };
    static const uint8_t zero6[6] = { 0 };
    subs[1].activated = 1;
    subs[1].tmid = TMID3;
    memcpy(subs[1].k, K2, 16);
    memcpy(subs[1].opc, OPC2, 16);
    lc_sig_number_to_bcd("+883160655500002", 16, subs[1].number);
    nsubs = 2;

    activate();
    run_ms(10000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));

    lc_sig_chan_list_t l = make_list(4, 1, 22);
    lc_sig_net_set_chan_list(&N, &l);
    lc_sig_net_service_req(&N, TMID, LC_SIG_SVC_CONFIG, now); /* T's own CHAN_LIST, left unacked below */
    lc_sig_net_tick(&N, now);
    TEST_ASSERT_TRUE(net_sess(TMID)->ch.pend);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_CHAN_LIST, net_sess(TMID)->ch.pend_type);

    /* terminal 2: a full registration handshake driven straight at N over a
     * bare chan (its own crypto/activation is not what this test is about). */
    lc_sig_chan_t f2;
    lc_sig_chan_init(&f2, 0);
    lc_sig_msg_t m, got;
    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_REG_REQ;
    m.u.reg_req.caps = 1;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&f2, &m, now));
    {
        const uint8_t *fp;
        uint8_t fn;
        while (lc_sig_chan_peek(&f2, &fp, &fn) == 0) {
            uint8_t c[LC_SIG_LINK_MAX];
            memcpy(c, fp, fn);
            lc_sig_chan_pop(&f2);
            lc_sig_net_rx(&N, TMID3, c, fn, now);
        }
    }
    memset(&got, 0, sizeof(got));
    {
        uint8_t dp[LC_SIG_LINK_MAX], dn;
        int done = 0;
        while (!done && qpop(&dlq3, dp, &dn) == 0) done = lc_sig_chan_rx(&f2, dp, dn, &got, now);
        TEST_ASSERT_TRUE(done);
    }
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_AUTH_REQ, got.type);

    lc_milenage_t o;
    uint8_t sqn[6];
    lc_milenage(K2, OPC2, got.u.auth_req.rand, zero6, amf0, &o); /* AK */
    for (int i = 0; i < 6; i++) sqn[i] = (uint8_t)(got.u.auth_req.autn[i] ^ o.ak[i]);
    lc_milenage(K2, OPC2, got.u.auth_req.rand, sqn, got.u.auth_req.autn + 6, &o);
    uint8_t rand2[16];
    memcpy(rand2, got.u.auth_req.rand, 16);

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_AUTH_RSP;
    memcpy(m.u.auth_rsp.res, o.res, 8);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&f2, &m, now));
    {
        const uint8_t *fp;
        uint8_t fn;
        while (lc_sig_chan_peek(&f2, &fp, &fn) == 0) {
            uint8_t c[LC_SIG_LINK_MAX];
            memcpy(c, fp, fn);
            lc_sig_chan_pop(&f2);
            lc_sig_net_rx(&N, TMID3, c, fn, now);
        }
    }
    TEST_ASSERT_TRUE(net_sess(TMID3)->registered);

    uint8_t ki[16], ke[16];
    lc_sig_session_keys(o.ck, o.ik, rand2, TMID3, ki, ke);
    lc_sig_sec_key(&f2.sec, ki, ke, 1); /* PART15: encrypted */

    memset(&got, 0, sizeof(got));
    {
        uint8_t dp[LC_SIG_LINK_MAX], dn;
        int done = 0;
        while (!done && qpop(&dlq3, dp, &dn) == 0) done = lc_sig_chan_rx(&f2, dp, dn, &got, now);
        TEST_ASSERT_TRUE(done);
    }
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_ACK, got.type);

    memset(&got, 0, sizeof(got));
    {
        uint8_t dp[LC_SIG_LINK_MAX], dn;
        int done = 0;
        while (!done && qpop(&dlq3, dp, &dn) == 0) done = lc_sig_chan_rx(&f2, dp, dn, &got, now);
        TEST_ASSERT_TRUE(done);
    }
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CHAN_LIST, got.type);

    /* TMID's own CHAN_LIST is still pending and unacked at this point. */
    TEST_ASSERT_TRUE(net_sess(TMID)->ch.pend);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_CHAN_LIST, net_sess(TMID)->ch.pend_type);

    memset(&m, 0, sizeof(m));
    m.type = LC_SIG_CHAN_LIST_ACK;
    m.u.chan_list_ack.ver = got.u.chan_list.ver;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&f2, &m, now));
    {
        const uint8_t *fp;
        uint8_t fn;
        while (lc_sig_chan_peek(&f2, &fp, &fn) == 0) {
            uint8_t c[LC_SIG_LINK_MAX];
            memcpy(c, fp, fn);
            lc_sig_chan_pop(&f2);
            lc_sig_net_rx(&N, TMID3, c, fn, now);
        }
    }

    /* TMID3's own push is now acked; TMID's is still pending, untouched by it. */
    TEST_ASSERT_FALSE(net_sess(TMID3)->ch.pend);
    TEST_ASSERT_TRUE(net_sess(TMID)->ch.pend);
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_CHAN_LIST, net_sess(TMID)->ch.pend_type);

    /* and T can still get its own list normally afterwards. */
    run_ms(5000);
    lc_sig_chan_list_t got_list;
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&T, &got_list));
}

/* M4: a CHAN_LIST already queued (queued into a session's outq, but not yet
 * handed to its chan) picks up a fresh lc_sig_net_set_chan_list body in
 * place, rather than going out with whatever the list was when it was
 * queued. */
static void test_queued_chan_list_refreshes_on_a_new_set(void)
{
    registered_world(LC_SIG_MODE_PART15);
    lc_sig_chan_list_t l1 = make_list(1, 1, 5);
    lc_sig_net_set_chan_list(&N, &l1);
    lc_sig_net_service_req(&N, TMID, LC_SIG_SVC_CONFIG, now); /* queues CHAN_LIST(v1); not flushed yet */
    TEST_ASSERT_EQUAL_UINT8(1, net_sess(TMID)->out_count);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CHAN_LIST, net_sess(TMID)->outq[0].type);
    TEST_ASSERT_EQUAL_UINT8(1, net_sess(TMID)->outq[0].u.chan_list.ver);

    lc_sig_chan_list_t l2 = make_list(2, 1, 40);
    lc_sig_net_set_chan_list(&N, &l2); /* refreshed in place before it ever went out */
    TEST_ASSERT_EQUAL_UINT8(2, net_sess(TMID)->outq[0].u.chan_list.ver);
    TEST_ASSERT_EQUAL_UINT32(922250000u, net_sess(TMID)->outq[0].u.chan_list.freq_hz[0]);

    run_ms(5000);
    lc_sig_chan_list_t got;
    TEST_ASSERT_EQUAL_INT(1, lc_sig_term_chan_list(&T, &got));
    TEST_ASSERT_EQUAL_UINT8(2, got.ver); /* the terminal got the refreshed list, not the stale one */
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
    command("\x02+883160655500100", 17);
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
    command("\x02+883160655500100", 17);
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
    command("\x02+883160655500100", 17); /* the old terminal tries to dial */
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
    lc_sig_number_to_bcd("+883160655500100", 16, caller);
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
    command("\x02+883160655500100", 17);
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
    command("\x02+883160655500100", 17);
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
    lc_sig_number_to_bcd("+883160655500100", 16, caller);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_call_in(&N, subs[0].number, caller, now, &cid));
    command("\x02+883160655500101", 17); /* the terminal dials at the same moment */
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

/* The network restarts (sessions lost, subscriber database kept); the
 * terminal loses the cell meanwhile and attaches again. */
static void net_restart(uint8_t mode)
{
    lc_sig_net_cfg_t cfg = { 1, { 0 }, mode, 1800 };
    memcpy(cfg.sk, SKN, 32);
    lc_sig_net_init(&N, &net_io, &cfg);
    memset(&ulq, 0, sizeof(ulq));
    memset(&dlq, 0, sizeof(dlq));
    granted = 0;
    grant_pending = 0;
    lc_sig_term_link(&T, 0, 0, now);
}

static int registered_both(void)
{
    return lc_sig_term_state(&T) == LC_SIG_ST_REGISTERED && lc_sig_net_registered(&N, TMID);
}

/* Fix round 2 (Task 8), finding 1: after a restart the network's AUTH_REQ is
 * its session's seq 0. A terminal still holding the previous network's
 * AUTH_REQ (seq 0, no request since: a call's own messages are replies or
 * the terminal's) took the new one for a repeat and sent the old AUTH_RSP
 * again: REG_REJ, REG_FAILED and 30 s. A new registration forgets the old
 * network's numbering. */
static void test_network_restart_after_a_call_registers_again_at_once(void)
{
    registered_world(LC_SIG_MODE_PART15);
    net_restart(LC_SIG_MODE_PART15); /* restart 1: this session's AUTH_REQ is seq 0 */
    run_ms(5000);
    TEST_ASSERT_TRUE(registered_both());
    command("\x02+883160655500100", 17);
    run_ms(3000);
    command("\x05", 1); /* HANGUP */
    run_ms(8000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    net_restart(LC_SIG_MODE_PART15); /* restart 2 */
    nevs = 0;
    run_ms(3000);
    TEST_ASSERT_TRUE(registered_both());
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_REG_FAILED));
}

/* Fix round 2, finding 1's mirror on the network side: the terminal reboots
 * between ACT_ACK and its first REG_REQ, so the REG_REQ goes out as seq 0 -
 * the seq of the ACT_REQ the network last heard. A message of another type
 * is not a repeat, whatever its seq: it must not be dropped (it was, every
 * retransmission, then REG_FAILED and 30 s). */
static void test_terminal_reboot_between_act_ack_and_reg_req_registers_at_once(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    activate();
    for (int i = 0; i < 200 && !has_event(LC_SIG_EV_ACTIVATED); i++) frame();
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_ACTIVATED));
    TEST_ASSERT_EQUAL_INT(0, ulq.count); /* REG_REQ (seq 1) not sent yet */
    lc_sig_term_init(&T, &term_io, &ID, TMID, now); /* reboot: numbering restarts at 0 */
    nevs = 0;
    run_ms(3000);
    TEST_ASSERT_TRUE(registered_both());
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_REG_FAILED));
}

/* Fix round 2, finding 2: a re-registration starts just as a config request
 * (cause 4) lands. The CHAN_LIST it queued is ignored by the registering
 * terminal, and held the session's one request slot: AUTH_REQ waited behind
 * it and the terminal's REG_REQ retransmissions were dropped as repeats
 * (REG_FAILED, 4.8 s). A REG_REQ drops a queued or in-flight CHAN_LIST; it
 * is pushed again after REG_ACK. */
static void test_reregistration_crossing_a_chan_list_push_registers_at_once(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    lc_sig_chan_list_t l = make_list(1, 2, 30);
    lc_sig_net_set_chan_list(&N, &l);
    activate();
    run_ms(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    l = make_list(2, 2, 40);
    lc_sig_net_set_chan_list(&N, &l);
    nevs = 0;
    T.rereg_at = now;                                         /* the terminal starts re-registering... */
    lc_sig_net_service_req(&N, TMID, LC_SIG_SVC_CONFIG, now); /* ...as its config request lands */
    run_ms(3000);
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_REG_FAILED));
    run_ms(5000);
    TEST_ASSERT_EQUAL_UINT8(2, T.list_ver); /* pushed after REG_ACK */
}

/* Fix round 2, finding 3: the terminal asks for cfg_ver 3 while a v2 push is
 * still in flight (retries frozen: no grant). v2 lands first and answers the
 * ask (M1: not asked again), so the network must follow a stale in-flight
 * push with the current list. */
static void test_new_list_while_a_stale_push_is_in_flight_still_arrives(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    lc_sig_chan_list_t l = make_list(1, 2, 30);
    lc_sig_net_set_chan_list(&N, &l);
    activate();
    run_ms(15000);
    TEST_ASSERT_EQUAL_UINT8(1, T.list_ver);
    l = make_list(2, 2, 40);
    lc_sig_net_set_chan_list(&N, &l);
    cfg_on = 1;
    cfg_ver = 2;
    dl_drop_all_sig = 1;
    for (int i = 0; i < 100 && !(granted && lc_sig_chan_busy(&net_sess(TMID)->ch)); i++) frame();
    TEST_ASSERT_EQUAL_HEX8(LC_SIG_CHAN_LIST, net_sess(TMID)->ch.pend_type);
    frame(); /* v2 goes out once, and is lost */
    TEST_ASSERT_TRUE(net_sess(TMID)->ch.pend);
    granted = 0; /* no more grants for a while; the beacon is still heard */
    grant_pending = 0;
    block_grant = 1;
    run_ms(35000);
    l = make_list(3, 2, 50);
    lc_sig_net_set_chan_list(&N, &l); /* the current list is queued behind the stale v2 at once */
    TEST_ASSERT_EQUAL_UINT8(1, net_sess(TMID)->out_count);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CHAN_LIST, net_sess(TMID)->outq[0].type);
    TEST_ASSERT_EQUAL_UINT8(3, net_sess(TMID)->outq[0].u.chan_list.ver);
    cfg_ver = 3;
    run_ms(35000);
    block_grant = 0;
    dl_drop_all_sig = 0;
    run_ms(60000);
    TEST_ASSERT_EQUAL_UINT8(3, T.list_ver);
}

/* Fix round 2, the task's residual: REG_ACK is lost, so the CHAN_LIST right
 * behind it reaches a terminal still registering (ignored, ruling B); its
 * retransmissions then fail the replay window (the resent REG_ACK was sealed
 * later), and it expires. With no cfg_ver change to prompt a config request,
 * the network pushes it once more. */
static void test_chan_list_lost_to_a_lost_reg_ack_is_pushed_again(void)
{
    world(LC_SIG_MODE_PART15, 1800);
    lc_sig_chan_list_t l = make_list(3, 4, 30);
    lc_sig_net_set_chan_list(&N, &l);
    activate();
    int armed = 0;
    for (int i = 0; i < 200 && !has_event(LC_SIG_EV_REGISTERED); i++) {
        frame();
        if (!armed && T.ch.pend && T.ch.pend_type == LC_SIG_AUTH_RSP) {
            dl_drop_msg = 1; /* the next DL signalling message is REG_ACK */
            armed = 1;
        }
    }
    TEST_ASSERT_TRUE(has_event(LC_SIG_EV_REGISTERED));
    run_ms(10000);
    TEST_ASSERT_EQUAL_UINT8(3, T.list_ver);
}

/* Fix round 3, A: the far end releases an MT call, the terminal's
 * RELEASE_COMPLETE is lost, and a re-registration is due. The network's
 * retransmitted RELEASE must still get the cached RELEASE_COMPLETE after
 * REG_REQ is queued (forgetting the cached request/reply there left the
 * RELEASE unanswered: it held the request slot, AUTH_REQ waited, 4.3 s). */
static void test_lost_release_complete_then_reregistration(void)
{
    registered_world(LC_SIG_MODE_PART15);
    uint8_t caller[LC_SIG_NUMBER_LEN];
    uint32_t cid;
    lc_sig_number_to_bcd("+883160655500100", 16, caller);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_call_in(&N, subs[0].number, caller, now, &cid));
    run_ms(2000);
    command("\x03", 1); /* ANSWER */
    run_ms(2000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_IN_CALL, lc_sig_term_state(&T));
    T.rereg_at = now; /* fell due during the call */
    nevs = 0;
    ncalls = 0;
    ul_drop_type = LC_SIG_RELEASE_COMPLETE;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_net_peer_release(&N, cid, LC_SIG_CAUSE_NORMAL, now));
    uint64_t t0 = now, t_end = 0, t_reg = 0;
    for (int i = 0; i < 100; i++) {
        frame();
        if (!t_end && net_ended(cid)) t_end = now - t0;
        if (!t_reg && has_event(LC_SIG_EV_REGISTERED)) t_reg = now - t0;
    }
    TEST_ASSERT_NOT_EQUAL(0, t_end);
    TEST_ASSERT_TRUE(t_end <= 2000000u);
    TEST_ASSERT_NOT_EQUAL(0, t_reg);
    TEST_ASSERT_TRUE(t_reg <= 2500000u);
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_REG_FAILED));
}

/* Fix round 3, C: a re-registration has started (reg_start) but REG_REQ is
 * not queued yet - the next tick, or the cell is out of reach. A recorded
 * AUTH_RSP played back draws the network's cached REG_ACK (sealed afresh
 * with the live keys, and past the terminal's rx_seq: a CHAN_LIST came
 * after it); it must not complete the registration without an AKA. */
static void replayed_auth_rsp_before_reg_req(int out_of_reach)
{
    world(LC_SIG_MODE_PART15, 1800);
    lc_sig_chan_list_t l = make_list(3, 4, 30);
    lc_sig_net_set_chan_list(&N, &l);
    activate();
    run_ms(15000);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERED, lc_sig_term_state(&T));
    TEST_ASSERT_NOT_EQUAL(0, rec_auth_rsp_n);
    memset(&ulq, 0, sizeof(ulq));
    memset(&dlq, 0, sizeof(dlq));
    if (out_of_reach) {
        lc_sig_term_link(&T, 0, 0, now);
        T.rereg_at = now;
        now += FRAME;
        lc_sig_term_tick(&T, now); /* the re-registration falls due out of reach: no REG_REQ */
        now += 30000000u;
        lc_sig_term_link(&T, 1, 1, now);
    } else {
        T.rereg_at = now;
        now += FRAME;
        lc_sig_term_tick(&T, now); /* reg_start; REG_REQ on the next tick */
    }
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&T));
    TEST_ASSERT_EQUAL_INT(0, ulq.count);
    nevs = 0;
    lc_sig_net_rx(&N, TMID, rec_auth_rsp, rec_auth_rsp_n, now); /* the recording */
    lc_sig_net_tick(&N, now);
    uint8_t p[LC_SIG_LINK_MAX], n;
    int dl = 0;
    while (qpop(&dlq, p, &n) == 0) {
        lc_sig_term_rx(&T, p, n, now);
        dl++;
    }
    TEST_ASSERT_TRUE(dl > 0); /* the network did answer it */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_ST_REGISTERING, lc_sig_term_state(&T));
    TEST_ASSERT_FALSE(has_event(LC_SIG_EV_REGISTERED));
    uint8_t sqn = ID.sqn[5];
    run_ms(5000); /* ...and a real AKA follows */
    TEST_ASSERT_TRUE(registered_both());
    TEST_ASSERT_NOT_EQUAL(sqn, ID.sqn[5]);
}

static void test_replayed_auth_rsp_before_reg_req_cannot_register(void)
{
    replayed_auth_rsp_before_reg_req(0);
    replayed_auth_rsp_before_reg_req(1);
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
    RUN_TEST(test_chan_list_pushed_after_every_registration);
    RUN_TEST(test_cfg_ver_change_gets_the_new_list_over_a_lossy_link);
    RUN_TEST(test_config_request_without_a_list);
    RUN_TEST(test_part97_list_survives_a_lost_reg_ack);
    RUN_TEST(test_chan_list_crossing_call_setup_survives_lost_call_proc);
    RUN_TEST(test_two_terminals_ack_does_not_clear_the_others_pending_push);
    RUN_TEST(test_queued_chan_list_refreshes_on_a_new_set);
    RUN_TEST(test_network_restart_after_a_call_registers_again_at_once);
    RUN_TEST(test_terminal_reboot_between_act_ack_and_reg_req_registers_at_once);
    RUN_TEST(test_reregistration_crossing_a_chan_list_push_registers_at_once);
    RUN_TEST(test_new_list_while_a_stale_push_is_in_flight_still_arrives);
    RUN_TEST(test_chan_list_lost_to_a_lost_reg_ack_is_pushed_again);
    RUN_TEST(test_lost_release_complete_then_reregistration);
    RUN_TEST(test_replayed_auth_rsp_before_reg_req_cannot_register);
    return UNITY_END();
}
