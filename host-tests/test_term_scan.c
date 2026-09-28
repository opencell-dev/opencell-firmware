/* The terminal's scan list (channel-list spec §5): assembly, the walk and
 * the fallback sweep. Pure: no radio. */
#include "unity.h"

#include <string.h>

#include "lc_crc.h"
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

/* §5.2: attaching sets the last serving entry and pushes the previous one to
 * the front of the learned entries; NVS is only written when it changes. */
static void test_serving_and_learned_lru(void)
{
    lc_term_scan_init(&s);
    lc_term_scan_serving(&s, ch(30), 0);
    TEST_ASSERT_EQUAL_UINT32(ch(30), s.last.freq_hz);
    TEST_ASSERT_EQUAL_UINT8(0, s.n_learn);
    TEST_ASSERT_TRUE(s.dirty);
    s.dirty = 0;
    lc_term_scan_serving(&s, ch(30), 0); /* the same cell again: nothing to save */
    TEST_ASSERT_FALSE(s.dirty);
    lc_term_scan_serving(&s, ch(40), 0);
    lc_term_scan_serving(&s, ch(10), 1);
    TEST_ASSERT_EQUAL_UINT32(ch(10), s.last.freq_hz);
    TEST_ASSERT_EQUAL_HEX8(LC_SCAN_F_FIXED, s.last.flags);
    TEST_ASSERT_EQUAL_UINT8(2, s.n_learn);
    TEST_ASSERT_EQUAL_UINT32(ch(40), s.learn[0].freq_hz); /* most recent first */
    TEST_ASSERT_EQUAL_UINT32(ch(30), s.learn[1].freq_hz);
    lc_term_scan_serving(&s, ch(30), 0); /* back to 30: it leaves the learned entries */
    TEST_ASSERT_EQUAL_UINT8(2, s.n_learn);
    TEST_ASSERT_EQUAL_UINT32(ch(10), s.learn[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(ch(40), s.learn[1].freq_hz);
    for (uint8_t c = 20; c < 24; c++) lc_term_scan_serving(&s, ch(c), 0);
    TEST_ASSERT_EQUAL_UINT8(LC_SCAN_MAX_LEARN, s.n_learn); /* the oldest dropped */
    TEST_ASSERT_EQUAL_UINT32(ch(22), s.learn[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(ch(30), s.learn[3].freq_hz);
    lc_term_scan_serving(&s, 0, 0); /* no anchor: ignored */
    TEST_ASSERT_EQUAL_UINT32(ch(23), s.last.freq_hz);
}

static void test_user_entries_validated(void)
{
    lc_term_scan_init(&s);
    const lc_scan_ent_t ok[2] = { { ch(10), 0xFF }, { ch(51), 0 } };
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 2, ok));
    TEST_ASSERT_EQUAL_UINT8(2, s.n_user);
    TEST_ASSERT_EQUAL_HEX8(LC_SCAN_F_FIXED, s.user[0].flags); /* only FIXED is kept */
    TEST_ASSERT_TRUE(s.dirty);
    s.dirty = 0;
    const lc_scan_ent_t off[1] = { { 917300000u, 0 } };
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_set_user(&s, 1, off));
    const lc_scan_ent_t out[1] = { { 928250000u, 0 } };
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_set_user(&s, 1, out));
    const lc_scan_ent_t five[5] = { { ch(6), 0 }, { ch(7), 0 }, { ch(8), 0 }, { ch(9), 0 }, { ch(11), 0 } };
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_set_user(&s, 5, five));
    TEST_ASSERT_EQUAL_UINT8(2, s.n_user); /* refused: nothing changed */
    TEST_ASSERT_FALSE(s.dirty);
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8(0, s.n_user);
}

static void test_fallback_settings_validated(void)
{
    lc_term_scan_init(&s);
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_fallback(&s, LC_SCAN_NEVER, 52));
    TEST_ASSERT_EQUAL_UINT8(15, s.fallback_after);
    TEST_ASSERT_EQUAL_UINT8(52, s.fallback_chunk);
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_fallback(&s, 0, 1));
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_set_fallback(&s, 16, 13));
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_set_fallback(&s, 2, 0));
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_set_fallback(&s, 2, 53));
    TEST_ASSERT_EQUAL_UINT8(0, s.fallback_after);
    TEST_ASSERT_EQUAL_UINT8(1, s.fallback_chunk);
}

