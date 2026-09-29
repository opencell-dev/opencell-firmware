#include "oc_phy.h"

#include <stddef.h>

/* Occupied bandwidth must fit the band's channel spacing: 915 channels are
 * 500 kHz, so FLRC there runs at 260 kb/s; 2.4 GHz channels are 2 MHz. */
static const oc_mode_t modes[OC_BAND_COUNT][OC_TIER_COUNT] = {
    [OC_BAND_915] = {
        [OC_TIER_NEAR] = { OC_MOD_FLRC, 0, OC_FLRC_CR_3_4, 16, 0, 260000 },
        [OC_TIER_MID]  = { OC_MOD_LORA, 5, 1, 8, 500000, 0 },
        [OC_TIER_EDGE] = { OC_MOD_LORA, 7, 1, 8, 500000, 0 },
    },
    [OC_BAND_2G4] = {
        [OC_TIER_NEAR] = { OC_MOD_FLRC, 0, OC_FLRC_CR_3_4, 16, 0, 1300000 },
        /* LoRa, not FLRC 520: ~11.5 dB more link budget (datasheet -116.5 vs
         * -105 dBm), the only real range step at 2.4 GHz (no edge tier). FLRC
         * 520 works too since TX starts from FS; decided 2026-09-26 for range. */
        [OC_TIER_MID]  = { OC_MOD_LORA, 7, 1, 8, 812500, 0 },
        /* OC_TIER_EDGE: unsupported, zeroed (bitrate 0 and bw 0) */
    },
};

const oc_mode_t *oc_tier_mode(oc_band_t band, oc_tier_t tier)
{
    if ((unsigned)band >= OC_BAND_COUNT || (unsigned)tier >= OC_TIER_COUNT) {
        return NULL;
    }
    const oc_mode_t *m = &modes[band][tier];
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
static uint32_t lora_airtime_us(const oc_mode_t *m, uint8_t payload_len)
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
static uint32_t flrc_airtime_us(const oc_mode_t *m, uint8_t payload_len)
{
    static const uint8_t cr_num[] = { 1, 3, 1 };
    static const uint8_t cr_den[] = { 2, 4, 1 };
    if (m->cr > OC_FLRC_CR_1_1 || m->bitrate_bps == 0) {
        return 0;
    }
    uint32_t coded_raw = 16u + 8u * payload_len + 16u;
    uint32_t coded = ceil_div_u32(coded_raw * cr_den[m->cr], cr_num[m->cr]);
    uint64_t bits = (uint64_t)m->preamble + 32u + coded;
    return (uint32_t)((bits * 1000000u + m->bitrate_bps - 1) / m->bitrate_bps);
}

uint32_t oc_rx_done_lag_us(const oc_mode_t *mode)
{
    if (mode == NULL) {
        return 0;
    }
    /* The tier modes: measured over 10 000-frame runs (2026-09-26). */
    static const uint32_t measured[OC_BAND_COUNT][OC_TIER_COUNT] = {
        [OC_BAND_915] = { [OC_TIER_NEAR] = 259, [OC_TIER_MID] = 140, [OC_TIER_EDGE] = 225 },
        [OC_BAND_2G4] = { [OC_TIER_NEAR] = 162, [OC_TIER_MID] = 166 },
    };
    for (unsigned b = 0; b < OC_BAND_COUNT; b++) {
        for (unsigned t = 0; t < OC_TIER_COUNT; t++) {
            const oc_mode_t *m = &modes[b][t];
            if (measured[b][t] != 0 && m->modulation == mode->modulation && m->sf == mode->sf &&
                m->cr == mode->cr && m->bw_hz == mode->bw_hz && m->bitrate_bps == mode->bitrate_bps) {
                return measured[b][t];
            }
        }
    }
    /* Other modes: fitted to those points (within ~12 us). */
    if (mode->modulation == OC_MOD_FLRC) {
        /* 260 kb/s: 264 us, 1.3 Mb/s: 158 us */
        return mode->bitrate_bps ? 131u + (uint32_t)(34400000ull / mode->bitrate_bps) : 0u;
    }
    if (mode->modulation == OC_MOD_LORA && mode->bw_hz != 0 && mode->sf <= 12) {
        /* demod latency grows with the symbol time: SF5/500k 4 us, SF7/812.5k 33 us, SF7/500k 85 us */
        uint32_t tsym = (uint32_t)(((uint64_t)1000000u << mode->sf) / mode->bw_hz);
        return 137u + (tsym > 54u ? (tsym - 54u) * 42u / 100u : 0u);
    }
    return 0;
}

uint32_t oc_airtime_us(const oc_mode_t *mode, uint8_t payload_len)
{
    if (mode == NULL) {
        return 0;
    }
    switch (mode->modulation) {
    case OC_MOD_LORA: return lora_airtime_us(mode, payload_len);
    case OC_MOD_FLRC: return flrc_airtime_us(mode, payload_len);
    default:          return 0;
    }
}

uint32_t oc_slot_len_us(const oc_mode_t *mode, uint8_t payload_len)
{
    uint32_t t = oc_airtime_us(mode, payload_len);
    return t == 0 ? 0 : t + OC_GUARD_US;
}
