#include "unity.h"

#include <string.h>

#include "lc_air.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t buf[LC_AIR_MAX_FRAME];
static lc_air_msg_t in;
static lc_air_msg_t out;

static size_t roundtrip(void)
{
    size_t n = lc_air_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(0, lc_air_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(in.type, out.type);
    return n;
}

static lc_air_msg_t make_beacon(uint8_t pages)
{
    lc_air_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_AIR_BEACON;
    m.u.beacon = (lc_beacon_t){ 0xCAFEF00Du, 123456, LC_BAND_915, LC_BCN_FLAG_ACCEPTING_ATTACH,
                                11000, 900, 17, pages, 30, 2, { 0x11111111u, 0x22222222u } };
    return m;
}

static lc_air_msg_t make_grant(void)
{
    lc_air_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = LC_AIR_GRANT;
    m.u.grant.tmid = 0xABCDEF01u;
    m.u.grant.effective_frame = 123460;
    m.u.grant.dl = (lc_grant_leg_t){ LC_BAND_915, LC_TIER_EDGE, 0, 3, 1600, 1690 };
    m.u.grant.ul = (lc_grant_leg_t){ LC_BAND_2G4, LC_TIER_NEAR, 1, 9, 7000, 50 };
    return m;
}

static void test_first_byte_carries_version_and_type(void)
{
    in = make_beacon(0);
    size_t n = lc_air_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_HEX8((LC_AIR_VERSION << 4) | LC_AIR_BEACON, buf[0]);
}

static void test_beacon_roundtrip_and_size(void)
{
    in = make_beacon(2);
    size_t n = roundtrip();
    TEST_ASSERT_EQUAL_size_t(LC_BEACON_MAX_BYTES, n);
    TEST_ASSERT_EQUAL_UINT32(123456, out.u.beacon.frame_number);
    TEST_ASSERT_EQUAL_UINT16(11000, out.u.beacon.rach_offset);
    TEST_ASSERT_EQUAL_UINT8(2, out.u.beacon.page_count);
    TEST_ASSERT_EQUAL_HEX32(0x22222222u, out.u.beacon.page_tmid[1]);

    TEST_ASSERT_EQUAL_UINT8(30, out.u.beacon.anchor);
    TEST_ASSERT_EQUAL_UINT8(2, out.u.beacon.cfg_ver);

    in = make_beacon(0);
    TEST_ASSERT_EQUAL_size_t(18, roundtrip());
}

/* Channel-list spec §4.2: the sync byte after page_count, anchor in bits 0-5
 * and cfg_ver in bits 6-7; LC_AIR_VERSION 2. */
static void test_beacon_v2_golden_bytes(void)
{
    in = make_beacon(1);
    in.u.beacon.flags = LC_BCN_FLAG_ACCEPTING_ATTACH | LC_BCN_FLAG_PART97 | LC_BCN_FLAG_FIXED_SYNC;
    static const uint8_t golden[22] = { 0x21, 0x0D, 0xF0, 0xFE, 0xCA, 0x40, 0xE2, 0x01, 0x00, 0x00, 0x0D,
                                        0xF8, 0x2A, 0x84, 0x03, 0x11, 0x01, 0x9E, 0x11, 0x11, 0x11, 0x11 };
    size_t n = lc_air_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(sizeof(golden), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden, buf, sizeof(golden));
    TEST_ASSERT_EQUAL_INT(0, lc_air_decode(golden, sizeof(golden), &out));
    TEST_ASSERT_EQUAL_UINT8(30, out.u.beacon.anchor);
    TEST_ASSERT_EQUAL_UINT8(2, out.u.beacon.cfg_ver);
    TEST_ASSERT_EQUAL_HEX8(0x0D, out.u.beacon.flags);
    TEST_ASSERT_EQUAL_HEX32(0x11111111u, out.u.beacon.page_tmid[0]);
}

static void test_beacon_anchor_range(void)
{
    in = make_beacon(0);
    in.u.beacon.anchor = 51;
    in.u.beacon.cfg_ver = 3;
    roundtrip();
    TEST_ASSERT_EQUAL_UINT8(51, out.u.beacon.anchor);
    TEST_ASSERT_EQUAL_UINT8(3, out.u.beacon.cfg_ver);
    in.u.beacon.anchor = 52;
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));
    in.u.beacon.anchor = 0;
    in.u.beacon.cfg_ver = 4;
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));

    in = make_beacon(0);
    size_t n = lc_air_encode(&in, buf, sizeof(buf));
    buf[17] = 0x80 | 52; /* anchor 52: not a 915 channel */
    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(buf, n, &out));
    buf[17] = 0xC0 | 63;
    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(buf, n, &out));
}

