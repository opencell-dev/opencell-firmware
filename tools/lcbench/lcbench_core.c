#include "lcbench_core.h"

#include <string.h>

#include "lc_exec.h" /* LC_EXEC_* limits the firmware enforces */

int lcb_parse_tier(const char *s, lc_tier_t *out)
{
    static const char *names[LC_TIER_COUNT] = { "near", "mid", "edge" };
    for (int i = 0; i < LC_TIER_COUNT; i++) {
        if (strcmp(s, names[i]) == 0) {
            *out = (lc_tier_t)i;
            return 0;
        }
    }
    return -1;
}

lc_band_t lcb_band_of(uint32_t freq_hz)
{
    return freq_hz >= 1500000000u ? LC_BAND_2G4 : LC_BAND_915;
}

void lcb_fill_payload(uint8_t *buf, uint8_t len, uint32_t frame_number)
{
    buf[0] = 'L';
    buf[1] = 'C';
    buf[2] = 'B';
    buf[3] = '1';
    buf[4] = (uint8_t)frame_number;
    buf[5] = (uint8_t)(frame_number >> 8);
    buf[6] = (uint8_t)(frame_number >> 16);
    buf[7] = (uint8_t)(frame_number >> 24);
    for (uint8_t i = LCB_MIN_PAYLOAD; i < len; i++) {
        buf[i] = i;
    }
}

int lcb_check_payload(const uint8_t *buf, uint8_t len, uint32_t *frame_number)
{
    if (len < LCB_MIN_PAYLOAD || memcmp(buf, "LCB1", 4) != 0) {
        return -1;
    }
    for (uint8_t i = LCB_MIN_PAYLOAD; i < len; i++) {
        if (buf[i] != i) {
            return -1;
        }
    }
    *frame_number = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) |
                    ((uint32_t)buf[7] << 24);
    return 0;
}

static void begin_schedule(lc_msg_t *out, uint32_t frame_number)
{
    memset(out, 0, sizeof(*out));
    out->type = LC_MSG_SCHEDULE;
    out->u.schedule.frame_number = frame_number;
    out->u.schedule.flags = LC_SCHED_FLAG_LAST;
}

int lcb_link_schedule(const lcb_link_cfg_t *cfg, uint32_t frame_number, int tx, uint8_t *payload_buf,
                      lc_msg_t *out)
{
    const lc_mode_t *mode = lc_tier_mode(lcb_band_of(cfg->freq_hz), cfg->tier);
    if (mode == NULL || cfg->payload_len < LCB_MIN_PAYLOAD) {
        return -1;
    }
    uint32_t tx_len = lc_slot_len_us(mode, cfg->payload_len);
    if ((uint64_t)cfg->offset_us + tx_len > LC_FRAME_US) {
        return -1;
    }
    begin_schedule(out, frame_number);
    out->u.schedule.slot_count = 1;
    lc_slot_t *s = &out->u.schedule.slots[0];
    s->freq_hz = cfg->freq_hz;
    s->mode = *mode;
    if (tx) {
        lcb_fill_payload(payload_buf, cfg->payload_len, frame_number);
        s->offset_us = cfg->offset_us;
        s->length_us = tx_len;
        s->dir = LC_DIR_TX;
        s->payload_len = cfg->payload_len;
        s->payload = payload_buf;
    } else {
        uint32_t win = cfg->rx_window_us > tx_len ? cfg->rx_window_us : tx_len;
        uint32_t pad = (win - tx_len) / 2;
        uint32_t start = cfg->offset_us > pad ? cfg->offset_us - pad : 0;
        if (start + win > LC_FRAME_US) {
            win = LC_FRAME_US - start;
        }
        s->offset_us = start;
        s->length_us = win;
        s->dir = LC_DIR_RX;
    }
    return 0;
}

