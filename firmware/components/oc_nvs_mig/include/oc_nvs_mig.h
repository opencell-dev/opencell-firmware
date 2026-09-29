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
 * A move that stops anywhere is simply run again at the next boot.
 *
 * The marker read is three-way, not a bool: "found, byte 1" (marked),
 * "not found" (unmarked), and anything else -- an I/O error, or a byte that
 * isn't 1 -- is neither. That third case is "don't know", not "unmarked":
 * ops->get() failing on `to`'s marker is not proof `to` lacks the data, and
 * erasing `to` on that assumption, when `to` is in fact the only copy left,
 * would destroy it. "Don't know" is OC_NVS_MIG_UNSURE, a result distinct
 * from FAILED: FAILED means `from` is known to hold everything, untouched;
 * UNSURE means neither namespace is known good, and the caller must not
 * write to either one this boot (see the result enum below). A byte that
 * isn't 1 can only come from something outside this module writing the
 * marker key, or flash corruption -- this module only ever writes 1 -- and
 * it fails every boot the same safe way for as long as it persists. */
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

/* ESP-IDF port contract for ops (Task 4's app_nvs.c, over the real nvs_*
 * API):
 *  - get(): only ESP_ERR_NVS_NOT_FOUND maps to -1 (key or namespace
 *    genuinely absent); every other esp_err_t -- including
 *    ESP_ERR_NVS_INVALID_LENGTH (buf too small) -- maps to -2. Opening a
 *    namespace that does not yet exist with NVS_READONLY itself returns
 *    ESP_ERR_NVS_NOT_FOUND, which maps the same way: -1, not an error.
 *  - nvs_get_str()'s returned length includes the terminating NUL; account
 *    for that when comparing against `cap` and when sizing a T_STR value.
 *  - set() and erase_all(): each nvs_set_*()/nvs_erase_*() call is followed
 *    by nvs_commit(); a write that is not committed does not count as done.
 *  - list(): built from nvs_entry_find()/nvs_entry_next(); release the
 *    iterator (nvs_release_iterator()) on every exit, including early ones,
 *    and keep counting entries past `max` instead of stopping the scan --
 *    the true count above OC_NVS_MIG_KEYS_MAX is what makes
 *    oc_nvs_mig_move() fail the move instead of silently truncating it.
 *  - `from` is opened with nvs_open(ns, NVS_READONLY, &h) and listed with
 *    nvs_entry_find_in_handle(h, NVS_TYPE_ANY, &it). NOT_FOUND from
 *    nvs_open() means that namespace is genuinely absent (list() returns 0,
 *    per the rule above -- not an error). NOT_FOUND from
 *    nvs_entry_find_in_handle()/nvs_entry_next() means no entries or the end
 *    of the scan, also not an error. Neither is the same thing as a
 *    partition-level NOT_FOUND (the NVS partition itself missing or not
 *    initialized): that is a real error and must never be folded into
 *    "namespace empty" -- list() must return -1 for it, not 0. */

typedef enum {
    OC_NVS_MIG_FAILED = -1, /* `from` holds everything, untouched: use `from` this boot; the next boot retries */
    OC_NVS_MIG_MOVED  = 0,  /* moved now (`*moved` keys, 0 on a fresh board): use `to` */
    OC_NVS_MIG_DONE   = 1,  /* moved on an earlier boot: use `to` */
    OC_NVS_MIG_UNSURE = 2,  /* neither namespace is known good this boot: see below */
} oc_nvs_mig_result_t;

/* OC_NVS_MIG_UNSURE: the marker in `to` could not be confirmed one way or
 * the other this boot, or `from` is empty while `to` isn't in a state a real
 * cut-short copy cannot produce. Neither namespace is known good: the caller
 * must write to NEITHER `from` nor `to` this boot, and must not treat
 * "identity not found" in either as proof there is none. Concretely (Task 4,
 * app_nvs.c): whatever this namespace pair's readers are handed goes into a
 * "hold" state; term_ident.c's loader returns its own "never overwrite"
 * error instead of creating a fresh identity; anything that would normally
 * save (a new activation, a changed CONFIG) skips saving and retries next
 * boot. The condition is almost always transient (a flash read glitch): the
 * next boot's marker check starts fresh and, in practice, resolves cleanly.
 * A marker byte that is genuinely corrupted (not 1) -- which cannot happen
 * from this module's own writes, only from something else writing that key
 * or from flash corruption -- fails every boot the same safe way for as long
 * as it persists, never mistaken for "absent" and used to justify erasing
 * data.
 *
 * Moves every key of namespace `from` to namespace `to`:
 *   marker unreadable (get() errors, or the byte isn't 1): UNSURE, nothing
 *                       written.
 *   marker present:     erase whatever `from` still holds (an erase cut
 *                       short, or an older firmware's writes after a
 *                       downgrade); DONE.
 *   marker confirmed absent, `from` empty, `to` not:
 *                       not a state a real cut-short copy can produce
 *                       (`from` is erased only after `to` is marked); left
 *                       alone, never erased; UNSURE.
 *   marker confirmed absent, otherwise:
 *                       a key in `from` named like the marker fails the move
 *                       before any write (FAILED); else erase `to` (at most
 *                       a copy cut short), copy every key, read every key
 *                       back from both and compare, write the marker (skip
 *                       erasing `from` if there were no keys to copy), erase
 *                       `from`; MOVED. A failure before the marker's set()
 *                       is attempted erases `to` best-effort (it can only
 *                       hold an unfinished copy at this point) and returns
 *                       FAILED, `from` untouched -- unless a fresh marker
 *                       check then finds `to` already marked regardless (the
 *                       set() reported failure but the write landed): finish
 *                       the move (erase `from`, `*moved = n`) and return
 *                       MOVED instead. If the marker's own set() succeeds
 *                       but its read-back cannot confirm it, UNSURE: `to` is
 *                       left alone, since it may already hold the complete,
 *                       marked data; the next boot's marker check settles it
 *                       either way.
 * Not reentrant (two static value buffers). */
oc_nvs_mig_result_t oc_nvs_mig_move(const oc_nvs_mig_ops_t *ops, const char *from, const char *to, int *moved);

#endif
