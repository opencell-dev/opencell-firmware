#include <string.h>

#include "app.h"
#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "oc_rxt.h"
#include "w12_board.h"

#define LINK_UART UART_NUM_1

/* The host link runs on the GNSS-header UART (the Pi) and, for bench work
 * without a USB-UART adapter, on the USB-Serial-JTAG port too. Replies go to
 * the port the last valid host message came from. */
enum { PORT_UART = 0, PORT_USB = 1, PORT_COUNT };

static oc_framer_t s_framer;     /* UART; ~4.8 KB each: static, not on a task stack */
static oc_framer_t s_framer_usb; /* USB-Serial-JTAG */
static volatile int s_reply_port = PORT_UART;
static volatile int s_uart_host_seen; /* a host has spoken on the header UART */
static uint8_t s_tx_buf[OC_FRAMER_RAW_CAP + 2];
static SemaphoreHandle_t s_tx_mutex;
static temperature_sensor_handle_t s_tsens;
static volatile int64_t s_host_last_us; /* last valid host message; 0 = never */

static void send_on(const oc_msg_t *msg, int uart, int usb)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    size_t n = oc_link_write_frame(msg, s_tx_buf, sizeof(s_tx_buf));
    if (n > 0 && usb) {
        usb_serial_jtag_write_bytes(s_tx_buf, n, pdMS_TO_TICKS(20));
    }
    if (n > 0 && uart) {
        uart_write_bytes(LINK_UART, s_tx_buf, n);
    }
    xSemaphoreGive(s_tx_mutex);
}

void app_link_send(const oc_msg_t *msg)
{
    send_on(msg, s_reply_port == PORT_UART, s_reply_port == PORT_USB);
}

/* RX reports leave the exec task through a queue: encoding and writing one
 * took ~150 us there (and could wait on the port mutex), inside the ~1 ms
 * between a full packet's RX done and a back-to-back slot (bench 2026-09-30). */
typedef struct {
    uint8_t        seq;
    oc_rx_report_t rep;
    uint8_t        data[sizeof(((oc_radio_event_t *)0)->data)];
} rx_item_t;

#define RX_QUEUE_LEN 16
static QueueHandle_t s_rx_q;
static volatile uint32_t s_rx_drops; /* written by the exec task only */

/* oc_exec sink: runs in the exec task with the app lock held. */
void app_link_on_rx(void *ctx, uint32_t frame, uint8_t slot, const oc_radio_event_t *ev)
{
    (void)ctx;
    static oc_msg_t report; /* exec task only */
    static rx_item_t item;
    oc_bsr_make_rx_report(&g_bsr, frame, slot, ev, &report); /* seq under the lock */
    item.seq = report.seq;
    item.rep = report.u.rx_report;
    memcpy(item.data, ev->data, ev->len);
    if (xQueueSend(s_rx_q, &item, 0) != pdTRUE) {
        s_rx_drops++; /* full (host not reading): counted, shown with the link's errors */
    }
    OC_RXT_MARK(OC_RXT_SINK);
}

