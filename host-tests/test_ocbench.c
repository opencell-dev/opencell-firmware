#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "oc_exec.h"
#include "ocbench_core.h"
#include "ocb_cell.h"
#include "ocb_merge.h"
#include "exec_fixture.h" /* board A's oc_exec on a locked clock */

void setUp(void) {}
void tearDown(void) {}

static uint8_t payload[255];
static oc_msg_t m;

static void test_parse_tier_and_band(void)
{
    oc_tier_t t;
    TEST_ASSERT_EQUAL_INT(0, ocb_parse_tier("mid", &t));
    TEST_ASSERT_EQUAL_INT(OC_TIER_MID, t);
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_tier("far", &t));
    TEST_ASSERT_EQUAL_INT(OC_BAND_915, ocb_band_of(915250000u));
    TEST_ASSERT_EQUAL_INT(OC_BAND_2G4, ocb_band_of(2402000000u));
}

static void test_payload_roundtrip_and_corruption(void)
{
    uint32_t f;
    ocb_fill_payload(payload, 28, 123456);
    TEST_ASSERT_EQUAL_INT(0, ocb_check_payload(payload, 28, &f));
    TEST_ASSERT_EQUAL_UINT32(123456, f);
    payload[20] ^= 1;
    TEST_ASSERT_EQUAL_INT(-1, ocb_check_payload(payload, 28, &f));
    TEST_ASSERT_EQUAL_INT(-1, ocb_check_payload(payload, 4, &f));
}

static void test_tx_schedule_single_slot(void)
{
    ocb_link_cfg_t cfg = { 915250000u, OC_TIER_EDGE, 20000, 0, 28, 0, 0 };
    TEST_ASSERT_EQUAL_INT(0, ocb_link_schedule(&cfg, 77, 1, payload, &m));
    TEST_ASSERT_EQUAL_UINT8(OC_MSG_SCHEDULE, m.type);
    TEST_ASSERT_EQUAL_UINT8(OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST, m.u.schedule.flags);
    TEST_ASSERT_EQUAL_UINT8(1, m.u.schedule.slot_count);
    const oc_slot_t *s = &m.u.schedule.slots[0];
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_TX, s->dir);
    TEST_ASSERT_EQUAL_UINT32(20000, s->offset_us);
    TEST_ASSERT_EQUAL_UINT32(16704u + OC_GUARD_US, s->length_us);
    uint32_t f;
    TEST_ASSERT_EQUAL_INT(0, ocb_check_payload(s->payload, s->payload_len, &f));
    TEST_ASSERT_EQUAL_UINT32(77, f);
}

static void test_rx_window_is_centred_and_clamped(void)
{
    ocb_link_cfg_t cfg = { 915250000u, OC_TIER_EDGE, 20000, 20904, 28, 0, 0 };
    TEST_ASSERT_EQUAL_INT(0, ocb_link_schedule(&cfg, 1, 0, payload, &m));
    const oc_slot_t *s = &m.u.schedule.slots[0];
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_RX, s->dir);
    /* centred on the TX slot (airtime 16704 + guard): half the spare before it */
    TEST_ASSERT_EQUAL_UINT32(20000u - (20904u - (16704u + OC_GUARD_US)) / 2u, s->offset_us);
    TEST_ASSERT_EQUAL_UINT32(20904, s->length_us);

    cfg.rx_window_us = 100000; /* wide window starting before 0 -> clamped */
    ocb_link_schedule(&cfg, 1, 0, payload, &m);
    TEST_ASSERT_EQUAL_UINT32(0, m.u.schedule.slots[0].offset_us);
    TEST_ASSERT_TRUE(m.u.schedule.slots[0].length_us <= OC_FRAME_US);
}

