#include "unity.h"

#include "lc_clock.h"
#include "lc_phy.h"

void setUp(void) {}
void tearDown(void) {}

#define T0      5000000ull        /* local µs of the first edge */
#define UTC0    (LC_EPOCH_UNIX_S + 3u * 1000u) /* frame-aligned second */
#define HOLD_US 30000000u

static lc_clock_t clk;

/* Feed n edges at a local period of period_us starting at T0; label edge 0. */
static void lock_at(uint32_t period_us, int edges)
{
    lc_clock_init(&clk, HOLD_US);
    for (int i = 0; i < edges; i++) {
        lc_clock_on_pps(&clk, T0 + (uint64_t)i * period_us);
        if (i == 0) {
            TEST_ASSERT_EQUAL_INT(0, lc_clock_on_time(&clk, UTC0, T0 + 200000));
        }
    }
}

static void test_frame_helpers_match_spec_formula(void)
{
    uint64_t epoch_us = (uint64_t)LC_EPOCH_UNIX_S * 1000000u;
    TEST_ASSERT_EQUAL_UINT32(0, lc_frame_from_unix_us(epoch_us));
    TEST_ASSERT_EQUAL_UINT32(0, lc_frame_from_unix_us(epoch_us + 119999));
    TEST_ASSERT_EQUAL_UINT32(1, lc_frame_from_unix_us(epoch_us + 120000));
    TEST_ASSERT_EQUAL_UINT32(25, lc_frame_from_unix_us(epoch_us + 3000000)); /* 25 frames = 3 s */
    TEST_ASSERT_EQUAL_UINT32(0, lc_frame_from_unix_us(1000));                 /* before epoch */
    TEST_ASSERT_EQUAL_UINT64(epoch_us + 25u * 120000u, lc_frame_start_unix_us(25));
}

static void test_unlocked_until_three_edges(void)
{
    uint64_t t;
    lock_at(1000000, 2);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_UNLOCKED, clk.state);
    TEST_ASSERT_EQUAL_INT(-1, lc_clock_frame_start_us(&clk, 25000, &t));
    lc_clock_on_pps(&clk, T0 + 2000000);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_LOCKED, clk.state);
}

static void test_frame_start_on_ideal_clock(void)
{
    lock_at(1000000, 3);
    /* last edge is T0 + 2 s = UTC0 + 2. UTC0 is frame 25000 (3000 s * 25/3). */
    uint64_t t;
    TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, 25000, &t));
    TEST_ASSERT_EQUAL_UINT64(T0, t);
    TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, 25001, &t));
    TEST_ASSERT_EQUAL_UINT64(T0 + 120000, t);
    TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, 25025, &t)); /* UTC0 + 3 s */
    TEST_ASSERT_EQUAL_UINT64(T0 + 3000000, t);
}

static void test_frame_start_scales_by_measured_period(void)
{
    /* Local crystal runs 20 ppm fast: 1 true second = 1000020 local µs. */
    lock_at(1000020, 6);
    uint64_t t;
    TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, 25000 + 25 * 10, &t)); /* UTC0 + 30 s */
    TEST_ASSERT_UINT64_WITHIN(2, T0 + 30ull * 1000020u, t);
}

static void test_frame_at_is_inverse_of_frame_start(void)
{
    lock_at(1000020, 5);
    for (uint32_t f = 24990; f < 25200; f += 7) {
        uint64_t t;
        uint32_t back;
        TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, f, &t));
        TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_at(&clk, t + 10, &back));
        TEST_ASSERT_EQUAL_UINT32(f, back);
        TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_at(&clk, t + 119990, &back));
        TEST_ASSERT_EQUAL_UINT32(f, back);
    }
}

static void test_glitch_edge_is_ignored(void)
{
    lock_at(1000000, 3);
    lc_clock_on_pps(&clk, T0 + 2000000 + 400000); /* noise 0.4 s after an edge */
    TEST_ASSERT_EQUAL_UINT64(T0 + 2000000, clk.last_edge_us);
    lc_clock_on_pps(&clk, T0 + 3000000);
    TEST_ASSERT_EQUAL_UINT32(UTC0 + 3, clk.anchor_unix_s);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_LOCKED, clk.state);
}

