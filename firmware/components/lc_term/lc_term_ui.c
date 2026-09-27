#include "lc_term_ui.h"

#include <string.h>

#include "lc_term_screen.h"

void lc_term_ui_init(lc_term_ui_t *u, uint64_t enable_at_us)
{
    memset(u, 0, sizeof(*u));
    u->enable_at = enable_at_us;
    u->screen = LC_SCREEN_STATUS;
}

lc_term_ui_action_t lc_term_ui_button(lc_term_ui_t *u, int pressed, uint64_t now_us)
{
    if (pressed && !u->down) { /* press */
        u->down = 1;
        u->down_since = now_us;
        u->hold_fired = 0;
        u->ignore = now_us < u->enable_at;
        u->press_screen = u->screen;
        return LC_UI_NONE;
    }
    if (!pressed && u->down) { /* release */
        u->down = 0;
        if (!u->ignore && !u->hold_fired && now_us - u->down_since < LC_UI_SHORT_US) {
            u->screen = (uint8_t)((u->screen + 1u) % LC_SCREEN_COUNT);
            u->auto_jumped = 0; /* the user took over: no automatic return */
            u->return_at = 0;
        }
        return LC_UI_NONE;
    }
    if (pressed && !u->ignore && !u->hold_fired && u->screen == LC_SCREEN_PAIRING &&
        u->press_screen == LC_SCREEN_PAIRING && now_us - u->down_since >= LC_UI_CLEAR_US) {
        u->hold_fired = 1;
        u->cleared_until = now_us + LC_UI_CLEARED_US;
        return LC_UI_CLEAR_BONDS;
    }
    return LC_UI_NONE;
}

void lc_term_ui_pairing_started(lc_term_ui_t *u)
{
    u->return_at = 0; /* a new attempt: stay until it ends */
    if (u->screen != LC_SCREEN_PAIRING) {
        u->return_to = u->screen;
        u->screen = LC_SCREEN_PAIRING;
        u->auto_jumped = 1;
    }
}

void lc_term_ui_pairing_ended(lc_term_ui_t *u, uint64_t now_us)
{
    if (u->auto_jumped) {
        u->return_at = now_us + LC_UI_RETURN_US;
    }
}

uint8_t lc_term_ui_screen(lc_term_ui_t *u, uint64_t now_us)
{
    if (u->auto_jumped && u->return_at != 0 && now_us >= u->return_at) {
        u->screen = u->return_to;
        u->auto_jumped = 0;
        u->return_at = 0;
    }
    return u->screen;
}

int lc_term_ui_cleared(const lc_term_ui_t *u, uint64_t now_us)
{
    return now_us < u->cleared_until;
}
