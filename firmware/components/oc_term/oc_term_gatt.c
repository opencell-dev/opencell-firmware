#include "oc_term_gatt.h"

#include <string.h>

#include "oc_sig.h"

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

void oc_term_pack_status(const oc_term_status_t *st, uint8_t out[OC_GATT_STATUS_LEN])
{
    memset(out, 0, OC_GATT_STATUS_LEN);
    out[0] = st->state;
    out[1] = st->band;
    out[2] = st->tier;
    put16(out + 4, (uint16_t)st->rssi_dbm);
    put16(out + 6, (uint16_t)st->snr_qdb);
    put32(out + 8, st->tmid);
    put32(out + 12, st->frame);
    put32(out + 16, st->cell_seed);
    out[20] = st->scan_pos;
    out[21] = st->scan_len;
    out[22] = st->scan_src;
    put32(out + 23, st->freq_khz);
}

size_t oc_term_pack_scan(const oc_term_scan_t *s, uint8_t out[OC_GATT_SCAN_MAX])
{
    oc_scan_ent_t l[OC_SCAN_MAX];
    uint8_t n = oc_term_scan_list(s, l);
    out[0] = OC_GATT_SCAN_FMT;
    out[1] = s->mode;
    out[2] = s->fallback_after;
    out[3] = s->fallback_chunk;
    out[4] = s->net_ver;
    out[5] = n;
    for (uint8_t i = 0; i < n; i++) {
        put32(out + 6 + 5 * i, l[i].freq_hz);
        out[6 + 5 * i + 4] = l[i].flags;
    }
    return 6u + 5u * n;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int oc_term_gatt_scan_command(oc_term_scan_t *s, const uint8_t *cmd, size_t len)
{
    if (len < 2 || cmd[0] != OC_SIG_CMD_SCAN) return OC_SIG_ATT_BAD_LEN;
    switch (cmd[1]) {
    case OC_GATT_SCAN_SET_USER: {
        if (len < 3) return OC_SIG_ATT_BAD_LEN;
        uint8_t count = cmd[2];
        if (count > OC_SCAN_MAX_USER) return OC_SIG_ATT_BAD_ARG;
        if (len != 3u + 5u * count) return OC_SIG_ATT_BAD_LEN;
        oc_scan_ent_t e[OC_SCAN_MAX_USER];
        for (uint8_t i = 0; i < count; i++) {
            e[i].freq_hz = get32(cmd + 3 + 5 * i);
            e[i].flags = cmd[3 + 5 * i + 4];
        }
        return oc_term_scan_set_user(s, count, e) == 0 ? 0 : OC_SIG_ATT_BAD_ARG;
    }
    case OC_GATT_SCAN_SET_FALLBACK:
        if (len != 4) return OC_SIG_ATT_BAD_LEN;
        return oc_term_scan_set_fallback(s, cmd[2], cmd[3]) == 0 ? 0 : OC_SIG_ATT_BAD_ARG;
    case OC_GATT_SCAN_FORGET_LEARNED:
        if (len != 2) return OC_SIG_ATT_BAD_LEN;
        oc_term_scan_forget_learned(s);
        return 0;
    default:
        return OC_SIG_ATT_BAD_ARG;
    }
}
