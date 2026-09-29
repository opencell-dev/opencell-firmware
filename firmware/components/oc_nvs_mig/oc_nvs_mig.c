#include "oc_nvs_mig.h"

#include <string.h>

static uint8_t s_a[OC_NVS_MIG_VALUE_MAX];
static uint8_t s_b[OC_NVS_MIG_VALUE_MAX];

typedef enum {
    MARK_ABSENT,  /* get() says NOT_FOUND (-1): `to` was genuinely never marked */
    MARK_PRESENT, /* get() returns exactly 1 byte, and it is 1 */
    MARK_UNKNOWN, /* anything else: an I/O error (-2), or a byte that isn't 1 */
} mark_t;

static mark_t marker_state(const oc_nvs_mig_ops_t *o, const char *to)
{
    uint8_t v = 0;
    int     r = o->get(o->ctx, to, OC_NVS_MIG_MARKER, o->u8_type, &v, 1);
    if (r == -1) {
        return MARK_ABSENT;
    }
    if (r == 1 && v == 1) {
        return MARK_PRESENT;
    }
    return MARK_UNKNOWN;
}

/* A failure partway through the unmarked (copy) path: `to` can only hold an
 * unfinished copy at this point, so best-effort erase it -- unless a fresh
 * check now says PRESENT (nothing here is reentrant, but cheap insurance
 * against ever erasing a completed move). Always returns FAILED. */
static oc_nvs_mig_result_t fail_clearing(const oc_nvs_mig_ops_t *o, const char *to)
{
    if (marker_state(o, to) != MARK_PRESENT) {
        (void)o->erase_all(o->ctx, to); /* best effort; a cut here just leaves cleanup for next boot */
    }
    return OC_NVS_MIG_FAILED;
}

oc_nvs_mig_result_t oc_nvs_mig_move(const oc_nvs_mig_ops_t *o, const char *from, const char *to, int *moved)
{
    oc_nvs_mig_key_t keys[OC_NVS_MIG_KEYS_MAX];
    *moved = 0;

    mark_t m = marker_state(o, to);
    if (m == MARK_UNKNOWN) {
        /* Can't tell whether `to` already holds the moved data: touch
         * nothing. If it does, this was a misread and the next boot's
         * marker check will see it and take the DONE path; if it doesn't,
         * the next boot starts the copy fresh. Nothing is lost by waiting. */
        return OC_NVS_MIG_FAILED;
    }
    if (m == MARK_PRESENT) {
        if (o->list(o->ctx, from, keys, OC_NVS_MIG_KEYS_MAX) != 0) {
            (void)o->erase_all(o->ctx, from); /* fails: the next boot erases it */
        }
        return OC_NVS_MIG_DONE;
    }

    /* MARK_ABSENT (confirmed) from here on: the unmarked path. */
    int n = o->list(o->ctx, from, keys, OC_NVS_MIG_KEYS_MAX);
    if (n < 0 || n > OC_NVS_MIG_KEYS_MAX) {
        return OC_NVS_MIG_FAILED;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(keys[i].key, OC_NVS_MIG_MARKER) == 0) {
            /* `from` names a key that collides with the marker: refuse
             * rather than copy it in and have `to` look already-moved. */
            return OC_NVS_MIG_FAILED;
        }
    }
    if (n == 0) {
        oc_nvs_mig_key_t to_keys[OC_NVS_MIG_KEYS_MAX];
        int              nt = o->list(o->ctx, to, to_keys, OC_NVS_MIG_KEYS_MAX);
        if (nt != 0) {
            /* `from` is empty but `to` isn't (or listing it failed): not a
             * state a real cut-short copy can produce (`from` is erased
             * only after `to` is marked). Leave both alone. A fresh check
             * might still find `to` marked (nothing here is atomic with the
             * check above); otherwise there is nothing safe to do but wait
             * for the next boot. */
            return marker_state(o, to) == MARK_PRESENT ? OC_NVS_MIG_DONE : OC_NVS_MIG_FAILED;
        }
    }
    /* Unmarked, `to` holds at most a copy cut short: start from nothing, so a
     * key `from` has since lost can't come back. */
    if (o->erase_all(o->ctx, to) != 0) {
        return OC_NVS_MIG_FAILED; /* that attempt itself was the best effort */
    }
    for (int i = 0; i < n; i++) {
        int len = o->get(o->ctx, from, keys[i].key, keys[i].type, s_a, sizeof(s_a));
        if (len < 0 || o->set(o->ctx, to, keys[i].key, keys[i].type, s_a, (size_t)len) != 0) {
            return fail_clearing(o, to);
        }
    }
    for (int i = 0; i < n; i++) {
        int la = o->get(o->ctx, from, keys[i].key, keys[i].type, s_a, sizeof(s_a));
        int lb = o->get(o->ctx, to, keys[i].key, keys[i].type, s_b, sizeof(s_b));
        if (la < 0 || la != lb || memcmp(s_a, s_b, (size_t)la) != 0) {
            return fail_clearing(o, to);
        }
    }
    const uint8_t one = 1;
    if (o->set(o->ctx, to, OC_NVS_MIG_MARKER, o->u8_type, &one, 1) != 0) {
        return fail_clearing(o, to); /* the write itself failed: no marker was persisted */
    }
    if (marker_state(o, to) != MARK_PRESENT) {
        /* The set reported success but the read-back can't confirm it.
         * Whether `to` truly holds the marker is now unknown, so -- exactly
         * like the top-of-function check -- touch nothing: if it does, the
         * next boot's DONE path finishes cleanly; if it doesn't, the next
         * boot's unmarked path starts the copy over. Never erase `to` here. */
        return OC_NVS_MIG_FAILED;
    }
    *moved = n;
    (void)o->erase_all(o->ctx, from); /* fails or power goes: the next boot's DONE path erases it */
    return OC_NVS_MIG_MOVED;
}
