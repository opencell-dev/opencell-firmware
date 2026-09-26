#include "lc_phy.h"

#include <stddef.h>

/* Occupied bandwidth must fit the band's channel spacing: 915 channels are
 * 500 kHz, so FLRC there runs at 260 kb/s; 2.4 GHz channels are 2 MHz. */
static const lc_mode_t modes[LC_BAND_COUNT][LC_TIER_COUNT] = {
    [LC_BAND_915] = {
        [LC_TIER_NEAR] = { LC_MOD_FLRC, 0, LC_FLRC_CR_3_4, 16, 0, 260000 },
        [LC_TIER_MID]  = { LC_MOD_LORA, 5, 1, 8, 500000, 0 },
        [LC_TIER_EDGE] = { LC_MOD_LORA, 7, 1, 8, 500000, 0 },
    },
    [LC_BAND_2G4] = {
        [LC_TIER_NEAR] = { LC_MOD_FLRC, 0, LC_FLRC_CR_3_4, 16, 0, 1300000 },
        /* LoRa, chosen while 2.4 GHz FLRC below 1.3 Mb/s failed (2026-09-25).
         * That failure was TX started from standby; lc_radio now starts TX from
         * FS and FLRC 520 kb/s passes (2026-09-26). Back to FLRC 520: open. */
        [LC_TIER_MID]  = { LC_MOD_LORA, 7, 1, 8, 812500, 0 },
        /* LC_TIER_EDGE: unsupported, zeroed (bitrate 0 and bw 0) */
    },
};

const lc_mode_t *lc_tier_mode(lc_band_t band, lc_tier_t tier)
{
    if ((unsigned)band >= LC_BAND_COUNT || (unsigned)tier >= LC_TIER_COUNT) {
        return NULL;
    }
    const lc_mode_t *m = &modes[band][tier];
    if (m->bw_hz == 0 && m->bitrate_bps == 0) {
        return NULL;
    }
    return m;
}

static uint32_t ceil_div_u32(uint32_t num, uint32_t den)
{
    return (num + den - 1) / den;
}

/* Semtech SX126x/LR20xx LoRa time-on-air, explicit header, CRC on.
 * Computed in quarter-symbols to stay in integers. */
static uint32_t lora_airtime_us(const lc_mode_t *m, uint8_t payload_len)
{
    if (m->sf < 5 || m->sf > 12 || m->cr < 1 || m->cr > 4 || m->bw_hz == 0) {
        return 0;
    }
    const int32_t crc_bits = 16;
    const int32_t header_bits = 20;
    int32_t sf = m->sf;
    int32_t num;
    uint32_t den;
    uint32_t fixed_q; /* preamble + sync overhead, in quarter symbols */

    if (sf <= 6) {
        num = 8 * payload_len + crc_bits - 4 * sf + header_bits;
        den = 4u * (uint32_t)sf;
        fixed_q = 4u * m->preamble + 25u; /* +6.25 */
    } else {
        int32_t ldro = (sf >= 11 && m->bw_hz <= 125000) ? 1 : 0;
        num = 8 * payload_len + crc_bits - 4 * sf + 8 + header_bits;
        den = 4u * (uint32_t)(sf - 2 * ldro);
        fixed_q = 4u * m->preamble + 17u; /* +4.25 */
    }
    uint32_t payload_syms = 8u + (num > 0 ? ceil_div_u32((uint32_t)num, den) : 0u) * (m->cr + 4u);
    uint64_t quarter_syms = fixed_q + 4u * payload_syms;
    uint64_t numer = quarter_syms * ((uint64_t)1 << sf) * 1000000u;
    uint64_t denom = 4u * (uint64_t)m->bw_hz;
    return (uint32_t)((numer + denom - 1) / denom);
}

/* FLRC: preamble + 32-bit sync word, then 16-bit header + payload + 16-bit CRC
 * expanded by the coding rate. Framing constants are calibrated on the bench
 * in plan 2. */
static uint32_t flrc_airtime_us(const lc_mode_t *m, uint8_t payload_len)
{
    static const uint8_t cr_num[] = { 1, 3, 1 };
    static const uint8_t cr_den[] = { 2, 4, 1 };
    if (m->cr > LC_FLRC_CR_1_1 || m->bitrate_bps == 0) {
        return 0;
    }
    uint32_t coded_raw = 16u + 8u * payload_len + 16u;
    uint32_t coded = ceil_div_u32(coded_raw * cr_den[m->cr], cr_num[m->cr]);
    uint64_t bits = (uint64_t)m->preamble + 32u + coded;
    return (uint32_t)((bits * 1000000u + m->bitrate_bps - 1) / m->bitrate_bps);
}

uint32_t lc_rx_done_lag_us(const lc_mode_t *mode)
{
    if (mode == NULL) {
        return 0;
    }
    if (mode->modulation == LC_MOD_FLRC) {
        /* 260 kb/s: 264 us, 1.3 Mb/s: 158 us */
        return mode->bitrate_bps ? 131u + (uint32_t)(34400000ull / mode->bitrate_bps) : 0u;
    }
    if (mode->modulation == LC_MOD_LORA && mode->bw_hz != 0 && mode->sf <= 12) {
        /* demod latency grows with the symbol time: SF5/500k 4 us, SF7/812.5k 33 us, SF7/500k 85 us */
        uint32_t tsym = (uint32_t)(((uint64_t)1000000u << mode->sf) / mode->bw_hz);
        return 137u + (tsym > 54u ? (tsym - 54u) * 42u / 100u : 0u);
    }
    return 0;
}

uint32_t lc_airtime_us(const lc_mode_t *mode, uint8_t payload_len)
{
    if (mode == NULL) {
        return 0;
    }
    switch (mode->modulation) {
    case LC_MOD_LORA: return lora_airtime_us(mode, payload_len);
    case LC_MOD_FLRC: return flrc_airtime_us(mode, payload_len);
    default:          return 0;
    }
}

uint32_t lc_slot_len_us(const lc_mode_t *mode, uint8_t payload_len)
{
    uint32_t t = lc_airtime_us(mode, payload_len);
    return t == 0 ? 0 : t + LC_GUARD_US;
}
