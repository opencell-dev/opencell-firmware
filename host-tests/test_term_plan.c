#include "unity.h"

#include <string.h>

#include "lc_exec.h"
#include "lc_term.h"

void setUp(void) {}
void tearDown(void) {}

#define SEED 0xCAFEF00Du

static lc_term_t term;

static lc_grant_leg_t leg(lc_band_t band, lc_tier_t tier, uint8_t slot, uint32_t off_us, uint8_t bytes)
{
    const lc_mode_t *m = lc_tier_mode(band, tier);
    uint32_t len = lc_slot_len_us(m, bytes);
    return (lc_grant_leg_t){ (uint8_t)band, (uint8_t)tier, 0, slot, (uint16_t)(off_us / 10u),
                             (uint16_t)((len + 9u) / 10u) };
}

/* A terminal synced to cell SEED at frame 1000 = local 1 s, granted legs.
 * Plan building only reads lc_term_t, so no radio is needed. */
static void granted_term(const lc_grant_leg_t *dl, const lc_grant_leg_t *ul)
{
    memset(&term, 0, sizeof(term));
    term.tmid = 0x11223344u;
    term.cell_seed = SEED;
    lc_term_trk_observe(&term.trk, 1000, 1000000u);
    term.state = LC_TERM_GRANTED;
    term.have_grant = 1;
    memset(&term.grant, 0, sizeof(term.grant));
    term.grant.tmid = term.tmid;
    term.grant.effective_frame = 990;
    if (dl) term.grant.dl = *dl;
    if (ul) term.grant.ul = *ul;
}

/* ------------------------------------------------------------ tracker */

static void test_tracker_follows_drifting_clock(void)
{
    lc_term_tracker_t k;
    memset(&k, 0, sizeof(k));
    /* Terminal crystal +15 ppm fast: each 120 ms frame is 120001.8 local µs.
     * Observations carry ±3 µs of alternating timestamp noise. */
    const double period = 120000.0 * 1.000015;
    for (uint32_t i = 0; i < 200; i++) {
        uint64_t t = 5000000u + (uint64_t)(i * period) + (i & 1u ? 3u : 0u);
        lc_term_trk_observe(&k, 7000u + i, t);
    }
    /* Predict 25 frames past the last observation (the sync-loss horizon). */
    uint64_t truth = 5000000u + (uint64_t)(224 * period);
    int64_t err = (int64_t)(lc_term_trk_frame_start(&k, 7224u) - truth);
    TEST_ASSERT_INT64_WITHIN(20, 0, err);
}

static void test_tracker_reanchors_on_wrong_frame_label(void)
{
    lc_term_tracker_t k;
    memset(&k, 0, sizeof(k));
    lc_term_trk_observe(&k, 100, 1000000u);
    lc_term_trk_observe(&k, 101, 1120000u);
    /* The cell's frame numbering jumped (restart): trust the new label. */
    lc_term_trk_observe(&k, 5000, 1240000u);
    TEST_ASSERT_EQUAL_UINT64(1240000u, lc_term_trk_frame_start(&k, 5000));
    TEST_ASSERT_EQUAL_UINT64(1360000u, lc_term_trk_frame_start(&k, 5001));
}

static void test_tracker_frame_at_floors_both_sides(void)
{
    lc_term_tracker_t k;
    memset(&k, 0, sizeof(k));
    lc_term_trk_observe(&k, 100, 1000000u);
    TEST_ASSERT_EQUAL_UINT32(100, lc_term_trk_frame_at(&k, 1000000u));
    TEST_ASSERT_EQUAL_UINT32(100, lc_term_trk_frame_at(&k, 1119999u));
    TEST_ASSERT_EQUAL_UINT32(101, lc_term_trk_frame_at(&k, 1120000u));
    TEST_ASSERT_EQUAL_UINT32(99, lc_term_trk_frame_at(&k, 999999u));
    TEST_ASSERT_EQUAL_UINT32(99, lc_term_trk_frame_at(&k, 880000u));
    TEST_ASSERT_EQUAL_UINT32(98, lc_term_trk_frame_at(&k, 879999u));
}

/* ------------------------------------------------------------ helpers */