static void test_invalid_link_configs(void)
{
    ocb_link_cfg_t cfg = { 2402000000u, OC_TIER_EDGE, 0, 0, 28, 0, 0 }; /* no edge tier on 2.4 */
    TEST_ASSERT_EQUAL_INT(-1, ocb_link_schedule(&cfg, 1, 1, payload, &m));
    cfg = (ocb_link_cfg_t){ 915250000u, OC_TIER_EDGE, 110000, 0, 28, 0, 0 }; /* runs past frame end */
    TEST_ASSERT_EQUAL_INT(-1, ocb_link_schedule(&cfg, 1, 1, payload, &m));
    cfg.offset_us = 0;
    cfg.payload_len = 4;
    TEST_ASSERT_EQUAL_INT(-1, ocb_link_schedule(&cfg, 1, 1, payload, &m));
}

static void test_guard_schedule_places_b_after_gap(void)
{
    ocb_link_cfg_t cfg = { 915250000u, OC_TIER_NEAR, 1000, 0, 28, 0, 0 };
    TEST_ASSERT_EQUAL_INT(0, ocb_guard_schedule(&cfg, 0, OC_TIER_EDGE, OC_DIR_TX, 150, 3, 1, payload, &m));
    TEST_ASSERT_EQUAL_UINT8(2, m.u.schedule.slot_count);
    const oc_slot_t *a = &m.u.schedule.slots[0];
    const oc_slot_t *b = &m.u.schedule.slots[1];
    TEST_ASSERT_EQUAL_UINT8(OC_MOD_LORA, a->mode.modulation);
    TEST_ASSERT_EQUAL_UINT32(16704u + 150u, a->length_us);
    TEST_ASSERT_EQUAL_UINT8(OC_MOD_FLRC, b->mode.modulation);
    TEST_ASSERT_EQUAL_UINT32(1000u + 16704u + 150u, b->offset_us);
    uint32_t b_start = b->offset_us;
    uint32_t b_end = b->offset_us + b->length_us;

    /* RX board listens around B */
    TEST_ASSERT_EQUAL_INT(0, ocb_guard_schedule(&cfg, 0, OC_TIER_EDGE, OC_DIR_TX, 150, 3, 0, payload, &m));
    const oc_slot_t *w = &m.u.schedule.slots[0];
    TEST_ASSERT_EQUAL_UINT8(1, m.u.schedule.slot_count);
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_RX, w->dir);
    TEST_ASSERT_TRUE(w->offset_us <= b_start);
    TEST_ASSERT_TRUE(w->offset_us + w->length_us >= b_end);
}

