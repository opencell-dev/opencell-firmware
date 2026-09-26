/* lc_bsr — bs-radio message handling, independent of ESP-IDF.
 *
 * The firmware's UART task decodes each host message and passes it to
 * lc_bsr_handle(), which updates config/clock/executor/firmware-update state
 * and fills an ACK. The same object builds outgoing STATUS and RX_REPORT
 * messages. */
#ifndef LC_BSR_H
#define LC_BSR_H

#include <stdint.h>

#include "lc_clock.h"
#include "lc_exec.h"
#include "lc_fwupd.h"
#include "lc_link.h"

typedef struct {
    void *ctx;
    int (*save_config)(void *ctx, const lc_config_t *cfg); /* persist (NVS); 0 ok */
} lc_bsr_ops_t;

typedef struct {
    lc_bsr_ops_t ops;
    lc_clock_t  *clock;
    lc_exec_t   *exec;
    lc_fwupd_t  *fwupd;
    lc_config_t  config;
    int          configured;
    int          reboot_pending; /* set after a successful FW_COMMIT */
    uint8_t      tx_seq;
} lc_bsr_t;

void lc_bsr_init(lc_bsr_t *b, const lc_bsr_ops_t *ops, lc_clock_t *clock, lc_exec_t *exec,
                 lc_fwupd_t *fwupd, const lc_config_t *saved /* NULL if none */);

/* Handle one host message. Returns 1 and fills *ack when a reply is due. */
int lc_bsr_handle(lc_bsr_t *b, const lc_msg_t *in, uint64_t now_us, lc_msg_t *ack);

/* Advance the clock and apply the TX policy: TX only while configured and
 * the clock is LOCKED or in HOLDOVER. */
void lc_bsr_tick(lc_bsr_t *b, uint64_t now_us);

void lc_bsr_make_status(lc_bsr_t *b, uint64_t now_us, uint32_t uptime_ms, int8_t temp_c,
                        uint16_t uart_crc_errors, lc_msg_t *out);

/* payload in *out aliases ev->data. */
void lc_bsr_make_rx_report(lc_bsr_t *b, uint32_t frame_number, uint8_t slot_index,
                           const lc_radio_event_t *ev, lc_msg_t *out);

/* What the OLED shows. lc_bsr_view fills everything except host_ok and
 * uart_errors, which belong to the UART link task. */
#define LC_BSR_SCREEN_LINES 6
#define LC_BSR_SCREEN_COLS  21

typedef struct {
    uint8_t  configured;
    uint8_t  band;
    uint8_t  radio_index;
    uint8_t  clock_state;  /* lc_clock_state_t */
    uint8_t  ppm_valid;
    int32_t  ppm;          /* crystal error vs PPS */
    uint8_t  have_frame;
    uint32_t frame;
    uint8_t  tx_on;
    uint16_t misses;
    uint8_t  host_ok;      /* a valid host message arrived recently */
    uint32_t uart_errors;
} lc_bsr_view_t;

void lc_bsr_view(const lc_bsr_t *b, uint64_t now_us, lc_bsr_view_t *v);
void lc_bsr_status_lines(const lc_bsr_view_t *v, char lines[LC_BSR_SCREEN_LINES][LC_BSR_SCREEN_COLS + 1]);

#endif
