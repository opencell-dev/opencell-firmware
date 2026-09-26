/* Terminal role: lc_term driven from one task, BLE GATT bridge, OLED. */
#ifndef TERM_H
#define TERM_H

#include <stdint.h>

#include "lc_term.h"

extern lc_term_t g_term;

void term_lock(void);
void term_unlock(void);

void term_app_main(void);                          /* never returns */

/* term_ble.c */
void term_ble_start(uint32_t tmid);
void term_ble_downlink(const uint8_t *data, uint8_t len); /* queue a DOWN notification */
void term_ble_status_changed(void);

/* term_oled.c */
void term_oled_start(void);                        /* refreshes the status screen at 2 Hz */

#endif
