/* The terminal's scan list (lc_term_scan_t, channel-list spec §5.4) in NVS
 * namespace "lc_scan", key "list": its own namespace, so the identity blob
 * and its saver (term_ident.c) are untouched. Loaded once at boot, before
 * signalling starts. Saves come from the link task (term_lock held), so the
 * blob is handed to a core-0 task: an NVS write can stall for milliseconds
 * and the link task must not. */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "term.h"

#define NVS_NS  "lc_scan"
#define NVS_KEY "list"
#define SAVE_RETRY_MS 30000u /* after a failed write; no tight loop on a bad flash */

static const char *TAG = "lc_scan";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_pending[LC_SCAN_BLOB_MAX];
static size_t s_pending_len;
static TaskHandle_t s_saver;

static void saver_task(void *arg)
{
    (void)arg;
    uint8_t blob[LC_SCAN_BLOB_MAX];
    TickType_t wait = portMAX_DELAY;
    for (;;) {
        /* a new save, or (after a failed write) the retry timeout: either way
         * write what s_pending holds now, the latest list */
        ulTaskNotifyTake(pdTRUE, wait);
        taskENTER_CRITICAL(&s_mux);
        size_t len = s_pending_len; /* the latest save wins */
        memcpy(blob, s_pending, len);
        taskEXIT_CRITICAL(&s_mux);
        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, NVS_KEY, blob, len);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err != ESP_OK) {
            /* term_scan_save already cleared `dirty`: retry here, at the next
             * change or SAVE_RETRY_MS from now, whichever comes first */
            ESP_LOGE(TAG, "scan list save failed: %s; retry in %u s", esp_err_to_name(err),
                     (unsigned)(SAVE_RETRY_MS / 1000u));
            wait = pdMS_TO_TICKS(SAVE_RETRY_MS);
        } else {
            wait = portMAX_DELAY;
        }
    }
}

void term_scan_load(lc_term_scan_t *s)
{
    uint8_t blob[LC_SCAN_BLOB_MAX];
    size_t len = sizeof(blob);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, NVS_KEY, blob, &len);
        nvs_close(h);
    }
    if (err == ESP_OK && lc_term_scan_unpack(s, blob, len) == 0) {
        ESP_LOGI(TAG, "scan list: %u user, %u network (v%u), %u learned, last %lu kHz, fallback %u/%u",
                 s->n_user, s->n_net, s->net_ver, s->n_learn, (unsigned long)(s->last.freq_hz / 1000u),
                 s->fallback_after, s->fallback_chunk);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no scan list yet: defaults");
    } else {
        /* spec §5.4: a corrupt blob loads as empty, which leaves the defaults
         * working; the next save replaces it */
        ESP_LOGW(TAG, "scan list unreadable (%s, %u bytes): defaults", esp_err_to_name(err), (unsigned)len);
    }
    xTaskCreatePinnedToCore(saver_task, "lc_scan", 3072, NULL, 3, &s_saver, 0); /* core 1 is the radio's */
}

void term_scan_save(lc_term_scan_t *s)
{
    uint8_t blob[LC_SCAN_BLOB_MAX];
    size_t len = lc_term_scan_pack(s, blob);
    s->dirty = 0;
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_pending, blob, len);
    s_pending_len = len;
    taskEXIT_CRITICAL(&s_mux);
    if (s_saver != NULL) {
        xTaskNotifyGive(s_saver);
    }
}
