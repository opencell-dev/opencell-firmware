/* Terminal role main loop. oc_term and the signalling glue (oc_term_sig) run
 * in one task pinned to core 1; the LR2021 IRQ line (DIO8 on GPIO14) is
 * timestamped in an ISR so beacon and DL timing observations are accurate to
 * a few µs, not to the poll interval. */
#include <stdio.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "oc_radio.h"
#include "oc_sig_crypto.h"
#include "term.h"
#include "w12_board.h"

#define SPIN_US 1500 /* busy-wait the last stretch before each op for µs accuracy */

static const char *TAG = "oc_term";

oc_term_t g_term;
oc_term_sig_t g_sig;
int g_sig_ok;
static oc_sig_ident_t s_ident;
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

/* Called from oc_term_step (link task, lock held): signalling or app data. */
static void on_downlink(void *ctx, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    if (g_sig_ok) {
        oc_term_sig_downlink(&g_sig, data, len, (uint64_t)esp_timer_get_time());
    }
}

static void on_app_down(void *ctx, const uint8_t *d, uint8_t n)
{
    (void)ctx;
    term_ble_downlink(d, n);
}

static void on_sig_save(void *ctx, const oc_sig_ident_t *id)
{
    (void)ctx;
    term_ident_save(id);
}

static void on_sig_event(void *ctx, const uint8_t *ev, uint8_t n)
{
    (void)ctx;
    term_ble_event(ev, n);
    term_ble_status_changed(); /* STATUS byte 3 is the signalling state */
}

/* Set by on_status() (term_lock held) for log_search_status() to print once
 * the lock is released: ESP_LOGI can block on the USB-Serial-JTAG console up
 * to its TX timeout with no host attached, and must not do that while
 * term_task (and so the OLED/BLE tasks and beacon handling) holds term_lock. */
static volatile int s_search_log_pending;
static uint8_t s_search_log_heard;
static int16_t s_search_log_rssi;
static int16_t s_search_log_snr;
static int16_t s_search_log_noise;
static uint8_t s_search_log_pos, s_search_log_len, s_search_log_src, s_search_log_pass;
static uint32_t s_search_log_khz;
static uint8_t s_prev_state;               /* oc_term state at the last on_status */
static volatile int s_sync_log_pending;    /* the search just found a cell (bench timing) */
static uint32_t s_sync_log_seed, s_sync_log_khz;

static void on_status(void *ctx)
{
    (void)ctx;
    term_ble_status_changed();
    /* oc_term calls this with term_lock held. While searching it
     * fires at the start of every dwell and when the scan first hears a
     * packet: capture what the OLED shows (spec 2026-09-27 §3.1, channel-list
     * §9) here; logged from log_search_status() after term_unlock(). */
    oc_term_status_t st;
    oc_term_status(&g_term, &st);
    if (st.state == OC_TERM_SEARCH) {
        s_search_log_heard = st.heard;
        s_search_log_rssi = st.rssi_dbm;
        s_search_log_snr = st.snr_qdb;
        s_search_log_noise = st.noise_dbm;
        s_search_log_pos = st.scan_pos;
        s_search_log_len = st.scan_len;
        s_search_log_src = st.scan_src;
        s_search_log_pass = st.scan_pass;
        s_search_log_khz = st.freq_khz;
        s_search_log_pending = 1;
    } else if (s_prev_state == OC_TERM_SEARCH) {
        s_sync_log_seed = st.cell_seed;
        s_sync_log_khz = st.freq_khz;
        s_sync_log_pending = 1;
    }
    s_prev_state = st.state;
}

/* Logs the SEARCH status on_status() last captured, if any (see above).
 * Called from term_task after term_unlock(), never while term_lock is held. */
