/* OpenCell W12 firmware. One image, three roles (spec §5.1; single-board cell spec §3):
 *   bs-radio  a slot executor driven by the Pi over the GNSS-header UART
 *             (oc_link), timed by GPS PPS (default);
 *   terminal  oc_term + BLE bridge + OLED (term_app.c);
 *   cell      a whole cell on this W12 (app_cell_main, from the cell
 *             repository's w12_cell component; a firmware-only build has
 *             none and runs bs-radio).
 * The role is an NVS value; BOOT toggles terminal/bs-radio (app_role.h). See
 * docs/superpowers/specs/2026-09-23-lr2021-hardware-design.md. */
#include "app.h"
#include "app_role.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "oc_radio.h"
#include "term.h"
#include "w12_board.h"

static const char *TAG = "oc_main";

oc_clock_t g_clock;
oc_exec_t  g_exec;
oc_fwupd_t g_fwupd;
oc_bsr_t   g_bsr;
int        g_radio_err;

static SemaphoreHandle_t s_lock;

void app_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void app_unlock(void) { xSemaphoreGive(s_lock); }

/* A freshly flashed image stays PENDING_VERIFY until app_link marks it valid
 * (first host message with a working radio). If that never happens, restart so
 * the bootloader rolls back to the previous image. */
static void ota_verify_timeout(void *arg)
{
    (void)arg;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "new image never verified (radio_err=%d); restarting to roll back", g_radio_err);
        esp_restart();
    }
}

static void arm_ota_verify_timer(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = { .callback = ota_verify_timeout, .name = "ota_verify" };
    esp_timer_create(&args, &t);
    esp_timer_start_once(t, APP_OTA_VERIFY_US);
}

/* Role cell: the cell repository's w12_cell component defines the strong
 * one (single-board cell spec §4). Without it, the board runs bs-radio. */
__attribute__((weak)) void app_cell_main(void)
{
    ESP_LOGE(TAG, "role cell: not in this build (build it with opencell-pi's w12-cell/build.sh); running bs-radio");
}

void app_bsr_start(const oc_exec_sink_t *sink, int host_link)
{
    oc_config_t saved;
    int have_cfg = app_store_load_config(&saved) == 0;
    oc_band_t band = (have_cfg && saved.band < OC_BAND_COUNT) ? (oc_band_t)saved.band : OC_BAND_915;

    int err = oc_radio_init(band);
    g_radio_err = err;
    if (err != 0) {
        ESP_LOGE(TAG, "radio init failed: %d", err);
    }
    oc_radio_stamp_irq();

    oc_clock_init(&g_clock, APP_HOLDOVER_US);
    oc_exec_init(&g_exec, oc_radio_ops(), sink);
    oc_fwupd_init(&g_fwupd, &g_fwupd_ops);
    oc_bsr_init(&g_bsr, &g_bsr_ops, &g_clock, &g_exec, &g_fwupd, have_cfg ? &saved : NULL);

    ESP_LOGI(TAG, "bs-radio up: configured=%d band=%d role=%d radio_err=%d", g_bsr.configured, band,
             g_bsr.config.role, err);

    arm_ota_verify_timer();
    if (host_link) {
        app_link_start();
        app_oled_start();
    }
    app_exec_start(g_bsr.configured && g_bsr.config.role == OC_ROLE_BS_RADIO_BENCH);
}

void app_main(void)
{
    s_lock = xSemaphoreCreateMutex();
    w12_board_init();
    app_store_init();
    app_role_start_button_watch();
    app_role_t role = app_role_get();
    if (role == APP_ROLE_TERMINAL) {
        ESP_LOGI(TAG, "role: terminal");
        term_app_main(); /* never returns */
    }
    if (role == APP_ROLE_CELL) {
        ESP_LOGI(TAG, "role: cell");
        app_cell_main(); /* returns only when this build has no cell */
    }
    static const oc_exec_sink_t sink = { NULL, app_link_on_rx };
    app_bsr_start(&sink, 1);
}
