#include "unity.h"

#include <string.h>

#include "lc_sig_chan.h"

void setUp(void) {}
void tearDown(void) {}

static lc_sig_chan_t term, net;

/* Move every waiting payload from a to b (optionally dropping all). Returns
 * how many new messages b produced; the last one is in *m. */
static int pump(lc_sig_chan_t *a, lc_sig_chan_t *b, int drop, lc_sig_msg_t *m, uint64_t now)
{
    const uint8_t *p;
    uint8_t n;
    int got = 0;
    while (lc_sig_chan_peek(a, &p, &n) == 0) {
        uint8_t copy[LC_SIG_LINK_MAX];
        memcpy(copy, p, n);
        lc_sig_chan_pop(a);
        if (!drop && lc_sig_chan_rx(b, copy, n, m, now) == 1) got++;
    }
    return got;
}

static lc_sig_msg_t msg(uint8_t type)
{
    lc_sig_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    if (type == LC_SIG_CALL_SETUP) { /* decoding checks the number */
        TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+883160655500100", 16, m.u.call_setup.called));
    }
    return m;
}

static void setup(void)
{
    lc_sig_chan_init(&term, 0);
    lc_sig_chan_init(&net, 1);
}

static void test_request_reply_clears_pending(void)
{
    setup();
    lc_sig_msg_t r = msg(LC_SIG_REG_REQ), got;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&term, &r, 0));
    TEST_ASSERT_TRUE(lc_sig_chan_busy(&term));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_chan_send(&term, &r, 0)); /* one request at a time */
    TEST_ASSERT_EQUAL_INT(1, pump(&term, &net, 0, &got, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, got.type);
    lc_sig_msg_t a = msg(LC_SIG_AUTH_REQ);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&net, &a, 0));
    TEST_ASSERT_EQUAL_INT(1, pump(&net, &term, 0, &got, 0));
    TEST_ASSERT_FALSE(lc_sig_chan_busy(&term));
}

static void test_retransmit_then_give_up(void)
{
    setup();
    lc_sig_msg_t r = msg(LC_SIG_REG_REQ), got;
    uint8_t expired = 0;
    lc_sig_chan_send(&term, &r, 0);
    pump(&term, &net, 1, &got, 0);
    for (int i = 1; i <= 3; i++) {
        TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_tick(&term, (uint64_t)i * LC_SIG_RETX_US, 1, &expired));
        const uint8_t *p;
        uint8_t n;
        TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_peek(&term, &p, &n)); /* resent */
        pump(&term, &net, 1, &got, 0);
    }
    TEST_ASSERT_EQUAL_INT(1, lc_sig_chan_tick(&term, 4 * LC_SIG_RETX_US, 1, &expired));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REQ, expired);
    TEST_ASSERT_FALSE(lc_sig_chan_busy(&term));
}

static void test_no_retransmit_while_link_cannot_send(void)
{
    setup();
    lc_sig_msg_t r = msg(LC_SIG_REG_REQ);
    uint8_t expired = 0;
    lc_sig_chan_send(&term, &r, 0);
    for (int i = 1; i <= 10; i++) {
        TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_tick(&term, (uint64_t)i * LC_SIG_RETX_US, 0, &expired));
    }
    TEST_ASSERT_TRUE(lc_sig_chan_busy(&term)); /* still waiting, tries not used */
}

static void test_duplicate_request_answered_from_cache(void)
{
    setup();
    lc_sig_msg_t r = msg(LC_SIG_REG_REQ), got;
    uint8_t expired;
    lc_sig_chan_send(&term, &r, 0);
    TEST_ASSERT_EQUAL_INT(1, pump(&term, &net, 0, &got, 0)); /* network processes it once */
    lc_sig_msg_t rej = msg(LC_SIG_REG_REJ);
    lc_sig_chan_send(&net, &rej, 0);
    pump(&net, &term, 1, &got, 0);                            /* the reply is lost */
    lc_sig_chan_tick(&term, LC_SIG_RETX_US, 1, &expired);     /* terminal resends REG_REQ */
    TEST_ASSERT_EQUAL_INT(0, pump(&term, &net, 0, &got, 0));  /* duplicate: not processed again */
    TEST_ASSERT_EQUAL_INT(1, pump(&net, &term, 0, &got, 0));  /* ...but the cached reply goes out */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REJ, got.type);
    TEST_ASSERT_FALSE(lc_sig_chan_busy(&term));
}