static void test_network_entries_mode_and_deactivate(void)
{
    lc_term_scan_init(&s);
    lc_scan_ent_t e[13];
    for (uint8_t i = 0; i < 13; i++) e[i] = (lc_scan_ent_t){ ch((uint8_t)(20 + i)), (uint8_t)(i == 0 ? 0x81 : 0) };
    lc_term_scan_set_net(&s, 7, 13, e);
    TEST_ASSERT_EQUAL_UINT8(12, s.n_net); /* cut at 12 */
    TEST_ASSERT_EQUAL_UINT8(7, s.net_ver);
    TEST_ASSERT_EQUAL_HEX8(LC_SCAN_F_FIXED, s.net[0].flags);
    const lc_scan_ent_t u[1] = { { ch(9), 0 } };
    lc_term_scan_set_user(&s, 1, u);
    lc_term_scan_serving(&s, ch(20), 0);
    lc_term_scan_serving(&s, ch(21), 0);
    TEST_ASSERT_EQUAL_UINT8(1, s.n_learn);

    s.dirty = 0;
    lc_term_scan_set_mode(&s, LC_PHY_MODE_PART15); /* unchanged */
    lc_term_scan_set_mode(&s, 0);
    lc_term_scan_set_mode(&s, 3);
    TEST_ASSERT_FALSE(s.dirty);
    lc_term_scan_set_mode(&s, LC_PHY_MODE_PART97);
    TEST_ASSERT_EQUAL_UINT8(LC_PHY_MODE_PART97, s.mode);
    TEST_ASSERT_TRUE(s.dirty);

    lc_term_scan_deactivate(&s); /* §15 Q5: the user's entries stay */
    TEST_ASSERT_EQUAL_UINT8(0, s.n_net);
    TEST_ASSERT_EQUAL_UINT8(0, s.net_ver);
    TEST_ASSERT_EQUAL_UINT8(0, s.n_learn);
    TEST_ASSERT_EQUAL_UINT8(1, s.n_user);
    TEST_ASSERT_EQUAL_UINT32(ch(21), s.last.freq_hz);

    lc_term_scan_forget_learned(&s);
    TEST_ASSERT_EQUAL_UINT8(0, s.n_learn);
}

/* The network pushes its list after every registration: an identical one
 * (same version, count and entries, after the cut and the flag mask) must not
 * set dirty (no NVS write); any real change - entries under the same version
 * included - is applied and does. */
static void test_identical_network_list_leaves_dirty_clear(void)
{
    lc_term_scan_init(&s);
    lc_scan_ent_t e[13];
    for (uint8_t i = 0; i < 13; i++) e[i] = (lc_scan_ent_t){ ch((uint8_t)(20 + i)), (uint8_t)(i == 0 ? 0x81 : 0) };
    lc_term_scan_set_net(&s, 7, 13, e);
    TEST_ASSERT_TRUE(s.dirty);
    s.dirty = 0;
    lc_term_scan_set_net(&s, 7, 13, e); /* the same push again */
    TEST_ASSERT_FALSE(s.dirty);
    e[0].flags = LC_SCAN_F_FIXED;
    lc_term_scan_set_net(&s, 7, 12, e); /* the same after the cut and the mask */
    TEST_ASSERT_FALSE(s.dirty);

    e[0].flags = 0; /* a different list under the same version */
    lc_term_scan_set_net(&s, 7, 12, e);
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_HEX8(0, s.net[0].flags);
    s.dirty = 0;
    e[11].freq_hz = ch(50);
    lc_term_scan_set_net(&s, 7, 12, e);
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT32(ch(50), s.net[11].freq_hz);
    s.dirty = 0;
    lc_term_scan_set_net(&s, 7, 11, e); /* shorter */
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT8(11, s.n_net);
    s.dirty = 0;
    lc_term_scan_set_net(&s, 8, 11, e); /* a new version, same entries */
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT8(8, s.net_ver);
    s.dirty = 0;
    lc_term_scan_set_net(&s, 8, 0, NULL); /* count 0 clears */
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT8(0, s.n_net);
    s.dirty = 0;
    lc_term_scan_set_net(&s, 8, 0, NULL);
    TEST_ASSERT_FALSE(s.dirty);
}

/* The phone may send the same SET_USER / SET_FALLBACK again (COMMAND 0x07):
 * the same content must not set dirty (no NVS write); a real change does. */
static void test_identical_user_list_leaves_dirty_clear(void)
{
    lc_term_scan_init(&s);
    lc_scan_ent_t e[2] = { { ch(30), LC_SCAN_F_FIXED }, { ch(1), 0 } };
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 2, e));
    TEST_ASSERT_TRUE(s.dirty);
    s.dirty = 0;
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 2, e)); /* the same again */
    TEST_ASSERT_FALSE(s.dirty);
    e[1].flags = 0x80; /* the same after the flag mask */
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 2, e));
    TEST_ASSERT_FALSE(s.dirty);

    e[1].flags = LC_SCAN_F_FIXED; /* a flag changes */
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 2, e));
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_HEX8(LC_SCAN_F_FIXED, s.user[1].flags);
    s.dirty = 0;
    e[0].freq_hz = ch(31); /* a frequency changes */
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 2, e));
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT32(ch(31), s.user[0].freq_hz);
    s.dirty = 0;
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 1, e)); /* shorter */
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT8(1, s.n_user);
    s.dirty = 0;
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 0, NULL)); /* clear */
    TEST_ASSERT_TRUE(s.dirty);
    s.dirty = 0;
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_user(&s, 0, NULL)); /* already clear */
    TEST_ASSERT_FALSE(s.dirty);
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_set_user(&s, 5, e)); /* refused: no change */
    TEST_ASSERT_FALSE(s.dirty);
}

