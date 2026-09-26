#include "lc_term_gatt.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

void lc_term_pack_status(const lc_term_status_t *st, uint8_t out[LC_GATT_STATUS_LEN])
{
    memset(out, 0, LC_GATT_STATUS_LEN);
    out[0] = st->state;
    out[1] = st->band;
    out[2] = st->tier;
    put16(out + 4, (uint16_t)st->rssi_dbm);
    put16(out + 6, (uint16_t)st->snr_qdb);
    put32(out + 8, st->tmid);
    put32(out + 12, st->frame);
    put32(out + 16, st->cell_seed);
}
