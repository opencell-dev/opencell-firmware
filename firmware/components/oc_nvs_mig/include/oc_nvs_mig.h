/* lc2oc: keep-file (this file names the legacy NVS namespaces on purpose) */
/* oc_nvs_mig — moves one NVS namespace to a new name, once, safe against
 * power loss at any point. Written for the lc -> oc rename (2026-09-29):
 * boards flashed before it keep their data in lc, lc_id, lc_scan and lc_ble.
 * Portable: the store is reached through oc_nvs_mig_ops_t (app_nvs.c on the
 * board, a fake in host-tests/test_nvs_mig.c).
 *
 * It ships in ONE firmware version (the plan's user amendment of
 * 2026-09-29): once every board has booted that version and its NVS has
 * been read back and checked, Task 12 removes the migration, and the next
 * firmware opens oc* directly. The caller's contract below holds for that
 * one version.
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
 * UNSURE means neither namespace is known good. After either one the caller
 * writes to NEITHER namespace that boot (see the result enum below). A byte
 * that isn't 1 can only come from something outside this module writing the
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
     * return above max means there are more. 0 is a claim that the
     * namespace holds nothing, and it is acted on: an error must NEVER come
     * back as 0 (see the port contract below). */
    int (*list)(void *ctx, const char *ns, oc_nvs_mig_key_t *keys, int max);
    /* A value into buf: its length, -1 when the key does not exist, -2 on an
     * error or when it is longer than cap. */
    int (*get)(void *ctx, const char *ns, const char *key, uint8_t type, uint8_t *buf, size_t cap);
    int (*set)(void *ctx, const char *ns, const char *key, uint8_t type, const uint8_t *val, size_t len); /* 0 or -1 */
    int (*erase_all)(void *ctx, const char *ns);                                                         /* 0 or -1 */
} oc_nvs_mig_ops_t;

/* ESP-IDF port contract for ops (Task 4's app_nvs.c, over the real nvs_*
 * API):
 *  - list() must never fold any error into 0. A spurious 0 loses the
 *    identity on its own, one fault, no marker glitch needed: on the first
 *    boot, list(from) wrongly returning 0 makes the move copy nothing, mark
 *    `to` and return MOVED -- the caller then finds no `ident` in `to` and
 *    term_ident.c makes a fresh identity there -- and the next boot, seeing
 *    the marker, takes the DONE path and erases `from`, the real identity
 *    with it. So list() returns -1 for EVERY error: any esp_err_t from
 *    nvs_open() other than the namespace-absent NOT_FOUND below, any from
 *    nvs_entry_find_in_handle()/nvs_entry_next() other than their
 *    end-of-scan NOT_FOUND, and a partition-level NOT_FOUND (the NVS
 *    partition itself missing or not initialized), which is a real error
 *    and never "namespace empty".
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
 *    not an error). NOT_FOUND from nvs_entry_find_in_handle()/
 *    nvs_entry_next() means no entries or the end of the scan, also not an
 *    error. Those two are the only NOT_FOUNDs that may become 0. */

typedef enum {
    OC_NVS_MIG_FAILED = -1, /* `from` holds everything, untouched: read `from`, write NEITHER namespace this boot */
    OC_NVS_MIG_MOVED  = 0,  /* moved now (`*moved` keys, 0 on a fresh board): use `to` */
    OC_NVS_MIG_DONE   = 1,  /* moved on an earlier boot: use `to` */
    OC_NVS_MIG_UNSURE = 2,  /* neither namespace is known good: read `to`, then `from`; write NEITHER this boot */
} oc_nvs_mig_result_t;