int lcb_guard_schedule(const lcb_link_cfg_t *cfg, uint32_t freq_a_hz, lc_tier_t tier_a, uint8_t dir_a,
                       uint32_t gap_us, uint32_t frame_number, int tx, uint8_t *payload_buf, lc_msg_t *out)
{
    if (freq_a_hz == 0) {
        freq_a_hz = cfg->freq_hz;
    }
    const lc_mode_t *mode_a = lc_tier_mode(lcb_band_of(freq_a_hz), tier_a);
    const lc_mode_t *mode_b = lc_tier_mode(lcb_band_of(cfg->freq_hz), cfg->tier);
    if (mode_a == NULL || mode_b == NULL || cfg->payload_len < LCB_MIN_PAYLOAD ||
        (dir_a != LC_DIR_TX && dir_a != LC_DIR_RX)) {
        return -1;
    }
    uint32_t len_a = lc_airtime_us(mode_a, cfg->payload_len) + gap_us;
    if (dir_a == LC_DIR_RX && len_a <= LC_GUARD_US) {
        return -1; /* the executor's RX timeout is length - LC_GUARD_US */
    }
    uint32_t off_b = cfg->offset_us + len_a;
    uint32_t len_b = lc_slot_len_us(mode_b, cfg->payload_len);
    if ((uint64_t)off_b + len_b > LC_FRAME_US) {
        return -1;
    }
    if (!tx) {
        lcb_link_cfg_t rx = *cfg;
        rx.offset_us = off_b;
        return lcb_link_schedule(&rx, frame_number, 0, payload_buf, out);
    }
    lcb_fill_payload(payload_buf, cfg->payload_len, frame_number);
    begin_schedule(out, frame_number);
    out->u.schedule.slot_count = 2;
    lc_slot_t *a = &out->u.schedule.slots[0];
    lc_slot_t *b = &out->u.schedule.slots[1];
    *a = (lc_slot_t){ cfg->offset_us, len_a, freq_a_hz, *mode_a, dir_a,
                      dir_a == LC_DIR_TX ? cfg->payload_len : 0, dir_a == LC_DIR_TX ? payload_buf : NULL };
    *b = (lc_slot_t){ off_b, len_b, cfg->freq_hz, *mode_b, LC_DIR_TX, cfg->payload_len, payload_buf };
    return 0;
}

int lcb_cw_schedule(const lcb_link_cfg_t *cfg, uint32_t frame_number, uint8_t *payload_buf, lc_msg_t *out)
{
    const lc_mode_t *mode = lc_tier_mode(lcb_band_of(cfg->freq_hz), cfg->tier);
    if (mode == NULL || cfg->payload_len < LCB_MIN_PAYLOAD) {
        return -1;
    }
    uint32_t len = lc_slot_len_us(mode, cfg->payload_len);
    uint32_t by_time = LC_FRAME_US / len;
    uint32_t by_pool = LC_EXEC_PAYLOAD_POOL / cfg->payload_len; /* every slot's payload is copied */
    uint32_t n = by_time < by_pool ? by_time : by_pool;
    if (n > LC_EXEC_MAX_SLOTS) {
        n = LC_EXEC_MAX_SLOTS;
    }
    /* One SCHEDULE message must fit LC_LINK_MAX_MSG: 8 header + (27 + len) per slot. */
    uint32_t by_msg = (LC_LINK_MAX_MSG - 8u) / (27u + cfg->payload_len);
    if (n > by_msg) {
        n = by_msg;
    }
    if (n == 0) {
        return -1;
    }
    lcb_fill_payload(payload_buf, cfg->payload_len, frame_number);
    begin_schedule(out, frame_number);
    out->u.schedule.slot_count = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) {
        lc_slot_t *s = &out->u.schedule.slots[i];
        s->offset_us = i * len;
        s->length_us = len;
        s->freq_hz = cfg->freq_hz;
        s->mode = *mode;
        s->dir = LC_DIR_TX;
        s->payload_len = cfg->payload_len;
        s->payload = payload_buf;
    }
    return (int)n;
}

void lcb_stats_init(lcb_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->rssi_min = INT16_MAX;
    s->rssi_max = INT16_MIN;
}

void lcb_stats_add_rx(lcb_stats_t *s, const lc_rx_report_t *r)
{
    if (!r->crc_ok) {
        s->crc_fail++;
        return;
    }
    uint32_t f;
    if (lcb_check_payload(r->payload, r->payload_len, &f) != 0) {
        s->bad_payload++;
        return;
    }
    s->received++;
    s->rssi_sum += r->rssi_dbm;
    s->snr_sum_qdb += r->snr_qdb;
    if (r->rssi_dbm < s->rssi_min) s->rssi_min = r->rssi_dbm;
    if (r->rssi_dbm > s->rssi_max) s->rssi_max = r->rssi_dbm;
}