static void test_reply_type_table(void)
{
    TEST_ASSERT_TRUE(lc_sig_is_request(LC_SIG_CALL_SETUP));
    TEST_ASSERT_FALSE(lc_sig_is_request(LC_SIG_CALL_PROC));
    TEST_ASSERT_TRUE(lc_sig_is_reply(LC_SIG_CALL_SETUP, LC_SIG_RELEASE)); /* busy */
    TEST_ASSERT_TRUE(lc_sig_is_reply(LC_SIG_SETUP_IND, LC_SIG_ALERTING));
    TEST_ASSERT_FALSE(lc_sig_is_reply(LC_SIG_RELEASE, LC_SIG_ALERTING));
}

static void test_retransmit_waits_for_queue_room(void)
{
    setup();
    lc_sig_msg_t r = msg(LC_SIG_REG_REQ);
    uint8_t expired = 0;

    /* Send a request from term */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&term, &r, 0));

    /* Simulate a full queue by setting txq_count to max, without pumping.
     * This forces enqueue() to fail on the next retransmit. */
    term.txq_count = LC_SIG_TXQ;

    /* Try to retransmit when queue is full. With the fix, enqueue() fails
     * and we return immediately without advancing pend_tries or pend_due. */
    uint8_t old_pend_tries = term.pend_tries;
    uint64_t old_pend_due = term.pend_due;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_tick(&term, LC_SIG_RETX_US, 1, &expired));

    /* Verify pend_tries and pend_due were NOT advanced (enqueue failed) */
    TEST_ASSERT_EQUAL_UINT8(old_pend_tries, term.pend_tries);
    TEST_ASSERT_EQUAL_UINT64(old_pend_due, term.pend_due);
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_TXQ, term.txq_count); /* queue still full */

    /* Now empty the queue to make room */
    term.txq_count = 0;
    term.txq_head = 0;

    /* Tick again at the same time; retransmit should now succeed */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_tick(&term, LC_SIG_RETX_US, 1, &expired));

    /* Verify pend_tries advanced and queue has the retransmission */
    TEST_ASSERT_EQUAL_UINT8(old_pend_tries + 1, term.pend_tries);
    TEST_ASSERT_GREATER_THAN_UINT8(0, term.txq_count); /* queue has retransmit */
}

/* Final review C1: a reply that crosses the requester's retransmission must
 * not start a duplicate ping-pong. The requester gets the reply and then a
 * second copy of it (answering the duplicate request); a duplicate of a
 * non-request is never answered, so the exchange dies out. */
static void test_crossing_reply_does_not_ping_pong(void)
{
    setup();
    lc_sig_msg_t r = msg(LC_SIG_REG_REQ), got;
    uint8_t expired;
    lc_sig_chan_send(&term, &r, 0);
    TEST_ASSERT_EQUAL_INT(1, pump(&term, &net, 0, &got, 0));
    lc_sig_msg_t rej = msg(LC_SIG_REG_REJ);
    lc_sig_chan_send(&net, &rej, 0);                      /* the reply is in the air... */
    lc_sig_chan_tick(&term, LC_SIG_RETX_US, 1, &expired); /* ...as the terminal retransmits */
    int frames = 0;
    for (int round = 0; round < 20; round++) {
        const uint8_t *p;
        uint8_t n;
        if (lc_sig_chan_peek(&term, &p, &n) == 0) frames++;
        if (lc_sig_chan_peek(&net, &p, &n) == 0) frames++;
        pump(&net, &term, 0, &got, 0);
        pump(&term, &net, 0, &got, 0);
    }
    TEST_ASSERT_FALSE(lc_sig_chan_busy(&term));
    TEST_ASSERT_TRUE(frames <= 3); /* retransmission, reply, one answer to the duplicate */
    const uint8_t *p;
    uint8_t n;
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_chan_peek(&term, &p, &n));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_chan_peek(&net, &p, &n));
}

