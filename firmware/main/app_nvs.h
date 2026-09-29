/* lc2oc: keep-file (this file names the legacy NVS namespaces on purpose) */
/* Our NVS namespaces. Boards flashed before the lc -> oc rename (2026-09-29)
 * hold them under the LoRaCell names; app_nvs_migrate() moves each one once
 * (oc_nvs_mig), on the first boot of this firmware, before anything reads
 * NVS. NimBLE's bond store ("nimble_bond") and "phy" are not ours and stay.
 *
 * Each pair has its own result (oc_nvs_mig.h, the caller's contract):
 *   MOVED, DONE  read and write the new namespace
 *   FAILED       read the old one; write neither this boot
 *   UNSURE       read the new one, then (key not found) the old; write neither
 * So every user reads through app_nvs_get_u8/_blob (or, for a key that must
 * come from the namespace another key came from, opens that `src` itself,
 * NVS_READONLY), and writes only when app_nvs_writable() says so, into
 * app_nvs_ns(). Nothing may open either namespace NVS_READWRITE otherwise:
 * that alone can create a namespace. */
#ifndef APP_NVS_H
#define APP_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "oc_nvs_mig.h"

typedef enum {
    APP_NS_MAIN, /* oc:      term (role flag, u8), cfg (bs-radio CONFIG, blob) */
    APP_NS_ID,   /* oc_id:   ident (the subscriber identity, blob) */
    APP_NS_SCAN, /* oc_scan: list (the scan list, blob) */
    APP_NS_BLE,  /* oc_ble:  gatt_ver, sc_pend (u8), sc_done (blob) */
    APP_NS_COUNT
} app_ns_t;

/* Moves lc -> oc, lc_id -> oc_id, lc_scan -> oc_scan, lc_ble -> oc_ble.
 * Called by app_store_init right after nvs_flash_init, in both roles. Until
 * it has run every pair counts as FAILED: read the old name, write nothing. */
void app_nvs_migrate(void);

/* The same over other store ops, and the real ones (ESP-IDF nvs_*): for
 * host-tests/nvs_linux, which wraps them to inject faults. */
void                    app_nvs_migrate_ops(const oc_nvs_mig_ops_t *ops);
const oc_nvs_mig_ops_t *app_nvs_ops(void);

oc_nvs_mig_result_t app_nvs_result(app_ns_t ns);

/* The namespace read first: the new name, or the old one when the move
 * FAILED. On MOVED/DONE it is also the one written. */
const char *app_nvs_ns(app_ns_t ns);

/* 1 when this pair may be written this boot (MOVED or DONE), else 0. */
int app_nvs_writable(app_ns_t ns);

/* key by the read rule. ESP_OK (*src: the namespace it came from),
 * ESP_ERR_NVS_NOT_FOUND in every namespace read (*src: NULL), or the first
 * other error (*src: the namespace that gave it). src may be NULL. For a
 * blob, *len is the capacity in and the length out. */
esp_err_t app_nvs_get_u8(app_ns_t ns, const char *key, uint8_t *out, const char **src);
esp_err_t app_nvs_get_blob(app_ns_t ns, const char *key, void *buf, size_t *len, const char **src);

#endif
