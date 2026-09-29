/* oc_radio_if — the radio operations the slot executor needs. The W12
 * implementation is in components/oc_radio; host tests supply a fake. */
#ifndef OC_RADIO_IF_H
#define OC_RADIO_IF_H

#include <stdint.h>

#include "oc_phy.h"

typedef enum {
    OC_RADIO_EV_NONE       = 0,
    OC_RADIO_EV_TX_DONE    = 1,
    OC_RADIO_EV_RX_DONE    = 2,
    OC_RADIO_EV_RX_TIMEOUT = 3,
    OC_RADIO_EV_ERROR      = 4
} oc_radio_ev_type_t;

typedef struct {
    uint8_t type;   /* oc_radio_ev_type_t */
    uint8_t crc_ok;
    uint8_t len;
    int16_t rssi_dbm;
    int16_t snr_qdb; /* 0.25 dB units */
    uint8_t data[255];
    uint64_t irq_us;          /* local time of the radio's done IRQ; 0 = not captured */
    uint64_t start_us;        /* TX: local time the preamble started (BUSY low after SetTx); 0 = unknown */
    int32_t  frame_offset_us; /* irq_us from the frame start, set by oc_exec; OC_RX_END_UNKNOWN if not */
} oc_radio_event_t;

/* How early callers hand the radio a launch: covers its own start-up
 * compensation (SetTx to preamble ~140 us on the LR2021) plus margin. */
#define OC_RADIO_ARM_US 300u

typedef struct {
    void *ctx;
    /* Tune and set modulation. Band (sub-GHz or 2.4 GHz path) follows freq_hz. */
    int (*configure)(void *ctx, uint32_t freq_hz, const oc_mode_t *mode);
    /* Load a packet and arm TX; nothing is sent until launch(). */
    int (*stage_tx)(void *ctx, const uint8_t *data, uint8_t len);
    /* Arm RX with a timeout; nothing happens until launch(). */
    int (*stage_rx)(void *ctx, uint32_t timeout_us);
    /* Start the staged operation so that it begins at local time at_us: TX
     * preamble on air, or RX ready. Call at most OC_RADIO_ARM_US before
     * at_us; returns once started. at_us <= now starts it at once. */
    int (*launch)(void *ctx, uint64_t at_us);
    /* Returns 1 and fills *ev when the operation finished, 0 while busy. */
    int (*poll)(void *ctx, oc_radio_event_t *ev);
    void (*standby)(void *ctx);
    /* Optional (NULL if the radio can't): the instantaneous RSSI in dBm on
     * the frequency being received, read while RX runs and without stopping
     * it. Returns 0 on success. oc_term samples it for the noise floor while
     * searching; oc_exec doesn't use it. */
    int (*rssi_inst)(void *ctx, int16_t *dbm);
} oc_radio_ops_t;

#endif