/* No mixed versions (spec §4.2): a v1 beacon (17 bytes, no sync byte) is dropped. */
static void test_v1_beacon_rejected(void)
{
    static const uint8_t v1[17] = { 0x11, 0x0D, 0xF0, 0xFE, 0xCA, 0x40, 0xE2, 0x01, 0x00,
                                    0x00, 0x01, 0xF8, 0x2A, 0x84, 0x03, 0x11, 0x00 };
    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(v1, sizeof(v1), &out));
}

/* The sync byte is free (spec §4.2): 26 B takes as long as 25 B did, and 18 B
 * as long as 17 B, so every slot offset stays where it was. */
static void test_sync_byte_costs_no_airtime(void)
{
    const lc_mode_t *edge = lc_tier_mode(LC_BAND_915, LC_TIER_EDGE);
    TEST_ASSERT_EQUAL_UINT32(15424u, lc_airtime_us(edge, 25));
    TEST_ASSERT_EQUAL_UINT32(15424u, lc_airtime_us(edge, LC_BEACON_MAX_BYTES));
    TEST_ASSERT_EQUAL_UINT32(lc_airtime_us(edge, 17), lc_airtime_us(edge, 18));
}

static void test_max_beacon_fits_edge_tier_budget(void)
{
    /* Beacon goes out every frame at edge tier; keep it under 16 ms on air. */
    const lc_mode_t *edge = lc_tier_mode(LC_BAND_915, LC_TIER_EDGE);
    TEST_ASSERT_TRUE(lc_airtime_us(edge, LC_BEACON_MAX_BYTES) < 16000u);
}

static void test_grant_roundtrip(void)
{
    in = make_grant();
    size_t n = roundtrip();
    TEST_ASSERT_EQUAL_size_t(25, n);
    TEST_ASSERT_EQUAL_HEX32(0xABCDEF01u, out.u.grant.tmid);
    TEST_ASSERT_EQUAL_UINT8(LC_BAND_2G4, out.u.grant.ul.band);
    TEST_ASSERT_EQUAL_UINT8(1, out.u.grant.ul.radio_index);
    TEST_ASSERT_EQUAL_UINT16(1690, out.u.grant.dl.len);
}

static void test_revoke_grant_has_empty_legs(void)
{
    in = make_grant();
    memset(&in.u.grant.dl, 0, sizeof(in.u.grant.dl));
    memset(&in.u.grant.ul, 0, sizeof(in.u.grant.ul));
    in.u.grant.dl.band = 0xEE; /* ignored when len == 0 */
    roundtrip();
    TEST_ASSERT_EQUAL_UINT16(0, out.u.grant.dl.len);
    TEST_ASSERT_EQUAL_UINT8(LC_INVALID_CHANNEL, lc_grant_leg_channel(1, &out.u.grant.dl, 5));
}

static void test_rach_and_data_roundtrip(void)
{
    static const uint8_t req[5] = { 1, 2, 0, 4, 5 };
    memset(&in, 0, sizeof(in));
    in.type = LC_AIR_RACH;
    in.u.rach = (lc_rach_t){ 0x01020304u, LC_RACH_ATTACH, sizeof(req), req };
    TEST_ASSERT_EQUAL_size_t(1 + 4 + 1 + 1 + 5, roundtrip());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(req, out.u.rach.payload, 5);

    static uint8_t voice[20];
    memset(voice, 0x5A, sizeof(voice));
    memset(&in, 0, sizeof(in));
    in.type = LC_AIR_DATA;
    in.u.data = (lc_data_t){ 0x0A0B0C0Du, 250, 0, sizeof(voice), voice };
    /* 28 bytes: exactly the voice frame size lc_phy's slot budgets assume. */
    TEST_ASSERT_EQUAL_size_t(28, roundtrip());
    TEST_ASSERT_EQUAL_UINT8(250, out.u.data.seq);

    static uint8_t big[LC_DATA_MAX_PAYLOAD];
    in.u.data.payload_len = LC_DATA_MAX_PAYLOAD;
    in.u.data.payload = big;
    TEST_ASSERT_TRUE(roundtrip() <= LC_AIR_MAX_FRAME);
}

