/* The subscriber identity (oc_sig_ident_t: X25519 key pair, K, OPc, SQN,
 * number) in NVS namespace "oc_id". Loaded once at boot. Saves come from
 * oc_sig on the core-1 link task, so they are handed to a core-0 task: an
 * NVS write can stall for milliseconds and the link task must not. */
#include <string.h>

#include "bootloader_random.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "term.h"

#define NVS_NS  "oc_id"
#define NVS_KEY "ident"

static const char *TAG = "oc_ident";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_pending[OC_SIG_IDENT_BLOB];
static TaskHandle_t s_saver;

static int write_blob(const uint8_t blob[OC_SIG_IDENT_BLOB])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY, blob, OC_SIG_IDENT_BLOB);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

static void saver_task(void *arg)
{
    (void)arg;
    uint8_t blob[OC_SIG_IDENT_BLOB];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL(&s_mux);
        memcpy(blob, s_pending, sizeof(blob)); /* the latest save wins */
        taskEXIT_CRITICAL(&s_mux);
        if (write_blob(blob) != 0) {
            ESP_LOGE(TAG, "identity save failed");
        }
    }
}

/* A new, not activated identity: first boot, or a numbering-v1 blob. Runs
 * before term_ble_start, so neither Wi-Fi nor BT is up and esp_fill_random
 * alone is only pseudo-random (ESP-IDF "Random Number Generation"): the
 * bootloader's entropy source (SAR ADC noise) is switched on for the draw.
 * The terminal role uses no ADC, and BT has not started, so nothing else
 * needs the SAR ADC meanwhile. */
static void fresh_identity(oc_sig_ident_t *id, uint8_t blob[OC_SIG_IDENT_BLOB])
{
    uint8_t r[32];
    bootloader_random_enable();
    esp_fill_random(r, sizeof(r));
    bootloader_random_disable();
    oc_sig_ident_new(id, r);
    memset(r, 0, sizeof(r));
    oc_sig_ident_pack(id, blob);
    ESP_LOGI(TAG, "new identity%s", write_blob(blob) == 0 ? "" : " (save failed)");
}

int term_ident_load(oc_sig_ident_t *id)
{
    uint8_t blob[OC_SIG_IDENT_BLOB];
    size_t len = sizeof(blob);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, NVS_KEY, blob, &len);
        nvs_close(h);
    }
    int need_new = err == ESP_ERR_NVS_NOT_FOUND; /* no namespace, or no blob yet: first boot */
    if (!need_new) {
        if (err != ESP_OK) { /* any other NVS error: never overwrite what might be an activated identity */
            ESP_LOGE(TAG, "identity read failed: %s", esp_err_to_name(err));
            return -1;
        }
        int r = oc_sig_ident_unpack(blob, len, id);
        if (r == OC_SIG_IDENT_OLD) { /* numbering v2 §6.2: the user chose re-activation */
            ESP_LOGW(TAG, "identity v1 (13-digit number): re-activation needed");
            need_new = 1;
        } else if (r != 0) { /* never overwrite what might be an activated identity */
            ESP_LOGE(TAG, "identity blob unreadable (%u bytes)", (unsigned)len);
            return -1;
        }
    }
    if (need_new) { /* a new key pair, not activated */
        fresh_identity(id, blob);
    }
    ESP_LOGI(TAG, "identity: %s, key id %u", id->activated ? "activated" : "not activated", id->key_id);
    if (xTaskCreatePinnedToCore(saver_task, "oc_ident", 3072, NULL, 3, &s_saver, 0) != pdPASS) { /* core 1 is the radio's */
        s_saver = NULL;
        ESP_LOGE(TAG, "identity saver task not created: an activation will not survive a reboot");
    }
    return 0;
}

void term_ident_save(const oc_sig_ident_t *id)
{
    uint8_t blob[OC_SIG_IDENT_BLOB];
    oc_sig_ident_pack(id, blob);
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_pending, blob, sizeof(blob));
    taskEXIT_CRITICAL(&s_mux);
    if (s_saver != NULL) {
        xTaskNotifyGive(s_saver);
    }
}
