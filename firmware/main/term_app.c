/* Terminal role main loop. lc_term runs in one task pinned to core 1; the
 * LR2021 IRQ line (DIO8 on GPIO14) is timestamped in an ISR so beacon and DL
 * timing observations are accurate to a few µs, not to the poll interval. */
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lc_radio.h"
#include "term.h"
#include "w12_board.h"

#define SPIN_US 1500 /* busy-wait the last stretch before each op for µs accuracy */

static const char *TAG = "lc_term";

lc_term_t g_term;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static esp_timer_handle_t s_wake;
static volatile int64_t s_irq_us;

void term_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void term_unlock(void) { xSemaphoreGive(s_lock); }

static void IRAM_ATTR irq_isr(void *arg)
{
    (void)arg;
    s_irq_us = esp_timer_get_time();
    BaseType_t woke = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woke);
    portYIELD_FROM_ISR(woke);
}

static void wake_cb(void *arg)
{
    (void)arg;
    xTaskNotifyGive(s_task);
}

static void on_downlink(void *ctx, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    term_ble_downlink(data, len);
}

static void on_status(void *ctx)
{
    (void)ctx;
    term_ble_status_changed();
}

static uint32_t rand32(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static void term_task(void *arg)
{
    (void)arg;
    for (;;) {
        term_lock();
        int64_t irq = s_irq_us;
        if (irq != 0) {
            s_irq_us = 0;
            lc_term_note_irq(&g_term, (uint64_t)irq);
        }
        uint64_t next = lc_term_step(&g_term, (uint64_t)esp_timer_get_time());
        term_unlock();

        int64_t wait = (int64_t)next - esp_timer_get_time();
        if (wait > SPIN_US) {
            esp_timer_start_once(s_wake, (uint64_t)(wait - SPIN_US));
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* timer or radio IRQ */
            esp_timer_stop(s_wake);
        }
        while (esp_timer_get_time() < (int64_t)next && s_irq_us == 0) {
        }
    }
}

void term_app_main(void)
{
    s_lock = xSemaphoreCreateMutex();

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint32_t tmid = lc_term_tmid_from_mac(mac);

    int err = lc_radio_init_terminal();
    if (err != 0) {
        ESP_LOGE(TAG, "radio init failed: %d", err);
    }
    const lc_term_sink_t sink = { NULL, on_downlink, on_status, rand32 };
    lc_term_init(&g_term, lc_radio_ops(), &sink, tmid);
    ESP_LOGI(TAG, "terminal up: tmid %08lx radio_err %d", (unsigned long)tmid, err);

    term_ble_start(tmid);
    term_oled_start();

    const esp_timer_create_args_t wake = { .callback = wake_cb, .name = "lc_term_wake" };
    esp_timer_create(&wake, &s_wake);
    xTaskCreatePinnedToCore(term_task, "lc_term", 6144, NULL, configMAX_PRIORITIES - 2, &s_task, 1);

    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << W12_PIN_LORA_IRQ,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&in);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM); /* may already be installed */
    gpio_isr_handler_add(W12_PIN_LORA_IRQ, irq_isr, NULL);

    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
