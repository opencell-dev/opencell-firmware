/* Terminal OLED: four screens (Status, Pairing, Subscriber, Radio; spec
 * 2026-09-27-ble-pairing-design.md §3) drawn with lc_oled on the shared panel
 * from app_oled.c (which handles I2C, reset and the 180-degree mount).
 *
 * One low-priority core-0 task polls the PRG (BOOT, GPIO0) button every
 * 20 ms and feeds lc_term_ui, which picks the screen; the screen is redrawn
 * at 2 Hz and at once when it changes. GPIO0 is already an input with its
 * pull-up (app_role_start_button_watch); presses during the role-switch
 * window after boot are left to app_role. Without a panel the task still
 * runs, so the bond-clearing hold works blind. */
#include <string.h>

#include "app.h"
#include "app_role.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lc_oled.h"
#include "lc_term_screen.h"
#include "lc_term_ui.h"
#include "term.h"
#include "w12_board.h"

#define POLL_MS   20
#define REDRAW_US 500000

static esp_lcd_panel_handle_t s_panel;
static uint8_t s_fb[LC_OLED_FB];
static volatile uint32_t s_pair_starts, s_pair_ends; /* counted by the NimBLE host task */

void term_oled_pairing_started(void)
{
    s_pair_starts++;
}

void term_oled_pairing_ended(void)
{
    s_pair_ends++;
}

/* Everything the screens show, read under the link lock in one go. */
static void gather(lc_term_view_t *v, uint64_t now, int cleared)
{
    term_lock();
    lc_term_status(&g_term, &v->link);
    v->beacons = g_term.beacons;
    v->sync_losses = g_term.sync_losses;
    v->sub.sig_ok = (uint8_t)g_sig_ok;
    if (g_sig_ok) {
        v->sub.state = lc_sig_term_state(&g_sig.sig);
        v->sub.mode = g_sig.sig.reg_mode;
        v->sub.activated = (uint8_t)g_sig.sig.id->activated;
        memcpy(v->sub.number, g_sig.sig.id->number, sizeof(v->sub.number));
    }
    term_unlock();
    term_ble_pair_view(&v->pair, now);
    v->pair.cleared = (uint8_t)cleared;
}

static void draw(uint8_t screen, const lc_term_view_t *v)
{
    static lc_term_lines_t lines;
    if (s_panel == NULL) {
        return;
    }
    lc_term_screen_lines(screen, v, lines);
    if (screen == LC_SCREEN_PAIRING) {
        /* title on the yellow rows, the code in 12x16 digits on pages 2-3,
         * the rest from page 5 */
        lc_oled_clear(s_fb);
        lc_oled_text(s_fb, 0, 0, lines[0]);
        size_t w = strlen(lines[1]) * 12u;
        lc_oled_text_x2(s_fb, 2, (uint8_t)(w < LC_OLED_W ? (LC_OLED_W - w) / 2u : 0u), lines[1]);
        for (int i = 2; i < LC_TERM_SCREEN_LINES && 3 + i < (int)LC_OLED_PAGES; i++) {
            lc_oled_text(s_fb, (uint8_t)(3 + i), 0, lines[i]);
        }
    } else {
        lc_oled_render_lines(s_fb, (const char (*)[LC_OLED_COLS + 1])lines, LC_TERM_SCREEN_LINES);
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LC_OLED_W, LC_OLED_PAGES * 8, s_fb);
}

static void oled_task(void *arg)
{
    (void)arg;
    static lc_term_ui_t ui;
    static lc_term_view_t view;
    lc_term_ui_init(&ui, (uint64_t)APP_ROLE_WINDOW_MS * 1000u);
    uint32_t seen_starts = 0, seen_ends = 0;
    uint64_t next_draw = 0;
    uint8_t shown = 0xFF;
    for (;;) {
        uint64_t now = (uint64_t)esp_timer_get_time();
        /* starts before ends: a start and its end seen in one poll stay in order */
        uint32_t starts = s_pair_starts, ends = s_pair_ends;
        for (; seen_starts != starts; seen_starts++) {
            lc_term_ui_pairing_started(&ui);
        }
        for (; seen_ends != ends; seen_ends++) {
            lc_term_ui_pairing_ended(&ui, now);
        }
        if (lc_term_ui_button(&ui, gpio_get_level(W12_PIN_BOOT_BTN) == 0, now) == LC_UI_CLEAR_BONDS) {
            term_ble_clear_bonds();
        }
        uint8_t screen = lc_term_ui_screen(&ui, now);
        if (screen != shown || now >= next_draw) {
            memset(&view, 0, sizeof(view));
            gather(&view, now, lc_term_ui_cleared(&ui, now));
            draw(screen, &view);
            shown = screen;
            next_draw = now + REDRAW_US;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void term_oled_start(void)
{
    s_panel = app_oled_panel(); /* NULL without a panel: the button still works */
    xTaskCreatePinnedToCore(oled_task, "term_oled", 4096, NULL, 1, NULL, 0); /* core 1 is the radio's */
}