static void test_identical_fallback_leaves_dirty_clear(void)
{
    lc_term_scan_init(&s);
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_fallback(&s, s.fallback_after, s.fallback_chunk)); /* the defaults */
    TEST_ASSERT_FALSE(s.dirty);
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_fallback(&s, 3, 13)); /* after changes */
    TEST_ASSERT_TRUE(s.dirty);
    s.dirty = 0;
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_fallback(&s, 3, 13));
    TEST_ASSERT_FALSE(s.dirty);
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_set_fallback(&s, 3, 20)); /* chunk changes */
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT8(20, s.fallback_chunk);
}

/* FORGET_LEARNED and DEACTIVATE with nothing to clear: no NVS write. */
static void test_clearing_nothing_leaves_dirty_clear(void)
{
    lc_term_scan_init(&s);
    lc_term_scan_forget_learned(&s);
    TEST_ASSERT_FALSE(s.dirty);
    lc_term_scan_deactivate(&s);
    TEST_ASSERT_FALSE(s.dirty);
    lc_term_scan_serving(&s, ch(20), 0);
    lc_term_scan_serving(&s, ch(21), 0); /* ch 20 is now learned */
    s.dirty = 0;
    lc_term_scan_forget_learned(&s);
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT8(0, s.n_learn);
    const lc_scan_ent_t e = { ch(40), 0 };
    lc_term_scan_set_net(&s, 2, 1, &e);
    s.dirty = 0;
    lc_term_scan_deactivate(&s);
    TEST_ASSERT_TRUE(s.dirty);
    TEST_ASSERT_EQUAL_UINT8(0, s.n_net);
    s.dirty = 0;
    lc_term_scan_deactivate(&s);
    TEST_ASSERT_FALSE(s.dirty);
}

static const uint8_t k_blob[30] = { 0x01, 0x02, 0x02, 0x0D, 0x05, 0x01, 0x01, 0x01, 0xD0, 0x1F,
                                    0xAC, 0x36, 0x00, 0x50, 0x89, 0x13, 0x36, 0x01, 0x10, 0x6B,
                                    0xF8, 0x36, 0x00, 0x90, 0xD4, 0x5F, 0x36, 0x00, 0x75, 0x4D };

static void blob_list(void)
{
    lc_term_scan_init(&s);
    s.mode = LC_PHY_MODE_PART97;
    s.net_ver = 5;
    s.last = (lc_scan_ent_t){ ch(30), 0 };
    s.n_user = 1;
    s.user[0] = (lc_scan_ent_t){ ch(10), LC_SCAN_F_FIXED };
    s.n_net = 1;
    s.net[0] = (lc_scan_ent_t){ ch(40), 0 };
    s.n_learn = 1;
    s.learn[0] = (lc_scan_ent_t){ ch(20), 0 };
}

