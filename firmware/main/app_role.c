#include "app_role.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "w12_board.h"

#define NVS_NS  "oc"
#define NVS_KEY "term"

static const char *TAG = "oc_role";

app_role_t app_role_get(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &v);
        nvs_close(h);
    }
    return app_role_from_nvs(v);
}

int app_role_is_terminal(void) { return app_role_get() == APP_ROLE_TERMINAL; }

void app_role_set(app_role_t r)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, (uint8_t)r);
        nvs_commit(h);
        nvs_close(h);
    }
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
            app_role_t next = app_role_after_hold(app_role_get());
            static const char *const NAME[] = { "bs-radio", "terminal", "cell" };
            ESP_LOGW(TAG, "BOOT held: switching role to %s", NAME[next]);
            app_role_set(next);
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
    xTaskCreatePinnedToCore(watch_task, "oc_role", 3072, NULL, 2, NULL, 0); /* core 1 is the radio's */
}