static void test_guard_band_switch_puts_a_on_other_band(void)
{
    ocb_link_cfg_t cfg = { 915250000u, OC_TIER_EDGE, 1000, 0, 28, 0, 0 };
    TEST_ASSERT_EQUAL_INT(0, ocb_guard_schedule(&cfg, 2440000000u, OC_TIER_NEAR, OC_DIR_TX, 500, 3, 1, payload, &m));
    TEST_ASSERT_EQUAL_UINT32(2440000000u, m.u.schedule.slots[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT32(1300000u, m.u.schedule.slots[0].mode.bitrate_bps); /* 2.4 near tier */
    TEST_ASSERT_EQUAL_UINT32(915250000u, m.u.schedule.slots[1].freq_hz);
    TEST_ASSERT_EQUAL_INT(-1, ocb_guard_schedule(&cfg, 2440000000u, OC_TIER_EDGE, OC_DIR_TX, 500, 3, 1, payload, &m));
}

static void test_guard_schedules_pass_firmware_validation(void)
{
    /* Both boards' schedules must be accepted by oc_exec (sorted, in frame, airtime fits). */
    ocb_link_cfg_t cfg = { 915250000u, OC_TIER_NEAR, 1000, 0, 28, 0, 0 };
    for (int dir = OC_DIR_RX; dir <= OC_DIR_TX; dir++) {
        TEST_ASSERT_EQUAL_INT(0, ocb_guard_schedule(&cfg, 0, OC_TIER_MID, (uint8_t)dir, 300, 3, 1, payload, &m));
        for (uint8_t i = 0; i < m.u.schedule.slot_count; i++) {
            const oc_slot_t *s = &m.u.schedule.slots[i];
            uint32_t air = oc_airtime_us(&s->mode, s->dir == OC_DIR_TX ? s->payload_len : 0);
            TEST_ASSERT_TRUE(air > 0);
            if (s->dir == OC_DIR_TX) TEST_ASSERT_TRUE(air <= s->length_us);
            if (i > 0) {
                const oc_slot_t *p = &m.u.schedule.slots[i - 1];
                TEST_ASSERT_TRUE(s->offset_us >= p->offset_us + p->length_us);
            }
        }
    }
}

static void test_cw_schedule_fits_firmware_limits(void)
{
    const ocb_link_cfg_t cfgs[] = {
        { 915250000u, OC_TIER_EDGE, 0, 0, 28, 0, 0 },
        { 915250000u, OC_TIER_NEAR, 0, 0, 255, 0, 0 },
        { 2402000000u, OC_TIER_NEAR, 0, 0, 28, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++) {
        int n = ocb_cw_schedule(&cfgs[i], 5, payload, &m);
        TEST_ASSERT_TRUE(n > 0);
        TEST_ASSERT_TRUE((uint32_t)n <= OC_EXEC_MAX_SLOTS);
        TEST_ASSERT_TRUE((uint32_t)n * cfgs[i].payload_len <= OC_EXEC_PAYLOAD_POOL);
        const oc_slot_t *last = &m.u.schedule.slots[n - 1];
        TEST_ASSERT_TRUE(last->offset_us + last->length_us <= OC_FRAME_US);
        uint8_t buf[OC_LINK_MAX_MSG];
        TEST_ASSERT_TRUE(oc_msg_encode(&m, buf, sizeof(buf)) > 0); /* fits one message */
    }
}

/* Packet-end timing: only good packets with a measured end count. */
static void test_stats_end_timing(void)
{
    ocb_stats_t st;
    ocb_stats_init(&st);
    ocb_fill_payload(payload, 28, 9);
    oc_rx_report_t a = { 9, 0, -90, 20, 1, 28, payload, 20100 };
    oc_rx_report_t b = { 9, 0, -90, 20, 1, 28, payload, 20140 };
    oc_rx_report_t unknown = { 9, 0, -90, 20, 1, 28, payload, OC_RX_END_UNKNOWN };
    oc_rx_report_t crc = { 9, 0, -90, 20, 0, 28, payload, 99999 };
    ocb_stats_add_rx(&st, &a);
    ocb_stats_add_rx(&st, &b);
    ocb_stats_add_rx(&st, &unknown);
    ocb_stats_add_rx(&st, &crc);
    TEST_ASSERT_EQUAL_UINT32(3, st.received);
    TEST_ASSERT_EQUAL_UINT32(2, st.timed);
    TEST_ASSERT_EQUAL_INT32(20100, st.end_min);
    TEST_ASSERT_EQUAL_INT32(20140, st.end_max);
    TEST_ASSERT_EQUAL_INT(20120000, (int)(ocb_stats_end_mean(&st) * 1000.0 + 0.5));
    TEST_ASSERT_EQUAL_INT(20000, (int)(ocb_stats_end_sd(&st) * 1000.0 + 0.5));
}

static void test_stats_accumulate(void)
{
    ocb_stats_t st;
    ocb_stats_init(&st);
    ocb_fill_payload(payload, 28, 9);
    oc_rx_report_t good = { 9, 0, -90, 20, 1, 28, payload, OC_RX_END_UNKNOWN };
    oc_rx_report_t weak = { 10, 0, -110, -8, 1, 28, payload, OC_RX_END_UNKNOWN };
    oc_rx_report_t crc = { 11, 0, -120, 0, 0, 28, payload, OC_RX_END_UNKNOWN };
    ocb_stats_add_rx(&st, &good);
    ocb_stats_add_rx(&st, &weak);
    ocb_stats_add_rx(&st, &crc);
    TEST_ASSERT_EQUAL_UINT32(2, st.received);
    TEST_ASSERT_EQUAL_UINT32(1, st.crc_fail);
    TEST_ASSERT_EQUAL_INT16(-110, st.rssi_min);
    TEST_ASSERT_EQUAL_INT16(-90, st.rssi_max);
    TEST_ASSERT_EQUAL_INT32(-200, st.rssi_sum);
    TEST_ASSERT_EQUAL_INT32(12, st.snr_sum_qdb);

    uint8_t junk[28] = { 0 };
    oc_rx_report_t bad = { 12, 0, -90, 0, 1, 28, junk, OC_RX_END_UNKNOWN };
    ocb_stats_add_rx(&st, &bad);
    TEST_ASSERT_EQUAL_UINT32(1, st.bad_payload);
}

/* Cross-band duplex probe: DL then UL in one frame, possibly on different bands. */
static void test_duplex_schedule_base_and_terminal_mirror(void)
{
    static uint8_t dl[28], ul[28];
    ocb_duplex_cfg_t cfg = { 915250000u, OC_TIER_EDGE, 2440000000u, OC_TIER_NEAR, 20000, 1500, 28 };
    uint32_t dl_len = oc_slot_len_us(oc_tier_mode(OC_BAND_915, OC_TIER_EDGE), 28);
    uint32_t ul_len = oc_slot_len_us(oc_tier_mode(OC_BAND_2G4, OC_TIER_NEAR), 28);

    TEST_ASSERT_EQUAL_INT(0, ocb_duplex_schedule(&cfg, 500, 1, dl, ul, &m));
    TEST_ASSERT_EQUAL_UINT8(OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST, m.u.schedule.flags);
    TEST_ASSERT_EQUAL_UINT8(2, m.u.schedule.slot_count);
    const oc_slot_t *a = &m.u.schedule.slots[0], *b = &m.u.schedule.slots[1];
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_TX, a->dir);
    TEST_ASSERT_EQUAL_UINT32(915250000u, a->freq_hz);
    TEST_ASSERT_EQUAL_UINT32(20000u, a->offset_us);
    TEST_ASSERT_EQUAL_UINT32(dl_len, a->length_us);
    uint32_t f;
    TEST_ASSERT_EQUAL_INT(0, ocb_check_payload(a->payload, a->payload_len, &f));
    TEST_ASSERT_EQUAL_UINT32(500, f);
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_RX, b->dir);
    TEST_ASSERT_EQUAL_UINT32(2440000000u, b->freq_hz);
    TEST_ASSERT_EQUAL_UINT32(20000u + dl_len + 1500u, b->offset_us);
    TEST_ASSERT_EQUAL_UINT32(ul_len, b->length_us);

    TEST_ASSERT_EQUAL_INT(0, ocb_duplex_schedule(&cfg, 500, 0, dl, ul, &m));
    a = &m.u.schedule.slots[0];
    b = &m.u.schedule.slots[1];
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_RX, a->dir);
    TEST_ASSERT_EQUAL_UINT32(20000u, a->offset_us);
    TEST_ASSERT_EQUAL_UINT32(dl_len, a->length_us);
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_TX, b->dir);
    TEST_ASSERT_EQUAL_UINT32(20000u + dl_len + 1500u, b->offset_us);
    TEST_ASSERT_EQUAL_INT(0, ocb_check_payload(b->payload, b->payload_len, &f));
    TEST_ASSERT_EQUAL_UINT32(500, f);
}

