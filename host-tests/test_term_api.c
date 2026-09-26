#include "unity.h"

#include <string.h>

#include "lc_term.h"

void setUp(void) {}
void tearDown(void) {}

#define SEED 0xCAFEF00Du

/* Radio ops that do nothing: plan-building tests never drive the radio. */
static int n_configure(void *c, uint32_t f, const lc_mode_t *m) { (void)c; (void)f; (void)m; return 0; }
static int n_stage_tx(void *c, const uint8_t *d, uint8_t l) { (void)c; (void)d; (void)l; return 0; }
static int n_stage_rx(void *c, uint32_t t) { (void)c; (void)t; return 0; }
static int n_launch(void *c, uint64_t at_us) { (void)c; (void)at_us; return 0; }
static int n_poll(void *c, lc_radio_event_t *e) { (void)c; (void)e; return 0; }
static void n_standby(void *c) { (void)c; }
static const lc_radio_ops_t null_radio = { NULL, n_configure, n_stage_tx, n_stage_rx, n_launch, n_poll, n_standby };

static lc_term_t term;

static lc_grant_leg_t leg(lc_band_t band, lc_tier_t tier, uint8_t slot, uint32_t off_us, uint8_t bytes)
{
    const lc_mode_t *m = lc_tier_mode(band, tier);
    uint32_t len = lc_slot_len_us(m, bytes);
    return (lc_grant_leg_t){ (uint8_t)band, (uint8_t)tier, 0, slot, (uint16_t)(off_us / 10u),
                             (uint16_t)((len + 9u) / 10u) };
}

/* A terminal synced to cell SEED at frame 1000 = local 1 s, granted legs. */
static void granted_term(const lc_grant_leg_t *dl, const lc_grant_leg_t *ul)
{
    lc_term_init(&term, &null_radio, NULL, 0x11223344u);
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

/* ------------------------------------------------------------ upper */

static void test_send_upper_rules(void)
{
    uint8_t data[LC_TERM_DATA_MAX_PAYLOAD + 1];
    memset(data, 7, sizeof(data));
    lc_grant_leg_t dl = leg(LC_BAND_915, LC_TIER_EDGE, 8, 40000, 28);
    lc_grant_leg_t ul = leg(LC_BAND_915, LC_TIER_EDGE, 9, 40000 + dl.len * 10u, 28);
    granted_term(&dl, &ul);
    TEST_ASSERT_EQUAL_INT(0, lc_term_send_upper(&term, data, 20));   /* 28 air bytes */
    TEST_ASSERT_EQUAL_INT(-1, lc_term_send_upper(&term, data, 21));  /* over the plan 4 limit */
    for (int i = 1; i < (int)LC_TERM_UPQ_DEPTH; i++) {
        TEST_ASSERT_EQUAL_INT(0, lc_term_send_upper(&term, data, 4));
    }
    TEST_ASSERT_EQUAL_INT(-1, lc_term_send_upper(&term, data, 4));  /* queue full */

    granted_term(NULL, NULL);
    term.have_grant = 0;
    term.state = LC_TERM_IDLE;
    TEST_ASSERT_EQUAL_INT(-1, lc_term_send_upper(&term, data, LC_TERM_RACH_MAX_PAYLOAD + 1));
    TEST_ASSERT_EQUAL_INT(0, lc_term_send_upper(&term, data, LC_TERM_RACH_MAX_PAYLOAD));
    TEST_ASSERT_TRUE(term.rach_pending);
    TEST_ASSERT_EQUAL_UINT8(LC_RACH_UPPER, term.rach_kind);
    TEST_ASSERT_EQUAL_INT(-1, lc_term_send_upper(&term, data, 1)); /* one RACH at a time */

    term.state = LC_TERM_SEARCH;
    term.rach_pending = 0;
    TEST_ASSERT_EQUAL_INT(-1, lc_term_send_upper(&term, data, 1));
}

static void test_status_reports_grant_band_and_tier(void)
{
    lc_grant_leg_t dl = leg(LC_BAND_2G4, LC_TIER_NEAR, 8, 40000, 28);
    granted_term(&dl, NULL);
    term.rssi_dbm = -71;
    lc_term_status_t st;
    lc_term_status(&term, &st);
    TEST_ASSERT_EQUAL_UINT8(LC_TERM_GRANTED, st.state);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_2G4, st.band);
    TEST_ASSERT_EQUAL_UINT8(LC_TIER_NEAR, st.tier);
    TEST_ASSERT_EQUAL_INT16(-71, st.rssi_dbm);
    TEST_ASSERT_EQUAL_HEX32(0x11223344u, st.tmid);
    TEST_ASSERT_EQUAL_HEX32(SEED, st.cell_seed);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_send_upper_rules);
    RUN_TEST(test_status_reports_grant_band_and_tier);
    return UNITY_END();
}
