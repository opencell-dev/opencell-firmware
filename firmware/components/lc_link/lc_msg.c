#include "lc_link.h"

#include <string.h>

/* Bounded little-endian writer/reader. Once an operation fails, ok stays 0
 * and later operations are no-ops, so callers check ok once at the end. */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    int      ok;
} writer_t;

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    int            ok;
} reader_t;

static void put_bytes(writer_t *w, const uint8_t *src, size_t n)
{
    if (!w->ok || n > w->cap - w->pos) {
        w->ok = 0;
        return;
    }
    if (n > 0) {
        memcpy(w->buf + w->pos, src, n);
    }
    w->pos += n;
}

static void put_u8(writer_t *w, uint8_t v) { put_bytes(w, &v, 1); }

static void put_u16(writer_t *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(w, b, 2);
}

static void put_u32(writer_t *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(w, b, 4);
}

static const uint8_t *get_bytes(reader_t *r, size_t n)
{
    if (!r->ok || n > r->len - r->pos) {
        r->ok = 0;
        return NULL;
    }
    const uint8_t *p = r->buf + r->pos;
    r->pos += n;
    return p;
}

static uint8_t get_u8(reader_t *r)
{
    const uint8_t *p = get_bytes(r, 1);
    return p ? p[0] : 0;
}

static uint16_t get_u16(reader_t *r)
{
    const uint8_t *p = get_bytes(r, 2);
    return p ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
}

static uint32_t get_u32(reader_t *r)
{
    const uint8_t *p = get_bytes(r, 4);
    return p ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                ((uint32_t)p[3] << 24))
             : 0;
}

static void put_slot(writer_t *w, const lc_slot_t *s)
{
    put_u32(w, s->offset_us);
    put_u32(w, s->length_us);
    put_u32(w, s->freq_hz);
    put_u8(w, s->mode.modulation);
    put_u8(w, s->mode.sf);
    put_u8(w, s->mode.cr);
    put_u16(w, s->mode.preamble);
    put_u32(w, s->mode.bw_hz);
    put_u32(w, s->mode.bitrate_bps);
    put_u8(w, s->dir);
    put_u8(w, s->payload_len);
    put_bytes(w, s->payload, s->payload_len);
}

static void get_slot(reader_t *r, lc_slot_t *s)
{
    s->offset_us = get_u32(r);
    s->length_us = get_u32(r);
    s->freq_hz = get_u32(r);
    s->mode.modulation = get_u8(r);
    s->mode.sf = get_u8(r);
    s->mode.cr = get_u8(r);
    s->mode.preamble = get_u16(r);
    s->mode.bw_hz = get_u32(r);
    s->mode.bitrate_bps = get_u32(r);
    s->dir = get_u8(r);
    s->payload_len = get_u8(r);
    s->payload = get_bytes(r, s->payload_len);
}

size_t lc_msg_encode(const lc_msg_t *msg, uint8_t *out, size_t cap)
{
    writer_t w = { out, cap < LC_LINK_MAX_MSG ? cap : LC_LINK_MAX_MSG, 0, 1 };
    put_u8(&w, msg->type);
    put_u8(&w, msg->seq);

    switch (msg->type) {
    case LC_MSG_CONFIG:
        put_u8(&w, msg->u.config.role);
        put_u8(&w, msg->u.config.band);
        put_u8(&w, msg->u.config.radio_index);
        put_u32(&w, msg->u.config.cell_seed);
        break;
    case LC_MSG_SCHEDULE:
        if (msg->u.schedule.slot_count > LC_MAX_SLOTS_PER_SCHEDULE) {
            return 0;
        }
        put_u32(&w, msg->u.schedule.frame_number);
        put_u8(&w, msg->u.schedule.flags);
        put_u8(&w, msg->u.schedule.slot_count);
        for (uint8_t i = 0; i < msg->u.schedule.slot_count; i++) {
            put_slot(&w, &msg->u.schedule.slots[i]);
        }
        break;
    case LC_MSG_RX_REPORT:
        put_u32(&w, msg->u.rx_report.frame_number);
        put_u8(&w, msg->u.rx_report.slot_index);
        put_u16(&w, (uint16_t)msg->u.rx_report.rssi_dbm);
        put_u16(&w, (uint16_t)msg->u.rx_report.snr_qdb);
        put_u8(&w, msg->u.rx_report.crc_ok);
        put_u8(&w, msg->u.rx_report.payload_len);
        put_bytes(&w, msg->u.rx_report.payload, msg->u.rx_report.payload_len);
        put_u32(&w, (uint32_t)msg->u.rx_report.end_us);
        break;
    case LC_MSG_STATUS:
        put_u32(&w, msg->u.status.uptime_ms);
        put_u8(&w, msg->u.status.pps_locked);
        put_u8(&w, (uint8_t)msg->u.status.temp_c);
        put_u16(&w, msg->u.status.schedule_misses);
        put_u16(&w, msg->u.status.uart_crc_errors);
        put_u32(&w, msg->u.status.frame_number);
        put_u32(&w, (uint32_t)msg->u.status.last_tx_end_us);
        put_u32(&w, (uint32_t)msg->u.status.last_tx_start_us);
        break;
    case LC_MSG_FW_CHUNK:
        if (msg->u.fw_chunk.len > LC_MAX_FW_CHUNK) {
            return 0;
        }
        put_u32(&w, msg->u.fw_chunk.offset);
        put_u16(&w, msg->u.fw_chunk.len);
        put_bytes(&w, msg->u.fw_chunk.data, msg->u.fw_chunk.len);
        break;
    case LC_MSG_FW_COMMIT:
        put_u32(&w, msg->u.fw_commit.image_size);
        break;
    case LC_MSG_ACK:
        put_u8(&w, msg->u.ack.acked_seq);
        put_u8(&w, msg->u.ack.status);
        break;
    case LC_MSG_TIME:
        put_u32(&w, msg->u.time.unix_s);
        break;
    default:
        return 0;
    }
    return w.ok ? w.pos : 0;
}