static void test_duplex_schedule_rejects_bad_config(void)
{
    static uint8_t dl[28], ul[28];
    ocb_duplex_cfg_t cfg = { 915250000u, OC_TIER_EDGE, 2440000000u, OC_TIER_EDGE, 20000, 1500, 28 };
    TEST_ASSERT_EQUAL_INT(-1, ocb_duplex_schedule(&cfg, 1, 1, dl, ul, &m)); /* no edge tier on 2.4 */
    cfg.ul_tier = OC_TIER_NEAR;
    cfg.offset_us = 110000; /* the pair doesn't fit in the frame */
    TEST_ASSERT_EQUAL_INT(-1, ocb_duplex_schedule(&cfg, 1, 1, dl, ul, &m));
    cfg.offset_us = 20000;
    cfg.payload_len = 4; /* below OCB_MIN_PAYLOAD */
    TEST_ASSERT_EQUAL_INT(-1, ocb_duplex_schedule(&cfg, 1, 1, dl, ul, &m));
}

static uint32_t hook_tmid;
static uint8_t hook_p[32], hook_n;
static int hook_ul, hook_upper;
static void on_ul(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    hook_ul++;
    hook_tmid = tmid;
    memcpy(hook_p, p, n);
    hook_n = n;
}
static void on_upper(void *c, uint32_t tmid, const uint8_t *p, uint8_t n)
{
    (void)c;
    hook_upper++;
    hook_tmid = tmid;
    memcpy(hook_p, p, n);
    hook_n = n;
}

