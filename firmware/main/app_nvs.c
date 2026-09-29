/* lc2oc: keep-file (this file names the legacy NVS namespaces on purpose) */
/* The NVS side of oc_nvs_mig (its "ESP-IDF port contract"), the migration of
 * our four pairs at boot, and the read rule every user of them goes through. */
#include "app_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "oc_nvs";

/* Frozen: the names boards flashed before 2026-09-29 use. */
static const char *const k_old[APP_NS_COUNT] = { "lc", "lc_id", "lc_scan", "lc_ble" };
static const char *const k_new[APP_NS_COUNT] = { "oc", "oc_id", "oc_scan", "oc_ble" };
/* Until app_nvs_migrate has run: FAILED, i.e. the old names, read-only. */
static oc_nvs_mig_result_t s_res[APP_NS_COUNT] = { OC_NVS_MIG_FAILED, OC_NVS_MIG_FAILED, OC_NVS_MIG_FAILED,
                                                   OC_NVS_MIG_FAILED };

typedef union {
    uint8_t  u8;
    int8_t   i8;
    uint16_t u16;
    int16_t  i16;
    uint32_t u32;
    int32_t  i32;
    uint64_t u64;
    int64_t  i64;
} num_t;

/* The keys of ns. nvs_open(NVS_READONLY)'s NOT_FOUND is "no such namespace"
 * (the partition itself missing or not initialised is ESP_ERR_NVS_PART_NOT_FOUND
 * or ESP_ERR_NVS_NOT_INITIALIZED, errors), and the iterator's NOT_FOUND is
 * "no more entries": those two, and only those, count toward the number.
 * Everything else is -1, never 0. Counts past max. */
static int nv_list(void *ctx, const char *ns, oc_nvs_mig_key_t *keys, int max)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 0;
    }
    if (err != ESP_OK) {
        return -1;
    }
    nvs_iterator_t it = NULL;
    int n = 0;
    err = nvs_entry_find_in_handle(h, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        err = nvs_entry_info(it, &info);
        if (err != ESP_OK) {
            break;
        }
        if (n < max) {
            memcpy(keys[n].key, info.key, sizeof(keys[n].key));
            keys[n].key[sizeof(keys[n].key) - 1] = '\0';
            keys[n].type = (uint8_t)info.type;
        }
        n++;
        err = nvs_entry_next(&it); /* frees the iterator and sets it NULL at the end */
    }
    nvs_release_iterator(it); /* NULL-safe: free() */
    nvs_close(h);
    return err == ESP_ERR_NVS_NOT_FOUND ? n : -1;
}

/* A value: its length (a string's with its NUL), -1 for NOT_FOUND (the key or
 * the namespace), -2 for any other error, ESP_ERR_NVS_INVALID_LENGTH (longer
 * than cap) included. */
static int nv_get(void *ctx, const char *ns, const char *key, uint8_t type, uint8_t *buf, size_t cap)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return -1;
    }
    if (err != ESP_OK) {
        return -2;
    }
    size_t len = cap;
    num_t v;
    switch ((nvs_type_t)type) {
    case NVS_TYPE_U8:  err = nvs_get_u8(h, key, &v.u8);   len = 1; break;
    case NVS_TYPE_I8:  err = nvs_get_i8(h, key, &v.i8);   len = 1; break;
    case NVS_TYPE_U16: err = nvs_get_u16(h, key, &v.u16); len = 2; break;
    case NVS_TYPE_I16: err = nvs_get_i16(h, key, &v.i16); len = 2; break;
    case NVS_TYPE_U32: err = nvs_get_u32(h, key, &v.u32); len = 4; break;
    case NVS_TYPE_I32: err = nvs_get_i32(h, key, &v.i32); len = 4; break;
    case NVS_TYPE_U64: err = nvs_get_u64(h, key, &v.u64); len = 8; break;
    case NVS_TYPE_I64: err = nvs_get_i64(h, key, &v.i64); len = 8; break;
    case NVS_TYPE_STR: err = nvs_get_str(h, key, (char *)buf, &len); break; /* len counts the NUL */
    case NVS_TYPE_BLOB: err = nvs_get_blob(h, key, buf, &len); break;
    default: err = ESP_ERR_NVS_TYPE_MISMATCH; break;
    }
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return -1;
    }
    if (err != ESP_OK || len > cap) {
        return -2;
    }
    if (type != NVS_TYPE_STR && type != NVS_TYPE_BLOB) {
        memcpy(buf, &v, len);
    }
    return (int)len;
}