/* The caller's contract (Task 4: app_nvs.c and every reader of the four
 * pairs). oc_nvs_mig_move() is called once for each of lc/oc, lc_id/oc_id,
 * lc_scan/oc_scan, lc_ble/oc_ble, and each call's result is independent --
 * a board can be MOVED for one pair this boot and UNSURE for another; there
 * is no combined result.
 *
 *   result   reads come from                         writes this boot
 *   MOVED    `to`                                    `to`
 *   DONE     `to`                                    `to`
 *   FAILED   `from` only                             NEITHER namespace
 *   UNSURE   `to` first, then `from`                 NEITHER namespace
 *
 * The read rule, per key, on FAILED and UNSURE: FAILED reads `from`. UNSURE
 * reads `to`, and on NOT_FOUND there, `from`. One attempt each, no retry. A
 * value found is used; NOT_FOUND in every namespace read gives that key's
 * default, as on a fresh board; any other error is handled per pair below.
 * Nothing the caller does may write, erase or commit in either namespace of
 * the pair that boot.
 *
 * Why FAILED writes nothing, like UNSURE. After FAILED, `to` may still hold
 * keys: the best-effort erase of an unfinished copy can itself fail. If the
 * caller then writes or erases keys in `from` -- term_ble.c's sc_save()
 * doing nvs_erase_key() on sc_done/sc_pend in lc_ble is exactly this --
 * `to` can be left holding a key `from` no longer has. The next boot's
 * cross-check refuses that shape and returns UNSURE, and, since UNSURE
 * writes nothing either, it returns UNSURE on every boot after: the pair is
 * stuck until an operator fixes it (below). Writing nothing on FAILED keeps
 * `from` exactly as the next boot's retry expects to find it. The cost:
 * nothing of that pair is saved that boot. An activation (a new or changed
 * identity) or a bs-radio CONFIG save attempted on a FAILED or UNSURE boot
 * fails that boot, is reported as a save error, and has to be done again on
 * a later boot, once the pair has migrated.
 *
 * OC_NVS_MIG_UNSURE, when: the marker in `to` could not be confirmed one way
 * or the other this boot; or `from` could not be listed; or `to` holds
 * anything that isn't a subset of `from`'s keys (a real cut-short copy only
 * ever holds keys taken from `from`); or a copy failed and `to`'s listing
 * could not rule out a marker there. The caller must not treat "not found"
 * in either namespace as proof there is nothing. The condition is almost
 * always transient (a flash read glitch): the next boot's marker check
 * starts fresh and, in practice, resolves cleanly. A marker byte that is
 * genuinely corrupted (not 1) -- which cannot happen from this module's own
 * writes, only from something else writing that key or from flash
 * corruption -- fails every boot the same safe way for as long as it
 * persists, never mistaken for "absent" and used to justify erasing data.
 *
 * Per pair, on FAILED or UNSURE:
 *   lc/oc (the role flag `term`, and bs-radio CONFIG `cfg`): `term` follows
 *     the read rule. `cfg` is read from the same namespace that supplied
 *     `term`; if `term` was NOT_FOUND in every namespace read, `cfg`
 *     follows the read rule on its own. A `term` read that returns an error
 *     (anything but NOT_FOUND, in any namespace read) makes the role
 *     UNKNOWN: the board boots in the role it would have without this pair
 *     -- the default, bs-radio (app_role_is_terminal()'s v == 0) -- the PRG
 *     button's role toggle (set_terminal) and every save of this pair (the
 *     role, CONFIG) are suppressed, and app_role.c logs, at error level, on
 *     every boot it happens:
 *       oc_role: role unreadable (<ns>: <esp_err_t name>): booting as bs-radio, PRG toggle and saves off this boot
 *     The PRG toggle and CONFIG save are suppressed on every FAILED or
 *     UNSURE boot, UNKNOWN or not. One boot of a stale role is possible on
 *     UNSURE: `to` may be an unmarked leftover (a copy cut short before its
 *     marker) whose `term` predates a later change to `term` in `from` --
 *     which, with no writes on FAILED/UNSURE boots, only an older firmware
 *     run in between can make. That boot runs in the role `to` remembers;
 *     the next boot that migrates cleanly erases `to`, copies `from` again,
 *     and the role is `from`'s.
 *   lc_id/oc_id (the subscriber identity `ident`): `ident` follows the read
 *     rule; the board runs that boot on the identity found. If none is found
 *     (NOT_FOUND in every namespace read) or a read errors, term_ident.c's
 *     loader returns its own never-overwrite error instead of creating a
 *     fresh identity: creating one is a write.
 *   lc_scan/oc_scan (`list`): read at boot by the read rule; absent or an
 *     error gives the empty list, as on a fresh board. Saves are skipped;
 *     the list lives in RAM that boot and is saved on a later one.
 *   lc_ble/oc_ble (`gatt_ver`, `sc_pend`, `sc_done`): `gatt_ver` follows the
 *     read rule, and the namespace that supplied it also supplies `sc_pend`
 *     and `sc_done`; absent or an error gives the defaults, as on a fresh
 *     board (no version stored: a Service Changed is queued for every
 *     bonded phone). Saves are skipped (gatt_table_check()'s version write,
 *     every sc_save()), so the Service Changed state lives in RAM only: a
 *     phone may get a spurious Service Changed that boot, and again on the
 *     next, until a boot that can save. It is harmless: Service Changed only
 *     makes the phone discover the GATT table again, and the table it finds
 *     is the one it already knew.
 *
 * Log lines (app_nvs.c, tag oc_nvs, error level), printed every boot the
 * result comes back:
 *   oc_nvs: NVS <from> -> <to> failed: reading <from>, writing neither this boot
 *   oc_nvs: NVS <from> -> <to> unsure: reading <to> then <from>, writing neither this boot
 * One such line is a glitch; the next boot normally migrates. A stuck pair
 * is one that prints the same line on two or more boots in a row. A pair
 * stuck FAILED is one the store refuses to take (NVS full, a value longer
 * than OC_NVS_MIG_VALUE_MAX, more than OC_NVS_MIG_KEYS_MAX keys): `from` is
 * intact and the board runs on it read-only; fix the cause (read the
 * partition as in step 1 below and look with nvs_tool.py -d storage_info),
 * not the pair. A pair stuck UNSURE is in a shape this module will not
 * guess about -- for example a marker byte that isn't 1, or `to` holding a
 * key `from` lacks -- and needs the operator, as follows.
 *
 * Recovering a pair stuck UNSURE, on the laptop, ESP-IDF environment active
 * (esptool.py, $IDF_PATH), the board on $PORT with nothing else holding the
 * port:
 *  1. Read the NVS partition (the nvs row of firmware/partitions.csv) and
 *     keep the file:
 *       esptool.py --chip esp32s3 -p $PORT read_flash 0x9000 0x6000 stuck.bin
 *     Task 5 also kept each board's partition from just before this
 *     firmware was flashed (~/Documents/opencell-archive/nvs-backups/
 *     <board>-...-nvs-20260929-pre-oc.bin): it is the reference for what the
 *     board held before the migration.
 *  2. Look at both namespaces of the pair:
 *       NT=$IDF_PATH/components/nvs_flash/nvs_partition_tool/nvs_tool.py
 *       python $NT -d minimal --color never stuck.bin | grep -E ' (lc_id|oc_id):'
 *       python $NT -d blobs --color never stuck.bin | grep -E '^(lc_id|oc_id):'
 *     The first prints "<ns>:<key> = <value>" (a blob shows as <key>[0]);
 *     the second prints each blob's size ("oc_id:ident - Type: Blob
 *     (Version 2), Size: 114" is a complete identity).
 *  3. Decide, per pair:
 *     lc_id/oc_id: if oc_id holds `ident` (the complete identity, 114 B),
 *       write `oc_moved` = 1 (u8) into oc_id, replacing any `oc_moved`
 *       already there; the next boot is DONE and erases lc_id. If oc_id
 *       lacks `ident` and lc_id holds it, erase oc_id only; the next boot
 *       copies lc_id again. NEVER erase both. If neither holds it, the
 *       identity is not in this partition: put the pre-oc backup's
 *       lc_id:ident into lc_id and erase oc_id, or accept a fresh identity
 *       (the board must then be provisioned again).
 *     lc/oc: the same rule, with `term` in place of `ident` on a terminal,
 *       and `cfg` on the bs-radio: if oc holds it, mark oc; if oc lacks it
 *       and lc holds it, erase oc only. NEVER erase both.
 *     lc_scan/oc_scan, lc_ble/oc_ble: erasing both namespaces of the pair is
 *       safe: the scan list and the BLE state are rebuilt (a Service Changed
 *       goes to the bonded phones, harmless as above).
 *  4. Make the fixed partition, fixed.bin, one of two ways:
 *     a) a small host tool that opens stuck.bin with ESP-IDF's own NVS code
 *        (the linux-target build Task 4 adds in host-tests/nvs_linux) and
 *        does only the step-3 change -- nvs_set_u8(h, "oc_moved", 1) or
 *        nvs_erase_all(h), then nvs_commit(h) -- leaving every other entry
 *        as it was; or
 *     b) by hand: dump every entry with
 *          python $NT -d minimal -f json stuck.bin > stuck.json
 *        and write fixed.csv from it -- the header "key,type,encoding,value",
 *        then per namespace a line "<ns>,namespace,," followed by one line
 *        per key: "<key>,data,u8,<n>" (uint8_t), "<key>,data,u32,<n>"
 *        (uint32_t), "<key>,data,base64,<data>" (blob_data), "<key>,data,
 *        string,<text>" (string) -- with the step-3 change made (a line
 *        "oc_moved,data,u8,1" under the namespace, or the namespace's lines
 *        left out). Keep NimBLE's namespaces: leaving them out drops the
 *        phone bonds. Leave out phy:cal_data only: the JSON does not return
 *        that multi-page blob whole, and the PHY calibrates again and stores
 *        it at the next boot. Then
 *          python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate fixed.csv fixed.bin 0x6000
 *     Either way, check fixed.bin with step 2's commands and compare
 *     python $NT -d minimal -f json on both files: only the pair's change
 *     (and, for b, phy:cal_data) may differ.
 *  5. Write it and let the board boot:
 *       esptool.py --chip esp32s3 -p $PORT write_flash 0x9000 fixed.bin
 *     The pair's unsure line must not come back. A marked pair moves
 *     nothing (DONE prints nothing); an erased `to` prints
 *     "oc_nvs: NVS <from> -> <to>: <n> key(s) moved". Read the partition
 *     again (step 1) and check it (step 2).
 *
 * Moves every key of namespace `from` to namespace `to`:
 *   marker unreadable (get() errors, or the byte isn't 1): UNSURE, nothing
 *                       written.
 *   marker present:     erase whatever `from` still holds (an erase cut
 *                       short, or an older firmware's writes after a
 *                       downgrade); DONE.
 *   marker confirmed absent, but listing `from` errors:
 *                       `from`'s contents were never observed, so FAILED
 *                       ("`from` holds everything, untouched") would be a
 *                       claim with no basis; UNSURE, nothing written.
 *   marker confirmed absent, `from` listed with more than
 *   OC_NVS_MIG_KEYS_MAX keys:
 *                       `from` was observed and is intact, just too big to
 *                       copy; FAILED.
 *   marker confirmed absent, a key in `from` named like the marker:
 *                       refuse rather than copy it in and have `to` look
 *                       already-moved; FAILED, before any write.
 *   marker confirmed absent, but `to`'s own listing doesn't check out
 *   (listing it errors, has more keys than `from`, or holds any key --
 *   including the marker's name -- that isn't one of `from`'s current
 *   keys):
 *                       `to` holds anything that isn't a subset of
 *                       `from`'s keys (a real cut-short copy only ever
 *                       holds keys taken from `from`), so it did not come
 *                       from one, and erasing it on that assumption would
 *                       not be safe. UNSURE, both namespaces left exactly
 *                       as found.
 *   marker confirmed absent, `to` checks out as at most a cut-short copy:
 *                       erase `to`, copy every key, read every key back
 *                       from both and compare, write the marker (skip
 *                       erasing `from` if there were no keys to copy), erase
 *                       `from`; MOVED. A failure before the marker's set()
 *                       is confirmed lists `to`, then takes a fresh marker
 *                       read: present (the failure's own report was wrong,
 *                       or came after the write actually landed) finishes
 *                       the move (erase `from`, `*moved = n`, MOVED)
 *                       instead of erasing `to`; unknown is UNSURE,
 *                       touching nothing; absent, but the listing shows the
 *                       marker key or could not be taken, is UNSURE too,
 *                       touching nothing (an erase of `to` cut short could
 *                       leave the marker standing over part of the data,
 *                       and the next boot's DONE path would trust it);
 *                       absent and not listed erases `to` best-effort (it
 *                       can only hold an unfinished copy) and returns
 *                       FAILED, `from` untouched. If the marker's own set()
 *                       succeeds but its read-back cannot confirm it,
 *                       UNSURE: `to` is left alone, since it may already
 *                       hold the complete, marked data; the next boot's
 *                       marker check settles it either way.
 * Not reentrant (two static value buffers). */
oc_nvs_mig_result_t oc_nvs_mig_move(const oc_nvs_mig_ops_t *ops, const char *from, const char *to, int *moved);

#endif