/* Slot index of the first slot of `kind` in frame f on 915, or -1. */
static int kind_slot(const ocb_cell_t *c, uint32_t f, uint8_t kind)
{
    const ocb_cell_kinds_t *kd = &c->kinds[OC_BAND_915][f % OCB_CELL_KIND_FRAMES];
    for (uint8_t i = 0; kd->frame == f && i < kd->count; i++) {
        if (kd->kind[i] == kind) return i;
    }
    return -1;
}

static void cell_rx(ocb_cell_t *c, uint32_t f, int slot, const oc_air_msg_t *a)
{
    static uint8_t buf[64];
    size_t n = oc_air_encode(a, buf, sizeof(buf));
    oc_rx_report_t r = { f, (uint8_t)slot, -60, 40, 1, (uint8_t)n, buf, OC_RX_END_UNKNOWN };
    ocb_cell_on_rx(c, OC_BAND_915, &r);
}

static void test_cell_beacon_carries_part97_flag(void)
{
    static ocb_cell_t c;
    oc_air_msg_t b;
    ocb_cell_init(&c, 0x1234u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_schedule(&c, OC_BAND_915, 10, &m));
    TEST_ASSERT_EQUAL_INT(0, oc_air_decode(m.u.schedule.slots[0].payload, m.u.schedule.slots[0].payload_len, &b));
    TEST_ASSERT_EQUAL_HEX8(0, b.u.beacon.flags & OC_BCN_FLAG_PART97);
    c.part97 = 1;
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_schedule(&c, OC_BAND_915, 11, &m));
    TEST_ASSERT_EQUAL_INT(0, oc_air_decode(m.u.schedule.slots[0].payload, m.u.schedule.slots[0].payload_len, &b));
    TEST_ASSERT_EQUAL_HEX8(OC_BCN_FLAG_PART97, b.u.beacon.flags & OC_BCN_FLAG_PART97);
}

/* Channel-list spec §4: the beacon carries the anchor and cfg_ver and goes
 * out on the anchor's cycle, or on the anchor itself with FIXED (Part 97). */
