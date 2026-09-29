/* oc_phy — OpenCell physical-layer constants shared by W12 firmware and the
 * Pi RHU daemon: channel plan, hop sequence, modulation tiers, airtime.
 * Changing any output of this library desynchronizes deployed radios; the
 * golden-vector tests in host-tests/test_hop.c pin the hop sequence. */
#ifndef OC_PHY_H
#define OC_PHY_H

#include <stdint.h>

#define OC_FRAME_US            120000u
#define OC_GUARD_US            1200u /* configure+stage+launch of the next slot (bench 2026-09-25: worst ~1 ms on W12, 16 MHz SPI) */
#define OC_MAX_RADIOS_PER_BAND 4u
#define OC_NUM_SYNC_CHANNELS   8u
#define OC_INVALID_CHANNEL     0xFFu

typedef enum {
    OC_BAND_915 = 0,
    OC_BAND_2G4 = 1,
    OC_BAND_COUNT
} oc_band_t;

/* Channel plan. Returns 0 for an invalid band or channel. */
uint8_t  oc_num_channels(oc_band_t band);
uint32_t oc_channel_freq_hz(oc_band_t band, uint8_t channel);

/* Channel for a voice/RACH slot. Radios with different radio_index on the same
 * band never share a channel within a frame. Returns OC_INVALID_CHANNEL for an
 * invalid band or radio_index >= OC_MAX_RADIOS_PER_BAND. */
uint8_t oc_hop_channel(uint32_t cell_seed, oc_band_t band, uint8_t radio_index,
                       uint32_t frame_number, uint8_t slot_index);

/* Channel for the beacon of frame_number: cycles through OC_NUM_SYNC_CHANNELS
 * fixed channels so unsynced terminals can park on one. The seed-derived
 * anchor: oc_sync_channel_at(cell_seed % (n / 8), band, frame_number). */
uint8_t oc_sync_channel(uint32_t cell_seed, oc_band_t band, uint32_t frame_number);

/* Sync channels from an operator-set anchor (channel-list spec §3.1): the
 * beacon of frame f is on (anchor + (f % 8) * n / 8) mod n, so the anchor is
 * the channel of frames with f % 8 == 0, where unsynced terminals park. Two
 * cells with different anchors never share a sync channel in the same frame.
 * OC_INVALID_CHANNEL for an invalid band or anchor >= oc_num_channels(band). */
uint8_t oc_sync_channel_at(uint8_t anchor, oc_band_t band, uint32_t frame_number);

/* Channel of freq_hz if it is exactly on band's grid, else OC_INVALID_CHANNEL. */
uint8_t oc_channel_of_freq(oc_band_t band, uint32_t freq_hz);

/* Sync patterns (channel-list spec §3.2). */
#define OC_SYNC_CYCLE 0u /* 8 channels, the anchor in frames f % 8 == 0 */
#define OC_SYNC_FIXED 1u /* the anchor in every frame: Part 97 only */

/* Operating modes: the values of oc_sig_mode_t (oc_phy does not depend on oc_sig). */
#define OC_PHY_MODE_PART15 1u
#define OC_PHY_MODE_PART97 2u

/* 1 if a cell in `mode` may put its anchor on freq_hz with sync `pattern`,
 * else 0 (spec §3.3; table-driven so later bands and regions are new rows).
 * Today: both modes allow the 915 grid (902.25-927.75 MHz, 500 kHz steps);
 * Part 15 allows CYCLE only, Part 97 CYCLE and FIXED. */
int oc_sync_anchor_ok(uint8_t mode, uint32_t freq_hz, uint8_t pattern);

typedef enum {
    OC_MOD_LORA = 0,
    OC_MOD_FLRC = 1
} oc_modulation_t;

typedef enum {
    OC_TIER_NEAR = 0,
    OC_TIER_MID  = 1,
    OC_TIER_EDGE = 2,
    OC_TIER_COUNT
} oc_tier_t;

/* FLRC coding-rate codes for oc_mode_t.cr */
#define OC_FLRC_CR_1_2 0u
#define OC_FLRC_CR_3_4 1u
#define OC_FLRC_CR_1_1 2u

/* LoRa:  sf 5..12, cr 1..4 (= 4/5..4/8), preamble in symbols, bw_hz set, bitrate_bps 0.
 * FLRC:  cr is an OC_FLRC_CR_* code, preamble in bits, bitrate_bps set, sf/bw_hz 0. */
typedef struct {
    uint8_t  modulation;
    uint8_t  sf;
    uint8_t  cr;
    uint16_t preamble;
    uint32_t bw_hz;
    uint32_t bitrate_bps;
} oc_mode_t;

/* Mode for a tier on a band, or NULL if the band doesn't support that tier. */
const oc_mode_t *oc_tier_mode(oc_band_t band, oc_tier_t tier);

/* Time on air in µs (explicit header, CRC on). 0 for an invalid mode. */
uint32_t oc_airtime_us(const oc_mode_t *mode, uint8_t payload_len);

/* Airtime plus OC_GUARD_US. 0 for an invalid mode. */
uint32_t oc_slot_len_us(const oc_mode_t *mode, uint8_t payload_len);

/* How long after (preamble start + oc_airtime_us) the receiving LR2021
 * raises RX_DONE: the transmitter's tail (PA ramp-down etc., ~137 us; FLRC
 * adds ~34 bits the airtime formula doesn't count) plus demodulator
 * latency. Measured on the W12 for the tier modes, fitted for others;
 * 0 for NULL. A receiver
 * timing a transmitter from RX_DONE subtracts airtime and this lag. */
uint32_t oc_rx_done_lag_us(const oc_mode_t *mode);

#endif