static void keyed(void)
{
    uint8_t ki[16], ke[16];
    memset(ki, 0x31, 16);
    memset(ke, 0x32, 16);
    lc_sig_sec_key(&term.sec, ki, ke, 1);
    lc_sig_sec_key(&net.sec, ki, ke, 1);
}

/* Final review C1: the reply is lost, another message follows it, then the
 * request is retransmitted: the answer is the reply to that request (fresh
 * enough to pass the replay window), not the last message sent. */
static void test_duplicate_request_gets_its_own_reply_not_the_last_message(void)
{
    setup();
    keyed();
    lc_sig_msg_t cs = msg(LC_SIG_CALL_SETUP), got;
    cs.u.call_setup.ref = 7;
    uint8_t expired;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&term, &cs, 0));
    TEST_ASSERT_EQUAL_INT(1, pump(&term, &net, 0, &got, 0));
    lc_sig_msg_t proc = msg(LC_SIG_CALL_PROC);
    proc.u.call_proc.ref = 7;
    proc.u.call_proc.call_id = 42;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&net, &proc, 0));
    pump(&net, &term, 1, &got, 0); /* CALL_PROC lost */
    lc_sig_msg_t al = msg(LC_SIG_ALERTING);
    al.u.call.call_id = 42;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&net, &al, 0));
    TEST_ASSERT_EQUAL_INT(1, pump(&net, &term, 0, &got, 0)); /* ALERTING arrives: not CALL_SETUP's reply */
    TEST_ASSERT_TRUE(lc_sig_chan_busy(&term));
    lc_sig_chan_tick(&term, LC_SIG_RETX_US, 1, &expired);    /* CALL_SETUP again */
    TEST_ASSERT_EQUAL_INT(0, pump(&term, &net, 0, &got, 0)); /* a duplicate: not processed again */
    memset(&got, 0, sizeof(got));
    TEST_ASSERT_EQUAL_INT(1, pump(&net, &term, 0, &got, 0));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_CALL_PROC, got.type);
    TEST_ASSERT_EQUAL_UINT32(42, got.u.call_proc.call_id);
    TEST_ASSERT_FALSE(lc_sig_chan_busy(&term));
}

/* A duplicate of a message that isn't a request is never answered. */
static void test_duplicate_non_request_not_answered(void)
{
    setup();
    keyed();
    lc_sig_msg_t al = msg(LC_SIG_ALERTING), got;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&net, &al, 0));
    const uint8_t *p;
    uint8_t n, copy[LC_SIG_LINK_MAX];
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_peek(&net, &p, &n));
    memcpy(copy, p, n);
    lc_sig_msg_t cs = msg(LC_SIG_CALL_SETUP);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&term, &cs, 0)); /* the terminal has sent something */
    pump(&term, &net, 1, &got, 0);
    TEST_ASSERT_EQUAL_INT(1, pump(&net, &term, 0, &got, 0));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_rx(&term, copy, n, &got, 0)); /* ALERTING again */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_chan_peek(&term, &p, &n));      /* nothing answered */
}

/* Fix round 2 (Task 8), finding 4: a repeat is the very same frame. A
 * request is retransmitted from its sealed bytes (pend_msg), so a frame that
 * reuses a cached request's seq and type but not its bytes is someone else's
 * (anyone can send a prot-0 AUTH_RSP): it must not draw the cached reply
 * (here, it could hand a re-registering terminal a REG_ACK without an AKA). */