static void test_cell_beacon_carries_anchor_and_sync(void)
{
    static ocb_cell_t c;
    oc_air_msg_t b;
    ocb_cell_init(&c, 0x1234u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    TEST_ASSERT_EQUAL_UINT8(0x1234u % 6u, c.sync_ch); /* the seed-derived anchor by default */
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&c, 30, 0));
    c.cfg_ver = 6; /* the beacon carries it mod 4 */
    for (uint32_t f = 16; f < 24; f++) {
        TEST_ASSERT_EQUAL_INT(0, ocb_cell_schedule(&c, OC_BAND_915, f, &m));
        const oc_slot_t *sl = &m.u.schedule.slots[0];
        TEST_ASSERT_EQUAL_INT(0, oc_air_decode(sl->payload, sl->payload_len, &b));
        TEST_ASSERT_EQUAL_UINT8(30, b.u.beacon.anchor);
        TEST_ASSERT_EQUAL_UINT8(2, b.u.beacon.cfg_ver);
        TEST_ASSERT_EQUAL_HEX8(0, b.u.beacon.flags & OC_BCN_FLAG_FIXED_SYNC);
        TEST_ASSERT_EQUAL_UINT32(oc_channel_freq_hz(OC_BAND_915, oc_sync_channel_at(30, OC_BAND_915, f)), sl->freq_hz);
    }
    TEST_ASSERT_EQUAL_INT(-1, ocb_cell_set_sync(&c, 20, 1)); /* FIXED needs Part 97 */
    TEST_ASSERT_EQUAL_INT(-1, ocb_cell_set_sync(&c, 52, 0));
    TEST_ASSERT_EQUAL_UINT8(30, c.sync_ch); /* refused: unchanged */
    TEST_ASSERT_FALSE(c.fixed_sync);
    c.part97 = 1;
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_set_sync(&c, 20, 1));
    for (uint32_t f = 24; f < 32; f++) {
        TEST_ASSERT_EQUAL_INT(0, ocb_cell_schedule(&c, OC_BAND_915, f, &m));
        const oc_slot_t *sl = &m.u.schedule.slots[0];
        TEST_ASSERT_EQUAL_INT(0, oc_air_decode(sl->payload, sl->payload_len, &b));
        TEST_ASSERT_EQUAL_HEX8(OC_BCN_FLAG_FIXED_SYNC | OC_BCN_FLAG_PART97,
                               b.u.beacon.flags & (OC_BCN_FLAG_FIXED_SYNC | OC_BCN_FLAG_PART97));
        TEST_ASSERT_EQUAL_UINT32(912250000u, sl->freq_hz); /* ch 20 every frame */
    }
}

/* Hooks take UL DATA and RACH UPPER; queued DL payloads go out in the DL
 * slot; release takes the legs away. */
