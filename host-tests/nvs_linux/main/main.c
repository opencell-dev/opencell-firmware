/* lc2oc: keep-file (this file names the legacy NVS namespaces on purpose) */
/* Runs app_nvs_migrate() on the flash image named by $OC_FLASH (0x8000: the
 * partition table, 0x9000: a board's NVS), then prints the namespace each
 * user would read first, whether it may write, and where the identity came
 * from. The image is changed in place, as the board's flash is.
 *
 * $OC_NVS_FAULT (optional, for check.sh's FAILED and UNSURE boots): a comma
 * separated list of KIND:NS, NS one of the new namespaces (oc, oc_id,
 * oc_scan, oc_ble) or * for all four. The real ESP-IDF store ops are wrapped
 * and fail, this boot only, like this:
 *   unsure  reading NS's marker is an I/O error (-2): UNSURE, nothing written
 *   failed  erasing NS fails (first write of a move): FAILED, nothing written
 *   cut     writing NS's marker fails, after every key was copied, and so
 *           does the erase that would clear the copy: FAILED, leaving NS an
 *           unmarked, complete-looking copy (a later boot's UNSURE shape) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_nvs.h"
#include "esp_err.h"
#include "esp_private/partition_linux.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "oc_nvs_mig.h"

static const char *const k_to[APP_NS_COUNT] = { "oc", "oc_id", "oc_scan", "oc_ble" };
static const oc_nvs_mig_ops_t *s_real;
static const char *s_fault;
static int s_cut[APP_NS_COUNT];

static int to_index(const char *ns)
{
    for (int i = 0; i < APP_NS_COUNT; i++) {
        if (strcmp(ns, k_to[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* Does $OC_NVS_FAULT hold kind:ns (or kind:*, ns a new namespace)? */
static int fault(const char *kind, const char *ns)
{
    if (s_fault == NULL || to_index(ns) < 0) {
        return 0;
    }
    size_t kl = strlen(kind);
    for (const char *p = s_fault; *p != '\0';) {
        const char *e = strchr(p, ',');
        size_t n = e != NULL ? (size_t)(e - p) : strlen(p);
        if (n > kl && strncmp(p, kind, kl) == 0 && p[kl] == ':') {
            const char *t = p + kl + 1;
            size_t tl = n - kl - 1;
            if ((tl == 1 && *t == '*') || (tl == strlen(ns) && strncmp(t, ns, tl) == 0)) {
                return 1;
            }
        }
        p += n + (e != NULL);
    }
    return 0;
}

static int f_list(void *ctx, const char *ns, oc_nvs_mig_key_t *keys, int max)
{
    (void)ctx;
    return s_real->list(s_real->ctx, ns, keys, max);
}

static int f_get(void *ctx, const char *ns, const char *key, uint8_t type, uint8_t *buf, size_t cap)
{
    (void)ctx;
    if (strcmp(key, OC_NVS_MIG_MARKER) == 0 && fault("unsure", ns)) {
        return -2;
    }
    return s_real->get(s_real->ctx, ns, key, type, buf, cap);
}

static int f_set(void *ctx, const char *ns, const char *key, uint8_t type, const uint8_t *val, size_t len)
{
    (void)ctx;
    if (strcmp(key, OC_NVS_MIG_MARKER) == 0 && fault("cut", ns)) {
        s_cut[to_index(ns)] = 1;
        return -1;
    }
    return s_real->set(s_real->ctx, ns, key, type, val, len);
}

static int f_erase_all(void *ctx, const char *ns)
{
    (void)ctx;
    int i = to_index(ns);
    if (fault("failed", ns) || (i >= 0 && s_cut[i])) {
        return -1;
    }
    return s_real->erase_all(s_real->ctx, ns);
}

/* FNV-1a, 64 bits: check.sh compares the identity read on a FAILED or UNSURE
 * boot with the one read after the clean boot, without printing it. */
static unsigned long long fnv(const uint8_t *p, size_t n)
{
    unsigned long long h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

void app_main(void)
{
    const char *img = getenv("OC_FLASH");
    if (img == NULL) {
        printf("set OC_FLASH\n");
        exit(2);
    }
    esp_partition_file_mmap_ctrl_t *in = esp_partition_get_file_mmap_ctrl_input();
    strlcpy(in->flash_file_name, img, sizeof(in->flash_file_name));
    if (nvs_flash_init() != ESP_OK) {
        printf("nvs_flash_init failed\n");
        exit(1);
    }
    s_fault = getenv("OC_NVS_FAULT");
    if (s_fault == NULL) {
        app_nvs_migrate();
    } else {
        s_real = app_nvs_ops();
        const oc_nvs_mig_ops_t ops = { NULL, s_real->u8_type, f_list, f_get, f_set, f_erase_all };
        app_nvs_migrate_ops(&ops);
    }
    printf("use: %s %s %s %s\n", app_nvs_ns(APP_NS_MAIN), app_nvs_ns(APP_NS_ID), app_nvs_ns(APP_NS_SCAN),
           app_nvs_ns(APP_NS_BLE));
    printf("write: %d %d %d %d\n", app_nvs_writable(APP_NS_MAIN), app_nvs_writable(APP_NS_ID),
           app_nvs_writable(APP_NS_SCAN), app_nvs_writable(APP_NS_BLE));
    uint8_t blob[OC_NVS_MIG_VALUE_MAX];
    size_t len = sizeof(blob);
    const char *src = NULL;
    esp_err_t err = app_nvs_get_blob(APP_NS_ID, "ident", blob, &len, &src);
    if (err == ESP_OK) {
        printf("ident: %s %u %016llx\n", src, (unsigned)len, fnv(blob, len));
    } else {
        printf("ident: none (%s)\n", esp_err_to_name(err));
    }
    nvs_flash_deinit();
    exit(0);
}
