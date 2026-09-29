/* ocbench_core — schedule building and statistics for the bench tool.
 * Kept free of I/O so it can be unit-tested. */
#ifndef OCBENCH_CORE_H
#define OCBENCH_CORE_H

#include <stdint.h>

#include "oc_link.h"
#include "oc_phy.h"

#define OCB_MIN_PAYLOAD 8u

typedef struct {
    uint32_t  freq_hz;
    oc_tier_t tier;
    uint32_t  offset_us;    /* TX slot start within the frame */
    uint32_t  rx_window_us; /* RX slot length, centred on the TX slot */
    uint8_t   payload_len;  /* >= OCB_MIN_PAYLOAD */
    int32_t   rx_offset_hz; /* bench: tune the receiver this far from freq_hz (crystal-offset sweeps) */
    int32_t   rx_shift_us;  /* bench: move the RX window this far from centred (timing-alignment sweeps) */
} ocb_link_cfg_t;

typedef struct {
    uint32_t sent;
    uint32_t received;     /* CRC-good packets with a valid test payload */
    uint32_t crc_fail;
    uint32_t bad_payload;
    int32_t  rssi_sum;
    int16_t  rssi_min;
    int16_t  rssi_max;
    int32_t  snr_sum_qdb;
    /* Packet end from the RX board's frame start, over received packets that carry one. */
    uint32_t timed;
    double   end_sum;
    double   end_sq_sum;
    int32_t  end_min;
    int32_t  end_max;
} ocb_stats_t;

int       ocb_parse_tier(const char *s, oc_tier_t *out);
/* A whole decimal number lo..hi (an optional '-', digits, nothing else).
 * 0 and *out set, or -1 and *out untouched. */
int       ocb_parse_int(const char *s, long lo, long hi, long *out);
oc_band_t ocb_band_of(uint32_t freq_hz);

/* Test payload: "LCB1", frame number (LE), then (i & 0xFF) filler. */
void ocb_fill_payload(uint8_t *buf, uint8_t len, uint32_t frame_number);
int  ocb_check_payload(const uint8_t *buf, uint8_t len, uint32_t *frame_number);

/* One-slot SCHEDULE for the TX board (tx=1) or the RX board (tx=0).
 * payload_buf must hold cfg->payload_len bytes and outlive *out.
 * Returns 0, or -1 if the tier doesn't exist on the band or slots don't fit. */
int ocb_link_schedule(const ocb_link_cfg_t *cfg, uint32_t frame_number, int tx, uint8_t *payload_buf,
                      oc_msg_t *out);

/* Guard-time probe. On the TX board: slot A (tier_a, dir_a, on freq_a_hz, or
 * cfg->freq_hz if 0) at cfg->offset_us lasting airtime(A) + gap_us, then TX
 * slot B (cfg->tier, cfg->freq_hz) immediately after. A freq_a_hz on the
 * other band measures the band-switch time.
 * On the RX board: one RX window around B. If B arrives, the radio finished
 * A and was reconfigured within gap_us. payload_buf must hold cfg->payload_len
 * bytes. Returns 0 or -1. */
int ocb_guard_schedule(const ocb_link_cfg_t *cfg, uint32_t freq_a_hz, oc_tier_t tier_a, uint8_t dir_a,
                       uint32_t gap_us, uint32_t frame_number, int tx, uint8_t *payload_buf, oc_msg_t *out);

/* Back-to-back TX slots filling the frame (power / spurious measurements).
 * payload_buf must hold cfg->payload_len bytes. Returns the slot count, or -1. */
int ocb_cw_schedule(const ocb_link_cfg_t *cfg, uint32_t frame_number, uint8_t *payload_buf, oc_msg_t *out);

/* Cross-band duplex probe: per frame a DL slot (dl_freq, dl_tier) at offset_us
 * and, gap_us after it ends, a UL slot (ul_freq, ul_tier). Base (base=1): TX DL,
 * RX UL; terminal side (base=0): RX DL, TX UL. RX windows equal the peer's TX
 * slot. Payloads carry the frame number. Returns 0 or -1. */
typedef struct {
    uint32_t  dl_freq_hz;
    oc_tier_t dl_tier;
    uint32_t  ul_freq_hz;
    oc_tier_t ul_tier;
    uint32_t  offset_us;
    uint32_t  gap_us;
    uint8_t   payload_len;
} ocb_duplex_cfg_t;

int ocb_duplex_schedule(const ocb_duplex_cfg_t *cfg, uint32_t frame_number, int base, uint8_t *dl_payload,
                        uint8_t *ul_payload, oc_msg_t *out);

void ocb_stats_init(ocb_stats_t *s);
void ocb_stats_add_rx(ocb_stats_t *s, const oc_rx_report_t *r);
void   ocb_stats_add_end(ocb_stats_t *s, int32_t end_us); /* one timing sample (skips OC_RX_END_UNKNOWN) */
double ocb_stats_end_mean(const ocb_stats_t *s); /* µs; 0 if none timed */
double ocb_stats_end_sd(const ocb_stats_t *s);   /* population SD, µs */

#endif