static void test_cell_hooks_dl_queue_and_release(void)
{
    static ocb_cell_t c;
    ocb_cell_init(&c, 0x1234u, OC_TIER_EDGE, OC_BAND_915, OC_BAND_915);
    ocb_cell_hooks_t h = { NULL, on_ul, on_upper };
    ocb_cell_set_hooks(&c, &h);
    hook_ul = hook_upper = 0;
    oc_air_msg_t a;

    uint32_t f = 100;
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_schedule(&c, OC_BAND_915, f, &m));
    memset(&a, 0, sizeof(a));
    a.type = OC_AIR_RACH;
    a.u.rach = (oc_rach_t){ 0x42u, OC_RACH_ATTACH, 0, NULL };
    cell_rx(&c, f, kind_slot(&c, f, OCB_SLOT_RACH), &a);
    TEST_ASSERT_FALSE(ocb_cell_granted(&c, 0x42u)); /* grant queued, not yet in force */
    for (f++; f < 120 && !ocb_cell_granted(&c, 0x42u); f++) ocb_cell_schedule(&c, OC_BAND_915, f, &m);
    TEST_ASSERT_TRUE(ocb_cell_granted(&c, 0x42u));

    static const uint8_t payload[3] = { 0x80, 0x07, 0x55 };
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_dl_push(&c, 0x42u, payload, 3));
    TEST_ASSERT_EQUAL_INT(-1, ocb_cell_dl_push(&c, 0x99u, payload, 3)); /* unknown terminal */
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_schedule(&c, OC_BAND_915, f, &m));
    int dl = kind_slot(&c, f, OCB_SLOT_DL), ul = kind_slot(&c, f, OCB_SLOT_UL);
    TEST_ASSERT_TRUE(dl >= 0 && ul >= 0);
    oc_air_msg_t d;
    TEST_ASSERT_EQUAL_INT(0, oc_air_decode(m.u.schedule.slots[dl].payload, m.u.schedule.slots[dl].payload_len, &d));
    TEST_ASSERT_EQUAL_UINT8(OC_AIR_DATA, d.type);
    TEST_ASSERT_EQUAL_UINT8(3, d.u.data.payload_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, d.u.data.payload, 3);

    memset(&a, 0, sizeof(a));
    a.type = OC_AIR_DATA;
    a.u.data = (oc_data_t){ 0x42u, 1, 0, 3, payload };
    cell_rx(&c, f, ul, &a);
    TEST_ASSERT_EQUAL_INT(1, hook_ul);
    TEST_ASSERT_EQUAL_UINT32(0x42u, hook_tmid);
    TEST_ASSERT_EQUAL_UINT8(3, hook_n);
    TEST_ASSERT_EQUAL_UINT8(0, c.terms[0].loop_len); /* hooked: no echo */

    static const uint8_t svc = 0x32;
    f++;
    TEST_ASSERT_EQUAL_INT(0, ocb_cell_schedule(&c, OC_BAND_915, f, &m));
    memset(&a, 0, sizeof(a));
    a.type = OC_AIR_RACH;
    a.u.rach = (oc_rach_t){ 0x42u, OC_RACH_UPPER, 1, &svc };
    cell_rx(&c, f, kind_slot(&c, f, OCB_SLOT_RACH), &a);
    TEST_ASSERT_EQUAL_INT(1, hook_upper);
    TEST_ASSERT_EQUAL_UINT8(1, hook_n);
    TEST_ASSERT_EQUAL_HEX8(0x32, hook_p[0]);

    ocb_cell_release(&c, 0x42u);
    for (f++; f < 140 && ocb_cell_granted(&c, 0x42u); f++) ocb_cell_schedule(&c, OC_BAND_915, f, &m);
    TEST_ASSERT_FALSE(ocb_cell_granted(&c, 0x42u));
}

/* Board A's oc_exec must accept every --one-board SCHEDULE once two
 * terminals hold grants (bench 2026-09-27: every one came back MALFORMED).
 * Each frame goes through oc_exec_add_part() one frame ahead, as ocbench
 * sends it; both terminals attach over RACH like on the air. */
