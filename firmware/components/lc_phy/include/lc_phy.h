/* lc_phy — OpenCell physical-layer constants shared by W12 firmware and the
 * Pi RHU daemon: channel plan, hop sequence, modulation tiers, airtime.
 * Changing any output of this library desynchronizes deployed radios; the
 * golden-vector tests in host-tests/test_hop.c pin the hop sequence. */
#ifndef LC_PHY_H
#define LC_PHY_H

#include <stdint.h>

#define LC_FRAME_US            120000u
#define LC_GUARD_US            1200u /* configure+stage+launch of the next slot (bench 2026-09-25: worst ~1 ms on W12, 16 MHz SPI) */
#define LC_MAX_RADIOS_PER_BAND 4u
#define LC_NUM_SYNC_CHANNELS   8u
#define LC_INVALID_CHANNEL     0xFFu

typedef enum {
    LC_BAND_915 = 0,
    LC_BAND_2G4 = 1,
    LC_BAND_COUNT
} lc_band_t;

/* Channel plan. Returns 0 for an invalid band or channel. */
uint8_t  lc_num_channels(lc_band_t band);
uint32_t lc_channel_freq_hz(lc_band_t band, uint8_t channel);

/* Channel for a voice/RACH slot. Radios with different radio_index on the same
 * band never share a channel within a frame. Returns LC_INVALID_CHANNEL for an
 * invalid band or radio_index >= LC_MAX_RADIOS_PER_BAND. */
uint8_t lc_hop_channel(uint32_t cell_seed, lc_band_t band, uint8_t radio_index,
                       uint32_t frame_number, uint8_t slot_index);

/* Channel for the beacon of frame_number: cycles through LC_NUM_SYNC_CHANNELS
 * fixed channels so unsynced terminals can park on one. */
uint8_t lc_sync_channel(uint32_t cell_seed, lc_band_t band, uint32_t frame_number);

typedef enum {
    LC_MOD_LORA = 0,
    LC_MOD_FLRC = 1
} lc_modulation_t;

typedef enum {
    LC_TIER_NEAR = 0,
    LC_TIER_MID  = 1,
    LC_TIER_EDGE = 2,
    LC_TIER_COUNT
} lc_tier_t;

/* FLRC coding-rate codes for lc_mode_t.cr */
#define LC_FLRC_CR_1_2 0u
#define LC_FLRC_CR_3_4 1u
#define LC_FLRC_CR_1_1 2u

/* LoRa:  sf 5..12, cr 1..4 (= 4/5..4/8), preamble in symbols, bw_hz set, bitrate_bps 0.
 * FLRC:  cr is an LC_FLRC_CR_* code, preamble in bits, bitrate_bps set, sf/bw_hz 0. */
typedef struct {
    uint8_t  modulation;
    uint8_t  sf;
    uint8_t  cr;
    uint16_t preamble;
    uint32_t bw_hz;
    uint32_t bitrate_bps;
} lc_mode_t;

/* Mode for a tier on a band, or NULL if the band doesn't support that tier. */
const lc_mode_t *lc_tier_mode(lc_band_t band, lc_tier_t tier);

/* Time on air in µs (explicit header, CRC on). 0 for an invalid mode. */
uint32_t lc_airtime_us(const lc_mode_t *mode, uint8_t payload_len);

/* Airtime plus LC_GUARD_US. 0 for an invalid mode. */
uint32_t lc_slot_len_us(const lc_mode_t *mode, uint8_t payload_len);

/* How long after (preamble start + lc_airtime_us) the receiving LR2021
 * raises RX_DONE: the transmitter's tail (PA ramp-down etc., ~137 us; FLRC
 * adds ~34 bits the airtime formula doesn't count) plus demodulator
 * latency. Fitted to W12 bench measurements; 0 for NULL. A receiver
 * timing a transmitter from RX_DONE subtracts airtime and this lag. */
uint32_t lc_rx_done_lag_us(const lc_mode_t *mode);

#endif
