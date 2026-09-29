#include "app_role.h"

#include "app_nvs.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "w12_board.h"

#define NVS_KEY "term"

static const char *TAG = "oc_role";

/* Read once, at the first call (app_role_start_button_watch, before main
 * asks): the role, the namespace it came from, and whether it could be read
 * at all. An error other than NOT_FOUND is UNKNOWN (oc_nvs_mig.h, lc/oc):
 * boot the default, bs-radio, and write nothing of the pair this boot. */
static int s_read;
static int s_term;
static int s_unknown;
static const char *s_src;

static void read_role(void)
{
    if (s_read) {
        return;
    }
    s_read = 1;
    uint8_t v = 0;
    const char *src = NULL;
    esp_err_t err = app_nvs_get_u8(APP_NS_MAIN, NVS_KEY, &v, &src);
    if (err == ESP_OK) {
        s_src = src;
        s_term = v == 1;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        s_unknown = 1;
        ESP_LOGE(TAG, "role unreadable (%s: %s): booting as bs-radio, PRG toggle and saves off this boot", src,
                 esp_err_to_name(err));
    }
}

int app_role_is_terminal(void)
{
    read_role();
    return s_term;
}

const char *app_role_source(void)
{
    read_role();
    return s_src;
}

int app_role_can_save(void)
{
    read_role();
    return !s_unknown && app_nvs_writable(APP_NS_MAIN);
}

static int set_terminal(int on)
{
    nvs_handle_t h;
    if (!app_role_can_save() || nvs_open(app_nvs_ns(APP_NS_MAIN), NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_u8(h, NVS_KEY, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

static void watch_task(void *arg)
{
    (void)arg;
    int64_t held_since = 0;
    while (esp_timer_get_time() < (int64_t)APP_ROLE_WINDOW_MS * 1000 || held_since != 0) {
        int down = gpio_get_level(W12_PIN_BOOT_BTN) == 0;
        int64_t now = esp_timer_get_time();
        if (!down) {
            held_since = 0;
        } else if (held_since == 0) {
            held_since = now;
        } else if (now - held_since >= (int64_t)APP_ROLE_HOLD_MS * 1000) {
            if (!app_role_can_save()) { /* FAILED, UNSURE or UNKNOWN: the flag is not written this boot */
                ESP_LOGE(TAG, "BOOT held: role switch off this boot (%s); try again after a reboot",
                         s_unknown ? "role unreadable" : "NVS oc not migrated");
                break;
            }
            int term = !app_role_is_terminal();
            ESP_LOGW(TAG, "BOOT held: switching role to %s", term ? "terminal" : "bs-radio");
            if (set_terminal(term) != 0) {
                ESP_LOGE(TAG, "role not saved");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}

void app_role_start_button_watch(void)
{
    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << W12_PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
    read_role(); /* here, before the task and before main asks: one read, one log line */
    xTaskCreatePinnedToCore(watch_task, "oc_role", 3072, NULL, 2, NULL, 0); /* core 1 is the radio's */
}
