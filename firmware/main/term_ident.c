/* The subscriber identity (lc_sig_ident_t: X25519 key pair, K, OPc, SQN,
 * number) in NVS namespace "lc_id". Loaded once at boot. Saves come from
 * lc_sig on the core-1 link task, so they are handed to a core-0 task: an
 * NVS write can stall for milliseconds and the link task must not. */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "term.h"

#define NVS_NS  "lc_id"
#define NVS_KEY "ident"

static const char *TAG = "lc_ident";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_pending[LC_SIG_IDENT_BLOB];
static TaskHandle_t s_saver;

static int write_blob(const uint8_t blob[LC_SIG_IDENT_BLOB])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY, blob, LC_SIG_IDENT_BLOB);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

static void saver_task(void *arg)
{
    (void)arg;
    uint8_t blob[LC_SIG_IDENT_BLOB];
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

int term_ident_load(lc_sig_ident_t *id)
{
    uint8_t blob[LC_SIG_IDENT_BLOB];
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
        if (lc_sig_ident_unpack(blob, len, id) != 0) {
            ESP_LOGE(TAG, "identity blob unreadable (%u bytes)", (unsigned)len);
            return -1;
        }
    }
    if (need_new) { /* first boot: a new key pair, not activated */
        uint8_t r[32];
        esp_fill_random(r, sizeof(r));
        lc_sig_ident_new(id, r);
        memset(r, 0, sizeof(r));
        lc_sig_ident_pack(id, blob);
        ESP_LOGI(TAG, "new identity%s", write_blob(blob) == 0 ? "" : " (save failed)");
    }
    ESP_LOGI(TAG, "identity: %s, key id %u", id->activated ? "activated" : "not activated", id->key_id);
    xTaskCreatePinnedToCore(saver_task, "lc_ident", 3072, NULL, 3, &s_saver, 0); /* core 1 is the radio's */
    return 0;
}

void term_ident_save(const lc_sig_ident_t *id)
{
    uint8_t blob[LC_SIG_IDENT_BLOB];
    lc_sig_ident_pack(id, blob);
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_pending, blob, sizeof(blob));
    taskEXIT_CRITICAL(&s_mux);
    xTaskNotifyGive(s_saver);
}
