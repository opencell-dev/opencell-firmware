#include <string.h>

#include "app.h"
#include "app_nvs.h"
#include "app_role.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_KEY "cfg"

static const char *TAG = "oc_store";

int app_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        app_nvs_migrate(); /* before anything opens a namespace: the role flag is read next */
    }
    return err == ESP_OK ? 0 : -1;
}

/* From the namespace the role flag came from; when it had none (not found,
 * or unreadable), by the read rule on its own (oc_nvs_mig.h, lc/oc). */
int app_store_load_config(oc_config_t *out)
{
    size_t len = sizeof(*out);
    esp_err_t err;
    const char *src = app_role_source();
    if (src != NULL) {
        nvs_handle_t h;
        err = nvs_open(src, NVS_READONLY, &h);
        if (err == ESP_OK) {
            err = nvs_get_blob(h, NVS_KEY, out, &len);
            nvs_close(h);
        }
    } else {
        err = app_nvs_get_blob(APP_NS_MAIN, NVS_KEY, out, &len, NULL);
    }
    return (err == ESP_OK && len == sizeof(*out)) ? 0 : -1;
}

static int save_config(void *ctx, const oc_config_t *cfg)
{
    (void)ctx;
    if (!app_role_can_save()) { /* the lc -> oc move FAILED or was UNSURE, or the role unreadable */
        ESP_LOGE(TAG, "CONFIG not saved this boot (%s); send it again after a reboot",
                 app_nvs_writable(APP_NS_MAIN) ? "role unreadable" : "NVS oc not migrated");
        return -1;
    }
    nvs_handle_t h;
    if (nvs_open(app_nvs_ns(APP_NS_MAIN), NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

const oc_bsr_ops_t g_bsr_ops = { NULL, save_config };

static esp_ota_handle_t s_ota;
static const esp_partition_t *s_part;

static int ota_begin(void *ctx)
{
    (void)ctx;
    s_part = esp_ota_get_next_update_partition(NULL);
    if (s_part == NULL) {
        return -1;
    }
    return esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota) == ESP_OK ? 0 : -1;
}

static int ota_write(void *ctx, uint32_t offset, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    (void)offset; /* oc_fwupd guarantees sequential offsets */
    return esp_ota_write(s_ota, data, len) == ESP_OK ? 0 : -1;
}

static int ota_finish(void *ctx, uint32_t image_size)
{
    (void)ctx;
    (void)image_size;
    if (esp_ota_end(s_ota) != ESP_OK) { /* verifies the image */
        return -1;
    }
    return esp_ota_set_boot_partition(s_part) == ESP_OK ? 0 : -1;
}

static void ota_abort(void *ctx)
{
    (void)ctx;
    esp_ota_abort(s_ota);
}

const oc_fwupd_ops_t g_fwupd_ops = { NULL, ota_begin, ota_write, ota_finish, ota_abort };
