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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_reply_clears_pending);
    RUN_TEST(test_retransmit_then_give_up);
    RUN_TEST(test_no_retransmit_while_link_cannot_send);
    RUN_TEST(test_duplicate_request_answered_from_cache);
    RUN_TEST(test_reply_type_table);
    return UNITY_END();
}