static void report_task(void *arg)
{
    (void)arg;
    static rx_item_t item;
    static oc_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_rx_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        msg.type = OC_MSG_RX_REPORT;
        msg.seq = item.seq;
        msg.u.rx_report = item.rep;
        msg.u.rx_report.payload = item.data;
        app_link_send(&msg);
    }
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
    static oc_msg_t in, out;
    int marked_valid = 0;
    int64_t next_status = esp_timer_get_time() + APP_STATUS_PERIOD_US;

    for (;;) {
        for (int port = 0; port < PORT_COUNT; port++) {
        int n = port == PORT_UART ? uart_read_bytes(LINK_UART, rx, sizeof(rx), pdMS_TO_TICKS(5))
                                  : usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(5));
        oc_framer_t *fr = port == PORT_UART ? &s_framer : &s_framer_usb;
        for (int i = 0; i < n; i++) {
            if (!oc_framer_push(fr, rx[i], &in)) {
                continue;
            }
            if (port == PORT_USB && s_reply_port != PORT_USB) {
                /* Log text shares the USB port with link frames: keep it quiet
                 * (the framers skip it, but it costs frames when it collides). */
                esp_log_level_set("*", ESP_LOG_ERROR);
            }
            s_reply_port = port;
            if (port == PORT_UART) {
                s_uart_host_seen = 1;
            }
            s_host_last_us = esp_timer_get_time();
            app_lock();
#if OC_RXT_TRACE
            int64_t held = esp_timer_get_time();
#endif
            oc_config_t before = g_bsr.config;
            int was_configured = g_bsr.configured;
            oc_bsr_handle(&g_bsr, &in, (uint64_t)esp_timer_get_time(), &out);
            int restart = g_bsr.reboot_pending ||
                          (was_configured && g_bsr.configured &&
                           (before.band != g_bsr.config.band || before.role != g_bsr.config.role)) ||
                          (!was_configured && g_bsr.configured);
#if OC_RXT_TRACE
            OC_RXT_HOLD((int32_t)(esp_timer_get_time() - held));
#endif
            app_unlock();
            app_link_send(&out);
            if (!marked_valid && g_radio_err == 0) {
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
        }
        int64_t now = esp_timer_get_time();
#if OC_RXT_TRACE
        static int64_t next_rxt;
        if (now >= next_rxt && now - s_host_last_us > 2000000) {
            static char txt[3072];
            next_rxt = now + 1000000;
            oc_rxt_rx_drops(s_rx_drops);
            int n = oc_rxt_format(txt, sizeof(txt));
            xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
            usb_serial_jtag_write_bytes(txt, n, pdMS_TO_TICKS(20));
            xSemaphoreGive(s_tx_mutex);
        }
#endif
        if (now >= next_status) {
            next_status += APP_STATUS_PERIOD_US;
            /* The sensor read takes a while: not under the lock the exec task needs. */
            int8_t temp = read_temp();
            app_lock();
#if OC_RXT_TRACE
            int64_t held = esp_timer_get_time();
#endif
            uint32_t crc_errs = s_framer.crc_errors + s_framer.cobs_errors + s_framer.malformed +
                                s_framer_usb.crc_errors + s_framer_usb.cobs_errors + s_framer_usb.malformed;
            oc_bsr_make_status(&g_bsr, (uint64_t)now, (uint32_t)(now / 1000), temp,
                               (uint16_t)(crc_errs > 0xFFFF ? 0xFFFF : crc_errs), &out);
#if OC_RXT_TRACE
            OC_RXT_HOLD((int32_t)(esp_timer_get_time() - held));
#endif
            app_unlock();
            /* Heartbeat on USB always (a listening bench host finds the board);
             * on the header UART only once a host has spoken there - with a
             * GNSS module fitted, that UART leads into the receiver. */
            send_on(&out, s_uart_host_seen, 1);
        }
    }
}

void app_link_health(int64_t now_us, uint8_t *host_ok, uint32_t *uart_errors, uint32_t *rx_drops)
{
    *rx_drops = s_rx_drops;
    int64_t last = s_host_last_us;
    *host_ok = (uint8_t)(last != 0 && now_us - last < APP_HOST_SEEN_US);
    /* Read without the lock: a torn read only affects one screen refresh. */
    *uart_errors = s_framer.crc_errors + s_framer.cobs_errors + s_framer.malformed + s_framer_usb.crc_errors +
                   s_framer_usb.cobs_errors + s_framer_usb.malformed;
}

void app_link_start(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    oc_framer_init(&s_framer);
    oc_framer_init(&s_framer_usb);
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 8192;
    ucfg.tx_buffer_size = 8192;
    usb_serial_jtag_driver_install(&ucfg);

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

    s_rx_q = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_item_t));
    xTaskCreatePinnedToCore(report_task, "oc_rxrep", 4096, NULL, 11, NULL, 0);
    xTaskCreatePinnedToCore(link_task, "oc_link", 8192, NULL, 10, NULL, 0);
}