static void test_tmid_from_mac(void)
{
    const uint8_t mac[6] = { 0xDC, 0x54, 0x75, 0x12, 0x34, 0x56 };
    TEST_ASSERT_EQUAL_HEX32(0x75123456u, lc_term_tmid_from_mac(mac));
    const uint8_t zero[6] = { 0xDC, 0x54, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_HEX32(0x5A5A5A5Au, lc_term_tmid_from_mac(zero));
    const uint8_t ones[6] = { 0xDC, 0x54, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5u, lc_term_tmid_from_mac(ones));
}

static void test_beacon_and_ag_slots_match_plan4_layout(void)
{
    /* Plan 4 (rhu_bs): beacon slot = slot for 25 bytes at EDGE rounded up to
     * 10 µs (1563 units while LC_GUARD_US is 200); AG = slot for 26 bytes. */
    const lc_mode_t *edge = lc_tier_mode(LC_BAND_915, LC_TIER_EDGE);
    TEST_ASSERT_EQUAL_UINT32((lc_slot_len_us(edge, 25) + 9u) / 10u * 10u, lc_term_beacon_len_us());
    TEST_ASSERT_EQUAL_UINT32((lc_slot_len_us(edge, 26) + 9u) / 10u * 10u, lc_term_ag_len_us());
}

static void test_grant_ok_rules(void)
{
    lc_grant_t g;
    memset(&g, 0, sizeof(g));
    g.dl = leg(LC_BAND_915, LC_TIER_EDGE, 8, 40000, 28);
    g.ul = leg(LC_BAND_915, LC_TIER_EDGE, 9, 40000 + g.dl.len * 10u, 28);
    TEST_ASSERT_TRUE(lc_term_grant_ok(&g)); /* back to back on one band: the guard is inside the core */

    g.ul.offset -= 1;
    TEST_ASSERT_FALSE(lc_term_grant_ok(&g)); /* overlaps */

    /* Cross-band duplex needs LC_TERM_BAND_SWITCH_US between the cores. */
    g.dl = leg(LC_BAND_915, LC_TIER_NEAR, 8, 40000, 28);
    g.ul = leg(LC_BAND_2G4, LC_TIER_NEAR, 9, 40000 + g.dl.len * 10u, 28);
    TEST_ASSERT_FALSE(lc_term_grant_ok(&g));
    g.ul.offset = (uint16_t)(g.ul.offset + LC_TERM_BAND_SWITCH_US / 10u);
    TEST_ASSERT_TRUE(lc_term_grant_ok(&g));

    lc_grant_t tiny = g;
    tiny.ul.len = 30; /* 300 µs: no empty DATA fits */
    TEST_ASSERT_FALSE(lc_term_grant_ok(&tiny));

    lc_grant_t revoke;
    memset(&revoke, 0, sizeof(revoke));
    TEST_ASSERT_TRUE(lc_term_grant_ok(&revoke));
}

/* ------------------------------------------------------------ plans */

static void test_synced_plan_is_beacon_only(void)
{
    granted_term(NULL, NULL);
    term.state = LC_TERM_SYNCED;
    term.have_grant = 0;
    lc_term_op_t ops[LC_TERM_MAX_OPS];
    TEST_ASSERT_EQUAL_UINT8(1, lc_term_build_plan(&term, 1001, ops));
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_BEACON_RX, ops[0].kind);
    TEST_ASSERT_EQUAL_UINT32(lc_channel_freq_hz(LC_BAND_915, lc_sync_channel(SEED, LC_BAND_915, 1001)),
                             ops[0].freq_hz);
    TEST_ASSERT_EQUAL_INT32(-(int32_t)LC_TERM_RX_MARGIN_US, ops[0].start_us);
    TEST_ASSERT_EQUAL_UINT32(lc_term_beacon_len_us() - LC_GUARD_US + 2u * LC_TERM_RX_MARGIN_US, ops[0].len_us);

    term.state = LC_TERM_SEARCH;
    TEST_ASSERT_EQUAL_UINT8(0, lc_term_build_plan(&term, 1001, ops));
}

