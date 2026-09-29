/* ocb_time against the board's own oc_clock (opencell-firmware#2): a W12
 * with its internal 1 Hz PPS gets a timebase within a few seconds whatever
 * the phase of its edge against the host second, and never takes a label
 * that disagrees with its count. */
#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "oc_clock.h"
#include "oc_link.h"
#include "ocb_time.h"

void setUp(void) {}
void tearDown(void) {}

#define LAT_US   4000u /* USB, one way */
#define HOST0_US 1790000000000000ull

typedef struct {
    oc_clock_t c;
    uint64_t   phase_us; /* its edges fall this far after each host second */
    uint64_t   next_edge;
} board_t;

typedef struct {
    uint64_t at; /* host time it arrives */
    uint32_t s;
    uint8_t  seq;
} label_t;

/* Board time is host time here (no crystal error: the phase is what
 * matters). Runs the host loop every 5 ms for ms, the way ocbench and
 * oc-cell do: STATUS once a second, labels as ocb_time says. Returns the
 * host time the board's clock became usable, or 0. */
static uint64_t run(board_t *b, ocb_time_t *t, uint64_t start, uint32_t ms, uint32_t *refused)
{
    label_t inflight[8];
    int n = 0;
    uint8_t seq = 0;
    uint32_t f;
    int has_time = oc_clock_frame_at(&b->c, start, &f) == 0; /* what the host learned from the last STATUS */
    uint64_t got = 0;
    for (uint64_t now = start; now < start + (uint64_t)ms * 1000u; now += 5000u) {
        while (b->next_edge <= now) {
            oc_clock_on_pps(&b->c, b->next_edge);
            b->next_edge += 1000000u;
        }
        for (int i = 0; i < n; i++) { /* labels arriving at the board; their ACKs come back at once */
            if (inflight[i].at > now) continue;
            int ok = oc_clock_on_time(&b->c, inflight[i].s, inflight[i].at) == 0;
            if (!ok && refused != NULL) (*refused)++;
            ocb_time_ack(t, inflight[i].seq, ok ? OC_ACK_OK : OC_ACK_ERR_LATE, now + LAT_US);
            inflight[i--] = inflight[--n];
        }
        if ((now - start) % 1000000u == 250000u) has_time = oc_clock_frame_at(&b->c, now, &f) == 0; /* STATUS */
        if (got == 0 && oc_clock_frame_at(&b->c, now, &f) == 0) got = now;
        uint32_t s = ocb_time_due(t, now, has_time);
        if (s != 0 && n < 8) {
            ocb_time_sent(t, seq, now);
            inflight[n++] = (label_t){ now + LAT_US, s, seq++ };
        }
    }
    return got;
}

static void board_init(board_t *b, uint64_t start, uint32_t phase_ms)
{
    oc_clock_init(&b->c, 5000000u);
    b->phase_us = (uint64_t)phase_ms * 1000u;
    b->next_edge = start - start % 1000000u + b->phase_us;
    if (b->next_edge < start) b->next_edge += 1000000u;
}

/* The bug: labels 0.1 s after the host second, edges 0.1-0.2 s after it. */
static void test_the_old_rule_never_labels_a_late_edge(void)
{
    board_t b;
    ocb_time_t t;
    uint32_t refused = 0;
    board_init(&b, HOST0_US, 150);
    ocb_time_init(&t, 0); /* the GPS rule, which ocbench used for --internal too */
    TEST_ASSERT_EQUAL_UINT64(0, run(&b, &t, HOST0_US, 10000, &refused));
    TEST_ASSERT_TRUE(refused >= 9);
}

static void test_every_edge_phase_gets_a_timebase_within_3_s(void)
{
    for (uint32_t phase = 0; phase < 1000; phase += 10) {
        board_t b;
        ocb_time_t t;
        board_init(&b, HOST0_US, phase);
        ocb_time_init(&t, 1);
        uint64_t got = run(&b, &t, HOST0_US, 6000, NULL);
        char msg[48];
        snprintf(msg, sizeof(msg), "edge phase %u ms", phase);
        TEST_ASSERT_TRUE_MESSAGE(got != 0, msg);
        /* 3 edges to lock, then at most one refusal and a 300 ms retry */
        TEST_ASSERT_TRUE_MESSAGE(got - HOST0_US <= 3600000u, msg);
        TEST_ASSERT_TRUE_MESSAGE(t.accepted == 1, msg);
    }
}

