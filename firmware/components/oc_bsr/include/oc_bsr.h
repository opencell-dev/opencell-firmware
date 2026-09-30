/* oc_bsr — bs-radio message handling, independent of ESP-IDF.
 *
 * The firmware's UART task decodes each host message and passes it to
 * oc_bsr_handle(), which updates config/clock/executor/firmware-update state
 * and fills an ACK. The same object builds outgoing STATUS and RX_REPORT
 * messages. */
#ifndef OC_BSR_H
#define OC_BSR_H

#include <stdint.h>

#include "oc_clock.h"
#include "oc_exec.h"
#include "oc_fwupd.h"
#include "oc_link.h"

typedef struct {
    void *ctx;
    int (*save_config)(void *ctx, const oc_config_t *cfg); /* persist (NVS); 0 ok */
} oc_bsr_ops_t;

typedef struct {
    oc_bsr_ops_t ops;
    oc_clock_t  *clock;
    oc_exec_t   *exec;
    oc_fwupd_t  *fwupd;
    oc_config_t  config;
    int          configured;
    int          reboot_pending; /* set after a successful FW_COMMIT */
    uint8_t      tx_seq;
} oc_bsr_t;

void oc_bsr_init(oc_bsr_t *b, const oc_bsr_ops_t *ops, oc_clock_t *clock, oc_exec_t *exec,
                 oc_fwupd_t *fwupd, const oc_config_t *saved /* NULL if none */);

/* Handle one host message. Returns 1 and fills *ack when a reply is due. */
int oc_bsr_handle(oc_bsr_t *b, const oc_msg_t *in, uint64_t now_us, oc_msg_t *ack);

/* Advance the clock and apply the TX policy: TX only while configured and
 * the clock is LOCKED or in HOLDOVER. */
void oc_bsr_tick(oc_bsr_t *b, uint64_t now_us);

void oc_bsr_make_status(oc_bsr_t *b, uint64_t now_us, uint32_t uptime_ms, int8_t temp_c,
                        uint16_t uart_crc_errors, oc_msg_t *out);

/* payload in *out aliases ev->data. */
void oc_bsr_make_rx_report(oc_bsr_t *b, uint32_t frame_number, uint8_t slot_index,
                           const oc_radio_event_t *ev, oc_msg_t *out);

/* What the OLED shows. oc_bsr_view fills everything except host_ok,
 * uart_errors and rx_drops, which belong to the link tasks. */
#define OC_BSR_SCREEN_LINES 6
#define OC_BSR_SCREEN_COLS  21

typedef struct {
    uint8_t  configured;
    uint8_t  band;
    uint8_t  radio_index;
    uint8_t  clock_state;  /* oc_clock_state_t */
    uint8_t  ppm_valid;
    int32_t  ppm;          /* crystal error vs PPS */
    uint8_t  have_frame;
    uint32_t frame;
    uint8_t  tx_on;
    uint16_t misses;
    uint8_t  host_ok;      /* a valid host message arrived recently */
    uint32_t uart_errors;
    uint32_t rx_drops;     /* RX reports dropped with the report queue full */
} oc_bsr_view_t;

void oc_bsr_view(const oc_bsr_t *b, uint64_t now_us, oc_bsr_view_t *v);
void oc_bsr_status_lines(const oc_bsr_view_t *v, char lines[OC_BSR_SCREEN_LINES][OC_BSR_SCREEN_COLS + 1]);

#endif
