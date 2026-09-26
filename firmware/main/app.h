/* Shared state of the bs-radio firmware. All lc_* objects below are guarded
 * by app_lock()/app_unlock(); lc_radio is only touched by the exec task. */
#ifndef APP_H
#define APP_H

#include <stdint.h>

#include "lc_bsr.h"
#include "lc_clock.h"
#include "lc_exec.h"
#include "lc_fwupd.h"
#include "lc_link.h"

/* Free-run limit without PPS. At the ESP32-S3 crystal's ~±10 ppm, 10 s drifts
 * <= 100 µs, half of LC_GUARD_US. Re-derive from the measured ppm (plan Task 12). */
#define APP_HOLDOVER_US    10000000u
#define APP_UART_BAUD      2000000
#define APP_STATUS_PERIOD_US 1000000
#define APP_OTA_VERIFY_US  60000000 /* a new image must hear the host within this, or it restarts (rollback) */
#define APP_HOST_SEEN_US   3000000 /* OLED shows HOST OK if the Pi spoke within this */

extern lc_clock_t g_clock;
extern lc_exec_t  g_exec;
extern lc_fwupd_t g_fwupd;
extern lc_bsr_t   g_bsr;
extern int        g_radio_err; /* lc_radio_init result; the image is only marked valid if 0 */

void app_lock(void);
void app_unlock(void);

/* app_store.c */
int  app_store_init(void);                        /* NVS */
int  app_store_load_config(lc_config_t *out);     /* 0 if a config was saved */
extern const lc_bsr_ops_t   g_bsr_ops;            /* save_config -> NVS */
extern const lc_fwupd_ops_t g_fwupd_ops;          /* esp_ota_* */

/* app_link.c: UART to the host; sends ACK/STATUS/RX_REPORT */
void app_link_start(void);
void app_link_send(const lc_msg_t *msg);          /* thread-safe */
void app_link_on_rx(void *ctx, uint32_t frame, uint8_t slot, const lc_radio_event_t *ev);
void app_link_health(int64_t now_us, uint8_t *host_ok, uint32_t *uart_errors);

/* app_oled.c: status screen, refreshed at 2 Hz from a low-priority core-0 task */
void app_oled_start(void);

/* app_exec.c: PPS capture and the slot executor task */
void app_exec_start(int internal_pps);

#endif
