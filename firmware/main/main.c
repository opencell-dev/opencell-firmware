/* OpenCell W12 firmware, bs-radio role: a slot executor driven by the Pi over
 * the GNSS-header UART (lc_link), timed by GPS PPS. See
 * docs/superpowers/specs/2026-09-23-lr2021-hardware-design.md §5.1. */
#include "app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lc_radio.h"
#include "w12_board.h"

static const char *TAG = "lc_main";

lc_clock_t g_clock;
lc_exec_t  g_exec;
lc_fwupd_t g_fwupd;
lc_bsr_t   g_bsr;

static SemaphoreHandle_t s_lock;

void app_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void app_unlock(void) { xSemaphoreGive(s_lock); }

void app_main(void)
{
    s_lock = xSemaphoreCreateMutex();
    w12_board_init();
    app_store_init();

    lc_config_t saved;
    int have_cfg = app_store_load_config(&saved) == 0;
    lc_band_t band = (have_cfg && saved.band < LC_BAND_COUNT) ? (lc_band_t)saved.band : LC_BAND_915;

    int err = lc_radio_init(band);
    if (err != 0) {
        ESP_LOGE(TAG, "radio init failed: %d", err);
    }

    const lc_exec_sink_t sink = { NULL, app_link_on_rx };
    lc_clock_init(&g_clock, APP_HOLDOVER_US);
    lc_exec_init(&g_exec, lc_radio_ops(), &sink);
    lc_fwupd_init(&g_fwupd, &g_fwupd_ops);
    lc_bsr_init(&g_bsr, &g_bsr_ops, &g_clock, &g_exec, &g_fwupd, have_cfg ? &saved : NULL);

    ESP_LOGI(TAG, "bs-radio up: configured=%d band=%d role=%d radio_err=%d", g_bsr.configured, band,
             g_bsr.config.role, err);

    app_link_start();
    app_oled_start();
    app_exec_start(g_bsr.configured && g_bsr.config.role == LC_ROLE_BS_RADIO_BENCH);
}