static void test_granted_plan_legs_on_hopped_channels(void)
{
    lc_grant_leg_t dl = leg(LC_BAND_915, LC_TIER_EDGE, 8, 40000, 28);
    lc_grant_leg_t ul = leg(LC_BAND_915, LC_TIER_EDGE, 9, 40000 + dl.len * 10u, 28);
    granted_term(&dl, &ul);
    lc_term_op_t ops[LC_TERM_MAX_OPS];
    TEST_ASSERT_EQUAL_UINT8(3, lc_term_build_plan(&term, 1005, ops));
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_BEACON_RX, ops[0].kind);
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_DL_RX, ops[1].kind);
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_UL_TX, ops[2].kind);
    TEST_ASSERT_EQUAL_UINT32(lc_channel_freq_hz(LC_BAND_915, lc_grant_leg_channel(SEED, &dl, 1005)), ops[1].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(lc_channel_freq_hz(LC_BAND_915, lc_grant_leg_channel(SEED, &ul, 1005)), ops[2].freq_hz);
    /* DL window opens a margin early; it must close before the UL starts. */
    TEST_ASSERT_EQUAL_INT32(40000 - (int32_t)LC_TERM_RX_MARGIN_US, ops[1].start_us);
    TEST_ASSERT_TRUE(ops[1].start_us + (int32_t)ops[1].len_us <= ops[2].start_us);
    TEST_ASSERT_EQUAL_INT32((int32_t)ul.offset * 10, ops[2].start_us);
}

static void test_leg_over_beacon_wins_and_beacon_is_dropped(void)
{
    lc_grant_leg_t dl = leg(LC_BAND_2G4, LC_TIER_NEAR, 8, 1000, 28); /* 2.4 radio, during the 915 beacon */
    granted_term(&dl, NULL);
    lc_term_op_t ops[LC_TERM_MAX_OPS];
    TEST_ASSERT_EQUAL_UINT8(1, lc_term_build_plan(&term, 1005, ops));
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_DL_RX, ops[0].kind);
}

static void test_cross_band_windows_leave_switch_time(void)
{
    lc_grant_leg_t dl = leg(LC_BAND_915, LC_TIER_NEAR, 8, 40000, 28);
    lc_grant_leg_t ul = leg(LC_BAND_2G4, LC_TIER_NEAR, 9, 40000 + dl.len * 10u + LC_TERM_BAND_SWITCH_US, 28);
    granted_term(&dl, &ul);
    lc_term_op_t ops[LC_TERM_MAX_OPS];
    TEST_ASSERT_EQUAL_UINT8(3, lc_term_build_plan(&term, 1005, ops));
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_DL_RX, ops[1].kind);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_2G4, ops[2].band);
    TEST_ASSERT_TRUE(ops[1].start_us + (int32_t)ops[1].len_us + (int32_t)LC_TERM_BAND_SWITCH_US <= ops[2].start_us);
}

/* LoRa <-> FLRC on one band needs LC_TERM_MOD_SWITCH_US between cores (the
 * beacon is edge LoRa; a 915 near leg is FLRC). The terminal's leads match
 * the base-station executor's. */
static void test_modulation_change_needs_switch_gap(void)
{
    TEST_ASSERT_EQUAL_UINT32(LC_EXEC_CONFIG_LEAD_US, LC_TERM_CONFIG_LEAD_US);
    TEST_ASSERT_EQUAL_UINT32(LC_EXEC_MOD_SWITCH_LEAD_US, LC_TERM_MOD_SWITCH_US);
    TEST_ASSERT_EQUAL_UINT32(LC_EXEC_BAND_SWITCH_LEAD_US, LC_TERM_BAND_SWITCH_US);
    lc_term_op_t ops[LC_TERM_MAX_OPS];
    lc_grant_leg_t dl = leg(LC_BAND_915, LC_TIER_NEAR, 8, lc_term_beacon_len_us() + 1000u, 28);
    granted_term(&dl, NULL);
    TEST_ASSERT_EQUAL_UINT8(1, lc_term_build_plan(&term, 1005, ops)); /* beacon dropped */
    dl = leg(LC_BAND_915, LC_TIER_NEAR, 8, lc_term_beacon_len_us() + LC_TERM_MOD_SWITCH_US, 28);
    granted_term(&dl, NULL);
    TEST_ASSERT_EQUAL_UINT8(2, lc_term_build_plan(&term, 1005, ops));
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_BEACON_RX, ops[0].kind);
}