static void test_missed_edge_keeps_time_label(void)
{
    lock_at(1000000, 3);
    lc_clock_on_pps(&clk, T0 + 4000000); /* edge at +3 s missed */
    TEST_ASSERT_EQUAL_UINT32(UTC0 + 4, clk.anchor_unix_s);
    uint64_t t;
    TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, 25025, &t));
    TEST_ASSERT_EQUAL_UINT64(T0 + 3000000, t);
}

static void test_off_grid_edge_drops_label_until_relabelled(void)
{
    lock_at(1000000, 3);
    lc_clock_on_pps(&clk, T0 + 2000000 + 1300000); /* 1.3 s: not a whole second */
    uint64_t t;
    TEST_ASSERT_EQUAL_INT(-1, lc_clock_frame_start_us(&clk, 25025, &t));
    TEST_ASSERT_EQUAL_INT(0, lc_clock_on_time(&clk, UTC0 + 3, T0 + 3300000 + 100000));
    TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, 25025, &t));
    TEST_ASSERT_EQUAL_UINT64(T0 + 3300000, t);
}

static void test_holdover_then_unlocked(void)
{
    lock_at(1000000, 3);
    uint64_t last = T0 + 2000000;
    lc_clock_tick(&clk, last + 1400000);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_LOCKED, clk.state);
    lc_clock_tick(&clk, last + 1600000);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_HOLDOVER, clk.state);

    uint64_t t;
    TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, 25100, &t)); /* still usable */

    lc_clock_tick(&clk, last + HOLD_US + 1);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_UNLOCKED, clk.state);
    TEST_ASSERT_EQUAL_INT(-1, lc_clock_frame_start_us(&clk, 25100, &t));
}

static void test_recovers_from_holdover_after_three_edges(void)
{
    lock_at(1000000, 3);
    lc_clock_tick(&clk, T0 + 4000000);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_HOLDOVER, clk.state);
    lc_clock_on_pps(&clk, T0 + 5000000); /* 3 s gap, still on the grid */
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_HOLDOVER, clk.state);
    TEST_ASSERT_EQUAL_UINT32(UTC0 + 5, clk.anchor_unix_s);
    lc_clock_on_pps(&clk, T0 + 6000000);
    lc_clock_on_pps(&clk, T0 + 7000000);
    TEST_ASSERT_EQUAL_UINT8(LC_CLOCK_LOCKED, clk.state);
}

static void test_time_label_rejected_when_stale(void)
{
    lc_clock_init(&clk, HOLD_US);
    TEST_ASSERT_EQUAL_INT(-1, lc_clock_on_time(&clk, UTC0, T0)); /* no edge yet */
    lc_clock_on_pps(&clk, T0);
    TEST_ASSERT_EQUAL_INT(-1, lc_clock_on_time(&clk, UTC0, T0 + 950000));
    TEST_ASSERT_EQUAL_INT(0, lc_clock_on_time(&clk, UTC0, T0 + 850000));
}

static void test_far_frames_rejected(void)
{
    lock_at(1000000, 3);
    uint64_t t;
    uint32_t hour_frames = 3600u * 1000u / 120u;
    TEST_ASSERT_EQUAL_INT(0, lc_clock_frame_start_us(&clk, 25000 + hour_frames - 100, &t));
    TEST_ASSERT_EQUAL_INT(-1, lc_clock_frame_start_us(&clk, 25000 + hour_frames + 100, &t));
    TEST_ASSERT_EQUAL_INT(-1, lc_clock_frame_start_us(&clk, 0xFFFFFFFFu, &t));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_helpers_match_spec_formula);
    RUN_TEST(test_unlocked_until_three_edges);
    RUN_TEST(test_frame_start_on_ideal_clock);
    RUN_TEST(test_frame_start_scales_by_measured_period);
    RUN_TEST(test_frame_at_is_inverse_of_frame_start);
    RUN_TEST(test_glitch_edge_is_ignored);
    RUN_TEST(test_missed_edge_keeps_time_label);
    RUN_TEST(test_off_grid_edge_drops_label_until_relabelled);
    RUN_TEST(test_holdover_then_unlocked);
    RUN_TEST(test_recovers_from_holdover_after_three_edges);
    RUN_TEST(test_time_label_rejected_when_stale);
    RUN_TEST(test_far_frames_rejected);
    return UNITY_END();
}
