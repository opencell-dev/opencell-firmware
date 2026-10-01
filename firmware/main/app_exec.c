#include "app.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "oc_rxt.h"
#include "w12_board.h"

#define SPIN_US 1500 /* busy-wait the last stretch before each slot for µs accuracy */

static const char *TAG = "oc_exec";

static QueueHandle_t s_pps_q;
static TaskHandle_t s_exec_task;
static esp_timer_handle_t s_wake_timer;
static esp_timer_handle_t s_bench_pps;

static void IRAM_ATTR pps_isr(void *arg)
{
    (void)arg;
    int64_t t = esp_timer_get_time();
    BaseType_t woke = pdFALSE;
    xQueueSendFromISR(s_pps_q, &t, &woke);
    portYIELD_FROM_ISR(woke);
}

/* Bench role without GPS: a 1 Hz software PPS from the local crystal. */
static void bench_pps_cb(void *arg)
{
    (void)arg;
    int64_t t = esp_timer_get_time();
    xQueueSend(s_pps_q, &t, 0);
}

static void wake_cb(void *arg)
{
    (void)arg;
    xTaskNotifyGive(s_exec_task);
}

static void exec_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL); /* a stuck radio must not hang this task silently */
    app_lock();
    for (;;) {
        int64_t edge;
        esp_task_wdt_reset();
        while (xQueueReceive(s_pps_q, &edge, 0) == pdTRUE) {
            oc_clock_on_pps(&g_clock, (uint64_t)edge);
            static unsigned edges;
            if (++edges % 10 == 0 && g_clock.period_us != 0) {
                /* Bench: crystal error vs GPS, used to set APP_HOLDOVER_US. */
                ESP_LOGI(TAG, "pps period %u us (%+d ppm) state %u", (unsigned)g_clock.period_us,
                         (int)g_clock.period_us - 1000000, g_clock.state);
            }
        }
        uint64_t now = (uint64_t)esp_timer_get_time();
        oc_bsr_tick(&g_bsr, now);
#if OC_RXT_TRACE
        uint32_t late0 = g_exec.late_slots;
#endif
        uint64_t next = oc_exec_step(&g_exec, &g_clock, now);
#if OC_RXT_TRACE
        if (g_exec.late_slots != late0) {
            OC_RXT_END_LATE();
        }
        OC_RXT_MARK(OC_RXT_STEP);
#endif
        int64_t wait = (int64_t)next - esp_timer_get_time();
        /* Staged and waiting to launch within the spin: keep the lock. Given
         * away here, a link task parsing a SCHEDULE held it past the launch
         * (bench 2026-09-30: 227 us, a slot skipped as late). While polling
         * or before a long wait, let the link task in. */
        int hold = wait <= SPIN_US && oc_exec_staged(&g_exec);
        if (!hold) {
            app_unlock();
            if (wait > SPIN_US) {
                esp_timer_start_once(s_wake_timer, (uint64_t)(wait - SPIN_US));
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            /* Take the lock before the final spin: a link task parsing a
             * SCHEDULE could otherwise delay the launch after `next`. */
#if OC_RXT_TRACE
            int holder = oc_rxt_holder_now();
            int64_t w0 = esp_timer_get_time();
            app_lock();
            oc_rxt_exec_wait_kind((int32_t)(esp_timer_get_time() - w0), g_exec.phase == OC_EXEC_PH_ACTIVE, holder);
#else
            app_lock();
#endif
        }
        OC_RXT_MARK(OC_RXT_LOCK);
#if OC_RXT_TRACE
        /* a gap between two reads: core 1 was taken away from this loop */
        for (int64_t prev = esp_timer_get_time(), t; (t = esp_timer_get_time()) < (int64_t)next; prev = t) {
            if (t - prev > 5) {
                oc_rxt_gap((int32_t)(t - prev));
            }
        }
#else
        while (esp_timer_get_time() < (int64_t)next) {
        }
#endif
    }
}

void app_exec_start(int internal_pps)
{
    s_pps_q = xQueueCreate(8, sizeof(int64_t));

    const esp_timer_create_args_t wake = { .callback = wake_cb, .name = "oc_wake" };
    esp_timer_create(&wake, &s_wake_timer);

    if (internal_pps) {
        const esp_timer_create_args_t pps = { .callback = bench_pps_cb, .name = "oc_bench_pps" };
        esp_timer_create(&pps, &s_bench_pps);
        esp_timer_start_periodic(s_bench_pps, 1000000);
    } else {
        const gpio_config_t in = {
            .pin_bit_mask = 1ULL << W12_PIN_HDR_PPS,
            .mode = GPIO_MODE_INPUT,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_POSEDGE,
        };
        gpio_config(&in);
        /* Installed from here (core 0), so GPIO ISRs run on core 0. Moving them
         * to core 1 with the exec task made no measurable difference (bench
         * 2026-09-26: terminal UL timing SD 3.9-4.3 us either way). */
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        gpio_isr_handler_add(W12_PIN_HDR_PPS, pps_isr, NULL);
    }

    xTaskCreatePinnedToCore(exec_task, "oc_exec", 6144, NULL, configMAX_PRIORITIES - 2, &s_exec_task, 1);
}