static void test_blob_golden_bytes_and_roundtrip(void)
{
    uint8_t b[LC_SCAN_BLOB_MAX];
    blob_list();
    TEST_ASSERT_EQUAL_size_t(sizeof(k_blob), lc_term_scan_pack(&s, b));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_blob, b, sizeof(k_blob));

    lc_term_scan_t back;
    lc_term_scan_init(&back);
    back.dirty = 1;
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_unpack(&back, k_blob, sizeof(k_blob)));
    TEST_ASSERT_FALSE(back.dirty);
    TEST_ASSERT_EQUAL_UINT8(LC_PHY_MODE_PART97, back.mode);
    TEST_ASSERT_EQUAL_UINT8(5, back.net_ver);
    TEST_ASSERT_EQUAL_UINT32(ch(30), back.last.freq_hz);
    TEST_ASSERT_EQUAL_HEX8(LC_SCAN_F_FIXED, back.user[0].flags);
    TEST_ASSERT_EQUAL_UINT32(ch(40), back.net[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(ch(20), back.learn[0].freq_hz);

    /* the largest list fits the blob */
    lc_term_scan_init(&s);
    s.n_user = LC_SCAN_MAX_USER;
    s.n_net = LC_SCAN_MAX_NET;
    s.n_learn = LC_SCAN_MAX_LEARN;
    TEST_ASSERT_EQUAL_size_t(115, lc_term_scan_pack(&s, b));
    TEST_ASSERT_EQUAL_INT(0, lc_term_scan_unpack(&back, b, 115));
}

/* §5.4: a corrupt blob loads as nothing, which leaves the defaults working. */
static void test_corrupt_blob_leaves_defaults(void)
{
    uint8_t b[sizeof(k_blob)];
    lc_term_scan_t t;
    lc_term_scan_init(&t);
    memcpy(b, k_blob, sizeof(b));
    b[9] ^= 0x01; /* CRC mismatch */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, k_blob, sizeof(k_blob) - 1)); /* short */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, k_blob, 3));
    memcpy(b, k_blob, sizeof(b));
    b[0] = 2; /* a later version */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, b, sizeof(b)));
    TEST_ASSERT_EQUAL_UINT8(0, t.n_user);
    TEST_ASSERT_EQUAL_UINT32(0, t.last.freq_hz);
    TEST_ASSERT_EQUAL_UINT8(LC_PHY_MODE_PART15, t.mode);
    lc_scan_ent_t l[LC_SCAN_MAX];
    TEST_ASSERT_EQUAL_UINT8(6, lc_term_scan_list(&t, l));

    /* a well-formed blob with a field out of range is refused too */
    uint8_t v[LC_SCAN_BLOB_MAX];
    blob_list();
    s.fallback_chunk = 0;
    size_t n = lc_term_scan_pack(&s, v);
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, v, n));
    blob_list();
    s.mode = 3;
    n = lc_term_scan_pack(&s, v);
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, v, n));
}
/* Extra (Task 3 review follow-up): unpack must also reject a well-formed,
 * CRC-valid blob whose mode is neither PART15 nor PART97 (covered above by
 * mode = 3), and any other out-of-range field: fallback_after > 15,
 * fallback_chunk > 52 (fallback_chunk == 0 is covered above), and a count
 * over its per-source cap (n_user, n_net, n_learn). These require poking
 * the packed bytes directly (the setters never allow an out-of-range value)
 * and recomputing the CRC so only the field under test is corrupt. */
static uint8_t *poke_and_recrc(uint8_t *b, size_t n, size_t at, uint8_t v)
{
    b[at] = v;
    uint16_t crc = lc_crc16(b, n - 2);
    b[n - 2] = (uint8_t)crc;
    b[n - 1] = (uint8_t)(crc >> 8);
    return b;
}

static void test_corrupt_blob_out_of_range_fields(void)
{
    uint8_t v[LC_SCAN_BLOB_MAX];
    lc_term_scan_t t;
    lc_term_scan_init(&t);

    blob_list();
    size_t n = lc_term_scan_pack(&s, v);
    poke_and_recrc(v, n, 2, 16); /* fallback_after > 15 */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, v, n));

    blob_list();
    n = lc_term_scan_pack(&s, v);
    poke_and_recrc(v, n, 3, 53); /* fallback_chunk > 52 */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, v, n));

    blob_list();
    n = lc_term_scan_pack(&s, v);
    poke_and_recrc(v, n, 5, LC_SCAN_MAX_USER + 1); /* n_user over cap */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, v, n));

    blob_list();
    n = lc_term_scan_pack(&s, v);
    poke_and_recrc(v, n, 6, LC_SCAN_MAX_NET + 1); /* n_net over cap */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, v, n));

    blob_list();
    n = lc_term_scan_pack(&s, v);
    poke_and_recrc(v, n, 7, LC_SCAN_MAX_LEARN + 1); /* n_learn over cap */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_scan_unpack(&t, v, n));

    /* untouched: still the defaults from init */
    TEST_ASSERT_EQUAL_UINT8(LC_PHY_MODE_PART15, t.mode);
    TEST_ASSERT_EQUAL_UINT8(0, t.n_user);
    TEST_ASSERT_EQUAL_UINT8(0, t.n_net);
    TEST_ASSERT_EQUAL_UINT8(0, t.n_learn);
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
    RUN_TEST(test_serving_and_learned_lru);
    RUN_TEST(test_user_entries_validated);
    RUN_TEST(test_fallback_settings_validated);
    RUN_TEST(test_network_entries_mode_and_deactivate);
    RUN_TEST(test_identical_network_list_leaves_dirty_clear);
    RUN_TEST(test_identical_user_list_leaves_dirty_clear);
    RUN_TEST(test_identical_fallback_leaves_dirty_clear);
    RUN_TEST(test_clearing_nothing_leaves_dirty_clear);
    RUN_TEST(test_blob_golden_bytes_and_roundtrip);
    RUN_TEST(test_corrupt_blob_leaves_defaults);
    RUN_TEST(test_corrupt_blob_out_of_range_fields);
    return UNITY_END();
}
