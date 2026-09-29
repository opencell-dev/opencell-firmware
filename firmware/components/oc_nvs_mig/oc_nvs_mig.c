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

static void finish(const oc_nvs_mig_ops_t *o, const char *from, int n, int *moved)
{
    *moved = n;
    if (n > 0) {
        (void)o->erase_all(o->ctx, from); /* fails or power goes: the next boot's DONE path erases it */
    }
}

static int key_in(const oc_nvs_mig_key_t *keys, int n, const char *key)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(keys[i].key, key) == 0) {
            return 1;
        }
    }
    return 0;
}

/* A failure partway through the unmarked (copy) path, before the marker's
 * own set() has been confirmed to have failed too: a fresh marker check
 * settles what `to` actually holds now.
 *   present: the set() that triggered this reported failure but the write
 *            landed regardless -- `to` already holds the complete data;
 *            finish the move instead of erasing it.
 *   unknown: can't tell, exactly like the top-of-function check -- touch
 *            nothing.
 *   absent (confirmed): `to` can only hold an unfinished copy, so
 *            best-effort erase it and fail; `from` is untouched either way. */
static oc_nvs_mig_result_t fail_clearing(const oc_nvs_mig_ops_t *o, const char *from, const char *to, int n, int *moved)
{
    mark_t m = marker_state(o, to);
    if (m == MARK_PRESENT) {
        finish(o, from, n, moved);
        return OC_NVS_MIG_MOVED;
    }
    if (m == MARK_UNKNOWN) {
        return OC_NVS_MIG_UNSURE;
    }
    (void)o->erase_all(o->ctx, to); /* confirmed absent: best effort; a cut here just leaves cleanup for next boot */
    return OC_NVS_MIG_FAILED;
}

oc_nvs_mig_result_t oc_nvs_mig_move(const oc_nvs_mig_ops_t *o, const char *from, const char *to, int *moved)
{
    oc_nvs_mig_key_t keys[OC_NVS_MIG_KEYS_MAX];
    *moved = 0;

    mark_t m = marker_state(o, to);
    if (m == MARK_UNKNOWN) {
        /* Can't tell whether `to` already holds the moved data: touch
         * nothing, and hand the caller a result it must not act on as if
         * either namespace were known good. If `to` does hold it, this was
         * a misread and the next boot's marker check will see it and take
         * the DONE path; if it doesn't, the next boot starts the copy
         * fresh. Nothing is lost by waiting. */
        return OC_NVS_MIG_UNSURE;
    }
    if (m == MARK_PRESENT) {
        if (o->list(o->ctx, from, keys, OC_NVS_MIG_KEYS_MAX) != 0) {
            (void)o->erase_all(o->ctx, from); /* fails: the next boot erases it */
        }
        return OC_NVS_MIG_DONE;
    }

    /* MARK_ABSENT (confirmed) from here on: the unmarked path. */
    int n = o->list(o->ctx, from, keys, OC_NVS_MIG_KEYS_MAX);
    if (n < 0) {
        /* `from`'s own contents could not be observed. FAILED means "`from`
         * holds everything, untouched" -- a claim we have no basis for
         * making, so this is UNSURE instead. */
        return OC_NVS_MIG_UNSURE;
    }
    if (n > OC_NVS_MIG_KEYS_MAX) {
        return OC_NVS_MIG_FAILED; /* `from` was observed and is intact, just too big to copy */
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(keys[i].key, OC_NVS_MIG_MARKER) == 0) {
            /* `from` names a key that collides with the marker: refuse
             * rather than copy it in and have `to` look already-moved. */
            return OC_NVS_MIG_FAILED;
        }
    }

    /* A cut-short copy can only hold keys taken from `from`: list `to` and
     * require every key it currently has (the marker's name included -- if
     * `to` shows it here despite the check above just reading it as absent,
     * that disagreement is itself reason not to trust either read) to be
     * explained by `from`'s listing just taken. A listing error, more
     * entries than `from` has, or any key `to` has that `from` doesn't,
     * means `to` did not come from a cut-short copy of this `from`, and
     * erasing it on that assumption would not be safe. */
    oc_nvs_mig_key_t to_keys[OC_NVS_MIG_KEYS_MAX];
    int              nt = o->list(o->ctx, to, to_keys, OC_NVS_MIG_KEYS_MAX);
    if (nt < 0 || nt > n) {
        return OC_NVS_MIG_UNSURE;
    }
    for (int i = 0; i < nt; i++) {
        if (!key_in(keys, n, to_keys[i].key)) {
            return OC_NVS_MIG_UNSURE;
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
            return fail_clearing(o, from, to, n, moved);
        }
    }
    for (int i = 0; i < n; i++) {
        int la = o->get(o->ctx, from, keys[i].key, keys[i].type, s_a, sizeof(s_a));
        int lb = o->get(o->ctx, to, keys[i].key, keys[i].type, s_b, sizeof(s_b));
        if (la < 0 || la != lb || memcmp(s_a, s_b, (size_t)la) != 0) {
            return fail_clearing(o, from, to, n, moved);
        }
    }
    const uint8_t one = 1;
    if (o->set(o->ctx, to, OC_NVS_MIG_MARKER, o->u8_type, &one, 1) != 0) {
        return fail_clearing(o, from, to, n, moved); /* the write itself failed: no marker was persisted (unless it landed anyway) */
    }
    if (marker_state(o, to) != MARK_PRESENT) {
        /* The set reported success but the read-back can't confirm it.
         * Whether `to` truly holds the marker is now unknown, so -- exactly
         * like the top-of-function check -- touch nothing and return
         * UNSURE: if it does, the next boot's DONE path finishes cleanly;
         * if it doesn't, the next boot's unmarked path starts the copy
         * over. Never erase `to` here. */
        return OC_NVS_MIG_UNSURE;
    }
    finish(o, from, n, moved);
    return OC_NVS_MIG_MOVED;
}
