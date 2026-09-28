#include "lc_air.h"

#include <string.h>

/* Bounded little-endian writer/reader (same pattern as lc_link/lc_msg.c):
 * once an operation fails, ok stays 0 and later operations are no-ops. */
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

static int window_valid(uint16_t offset, uint16_t len)
{
    return (uint32_t)offset + len <= LC_AIR_FRAME_UNITS;
}

static int leg_valid(const lc_grant_leg_t *leg)
{
    if (leg->len == 0) {
        return 1; /* unused direction; other fields ignored */
    }
    return lc_tier_mode((lc_band_t)leg->band, (lc_tier_t)leg->tier) != NULL &&
           leg->radio_index < LC_MAX_RADIOS_PER_BAND && window_valid(leg->offset, leg->len);
}

static int beacon_valid(const lc_beacon_t *b)
{
    return b->band < LC_BAND_COUNT && b->page_count <= LC_BCN_MAX_PAGES &&
           window_valid(b->rach_offset, b->rach_len) && b->anchor <= LC_BCN_MAX_ANCHOR &&
           b->cfg_ver <= LC_BCN_MAX_CFG_VER;
}

static void put_leg(writer_t *w, const lc_grant_leg_t *leg)
{
    put_u8(w, leg->band);
    put_u8(w, leg->tier);
    put_u8(w, leg->radio_index);
    put_u8(w, leg->slot_index);
    put_u16(w, leg->offset);
    put_u16(w, leg->len);
}

static void get_leg(reader_t *r, lc_grant_leg_t *leg)
{
    leg->band = get_u8(r);
    leg->tier = get_u8(r);
    leg->radio_index = get_u8(r);
    leg->slot_index = get_u8(r);
    leg->offset = get_u16(r);
    leg->len = get_u16(r);
}

size_t lc_air_encode(const lc_air_msg_t *msg, uint8_t *out, size_t cap)
{
    writer_t w = { out, cap < LC_AIR_MAX_FRAME ? cap : LC_AIR_MAX_FRAME, 0, 1 };
    put_u8(&w, (uint8_t)((LC_AIR_VERSION << 4) | (msg->type & 0x0Fu)));

    switch (msg->type) {
    case LC_AIR_BEACON: {
        const lc_beacon_t *b = &msg->u.beacon;
        if (!beacon_valid(b)) {
            return 0;
        }
        put_u32(&w, b->cell_seed);
        put_u32(&w, b->frame_number);
        put_u8(&w, b->band);
        put_u8(&w, b->flags);
        put_u16(&w, b->rach_offset);
        put_u16(&w, b->rach_len);
        put_u8(&w, b->rach_slot_index);
        put_u8(&w, b->page_count);
        put_u8(&w, (uint8_t)(b->anchor | (b->cfg_ver << 6)));
        for (uint8_t i = 0; i < b->page_count; i++) {
            put_u32(&w, b->page_tmid[i]);
        }
        break;
    }
    case LC_AIR_GRANT:
        if (!leg_valid(&msg->u.grant.dl) || !leg_valid(&msg->u.grant.ul)) {
            return 0;
        }
        put_u32(&w, msg->u.grant.tmid);
        put_u32(&w, msg->u.grant.effective_frame);
        put_leg(&w, &msg->u.grant.dl);
        put_leg(&w, &msg->u.grant.ul);
        break;
    case LC_AIR_RACH:
        if (msg->u.rach.payload_len > LC_RACH_MAX_PAYLOAD) {
            return 0;
        }
        put_u32(&w, msg->u.rach.tmid);
        put_u8(&w, msg->u.rach.kind);
        put_u8(&w, msg->u.rach.payload_len);
        put_bytes(&w, msg->u.rach.payload, msg->u.rach.payload_len);
        break;
    case LC_AIR_DATA:
        if (msg->u.data.payload_len > LC_DATA_MAX_PAYLOAD) {
            return 0;
        }
        put_u32(&w, msg->u.data.tmid);
        put_u8(&w, msg->u.data.seq);
        put_u8(&w, msg->u.data.flags);
        put_u8(&w, msg->u.data.payload_len);
        put_bytes(&w, msg->u.data.payload, msg->u.data.payload_len);
        break;
    default:
        return 0;
    }
    return w.ok ? w.pos : 0;
}

int lc_air_decode(const uint8_t *buf, size_t len, lc_air_msg_t *msg)
{
    if (len == 0 || len > LC_AIR_MAX_FRAME || (buf[0] >> 4) != LC_AIR_VERSION) {
        return -1;
    }
    reader_t r = { buf, len, 1, 1 };
    msg->type = buf[0] & 0x0Fu;

    switch (msg->type) {
    case LC_AIR_BEACON: {
        lc_beacon_t *b = &msg->u.beacon;
        b->cell_seed = get_u32(&r);
        b->frame_number = get_u32(&r);
        b->band = get_u8(&r);
        b->flags = get_u8(&r);
        b->rach_offset = get_u16(&r);
        b->rach_len = get_u16(&r);
        b->rach_slot_index = get_u8(&r);
        b->page_count = get_u8(&r);
        uint8_t sync = get_u8(&r);
        b->anchor = sync & 0x3Fu;
        b->cfg_ver = sync >> 6;
        if (!r.ok || !beacon_valid(b)) {
            return -1;
        }
        for (uint8_t i = 0; i < b->page_count; i++) {
            b->page_tmid[i] = get_u32(&r);
        }
        break;
    }
    case LC_AIR_GRANT:
        msg->u.grant.tmid = get_u32(&r);
        msg->u.grant.effective_frame = get_u32(&r);
        get_leg(&r, &msg->u.grant.dl);
        get_leg(&r, &msg->u.grant.ul);
        if (r.ok && (!leg_valid(&msg->u.grant.dl) || !leg_valid(&msg->u.grant.ul))) {
            return -1;
        }
        break;
    case LC_AIR_RACH:
        msg->u.rach.tmid = get_u32(&r);
        msg->u.rach.kind = get_u8(&r);
        msg->u.rach.payload_len = get_u8(&r);
        if (msg->u.rach.payload_len > LC_RACH_MAX_PAYLOAD) {
            return -1;
        }
        msg->u.rach.payload = get_bytes(&r, msg->u.rach.payload_len);
        break;
    case LC_AIR_DATA:
        msg->u.data.tmid = get_u32(&r);
        msg->u.data.seq = get_u8(&r);
        msg->u.data.flags = get_u8(&r);
        msg->u.data.payload_len = get_u8(&r);
        if (msg->u.data.payload_len > LC_DATA_MAX_PAYLOAD) {
            return -1;
        }
        msg->u.data.payload = get_bytes(&r, msg->u.data.payload_len);
        break;
    default:
        return -1;
    }
    return (r.ok && r.pos == len) ? 0 : -1;
}

uint8_t lc_grant_leg_channel(uint32_t cell_seed, const lc_grant_leg_t *leg, uint32_t frame_number)
{
    if (leg->len == 0) {
        return LC_INVALID_CHANNEL;
    }
    return lc_hop_channel(cell_seed, (lc_band_t)leg->band, leg->radio_index, frame_number,
                          leg->slot_index);
}