static void test_repeat_needs_the_identical_request(void)
{
    setup();
    lc_sig_msg_t r = msg(LC_SIG_AUTH_RSP), got;
    memset(r.u.auth_rsp.res, 0x5a, 8);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&term, &r, 0));
    uint8_t seq = term.pend_seq;
    TEST_ASSERT_EQUAL_INT(1, pump(&term, &net, 0, &got, 0));
    lc_sig_msg_t rej = msg(LC_SIG_REG_REJ);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&net, &rej, 0));
    pump(&net, &term, 1, &got, 0); /* the reply is lost */

    lc_sig_msg_t f = msg(LC_SIG_AUTH_RSP); /* same seq, same type, another RES */
    memset(f.u.auth_rsp.res, 0xa5, 8);
    lc_sig_sec_t sec;
    lc_sig_sec_init(&sec, 0);
    uint8_t buf[LC_SIG_MAX_MSG], frag[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX], flen[LC_SIG_MAX_FRAGS];
    size_t bn = lc_sig_seal(&sec, &f, buf, sizeof(buf));
    uint8_t nf = lc_sig_fragment(buf, bn, seq, frag, flen);
    for (uint8_t i = 0; i < nf; i++) TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_rx(&net, frag[i], flen[i], &got, 0));
    const uint8_t *p;
    uint8_t n;
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_chan_peek(&net, &p, &n)); /* not answered */

    uint8_t expired;
    lc_sig_chan_tick(&term, LC_SIG_RETX_US, 1, &expired);    /* the real retransmission */
    TEST_ASSERT_EQUAL_INT(0, pump(&term, &net, 0, &got, 0)); /* a repeat: not processed again */
    TEST_ASSERT_EQUAL_INT(1, pump(&net, &term, 0, &got, 0)); /* ...but answered from the cache */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_REG_REJ, got.type);
}

/* Fix round 3, B: a reply that is itself a request (RELEASE answering
 * CALL_SETUP). Its first copy is lost; the retransmitted CALL_SETUP draws
 * another copy, which the terminal takes (and caches); the terminal's
 * RELEASE_COMPLETE is lost. The network's own retransmissions of RELEASE
 * must be the same bytes as the copy the terminal cached, or they are never
 * answered and the RELEASE expires. */
static void test_reply_that_is_a_request_is_repeated_with_its_own_bytes(void)
{
    setup();
    keyed();
    lc_sig_msg_t cs = msg(LC_SIG_CALL_SETUP), rel = msg(LC_SIG_RELEASE), rc = msg(LC_SIG_RELEASE_COMPLETE), got;
    rel.u.release.cause = LC_SIG_CAUSE_BUSY;
    uint8_t expired = 0;
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&term, &cs, 0));
    TEST_ASSERT_EQUAL_INT(1, pump(&term, &net, 0, &got, 0));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&net, &rel, 0));
    pump(&net, &term, 1, &got, 0);                                       /* lost */
    lc_sig_chan_tick(&term, LC_SIG_RETX_US, 1, &expired);                /* CALL_SETUP again */
    TEST_ASSERT_EQUAL_INT(0, pump(&term, &net, 0, &got, LC_SIG_RETX_US)); /* a repeat: RELEASE again */
    TEST_ASSERT_EQUAL_INT(1, pump(&net, &term, 0, &got, LC_SIG_RETX_US));
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_RELEASE, got.type);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_chan_send(&term, &rc, LC_SIG_RETX_US));
    pump(&term, &net, 1, &got, LC_SIG_RETX_US); /* RELEASE_COMPLETE lost */
    TEST_ASSERT_TRUE(lc_sig_chan_busy(&net));
    lc_sig_chan_tick(&net, 2 * LC_SIG_RETX_US + 1, 1, &expired); /* the network's own retransmission */
    pump(&net, &term, 0, &got, 2 * LC_SIG_RETX_US + 1);
    memset(&got, 0, sizeof(got));
    TEST_ASSERT_EQUAL_INT(1, pump(&term, &net, 0, &got, 2 * LC_SIG_RETX_US + 1)); /* answered from the cache */
    TEST_ASSERT_EQUAL_UINT8(LC_SIG_RELEASE_COMPLETE, got.type);
    TEST_ASSERT_FALSE(lc_sig_chan_busy(&net));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_reply_clears_pending);
    RUN_TEST(test_retransmit_then_give_up);
    RUN_TEST(test_no_retransmit_while_link_cannot_send);
    RUN_TEST(test_duplicate_request_answered_from_cache);
    RUN_TEST(test_reply_type_table);
    RUN_TEST(test_retransmit_waits_for_queue_room);
    RUN_TEST(test_crossing_reply_does_not_ping_pong);
    RUN_TEST(test_duplicate_request_gets_its_own_reply_not_the_last_message);
    RUN_TEST(test_duplicate_non_request_not_answered);
    RUN_TEST(test_repeat_needs_the_identical_request);
    RUN_TEST(test_reply_that_is_a_request_is_repeated_with_its_own_bytes);
    return UNITY_END();
}