static void test_grant_leg_channel_matches_lc_hop(void)
{
    in = make_grant();
    const lc_grant_leg_t *dl = &in.u.grant.dl;
    for (uint32_t f = 0; f < 50; f++) {
        TEST_ASSERT_EQUAL_UINT8(lc_hop_channel(0xCAFEF00Du, LC_BAND_915, 0, f, 3),
                                lc_grant_leg_channel(0xCAFEF00Du, dl, f));
    }
}

static void test_encode_rejects_out_of_range_fields(void)
{
    in = make_beacon(LC_BCN_MAX_PAGES + 1);
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));
    in = make_beacon(0);
    in.u.beacon.band = LC_BAND_COUNT;
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));
    in = make_beacon(0);
    in.u.beacon.rach_offset = 11500;
    in.u.beacon.rach_len = 600; /* ends past 12000 */
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));

    in = make_grant();
    in.u.grant.ul.tier = LC_TIER_EDGE; /* EDGE unsupported on 2.4 */
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));
    in = make_grant();
    in.u.grant.dl.radio_index = LC_MAX_RADIOS_PER_BAND;
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));

    memset(&in, 0, sizeof(in));
    in.type = LC_AIR_RACH;
    in.u.rach.payload_len = LC_RACH_MAX_PAYLOAD + 1;
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));

    in.type = 9;
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, sizeof(buf)));

    in = make_grant();
    TEST_ASSERT_EQUAL_size_t(0, lc_air_encode(&in, buf, 10));
}

static void test_decode_rejects_bad_frames(void)
{
    in = make_grant();
    size_t n = lc_air_encode(&in, buf, sizeof(buf));

    uint8_t copy[LC_AIR_MAX_FRAME];
    memcpy(copy, buf, n);
    copy[0] = (uint8_t)((3u << 4) | LC_AIR_GRANT); /* future version */
    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(copy, n, &out));

    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(buf, n - 1, &out)); /* truncated */
    memcpy(copy, buf, n);
    copy[n] = 0;
    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(copy, n + 1, &out)); /* trailing */

    memcpy(copy, buf, n);
    copy[9] = LC_BAND_COUNT; /* dl.band */
    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(copy, n, &out));

    /* DATA claiming 200 payload bytes but carrying 3 */
    const uint8_t short_data[] = { (LC_AIR_VERSION << 4) | LC_AIR_DATA, 1, 2, 3, 4, 0, 0, 200, 9, 9, 9 };
    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(short_data, sizeof(short_data), &out));

    /* beacon claiming 3 pages */
    in = make_beacon(0);
    n = lc_air_encode(&in, buf, sizeof(buf));
    buf[16] = 3;
    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(buf, n, &out));

    TEST_ASSERT_EQUAL_INT(-1, lc_air_decode(buf, 0, &out));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_byte_carries_version_and_type);
    RUN_TEST(test_beacon_roundtrip_and_size);
    RUN_TEST(test_beacon_v2_golden_bytes);
    RUN_TEST(test_beacon_anchor_range);
    RUN_TEST(test_v1_beacon_rejected);
    RUN_TEST(test_sync_byte_costs_no_airtime);
    RUN_TEST(test_max_beacon_fits_edge_tier_budget);
    RUN_TEST(test_grant_roundtrip);
    RUN_TEST(test_revoke_grant_has_empty_legs);
    RUN_TEST(test_rach_and_data_roundtrip);
    RUN_TEST(test_grant_leg_channel_matches_lc_hop);
    RUN_TEST(test_encode_rejects_out_of_range_fields);
    RUN_TEST(test_decode_rejects_bad_frames);
    return UNITY_END();
}