int lc_msg_decode(const uint8_t *buf, size_t len, lc_msg_t *msg)
{
    if (len > LC_LINK_MAX_MSG) {
        return -1;
    }
    reader_t r = { buf, len, 0, 1 };
    msg->type = get_u8(&r);
    msg->seq = get_u8(&r);

    switch (msg->type) {
    case LC_MSG_CONFIG:
        msg->u.config.role = get_u8(&r);
        msg->u.config.band = get_u8(&r);
        msg->u.config.radio_index = get_u8(&r);
        msg->u.config.cell_seed = get_u32(&r);
        break;
    case LC_MSG_SCHEDULE:
        msg->u.schedule.frame_number = get_u32(&r);
        msg->u.schedule.flags = get_u8(&r);
        msg->u.schedule.slot_count = get_u8(&r);
        if (msg->u.schedule.slot_count > LC_MAX_SLOTS_PER_SCHEDULE) {
            return -1;
        }
        for (uint8_t i = 0; i < msg->u.schedule.slot_count && r.ok; i++) {
            get_slot(&r, &msg->u.schedule.slots[i]);
        }
        break;
    case LC_MSG_RX_REPORT:
        msg->u.rx_report.frame_number = get_u32(&r);
        msg->u.rx_report.slot_index = get_u8(&r);
        msg->u.rx_report.rssi_dbm = (int16_t)get_u16(&r);
        msg->u.rx_report.snr_qdb = (int16_t)get_u16(&r);
        msg->u.rx_report.crc_ok = get_u8(&r);
        msg->u.rx_report.payload_len = get_u8(&r);
        msg->u.rx_report.payload = get_bytes(&r, msg->u.rx_report.payload_len);
        msg->u.rx_report.end_us = (int32_t)get_u32(&r);
        break;
    case LC_MSG_STATUS:
        msg->u.status.uptime_ms = get_u32(&r);
        msg->u.status.pps_locked = get_u8(&r);
        msg->u.status.temp_c = (int8_t)get_u8(&r);
        msg->u.status.schedule_misses = get_u16(&r);
        msg->u.status.uart_crc_errors = get_u16(&r);
        msg->u.status.frame_number = get_u32(&r);
        msg->u.status.last_tx_end_us = (int32_t)get_u32(&r);
        msg->u.status.last_tx_start_us = (int32_t)get_u32(&r);
        break;
    case LC_MSG_FW_CHUNK:
        msg->u.fw_chunk.offset = get_u32(&r);
        msg->u.fw_chunk.len = get_u16(&r);
        if (msg->u.fw_chunk.len > LC_MAX_FW_CHUNK) {
            return -1;
        }
        msg->u.fw_chunk.data = get_bytes(&r, msg->u.fw_chunk.len);
        break;
    case LC_MSG_FW_COMMIT:
        msg->u.fw_commit.image_size = get_u32(&r);
        break;
    case LC_MSG_ACK:
        msg->u.ack.acked_seq = get_u8(&r);
        msg->u.ack.status = get_u8(&r);
        break;
    case LC_MSG_TIME:
        msg->u.time.unix_s = get_u32(&r);
        break;
    default:
        return -1;
    }
    /* Trailing bytes mean sender and receiver disagree on the layout. */
    return (r.ok && r.pos == len) ? 0 : -1;
}
