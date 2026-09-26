/* lc_radio_if — the radio operations the slot executor needs. The W12
 * implementation is in components/lc_radio; host tests supply a fake. */
#ifndef LC_RADIO_IF_H
#define LC_RADIO_IF_H

#include <stdint.h>

#include "lc_phy.h"

typedef enum {
    LC_RADIO_EV_NONE       = 0,
    LC_RADIO_EV_TX_DONE    = 1,
    LC_RADIO_EV_RX_DONE    = 2,
    LC_RADIO_EV_RX_TIMEOUT = 3,
    LC_RADIO_EV_ERROR      = 4
} lc_radio_ev_type_t;

typedef struct {
    uint8_t type;   /* lc_radio_ev_type_t */
    uint8_t crc_ok;
    uint8_t len;
    int16_t rssi_dbm;
    int16_t snr_qdb; /* 0.25 dB units */
    uint8_t data[255];
    uint64_t irq_us;          /* local time of the radio's done IRQ; 0 = not captured */
    uint64_t start_us;        /* TX: local time the preamble started (BUSY low after SetTx); 0 = unknown */
    int32_t  frame_offset_us; /* irq_us from the frame start, set by lc_exec; LC_RX_END_UNKNOWN if not */
} lc_radio_event_t;

typedef struct {
    void *ctx;
    /* Tune and set modulation. Band (sub-GHz or 2.4 GHz path) follows freq_hz. */
    int (*configure)(void *ctx, uint32_t freq_hz, const lc_mode_t *mode);
    /* Load a packet and arm TX; nothing is sent until launch(). */
    int (*stage_tx)(void *ctx, const uint8_t *data, uint8_t len);
    /* Arm RX with a timeout; nothing happens until launch(). */
    int (*stage_rx)(void *ctx, uint32_t timeout_us);
    /* Start the staged operation now. */
    int (*launch)(void *ctx);
    /* Returns 1 and fills *ev when the operation finished, 0 while busy. */
    int (*poll)(void *ctx, lc_radio_event_t *ev);
    void (*standby)(void *ctx);
} lc_radio_ops_t;

#endif
