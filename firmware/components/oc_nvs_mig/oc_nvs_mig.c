#include "oc_nvs_mig.h"

#include <string.h>

static uint8_t s_a[OC_NVS_MIG_VALUE_MAX];
static uint8_t s_b[OC_NVS_MIG_VALUE_MAX];

static int marked(const oc_nvs_mig_ops_t *o, const char *to)
{
    uint8_t v = 0;
    return o->get(o->ctx, to, OC_NVS_MIG_MARKER, o->u8_type, &v, 1) == 1 && v == 1;
}

oc_nvs_mig_result_t oc_nvs_mig_move(const oc_nvs_mig_ops_t *o, const char *from, const char *to, int *moved)
{
    oc_nvs_mig_key_t keys[OC_NVS_MIG_KEYS_MAX];
    *moved = 0;
    if (marked(o, to)) {
        if (o->list(o->ctx, from, keys, OC_NVS_MIG_KEYS_MAX) != 0) {
            (void)o->erase_all(o->ctx, from); /* fails: the next boot erases it */
        }
        return OC_NVS_MIG_DONE;
    }
    int n = o->list(o->ctx, from, keys, OC_NVS_MIG_KEYS_MAX);
    if (n < 0 || n > OC_NVS_MIG_KEYS_MAX) {
        return OC_NVS_MIG_FAILED;
    }
    /* Unmarked, `to` holds at most a copy cut short: start from nothing, so a
     * key `from` has since lost can't come back. */
    if (o->erase_all(o->ctx, to) != 0) {
        return OC_NVS_MIG_FAILED;
    }
    for (int i = 0; i < n; i++) {
        int len = o->get(o->ctx, from, keys[i].key, keys[i].type, s_a, sizeof(s_a));
        if (len < 0 || o->set(o->ctx, to, keys[i].key, keys[i].type, s_a, (size_t)len) != 0) {
            return OC_NVS_MIG_FAILED;
        }
    }
    for (int i = 0; i < n; i++) {
        int la = o->get(o->ctx, from, keys[i].key, keys[i].type, s_a, sizeof(s_a));
        int lb = o->get(o->ctx, to, keys[i].key, keys[i].type, s_b, sizeof(s_b));
        if (la < 0 || la != lb || memcmp(s_a, s_b, (size_t)la) != 0) {
            return OC_NVS_MIG_FAILED;
        }
    }
    const uint8_t one = 1;
    if (o->set(o->ctx, to, OC_NVS_MIG_MARKER, o->u8_type, &one, 1) != 0 || !marked(o, to)) {
        return OC_NVS_MIG_FAILED;
    }
    *moved = n;
    (void)o->erase_all(o->ctx, from); /* fails or power goes: the next boot's DONE path erases it */
    return OC_NVS_MIG_MOVED;
}
