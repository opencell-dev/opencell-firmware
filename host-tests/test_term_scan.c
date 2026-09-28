/* The terminal's scan list (channel-list spec §5): assembly, the walk and
 * the fallback sweep. Pure: no radio. */
#include "unity.h"

#include <string.h>

#include "lc_phy.h"
#include "lc_term_scan.h"

void setUp(void) {}
void tearDown(void) {}

static lc_term_scan_t s;

static uint32_t ch(uint8_t c) { return lc_channel_freq_hz(LC_BAND_915, c); }
static uint8_t src_of(const lc_scan_ent_t *e) { return (uint8_t)((e->flags & LC_SCAN_F_SRC_MASK) >> LC_SCAN_F_SRC_SHIFT); }

/* One dwell: returns its frequency, checks its length, advances. */
static uint32_t dwell(uint32_t want_us, int *round_end)
{
    uint32_t f, d;
    lc_term_scan_next(&s, &f, &d);
    TEST_ASSERT_EQUAL_UINT32(want_us, d);
    int e = lc_term_scan_advance(&s);
    if (round_end != NULL) *round_end = e;
    return f;
}

static void test_defaults_are_ch_0_to_5(void)
{
    lc_term_scan_init(&s);
    TEST_ASSERT_EQUAL_UINT8(1, s.mode); /* Part 15 */
    TEST_ASSERT_EQUAL_UINT8(2, s.fallback_after);
    TEST_ASSERT_EQUAL_UINT8(13, s.fallback_chunk);
    lc_scan_ent_t l[LC_SCAN_MAX];
    TEST_ASSERT_EQUAL_UINT8(6, lc_term_scan_list(&s, l));
    for (uint8_t i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_UINT32(ch(i), l[i].freq_hz);
        TEST_ASSERT_EQUAL_HEX8(LC_SCAN_F_ACTIVE | (LC_SCAN_SRC_DEFAULT << LC_SCAN_F_SRC_SHIFT), l[i].flags);
    }
    for (uint8_t i = 0; i < 6; i++) {
        int end;
        TEST_ASSERT_EQUAL_UINT32(ch(i), dwell(LC_SCAN_DWELL_US, &end));
        TEST_ASSERT_EQUAL_INT(i == 5, end);
    }
    TEST_ASSERT_EQUAL_UINT8(1, s.passes);
}

/* §5.1: last serving, user, network, learned, default; the first of a
 * frequency wins. */
static void test_priority_and_dedupe(void)
{
    lc_term_scan_init(&s);
    s.last = (lc_scan_ent_t){ ch(30), 0 };
    s.n_user = 3;
    s.user[0] = (lc_scan_ent_t){ ch(10), 0 };
    s.user[1] = (lc_scan_ent_t){ ch(30), 0 }; /* the last serving cell again */
    s.user[2] = (lc_scan_ent_t){ ch(0), 0 };  /* a default */
    s.n_net = 2;
    s.net[0] = (lc_scan_ent_t){ ch(40), 0 };
    s.net[1] = (lc_scan_ent_t){ ch(10), 0 };
    s.n_learn = 1;
    s.learn[0] = (lc_scan_ent_t){ ch(20), 0 };
    lc_scan_ent_t l[LC_SCAN_MAX];
    static const uint8_t want_ch[10] = { 30, 10, 0, 40, 20, 1, 2, 3, 4, 5 };
    static const uint8_t want_src[10] = { LC_SCAN_SRC_LAST, LC_SCAN_SRC_USER, LC_SCAN_SRC_USER, LC_SCAN_SRC_NET,
                                          LC_SCAN_SRC_LEARN, LC_SCAN_SRC_DEFAULT, LC_SCAN_SRC_DEFAULT,
                                          LC_SCAN_SRC_DEFAULT, LC_SCAN_SRC_DEFAULT, LC_SCAN_SRC_DEFAULT };
    TEST_ASSERT_EQUAL_UINT8(10, lc_term_scan_list(&s, l));
    for (uint8_t i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_UINT32(ch(want_ch[i]), l[i].freq_hz);
        TEST_ASSERT_EQUAL_UINT8(want_src[i], src_of(&l[i]));
        TEST_ASSERT_TRUE(l[i].flags & LC_SCAN_F_ACTIVE);
    }
    /* the walk follows the list */
    for (uint8_t i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_UINT32(ch(want_ch[i]), dwell(LC_SCAN_DWELL_US, NULL));
    }
    TEST_ASSERT_EQUAL_UINT8(1, s.passes);
}