static void check_two_terms_one_board(oc_tier_t tier, oc_band_t dl, oc_band_t ul)
{
    static ocb_cell_t c;
    static ocb_one_map_t maps[OCB_ONE_MAP_FRAMES];
    char msg[96];
    fixture_reset();
    ocb_cell_init(&c, 0x1234u, tier, dl, ul);
    const uint32_t tmid[2] = { 0x76ad0488u, 0x76ae2064u };
    int attached = 0;
    for (uint32_t f = F0 + 1; f < F0 + 70; f++) {
        if (f == F0 + 40) { /* both in force; then one released (net: 5 s of silence), one sending */
            TEST_ASSERT_TRUE_MESSAGE(ocb_cell_granted(&c, tmid[0]) && ocb_cell_granted(&c, tmid[1]), msg);
            ocb_cell_release(&c, tmid[0]);
            TEST_ASSERT_EQUAL_INT(0, ocb_cell_dl_push(&c, tmid[1], payload, OCB_CELL_PAYLOAD));
        }
        TEST_ASSERT_EQUAL_INT(0, ocb_merge_bands(&c, f, 0, maps, &m));
        uint64_t prev_start;
        TEST_ASSERT_EQUAL_INT(0, oc_clock_frame_start_us(&clk, f - 1, &prev_start));
        snprintf(msg, sizeof(msg), "tier %d dl %d ul %d frame +%u, %d attached, %u slots", (int)tier, (int)dl,
                 (int)ul, (unsigned)(f - F0), attached, m.u.schedule.slot_count);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(OC_ACK_OK, oc_exec_add_part(&exec_, &m.u.schedule, &clk, prev_start + 10000),
                                        msg);
        if (attached < 2 && (f - F0) % 4 == 1) { /* the next terminal's ATTACH in this frame's RACH */
            oc_air_msg_t a;
            memset(&a, 0, sizeof(a));
            a.type = OC_AIR_RACH;
            a.u.rach = (oc_rach_t){ tmid[attached++], OC_RACH_ATTACH, 0, NULL };
            cell_rx(&c, f, kind_slot(&c, f, OCB_SLOT_RACH), &a);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(!ocb_cell_granted(&c, tmid[0]) && ocb_cell_granted(&c, tmid[1]), msg);
}

static void test_cell_two_terminals_one_board_pass_firmware_validation(void)
{
    for (int t = 0; t < (int)OC_TIER_COUNT; t++) {
        for (int d = 0; d < (int)OC_BAND_COUNT; d++) {
            for (int u = 0; u < (int)OC_BAND_COUNT; u++) {
                static ocb_cell_t probe;
                oc_grant_leg_t l1, l2;
                ocb_cell_init(&probe, 0x1234u, (oc_tier_t)t, (oc_band_t)d, (oc_band_t)u);
                if (ocb_cell_legs(&probe, 1, &l1, &l2) == 0) { /* ocbench cell refuses the others */
                    check_two_terms_one_board((oc_tier_t)t, (oc_band_t)d, (oc_band_t)u);
                }
            }
        }
    }
}

/* Option values: a whole decimal number in range, nothing else. */
static void test_parse_int_strict(void)
{
    long v = 99;
    TEST_ASSERT_EQUAL_INT(0, ocb_parse_int("0", 0, 51, &v));
    TEST_ASSERT_EQUAL_INT(0, v);
    TEST_ASSERT_EQUAL_INT(0, ocb_parse_int("51", 0, 51, &v));
    TEST_ASSERT_EQUAL_INT(51, v);
    TEST_ASSERT_EQUAL_INT(0, ocb_parse_int("255", 0, 255, &v));
    TEST_ASSERT_EQUAL_INT(255, v);
    v = 7;
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int("abc", 0, 51, &v));
    TEST_ASSERT_EQUAL_INT(7, v); /* untouched on a refusal */
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int("", 0, 51, &v));
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int("12x", 0, 51, &v));
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int(" 12", 0, 51, &v));
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int("0x10", 0, 255, &v));
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int("52", 0, 51, &v));
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int("-1", 0, 51, &v));
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int("256", 0, 255, &v));
    TEST_ASSERT_EQUAL_INT(-1, ocb_parse_int("99999999999999999999", 0, 255, &v));
    TEST_ASSERT_EQUAL_INT(7, v);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_tier_and_band);
    RUN_TEST(test_payload_roundtrip_and_corruption);
    RUN_TEST(test_tx_schedule_single_slot);
    RUN_TEST(test_rx_window_is_centred_and_clamped);
    RUN_TEST(test_invalid_link_configs);
    RUN_TEST(test_guard_schedule_places_b_after_gap);
    RUN_TEST(test_guard_band_switch_puts_a_on_other_band);
    RUN_TEST(test_guard_schedules_pass_firmware_validation);
    RUN_TEST(test_cw_schedule_fits_firmware_limits);
    RUN_TEST(test_stats_accumulate);
    RUN_TEST(test_stats_end_timing);
    RUN_TEST(test_duplex_schedule_base_and_terminal_mirror);
    RUN_TEST(test_duplex_schedule_rejects_bad_config);
    RUN_TEST(test_cell_hooks_dl_queue_and_release);
    RUN_TEST(test_cell_beacon_carries_part97_flag);
    RUN_TEST(test_cell_beacon_carries_anchor_and_sync);
    RUN_TEST(test_parse_int_strict);
    RUN_TEST(test_cell_two_terminals_one_board_pass_firmware_validation);
    return UNITY_END();
}
