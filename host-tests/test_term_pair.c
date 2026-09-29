#include "unity.h"

#include <string.h>

#include "oc_term_pair.h"

void setUp(void) {}
void tearDown(void) {}

/* A scripted random source: returns vals[] in order, then repeats the last. */
typedef struct {
    const uint32_t *vals;
    unsigned n, i, calls;
} script_t;

static uint32_t scripted(void *ctx)
{
    script_t *s = ctx;
    s->calls++;
    uint32_t v = s->vals[s->i < s->n ? s->i : s->n - 1];
    if (s->i < s->n) {
        s->i++;
    }
    return v;
}

/* xorshift32, for the distribution test */
static uint32_t xorshift(void *ctx)
{
    uint32_t *x = ctx;
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x;
}

static void test_draw_maps_into_six_digits(void)
{
    static const uint32_t v[] = { 0, 999999, 1000000, 4293999999u };
    script_t s = { v, 4, 0, 0 };
    TEST_ASSERT_EQUAL_UINT32(0, oc_term_pair_draw(scripted, &s));
    TEST_ASSERT_EQUAL_UINT32(999999, oc_term_pair_draw(scripted, &s));
    TEST_ASSERT_EQUAL_UINT32(0, oc_term_pair_draw(scripted, &s));
    TEST_ASSERT_EQUAL_UINT32(999999, oc_term_pair_draw(scripted, &s));
}

static void test_draw_rejects_the_biased_tail(void)
{
    /* 4294000000 .. 2^32-1 would favour codes 0..967295: redrawn */
    static const uint32_t v[] = { 4294000000u, 0xFFFFFFFFu, 123456u };
    script_t s = { v, 3, 0, 0 };
    TEST_ASSERT_EQUAL_UINT32(123456, oc_term_pair_draw(scripted, &s));
    TEST_ASSERT_EQUAL_UINT(3, s.calls);
}

static void test_draw_is_roughly_uniform(void)
{
    /* 200000 draws into 10 buckets by leading digit: each within 3% of 20000 */
    uint32_t x = 0x12345678u;
    unsigned bucket[10] = { 0 };
    for (int i = 0; i < 200000; i++) {
        uint32_t c = oc_term_pair_draw(xorshift, &x);
        TEST_ASSERT_TRUE(c < 1000000u);
        bucket[c / 100000u]++;
    }
    for (int b = 0; b < 10; b++) {
        TEST_ASSERT_UINT_WITHIN(600, 20000, bucket[b]);
    }
}

static void test_new_code_at_boot_disconnect_and_failure(void)
{
    static const uint32_t v[] = { 111111, 222222, 333333, 444444 };
    script_t s = { v, 4, 0, 0 };
    oc_term_pair_t p;
    oc_term_pair_init(&p, scripted, &s);
    TEST_ASSERT_EQUAL_UINT32(111111, oc_term_pair_code(&p));
    oc_term_pair_disconnected(&p);
    TEST_ASSERT_EQUAL_UINT32(222222, oc_term_pair_code(&p));
    oc_term_pair_failed(&p, 1000);
    TEST_ASSERT_EQUAL_UINT32(333333, oc_term_pair_code(&p));
    oc_term_pair_succeeded(&p); /* success keeps the code: it changes when the phone leaves */
    TEST_ASSERT_EQUAL_UINT32(333333, oc_term_pair_code(&p));
}

static void test_three_failures_in_60_s_lock_for_60_s(void)
{
    uint32_t x = 1;
    oc_term_pair_t p;
    oc_term_pair_init(&p, xorshift, &x);
    const uint64_t t0 = 5000000;
    oc_term_pair_failed(&p, t0);
    oc_term_pair_failed(&p, t0 + 20000000);
    TEST_ASSERT_FALSE(oc_term_pair_locked(&p, t0 + 20000000));
    oc_term_pair_failed(&p, t0 + 59000000);
    TEST_ASSERT_TRUE(oc_term_pair_locked(&p, t0 + 59000000));
    TEST_ASSERT_EQUAL_UINT32(60, oc_term_pair_lock_left_s(&p, t0 + 59000000));
    TEST_ASSERT_EQUAL_UINT32(43, oc_term_pair_lock_left_s(&p, t0 + 59000000 + 17500000)); /* 42.5 s: rounded up */
    TEST_ASSERT_EQUAL_UINT32(42, oc_term_pair_lock_left_s(&p, t0 + 59000000 + 18000000));
    TEST_ASSERT_TRUE(oc_term_pair_locked(&p, t0 + 59000000 + 59999999));
    TEST_ASSERT_FALSE(oc_term_pair_locked(&p, t0 + 59000000 + 60000000));
    TEST_ASSERT_EQUAL_UINT32(0, oc_term_pair_lock_left_s(&p, t0 + 59000000 + 60000000));
}

static void test_failures_older_than_60_s_do_not_count(void)
{
    uint32_t x = 1;
    oc_term_pair_t p;
    oc_term_pair_init(&p, xorshift, &x);
    oc_term_pair_failed(&p, 0);
    oc_term_pair_failed(&p, 30000000);
    oc_term_pair_failed(&p, 60000000); /* the first is now 60 s old: 2 in the window */
    TEST_ASSERT_FALSE(oc_term_pair_locked(&p, 60000000));
    oc_term_pair_failed(&p, 61000000); /* 30, 60, 61 */
    TEST_ASSERT_TRUE(oc_term_pair_locked(&p, 61000000));
}

static void test_success_forgets_failures(void)
{
    uint32_t x = 1;
    oc_term_pair_t p;
    oc_term_pair_init(&p, xorshift, &x);
    oc_term_pair_failed(&p, 1000000);
    oc_term_pair_failed(&p, 2000000);
    oc_term_pair_succeeded(&p);
    oc_term_pair_failed(&p, 3000000);
    TEST_ASSERT_FALSE(oc_term_pair_locked(&p, 3000000));
}

static void test_count_starts_over_after_a_lock_out(void)
{
    uint32_t x = 1;
    oc_term_pair_t p;
    oc_term_pair_init(&p, xorshift, &x);
    oc_term_pair_failed(&p, 1000000);
    oc_term_pair_failed(&p, 2000000);
    oc_term_pair_failed(&p, 3000000); /* locked until 63 s */
    oc_term_pair_failed(&p, 64000000); /* one failure after the lock: not locked again */
    TEST_ASSERT_FALSE(oc_term_pair_locked(&p, 64000000));
}

static void test_not_locked_at_boot(void)
{
    uint32_t x = 1;
    oc_term_pair_t p;
    oc_term_pair_init(&p, xorshift, &x);
    TEST_ASSERT_FALSE(oc_term_pair_locked(&p, 0));
    TEST_ASSERT_EQUAL_UINT32(0, oc_term_pair_lock_left_s(&p, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_draw_maps_into_six_digits);
    RUN_TEST(test_draw_rejects_the_biased_tail);
    RUN_TEST(test_draw_is_roughly_uniform);
    RUN_TEST(test_new_code_at_boot_disconnect_and_failure);
    RUN_TEST(test_three_failures_in_60_s_lock_for_60_s);
    RUN_TEST(test_failures_older_than_60_s_do_not_count);
    RUN_TEST(test_success_forgets_failures);
    RUN_TEST(test_count_starts_over_after_a_lock_out);
    RUN_TEST(test_not_locked_at_boot);
    return UNITY_END();
}