static void test_per_source_limits(void)
{
    lc_term_scan_init(&s);
    s.n_user = 9; /* a corrupt count never reads past the arrays */
    s.n_net = 40;
    s.n_learn = 7;
    for (uint8_t i = 0; i < LC_SCAN_MAX_USER; i++) s.user[i] = (lc_scan_ent_t){ ch((uint8_t)(6 + i)), 0 };
    for (uint8_t i = 0; i < LC_SCAN_MAX_NET; i++) s.net[i] = (lc_scan_ent_t){ ch((uint8_t)(10 + i)), 0 };
    for (uint8_t i = 0; i < LC_SCAN_MAX_LEARN; i++) s.learn[i] = (lc_scan_ent_t){ ch((uint8_t)(30 + i)), 0 };
    s.last = (lc_scan_ent_t){ ch(50), 0 };
    lc_scan_ent_t l[LC_SCAN_MAX];
    TEST_ASSERT_EQUAL_UINT8(LC_SCAN_MAX, lc_term_scan_list(&s, l)); /* 1 + 4 + 12 + 4 + 6 = 27 */
    TEST_ASSERT_EQUAL_UINT32(ch(50), l[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(ch(9), l[4].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(ch(21), l[16].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(ch(33), l[20].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(ch(5), l[26].freq_hz);
}

/* §5.1 validation at scan time: entries the mode doesn't allow stay in the
 * list, inactive, and never hide an active entry of the same frequency. */
static void test_inactive_entries_per_mode(void)
{
    lc_term_scan_init(&s);
    s.n_user = 2;
    s.user[0] = (lc_scan_ent_t){ ch(0), LC_SCAN_F_FIXED }; /* FIXED: Part 97 only */
    s.user[1] = (lc_scan_ent_t){ 917300000u, 0 };         /* off the grid: never */
    lc_scan_ent_t l[LC_SCAN_MAX];
    TEST_ASSERT_EQUAL_UINT8(8, lc_term_scan_list(&s, l));
    TEST_ASSERT_EQUAL_HEX8(LC_SCAN_F_FIXED | (LC_SCAN_SRC_USER << LC_SCAN_F_SRC_SHIFT), l[0].flags); /* inactive */
    TEST_ASSERT_EQUAL_HEX8(LC_SCAN_SRC_USER << LC_SCAN_F_SRC_SHIFT, l[1].flags);
    TEST_ASSERT_EQUAL_UINT32(ch(0), l[2].freq_hz); /* the default ch 0 is still there */
    TEST_ASSERT_TRUE(l[2].flags & LC_SCAN_F_ACTIVE);
    for (uint8_t i = 0; i < 6; i++) { /* Part 15 scans only the six defaults */
        TEST_ASSERT_EQUAL_UINT32(ch(i), dwell(LC_SCAN_DWELL_US, NULL));
    }

    s.mode = LC_PHY_MODE_PART97;
    lc_term_scan_restart(&s);
    TEST_ASSERT_EQUAL_UINT8(8, lc_term_scan_list(&s, l));
    TEST_ASSERT_EQUAL_HEX8(LC_SCAN_F_FIXED | LC_SCAN_F_ACTIVE | (LC_SCAN_SRC_USER << LC_SCAN_F_SRC_SHIFT), l[0].flags);
    TEST_ASSERT_FALSE(l[1].flags & LC_SCAN_F_ACTIVE);
    /* FIXED ch 0 (short dwell), then the CYCLE default ch 0: a FIXED dwell
     * would miss a cycling cell there */
    TEST_ASSERT_EQUAL_UINT32(ch(0), dwell(LC_SCAN_FIXED_DWELL_US, NULL));
    TEST_ASSERT_EQUAL_UINT32(ch(0), dwell(LC_SCAN_DWELL_US, NULL));
    TEST_ASSERT_EQUAL_UINT32(ch(1), dwell(LC_SCAN_DWELL_US, NULL));
}

static void test_status_fields_follow_the_walk(void)
{
    lc_term_scan_init(&s);
    s.n_net = 1;
    s.net[0] = (lc_scan_ent_t){ ch(40), 0 };
    uint32_t f, d;
    lc_term_scan_next(&s, &f, &d);
    TEST_ASSERT_EQUAL_UINT8(1, s.cur_pos);
    TEST_ASSERT_EQUAL_UINT8(7, s.cur_len);
    TEST_ASSERT_EQUAL_UINT8(LC_SCAN_SRC_NET, s.cur_src);
    TEST_ASSERT_EQUAL_UINT32(ch(40), s.cur_freq);
    lc_term_scan_next(&s, &f, &d); /* same dwell until advance */
    TEST_ASSERT_EQUAL_UINT8(1, s.cur_pos);
    lc_term_scan_advance(&s);
    lc_term_scan_next(&s, &f, &d);
    TEST_ASSERT_EQUAL_UINT8(2, s.cur_pos);
    TEST_ASSERT_EQUAL_UINT8(LC_SCAN_SRC_DEFAULT, s.cur_src);
    TEST_ASSERT_EQUAL_UINT32(ch(0), f);
}

/* §5.3: N list-only rounds, then each round is the list plus the next chunk
 * of grid channels not in the list, round-robin; ch 0-5 are never swept. */
static void test_fallback_after_2_chunk_13(void)
{
    lc_term_scan_init(&s);
    s.n_user = 1;
    s.user[0] = (lc_scan_ent_t){ ch(10), 0 }; /* in the list: never swept */
    for (int r = 0; r < 2; r++) {
        for (uint8_t i = 0; i < 7; i++) dwell(LC_SCAN_DWELL_US, NULL);
    }
    TEST_ASSERT_EQUAL_UINT8(2, s.passes);
    uint8_t swept[52] = { 0 };
    uint8_t order[64], no = 0;
    for (int r = 0; r < 4; r++) { /* rounds 3-6: 7 list + 13 swept */
        for (uint8_t i = 0; i < 7; i++) {
            uint32_t f = dwell(LC_SCAN_DWELL_US, NULL);
            TEST_ASSERT_EQUAL_UINT8(i == 0 ? LC_SCAN_SRC_USER : LC_SCAN_SRC_DEFAULT, s.cur_src);
            (void)f;
        }
        for (uint8_t i = 0; i < 13; i++) {
            int end;
            uint32_t f = dwell(LC_SCAN_DWELL_US, &end);
            TEST_ASSERT_EQUAL_UINT8(LC_SCAN_SRC_SWEEP, s.cur_src);
            TEST_ASSERT_EQUAL_UINT8(20, s.cur_len);
            TEST_ASSERT_EQUAL_INT(i == 12, end);
            uint8_t c = lc_channel_of_freq(LC_BAND_915, f);
            swept[c]++;
            order[no++] = c;
        }
    }
    TEST_ASSERT_EQUAL_UINT8(6, s.passes);
    TEST_ASSERT_EQUAL_UINT8(6, order[0]);
    TEST_ASSERT_EQUAL_UINT8(9, order[3]);
    TEST_ASSERT_EQUAL_UINT8(11, order[4]); /* ch 10 skipped */
    for (uint8_t c = 0; c < 52; c++) {
        /* 52 swept dwells: the 45 free channels once, then 6, 7, 8, 9, 11, 12, 13 again */
        uint8_t want = (c < 6 || c == 10) ? 0 : ((c <= 13) ? 2 : 1);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, swept[c], "sweep count");
    }
}

static void test_fallback_never_and_zero(void)
{
    lc_term_scan_init(&s);
    s.fallback_after = LC_SCAN_NEVER;
    for (int i = 0; i < 6 * 20; i++) {
        dwell(LC_SCAN_DWELL_US, NULL);
        TEST_ASSERT_EQUAL_UINT8(LC_SCAN_SRC_DEFAULT, s.cur_src);
    }
    TEST_ASSERT_EQUAL_UINT8(20, s.passes);

    lc_term_scan_init(&s);
    s.fallback_after = 0;
    s.fallback_chunk = 52; /* one block sweep: all 46 free channels */
    for (uint8_t i = 0; i < 6; i++) dwell(LC_SCAN_DWELL_US, NULL);
    TEST_ASSERT_EQUAL_UINT8(52, s.cur_len);
    for (uint8_t c = 6; c < 52; c++) {
        TEST_ASSERT_EQUAL_UINT32(ch(c), dwell(LC_SCAN_DWELL_US, NULL));
    }
    TEST_ASSERT_EQUAL_UINT8(1, s.passes);
}

/* A new search starts over with the list; the sweep resumes where it was. */
static void test_restart_keeps_the_sweep_place(void)
{
    lc_term_scan_init(&s);
    s.fallback_after = 0;
    s.fallback_chunk = 3;
    for (uint8_t i = 0; i < 6; i++) dwell(LC_SCAN_DWELL_US, NULL);
    TEST_ASSERT_EQUAL_UINT32(ch(6), dwell(LC_SCAN_DWELL_US, NULL));
    lc_term_scan_restart(&s);
    TEST_ASSERT_EQUAL_UINT8(0, s.passes);
    TEST_ASSERT_EQUAL_UINT32(ch(0), dwell(LC_SCAN_DWELL_US, NULL));
    for (uint8_t i = 1; i < 6; i++) dwell(LC_SCAN_DWELL_US, NULL);
    TEST_ASSERT_EQUAL_UINT32(ch(7), dwell(LC_SCAN_DWELL_US, NULL));
}

/* §5.3: a FIXED entry sends every beacon, not every 8th, so a CYCLE dwell on
 * its channel still finds nothing else there; the sweep must keep visiting
 * it, with the full CYCLE dwell, not treat it as covered. */
static void test_fixed_entry_keeps_its_channel_in_the_sweep(void)
{
    lc_term_scan_init(&s);
    s.mode = LC_PHY_MODE_PART97;
    s.fallback_after = 0;
    s.fallback_chunk = 52;
    s.n_user = 1;
    s.user[0] = (lc_scan_ent_t){ ch(30), LC_SCAN_F_FIXED };
    /* the list round: the FIXED user entry, then the 6 defaults */
    for (uint8_t i = 0; i < 7; i++) dwell(i == 0 ? LC_SCAN_FIXED_DWELL_US : LC_SCAN_DWELL_US, NULL);
    int saw_30 = 0, end = 0;
    for (uint8_t i = 0; i < 52 && !end; i++) {
        uint32_t f, d;
        lc_term_scan_next(&s, &f, &d);
        TEST_ASSERT_EQUAL_UINT8(LC_SCAN_SRC_SWEEP, s.cur_src);
        if (f == ch(30)) {
            saw_30 = 1;
            TEST_ASSERT_EQUAL_UINT32(LC_SCAN_DWELL_US, d);
        }
        end = lc_term_scan_advance(&s);
    }
    TEST_ASSERT_TRUE(saw_30);
}

/* fallback_after above LC_SCAN_NEVER (15) clamps to never, the same as 15,
 * even once far more rounds than that have finished. */
static void test_fallback_after_over_15_is_never(void)
{
    lc_term_scan_init(&s);
    s.fallback_after = 200;
    s.passes = 250;
    uint32_t f, d;
    lc_term_scan_next(&s, &f, &d);
    TEST_ASSERT_EQUAL_UINT8(LC_SCAN_SRC_DEFAULT, s.cur_src);
    TEST_ASSERT_EQUAL_UINT8(6, s.cur_len); /* no sweep tail added */
}

/* fallback_chunk 0 clamps to 1: a single swept channel per round. */
static void test_fallback_chunk_zero_is_one(void)
{
    lc_term_scan_init(&s);
    s.fallback_after = 0;
    s.fallback_chunk = 0;
    for (uint8_t i = 0; i < 6; i++) dwell(LC_SCAN_DWELL_US, NULL); /* the list round */
    int end;
    uint32_t f = dwell(LC_SCAN_DWELL_US, &end);
    TEST_ASSERT_EQUAL_UINT8(LC_SCAN_SRC_SWEEP, s.cur_src);
    TEST_ASSERT_EQUAL_UINT8(7, s.cur_len); /* 6 list + 1 swept */
    TEST_ASSERT_TRUE(end);
    TEST_ASSERT_EQUAL_UINT32(ch(6), f);
}

/* A stale pos left over from a longer list (or round) must not be read as an
 * index: lc_term_scan_next resets it to the start instead. */
static void test_walk_resets_when_the_list_shrinks_under_it(void)
{
    lc_term_scan_init(&s);
    s.fallback_after = 0;
    s.fallback_chunk = 1;
    s.pos = 50; /* stale: e.g. left over from a bigger list a moment ago */
    uint32_t f, d;
    lc_term_scan_next(&s, &f, &d);
    TEST_ASSERT_EQUAL_UINT8(0, s.pos);
    TEST_ASSERT_EQUAL_UINT8(1, s.cur_pos);
    TEST_ASSERT_EQUAL_UINT8(7, s.cur_len); /* 6 defaults + 1 swept channel */
    TEST_ASSERT_EQUAL_UINT8(LC_SCAN_SRC_DEFAULT, s.cur_src);
    TEST_ASSERT_EQUAL_UINT32(ch(0), f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_ch_0_to_5);
    RUN_TEST(test_priority_and_dedupe);
    RUN_TEST(test_per_source_limits);
    RUN_TEST(test_inactive_entries_per_mode);
    RUN_TEST(test_status_fields_follow_the_walk);
    RUN_TEST(test_fallback_after_2_chunk_13);
    RUN_TEST(test_fallback_never_and_zero);
    RUN_TEST(test_restart_keeps_the_sweep_place);
    RUN_TEST(test_fixed_entry_keeps_its_channel_in_the_sweep);
    RUN_TEST(test_fallback_after_over_15_is_never);
    RUN_TEST(test_fallback_chunk_zero_is_one);
    RUN_TEST(test_walk_resets_when_the_list_shrinks_under_it);
    return UNITY_END();
}
