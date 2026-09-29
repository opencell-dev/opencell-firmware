/* lc2oc: keep-file (this file names the legacy NVS namespaces on purpose) */
/* oc_nvs_mig — moves one NVS namespace to a new name, once, safe against
 * power loss at any point. Written for the lc -> oc rename (2026-09-29):
 * boards flashed before it keep their data in lc, lc_id, lc_scan and lc_ble.
 * Portable: the store is reached through oc_nvs_mig_ops_t (app_nvs.c on the
 * board, a fake in host-tests/test_nvs_mig.c).
 *
 * The rule that makes it safe: the marker key OC_NVS_MIG_MARKER in the new
 * namespace decides which namespace holds the data. Without it the old one
 * does, untouched; the marker is written only after every key has been
 * copied and read back equal; the old namespace is erased only after that.
 * A move that stops anywhere is simply run again at the next boot. */
#ifndef OC_NVS_MIG_H
#define OC_NVS_MIG_H

#include <stddef.h>
#include <stdint.h>

#define OC_NVS_MIG_KEY_MAX   16u        /* a key name and its NUL (NVS_KEY_NAME_MAX_SIZE) */
#define OC_NVS_MIG_KEYS_MAX  16         /* keys one namespace may have; more fails the move */
#define OC_NVS_MIG_VALUE_MAX 512u       /* longest value copied; longer fails the move */
#define OC_NVS_MIG_MARKER    "oc_moved" /* u8 1 in the new namespace: copied and verified */

typedef struct {
    char    key[OC_NVS_MIG_KEY_MAX];
    uint8_t type; /* the store's own type code (nvs_type_t on the board) */
} oc_nvs_mig_key_t;

/* The store. Every write is committed when it returns. */
typedef struct {
    void   *ctx;
    uint8_t u8_type; /* the store's type code for a uint8_t (the marker) */
    /* The keys of namespace ns: returns their number (0 when the namespace is
     * empty or does not exist) or -1 on error. Fills at most max entries; a
     * return above max means there are more. */
    int (*list)(void *ctx, const char *ns, oc_nvs_mig_key_t *keys, int max);
    /* A value into buf: its length, -1 when the key does not exist, -2 on an
     * error or when it is longer than cap. */
    int (*get)(void *ctx, const char *ns, const char *key, uint8_t type, uint8_t *buf, size_t cap);
    int (*set)(void *ctx, const char *ns, const char *key, uint8_t type, const uint8_t *val, size_t len); /* 0 or -1 */
    int (*erase_all)(void *ctx, const char *ns);                                                         /* 0 or -1 */
} oc_nvs_mig_ops_t;

typedef enum {
    OC_NVS_MIG_FAILED = -1, /* not moved: use `from` this boot; the next boot tries again */
    OC_NVS_MIG_MOVED  = 0,  /* moved now (`*moved` keys, 0 on a fresh board): use `to` */
    OC_NVS_MIG_DONE   = 1,  /* moved on an earlier boot: use `to` */
} oc_nvs_mig_result_t;

/* Moves every key of namespace `from` to namespace `to`:
 *   marker in `to`:  erase whatever `from` still holds (an erase cut short, or
 *                    an older firmware's writes after a downgrade); DONE.
 *   no marker:       erase `to` (at most a copy cut short), copy every key,
 *                    read every key back from both and compare, write the
 *                    marker, erase `from`; MOVED. Any failure before the
 *                    marker: FAILED, `from` untouched.
 * Not reentrant (two static value buffers). */
oc_nvs_mig_result_t oc_nvs_mig_move(const oc_nvs_mig_ops_t *ops, const char *from, const char *to, int *moved);

#endif
