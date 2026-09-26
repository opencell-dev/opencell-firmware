/* Terminal status screen: lc_term_status_lines drawn with lc_oled on the
 * shared panel from app_oled.c (which handles I2C, reset and the 180-degree
 * mount). Refreshes at 2 Hz from a low-priority task. */
#include "app.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lc_oled.h"
#include "lc_term_screen.h"
#include "term.h"

static esp_lcd_panel_handle_t s_panel;
static uint8_t s_fb[LC_OLED_FB];

static void oled_task(void *arg)
{
    (void)arg;
    static char lines[LC_TERM_SCREEN_LINES][LC_TERM_SCREEN_COLS + 1];
    for (;;) {
        lc_term_status_t st;
        term_lock();
        lc_term_status(&g_term, &st);
        term_unlock();
        lc_term_status_lines(&st, lines);
        lc_oled_render_lines(s_fb, (const char (*)[LC_OLED_COLS + 1])lines, LC_TERM_SCREEN_LINES);
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LC_OLED_W, LC_OLED_PAGES * 8, s_fb);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void term_oled_start(void)
{
    s_panel = app_oled_panel();
    if (s_panel != NULL) {
        xTaskCreatePinnedToCore(oled_task, "term_oled", 3072, NULL, 1, NULL, 0); /* core 1 is the radio's */
    }
}
