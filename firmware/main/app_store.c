#include <string.h>

#include "app.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NS  "lc"
#define NVS_KEY "cfg"

int app_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err == ESP_OK ? 0 : -1;
}

int app_store_load_config(lc_config_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return -1;
    }
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, out, &len);
    nvs_close(h);
    return (err == ESP_OK && len == sizeof(*out)) ? 0 : -1;
}

static int save_config(void *ctx, const lc_config_t *cfg)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

const lc_bsr_ops_t g_bsr_ops = { NULL, save_config };

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
    (void)offset; /* lc_fwupd guarantees sequential offsets */
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

const lc_fwupd_ops_t g_fwupd_ops = { NULL, ota_begin, ota_write, ota_finish, ota_abort };
