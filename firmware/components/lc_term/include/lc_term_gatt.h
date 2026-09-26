/* lc_term_gatt — the terminal's BLE GATT contract with the phone app.
 *
 * One primary service, three characteristics (128-bit UUIDs):
 *   service  6c630001-7e2a-4b8e-9f2d-3c1a5e7b0d10
 *   UP       6c630002-...  write / write-without-response
 *            one opaque upper-layer payload (<= LC_TERM_DATA_MAX_PAYLOAD
 *            bytes) per write; queued for the UL slot, or sent as RACH UPPER
 *            (<= LC_TERM_RACH_MAX_PAYLOAD) when attached without a grant.
 *            A write the terminal can't take right now fails with ATT error
 *            0x80 (application: "not now"); the app retries.
 *   DOWN     6c630003-...  notify: one opaque payload per notification
 *   STATUS   6c630004-...  read / notify: LC_GATT_STATUS_LEN bytes, below
 *
 * STATUS layout (little-endian):
 *   0  u8  state (lc_term_state_t)   1 u8 band (lc_band_t)   2 u8 tier (lc_tier_t)
 *   3  u8  reserved (0)
 *   4  i16 rssi_dbm                  6 i16 snr_qdb (0.25 dB; 0 on FLRC links)
 *   8  u32 tmid                     12 u32 frame               16 u32 cell_seed
 * The app owns everything above: call control, MILENAGE, audio/codec. */
#ifndef LC_TERM_GATT_H
#define LC_TERM_GATT_H

#include <stdint.h>

#include "lc_term.h"

#define LC_GATT_STATUS_LEN 20u

/* UUID bytes in NimBLE order (least significant byte first):
 * BLE_UUID128_INIT(LC_GATT_UUID_BYTES(0x01)) is the service. */
#define LC_GATT_UUID_BYTES(id) \
    0x10, 0x0d, 0x7b, 0x5e, 0x1a, 0x3c, 0x2d, 0x9f, 0x8e, 0x4b, 0x2a, 0x7e, (id), 0x00, 0x63, 0x6c
#define LC_GATT_ID_SERVICE 0x01
#define LC_GATT_ID_UP      0x02
#define LC_GATT_ID_DOWN    0x03
#define LC_GATT_ID_STATUS  0x04
#define LC_GATT_ERR_NOT_NOW 0x80

void lc_term_pack_status(const lc_term_status_t *st, uint8_t out[LC_GATT_STATUS_LEN]);

#endif