static void test_attach_plan_rach_and_ag_windows(void)
{
    granted_term(NULL, NULL);
    term.have_grant = 0;
    term.state = LC_TERM_ATTACHING;
    term.beacon.rach_offset = 10800;
    term.beacon.rach_len = 1200;
    term.beacon.rach_slot_index = 2;
    term.rach_pending = 1;
    term.rach_kind = LC_RACH_ATTACH;
    term.rach_frame = 1003;
    lc_term_op_t ops[LC_TERM_MAX_OPS];

    TEST_ASSERT_EQUAL_UINT8(2, lc_term_build_plan(&term, 1003, ops));
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_RACH_TX, ops[1].kind);
    TEST_ASSERT_EQUAL_INT32(108000, ops[1].start_us);
    TEST_ASSERT_EQUAL_UINT32(lc_channel_freq_hz(LC_BAND_915, lc_hop_channel(SEED, LC_BAND_915, 0, 1003, 2)),
                             ops[1].freq_hz);

    term.rach_pending = 0;
    term.ag_waiting = 1;
    term.ag_until = 1009;
    TEST_ASSERT_EQUAL_UINT8(2, lc_term_build_plan(&term, 1004, ops));
    TEST_ASSERT_EQUAL_UINT8(LC_TOP_AG_RX, ops[1].kind);
    TEST_ASSERT_EQUAL_UINT32(lc_channel_freq_hz(LC_BAND_915,
                                                lc_hop_channel(SEED, LC_BAND_915, 0, 1004, LC_TERM_AG_SLOT_INDEX)),
                             ops[1].freq_hz);
    /* The beacon window ends one guard (less the RX margin) before the beacon
     * slot ends, clipped at the AG slot if that comes first; the AG window
     * opens at the beacon slot's end. */
    int32_t bl = (int32_t)lc_term_beacon_len_us();
    int32_t unclipped = bl - (int32_t)LC_GUARD_US + (int32_t)LC_TERM_RX_MARGIN_US;
    TEST_ASSERT_EQUAL_INT32(unclipped < bl ? unclipped : bl, ops[0].start_us + (int32_t)ops[0].len_us);
    /* AG window: its margin before the AG slot, but never overlapping the beacon window */
    int32_t beacon_end = ops[0].start_us + (int32_t)ops[0].len_us;
    int32_t ag_open = bl - (int32_t)LC_TERM_RX_MARGIN_US;
    TEST_ASSERT_EQUAL_INT32(ag_open > beacon_end ? ag_open : beacon_end, ops[1].start_us);
    /* ...and still covers the longest beacon. */
    TEST_ASSERT_TRUE(ops[0].len_us >= LC_TERM_RX_MARGIN_US +
                                         lc_airtime_us(lc_tier_mode(LC_BAND_915, LC_TIER_EDGE), LC_BEACON_MAX_BYTES));
    TEST_ASSERT_EQUAL_UINT8(1, lc_term_build_plan(&term, 1010, ops)); /* past ag_until */
}

static void test_rach_skipped_when_window_too_small(void)
{
    granted_term(NULL, NULL);
    term.have_grant = 0;
    term.state = LC_TERM_ATTACHING;
    term.beacon.rach_offset = 11900;
    term.beacon.rach_len = 100; /* 1 ms */
    term.rach_pending = 1;
    term.rach_frame = 1003;
    lc_term_op_t ops[LC_TERM_MAX_OPS];
    TEST_ASSERT_EQUAL_UINT8(1, lc_term_build_plan(&term, 1003, ops));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tracker_follows_drifting_clock);
    RUN_TEST(test_tracker_reanchors_on_wrong_frame_label);
    RUN_TEST(test_tracker_frame_at_floors_both_sides);
    RUN_TEST(test_tmid_from_mac);
    RUN_TEST(test_beacon_and_ag_slots_match_plan4_layout);
    RUN_TEST(test_grant_ok_rules);
    RUN_TEST(test_synced_plan_is_beacon_only);
    RUN_TEST(test_granted_plan_legs_on_hopped_channels);
    RUN_TEST(test_leg_over_beacon_wins_and_beacon_is_dropped);
    RUN_TEST(test_cross_band_windows_leave_switch_time);
    RUN_TEST(test_attach_plan_rach_and_ag_windows);
    RUN_TEST(test_rach_skipped_when_window_too_small);
    RUN_TEST(test_modulation_change_needs_switch_gap);
    return UNITY_END();
}
