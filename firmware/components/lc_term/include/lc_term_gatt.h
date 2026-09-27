/* lc_term_gatt — the terminal's BLE GATT contract with the phone app (v3,
 * spec 2026-09-26-activation-registration-calls-design.md §6, numbers per
 * 2026-09-27-numbering-v2-design.md §6.3; the contract has no version field:
 * an app tells v2 from v3 firmware by the ACTIVATED / REGISTERED / INCOMING
 * lengths, 8 / 9 / 12 bytes in v2 and 9 / 10 / 13 in v3).
 *
 * Security (spec 2026-09-27-ble-pairing-design.md): every characteristic
 * needs an encrypted link with an authenticated (MITM) key: LE Secure
 * Connections only, passkey entry, the terminal DisplayOnly. Its OLED's
 * Pairing screen shows the 6-digit code, which changes at boot, after every
 * phone disconnect and after every failed attempt; 3 failures in 60 s lock
 * pairing for 60 s. Until then, reads, writes and CCCD writes get ATT 0x05
 * (insufficient authentication), which makes Android pair, and no
 * notifications are sent. Bonds (up to 3 phones; a new one replaces the
 * oldest) persist in NVS, so a bonded phone re-encrypts without the code.
 * Holding PRG 5 s on the Pairing screen deletes them all.
 *
 * One primary service, five characteristics (128-bit UUIDs):
 *   service  6c630001-7e2a-4b8e-9f2d-3c1a5e7b0d10
 *   UP       6c630002-...  write / write-without-response
 *            one app data frame (<= LC_SIG_APP_MAX = 18 bytes) per write:
 *            voice in a call (encrypted on the air in Part 15 mode); outside
 *            a call, with a grant, the diagnostic loopback.
 *            ATT errors: 0x80 not now (the app retries), 0x0D too long
 *            (retrying can never succeed).
 *   DOWN     6c630003-...  notify: one app data frame per notification
 *   STATUS   6c630004-...  read / notify: LC_GATT_STATUS_LEN bytes, below.
 *            Notified on every link/radio change and on every signalling
 *            state change (byte 3), not only when an EVENT also fires.
 *   COMMAND  6c630005-...  write (with response): op (1) || args
 *            0x01 ACTIVATE + QR text (ASCII "opencell:2:...", 111 B, <= 120 B
 *                 with whitespace; a long write is fine)
 *            0x02 DIAL + number: ASCII, 1-24 B, any dialled form
 *                 ("606-555-1235", "+883-1-606-555-01235", ...); the terminal
 *                 completes it from its own number (0x81 if it is not a
 *                 number). The app sends the full form "+883160655501235".
 *            0x03 ANSWER   0x04 REJECT   0x05 HANGUP
 *            0x06 DEACTIVATE + 0xA5 (confirmation)
 *            ATT errors: 0x80 not in the right state, 0x0D bad length,
 *            0x81 malformed argument.
 *   EVENT    6c630006-...  notify: ev (1) || args (numbers are 8 BCD bytes,
 *            full form, 0xF filler; call ids 4 bytes big-endian)
 *            0x01 ACTIVATED + number       0x02 ACT_FAILED + reason
 *            0x03 REGISTERED + number + mode (1 Part 15, 2 Part 97)
 *            0x04 REG_FAILED + reason      0x05 INCOMING + call_id + caller
 *            0x06 RINGING + call_id        0x07 CONNECTED + call_id + codec
 *            0x08 ENDED + call_id + cause  0x09 DEACTIVATED
 *            Events are not queued while no phone is connected; read STATUS
 *            byte 3 on connect.
 *
 * STATUS layout (little-endian):
 *   0  u8  state (lc_term_state_t)   1 u8 band (lc_band_t)   2 u8 tier (lc_tier_t)
 *   3  u8  signalling state (lc_sig_state_t: 0 not activated ... 8 releasing)
 *   4  i16 rssi_dbm                  6 i16 snr_qdb (0.25 dB; 0 on FLRC links)
 *      (state 0, searching: the strongest packet heard in the recent scan,
 *      from any cell; 0 and 0 when nothing was heard. Spec 2026-09-27 §3.1.)
 *   8  u32 tmid                     12 u32 frame               16 u32 cell_seed
 * The terminal runs activation, registration (MILENAGE) and call control;
 * the app is the user interface and holds no keys. */
#ifndef LC_TERM_GATT_H
#define LC_TERM_GATT_H

#include <stdint.h>

#include "lc_term.h"

#define LC_GATT_STATUS_LEN  20u
#define LC_GATT_STATUS_SIG  3u   /* STATUS byte: signalling state */
#define LC_GATT_COMMAND_MAX 121u /* op + 120 bytes of QR text */
#define LC_GATT_EVENT_MAX   16u

/* UUID bytes in NimBLE order (least significant byte first):
 * BLE_UUID128_INIT(LC_GATT_UUID_BYTES(0x01)) is the service. */
#define LC_GATT_UUID_BYTES(id) \
    0x10, 0x0d, 0x7b, 0x5e, 0x1a, 0x3c, 0x2d, 0x9f, 0x8e, 0x4b, 0x2a, 0x7e, (id), 0x00, 0x63, 0x6c
#define LC_GATT_ID_SERVICE 0x01
#define LC_GATT_ID_UP      0x02
#define LC_GATT_ID_DOWN    0x03
#define LC_GATT_ID_STATUS  0x04
#define LC_GATT_ID_COMMAND 0x05
#define LC_GATT_ID_EVENT   0x06
#define LC_GATT_ERR_NOT_NOW 0x80

void lc_term_pack_status(const lc_term_status_t *st, uint8_t out[LC_GATT_STATUS_LEN]);

#endif
