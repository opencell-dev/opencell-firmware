/* Status screen on the W12's SSD1315 (SSD1306-compatible) over I2C. Vext
 * (GPIO45, active low) powering the panel is switched on by w12_board_init.
 * Runs on core 0 at low priority so it never competes with the exec task. */
#include "app.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lc_oled.h"

#define OLED_SDA  17
#define OLED_SCL  18
#define OLED_RST  21
#define OLED_ADDR 0x3C

static const char *TAG = "app_oled";
static esp_lcd_panel_handle_t s_panel;
static uint8_t s_fb[LC_OLED_FB];

static void oled_task(void *arg)
{
    (void)arg;
    static char lines[LC_BSR_SCREEN_LINES][LC_BSR_SCREEN_COLS + 1];
    for (;;) {
        int64_t now = esp_timer_get_time();
        lc_bsr_view_t v;
        app_lock();
        lc_bsr_view(&g_bsr, (uint64_t)now, &v);
        app_unlock();
        app_link_health(now, &v.host_ok, &v.uart_errors);
        lc_bsr_status_lines(&v, lines);
        lc_oled_render_lines(s_fb, (const char (*)[LC_OLED_COLS + 1])lines, LC_BSR_SCREEN_LINES);
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LC_OLED_W, LC_OLED_PAGES * 8, s_fb);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_oled_start(void)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = OLED_SDA,
        .scl_io_num = OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = OLED_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_ssd1306_config_t ssd = { .height = 64 };
    const esp_lcd_panel_dev_config_t dev = {
        .bits_per_pixel = 1,
        .reset_gpio_num = OLED_RST,
        .vendor_config = &ssd,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK || esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK ||
        esp_lcd_new_panel_ssd1306(io, &dev, &s_panel) != ESP_OK || esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK || esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed; continuing without a display");
        return;
    }
    xTaskCreatePinnedToCore(oled_task, "app_oled", 3072, NULL, 1, NULL, 0);
}