/* One value, committed; 0 or -1. A string's len counts its NUL. */
static int nv_set(void *ctx, const char *ns, const char *key, uint8_t type, const uint8_t *val, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    num_t v = { 0 };
    if (type != NVS_TYPE_STR && type != NVS_TYPE_BLOB) {
        memcpy(&v, val, len < sizeof(v) ? len : sizeof(v));
    }
    esp_err_t err;
    switch ((nvs_type_t)type) {
    case NVS_TYPE_U8:  err = nvs_set_u8(h, key, v.u8);   break;
    case NVS_TYPE_I8:  err = nvs_set_i8(h, key, v.i8);   break;
    case NVS_TYPE_U16: err = nvs_set_u16(h, key, v.u16); break;
    case NVS_TYPE_I16: err = nvs_set_i16(h, key, v.i16); break;
    case NVS_TYPE_U32: err = nvs_set_u32(h, key, v.u32); break;
    case NVS_TYPE_I32: err = nvs_set_i32(h, key, v.i32); break;
    case NVS_TYPE_U64: err = nvs_set_u64(h, key, v.u64); break;
    case NVS_TYPE_I64: err = nvs_set_i64(h, key, v.i64); break;
    case NVS_TYPE_STR:
        err = (len > 0 && val[len - 1] == '\0') ? nvs_set_str(h, key, (const char *)val) : ESP_ERR_INVALID_ARG;
        break;
    case NVS_TYPE_BLOB: err = nvs_set_blob(h, key, val, len); break;
    default: err = ESP_ERR_NVS_TYPE_MISMATCH; break;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

/* Every key of ns, committed; 0 or -1. A namespace that doesn't exist is
 * already empty: it is not opened NVS_READWRITE, which would create it. */
static int nv_erase_all(void *ctx, const char *ns)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 0;
    }
    if (err != ESP_OK) {
        return -1;
    }
    nvs_close(h);
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

static const oc_nvs_mig_ops_t k_ops = { NULL, NVS_TYPE_U8, nv_list, nv_get, nv_set, nv_erase_all };

const oc_nvs_mig_ops_t *app_nvs_ops(void)
{
    return &k_ops;
}

void app_nvs_migrate_ops(const oc_nvs_mig_ops_t *ops)
{
    for (int i = 0; i < APP_NS_COUNT; i++) {
        int moved = 0;
        oc_nvs_mig_result_t r = oc_nvs_mig_move(ops, k_old[i], k_new[i], &moved);
        s_res[i] = r;
        if (r == OC_NVS_MIG_MOVED) {
            ESP_LOGW(TAG, "NVS %s -> %s: %d key(s) moved", k_old[i], k_new[i], moved);
        } else if (r == OC_NVS_MIG_FAILED) {
            ESP_LOGE(TAG, "NVS %s -> %s FAILED: reading %s, no writes this boot", k_old[i], k_new[i], k_old[i]);
        } else if (r == OC_NVS_MIG_UNSURE) {
            ESP_LOGE(TAG, "NVS %s -> %s UNSURE: reading %s then %s, no writes this boot", k_old[i], k_new[i],
                     k_new[i], k_old[i]);
        } else if (r != OC_NVS_MIG_DONE) { /* not one of the four: treat it as UNSURE, the safest */
            s_res[i] = OC_NVS_MIG_UNSURE;
            ESP_LOGE(TAG, "NVS %s -> %s UNSURE: reading %s then %s, no writes this boot", k_old[i], k_new[i],
                     k_new[i], k_old[i]);
        }
    }
}

void app_nvs_migrate(void)
{
    app_nvs_migrate_ops(&k_ops);
}

oc_nvs_mig_result_t app_nvs_result(app_ns_t ns)
{
    return s_res[ns];
}

const char *app_nvs_ns(app_ns_t ns)
{
    return s_res[ns] == OC_NVS_MIG_FAILED ? k_old[ns] : k_new[ns];
}

int app_nvs_writable(app_ns_t ns)
{
    return s_res[ns] == OC_NVS_MIG_MOVED || s_res[ns] == OC_NVS_MIG_DONE;
}

/* The read rule: MOVED/DONE the new namespace; FAILED the old; UNSURE the
 * new, then the old when the key (or the namespace) is not found there. One
 * attempt each. get() reads key from an open handle; *len is reset to cap
 * before each attempt. */
typedef esp_err_t (*getter_t)(nvs_handle_t h, const char *key, void *out, size_t *len);

static esp_err_t read_rule(app_ns_t ns, const char *key, getter_t get, void *out, size_t *len, const char **src)
{
    const char *order[2] = { app_nvs_ns(ns), s_res[ns] == OC_NVS_MIG_UNSURE ? k_old[ns] : NULL };
    size_t cap = *len;
    if (src != NULL) {
        *src = NULL;
    }
    for (int i = 0; i < 2 && order[i] != NULL; i++) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(order[i], NVS_READONLY, &h);
        if (err == ESP_OK) {
            *len = cap;
            err = get(h, key, out, len);
            nvs_close(h);
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            if (src != NULL) {
                *src = order[i];
            }
            return err;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t get_u8(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)len;
    return nvs_get_u8(h, key, (uint8_t *)out);
}

static esp_err_t get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    return nvs_get_blob(h, key, out, len);
}

esp_err_t app_nvs_get_u8(app_ns_t ns, const char *key, uint8_t *out, const char **src)
{
    size_t len = 1;
    return read_rule(ns, key, get_u8, out, &len, src);
}

esp_err_t app_nvs_get_blob(app_ns_t ns, const char *key, void *buf, size_t *len, const char **src)
{
    return read_rule(ns, key, get_blob, buf, len, src);
}