static void log_search_status(void)
{
    if (s_sync_log_pending) {
        s_sync_log_pending = 0;
        ESP_LOGI(TAG, "synced: cell %08lx, anchor %lu.%02lu MHz", (unsigned long)s_sync_log_seed,
                 (unsigned long)(s_sync_log_khz / 1000u), (unsigned long)(s_sync_log_khz % 1000u / 10u));
    }
    if (!s_search_log_pending) {
        return;
    }
    s_search_log_pending = 0;
    static const char src[] = "?LUNKDS"; /* last user network learned default sweep */
    char noise[16] = "-";
    if (s_search_log_noise != OC_TERM_NO_DBM) {
        snprintf(noise, sizeof(noise), "%d dBm", s_search_log_noise);
    }
    char where[40];
    snprintf(where, sizeof(where), "%u/%u %c %lu.%02lu MHz pass %u", s_search_log_pos, s_search_log_len,
             s_search_log_src < sizeof(src) - 1u ? src[s_search_log_src] : '?',
             (unsigned long)(s_search_log_khz / 1000u), (unsigned long)(s_search_log_khz % 1000u / 10u),
             s_search_log_pass);
    if (s_search_log_heard) {
        ESP_LOGI(TAG, "search %s: signal %d dBm SNR %d dB; noise %s", where, s_search_log_rssi,
                 s_search_log_snr / 4, noise);
    } else {
        ESP_LOGI(TAG, "search %s: no signal; noise %s", where, noise);
    }
}

/* term_lock held by every caller (term_task, term_ble.c's command()). */
static uint8_t s_sig_last_state;

void term_sig_state_check(void)
{
    if (!g_sig_ok) {
        return;
    }
    uint8_t st = oc_sig_term_state(&g_sig.sig);
    if (st != s_sig_last_state) {
        s_sig_last_state = st;
        term_ble_status_changed();
    }
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
            oc_term_note_irq(&g_term, (uint64_t)irq);
        }
        uint64_t now = (uint64_t)esp_timer_get_time();
        uint64_t next = oc_term_step(&g_term, now);
        term_sig_state_check(); /* oc_term_step may deliver a downlink to oc_sig */
        if (g_sig_ok) {
            uint64_t sig_next = oc_term_sig_step(&g_sig, now);
            if (sig_next < next) {
                next = sig_next;
            }
            term_sig_state_check();
        }
        if (g_term.scan.dirty) { /* a new serving cell, CHAN_LIST, mode, DEACTIVATE */
            term_scan_save(&g_term.scan);
        }
        term_unlock();
        log_search_status(); /* outside term_lock: see on_status()/log_search_status() */

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
    uint32_t tmid = oc_term_tmid_from_mac(mac);

    int err = oc_radio_init_terminal();
    if (err != 0) {
        ESP_LOGE(TAG, "radio init failed: %d", err);
    }
    const oc_term_sink_t sink = { NULL, on_downlink, on_status, rand32 };
    oc_term_init(&g_term, oc_radio_ops(), &sink, tmid);
    term_scan_load(&g_term.scan); /* before oc_term_sig_init, which takes its net_ver */
    ESP_LOGI(TAG, "terminal up: tmid %08lx radio_err %d", (unsigned long)tmid, err);

    int64_t t0 = esp_timer_get_time();
    int st = oc_sig_selftest(); /* ~0.3 s: X25519 dominates */
    if (st != 0) {
        g_sig_ok = 0;
        ESP_LOGE(TAG, "crypto self-test FAILED (%d): signalling disabled", st);
    } else if (term_ident_load(&s_ident) != 0) {
        g_sig_ok = 0;
        ESP_LOGE(TAG, "identity unreadable: signalling disabled");
    } else {
        g_sig_ok = 1;
        const oc_sig_term_io_t io = { NULL, NULL, NULL, on_sig_save, on_sig_event };
        oc_term_sig_init(&g_sig, &g_term, &io, &s_ident, tmid, (uint64_t)esp_timer_get_time());
        g_sig.app_down = on_app_down;
        ESP_LOGI(TAG, "crypto self-test passed in %lld ms; signalling state %u",
                 (esp_timer_get_time() - t0) / 1000, oc_sig_term_state(&g_sig.sig));
    }

    term_ble_start(tmid);
    term_oled_start();

    const esp_timer_create_args_t wake = { .callback = wake_cb, .name = "oc_term_wake" };
    esp_timer_create(&wake, &s_wake);
    xTaskCreatePinnedToCore(term_task, "oc_term", 10240, NULL, configMAX_PRIORITIES - 2, &s_task, 1);

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