/* Once the board counts on its own, nothing more is sent: no second label
 * can disagree with its count (three would re-anchor it, oc_clock.h). */
static void test_no_label_once_the_board_has_time(void)
{
    board_t b;
    ocb_time_t t;
    board_init(&b, HOST0_US, 150);
    ocb_time_init(&t, 1);
    TEST_ASSERT_TRUE(run(&b, &t, HOST0_US, 5000, NULL) != 0);
    uint32_t sent = t.sent;
    uint32_t anchor = b.c.anchor_unix_s;
    run(&b, &t, HOST0_US + 5000000u, 20000, NULL);
    TEST_ASSERT_EQUAL_UINT32(sent, t.sent);
    TEST_ASSERT_EQUAL_UINT32(anchor + 20u, b.c.anchor_unix_s); /* counted on by itself, 20 edges */
}

/* A board that reboots (STATUS frame 0 again) is labelled again. */
static void test_a_rebooted_board_is_labelled_again(void)
{
    board_t b;
    ocb_time_t t;
    board_init(&b, HOST0_US, 420);
    ocb_time_init(&t, 1);
    TEST_ASSERT_TRUE(run(&b, &t, HOST0_US, 5000, NULL) != 0);
    board_init(&b, HOST0_US + 5000000u, 870); /* a new boot: a new phase, no time */
    TEST_ASSERT_TRUE(run(&b, &t, HOST0_US + 5000000u, 6000, NULL) != 0);
    TEST_ASSERT_EQUAL_UINT32(2, t.accepted);
}

static void test_a_lost_ack_is_sent_again(void)
{
    ocb_time_t t;
    ocb_time_init(&t, 1);
    uint64_t now = HOST0_US + 300000u;
    uint32_t s = ocb_time_due(&t, now, 0);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(now / 1000000u), s);
    ocb_time_sent(&t, 7, now);
    TEST_ASSERT_EQUAL_UINT32(0, ocb_time_due(&t, now + 100000u, 0)); /* waiting for its ACK */
    ocb_time_ack(&t, 8, OC_ACK_OK, now + 100000u);                  /* someone else's ACK */
    TEST_ASSERT_EQUAL_UINT32(0, ocb_time_due(&t, now + 200000u, 0));
    TEST_ASSERT_TRUE(ocb_time_due(&t, now + OCB_TIME_ACK_US, 0) != 0);
}

/* GPS PPS: one label per host second, 0.1-0.8 s into it, as before. */
static void test_gps_labels_once_a_second(void)
{
    ocb_time_t t;
    ocb_time_init(&t, 0);
    TEST_ASSERT_EQUAL_UINT32(0, ocb_time_due(&t, HOST0_US + 50000u, 1));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(HOST0_US / 1000000u), ocb_time_due(&t, HOST0_US + 150000u, 1));
    TEST_ASSERT_EQUAL_UINT32(0, ocb_time_due(&t, HOST0_US + 200000u, 1));
    TEST_ASSERT_EQUAL_UINT32(0, ocb_time_due(&t, HOST0_US + 900000u, 1));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(HOST0_US / 1000000u) + 1u, ocb_time_due(&t, HOST0_US + 1300000u, 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_old_rule_never_labels_a_late_edge);
    RUN_TEST(test_every_edge_phase_gets_a_timebase_within_3_s);
    RUN_TEST(test_no_label_once_the_board_has_time);
    RUN_TEST(test_a_rebooted_board_is_labelled_again);
    RUN_TEST(test_a_lost_ack_is_sent_again);
    RUN_TEST(test_gps_labels_once_a_second);
    return UNITY_END();
}
