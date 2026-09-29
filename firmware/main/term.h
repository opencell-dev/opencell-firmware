/* Terminal role: oc_term driven from one task, signalling (oc_term_sig),
 * BLE GATT bridge, OLED. */
#ifndef TERM_H
#define TERM_H

#include <stdint.h>

#include "oc_term.h"
#include "oc_term_screen.h"
#include "oc_term_sig.h"

extern oc_term_t g_term;
extern oc_term_sig_t g_sig;
extern int g_sig_ok; /* 0: the crypto self-test failed; signalling and app data are refused */

void term_lock(void);
void term_unlock(void);

void term_app_main(void);                          /* never returns */

/* Notifies STATUS if the signalling state has changed since the last check.
 * Call with term_lock held, after anything that may run oc_sig_term code
 * (oc_term_sig_step, a downlink delivered by oc_term_step, or a BLE command).
 * on_sig_event() already notifies on the state changes that also emit an
 * EVENT; this catches the ones that don't (DIAL, ANSWER, HANGUP/REJECT,
 * ACTIVATE, periodic re-registration, the ring timeout, ...). */
void term_sig_state_check(void);

/* term_ble.c */
void term_ble_start(uint32_t tmid);
void term_ble_downlink(const uint8_t *data, uint8_t len); /* queue a DOWN notification */
void term_ble_event(const uint8_t *ev, uint8_t len);      /* queue an EVENT notification */
void term_ble_status_changed(void);
/* The Pairing screen's fields: code, lock-out, bonds, phone (any task). */
void term_ble_pair_view(oc_term_pair_view_t *out, uint64_t now_us);
/* Deletes every bond (on the NimBLE host task) and drops a connected phone. */
void term_ble_clear_bonds(void);

/* term_oled.c */
void term_oled_start(void);                        /* refreshes the status screen at 2 Hz */
/* A phone started / finished (either way) a passkey pairing. Called from the
 * NimBLE host task; the OLED jumps to the Pairing screen and back. */
void term_oled_pairing_started(void);
void term_oled_pairing_ended(void);

/* term_ident.c */
int  term_ident_load(oc_sig_ident_t *id);          /* NVS, or a new key pair on first boot; 0 ok, -1 failed */
void term_ident_save(const oc_sig_ident_t *id);    /* written later by a core-0 task */

/* term_scan.c: the scan list in NVS (channel-list spec §5.4) */
void term_scan_load(oc_term_scan_t *s);            /* at boot, before oc_term_sig_init; defaults if none or corrupt */
void term_scan_save(oc_term_scan_t *s);            /* term_lock held; clears dirty, written later by a core-0 task */

#endif
