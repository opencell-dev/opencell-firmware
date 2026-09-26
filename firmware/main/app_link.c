#include <string.h>

#include "app.h"
#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "w12_board.h"

#define LINK_UART UART_NUM_1

static lc_framer_t s_framer; /* ~4.8 KB: static, not on a task stack */
static uint8_t s_tx_buf[LC_FRAMER_RAW_CAP + 2];
static SemaphoreHandle_t s_tx_mutex;
static temperature_sensor_handle_t s_tsens;
static volatile int64_t s_host_last_us; /* last valid host message; 0 = never */

void app_link_send(const lc_msg_t *msg)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    size_t n = lc_link_write_frame(msg, s_tx_buf, sizeof(s_tx_buf));
    if (n > 0) {
        uart_write_bytes(LINK_UART, s_tx_buf, n);
    }
    xSemaphoreGive(s_tx_mutex);
}

/* lc_exec sink: runs in the exec task with the app lock held. */
void app_link_on_rx(void *ctx, uint32_t frame, uint8_t slot, const lc_radio_event_t *ev)
{
    (void)ctx;
    static lc_msg_t report;
    lc_bsr_make_rx_report(&g_bsr, frame, slot, ev, &report);
    app_link_send(&report);
}

static int8_t read_temp(void)
{
    float c = 0;
    if (s_tsens == NULL || temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return 0;
    }
    return (int8_t)c;
}

static void link_task(void *arg)
{
    (void)arg;
    static uint8_t rx[256];
    static lc_msg_t in, out;
    int marked_valid = 0;
    int64_t next_status = esp_timer_get_time() + APP_STATUS_PERIOD_US;

    for (;;) {
        int n = uart_read_bytes(LINK_UART, rx, sizeof(rx), pdMS_TO_TICKS(10));
        for (int i = 0; i < n; i++) {
            if (!lc_framer_push(&s_framer, rx[i], &in)) {
                continue;
            }
            s_host_last_us = esp_timer_get_time();
            app_lock();
            lc_config_t before = g_bsr.config;
            int was_configured = g_bsr.configured;
            lc_bsr_handle(&g_bsr, &in, (uint64_t)esp_timer_get_time(), &out);
            int restart = g_bsr.reboot_pending ||
                          (was_configured && g_bsr.configured &&
                           (before.band != g_bsr.config.band || before.role != g_bsr.config.role)) ||
                          (!was_configured && g_bsr.configured);
            app_unlock();
            app_link_send(&out);
            if (!marked_valid) {
                /* A host is talking to us: this image works, cancel rollback. */
                esp_ota_mark_app_valid_cancel_rollback();
                marked_valid = 1;
            }
            if (restart) {
                /* New image, or a band/role change that needs a clean radio init. */
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_status) {
            next_status += APP_STATUS_PERIOD_US;
            app_lock();
            uint32_t crc_errs = s_framer.crc_errors + s_framer.cobs_errors + s_framer.malformed;
            lc_bsr_make_status(&g_bsr, (uint64_t)now, (uint32_t)(now / 1000), read_temp(),
                               (uint16_t)(crc_errs > 0xFFFF ? 0xFFFF : crc_errs), &out);
            app_unlock();
            app_link_send(&out);
        }
    }
}

void app_link_health(int64_t now_us, uint8_t *host_ok, uint32_t *uart_errors)
{
    int64_t last = s_host_last_us;
    *host_ok = (uint8_t)(last != 0 && now_us - last < APP_HOST_SEEN_US);
    /* Read without the lock: a torn read only affects one screen refresh. */
    *uart_errors = s_framer.crc_errors + s_framer.cobs_errors + s_framer.malformed;
}

void app_link_start(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    lc_framer_init(&s_framer);

    const uart_config_t cfg = {
        .baud_rate = APP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(LINK_UART, 8192, 8192, 0, NULL, 0);
    uart_param_config(LINK_UART, &cfg);
    uart_set_pin(LINK_UART, W12_PIN_HDR_TX, W12_PIN_HDR_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    const temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    }

    xTaskCreatePinnedToCore(link_task, "lc_link", 8192, NULL, 10, NULL, 0);
}
