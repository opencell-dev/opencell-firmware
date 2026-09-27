/* lc_term_ui — which OLED screen the terminal shows, driven by the PRG
 * (BOOT, GPIO0) button and by BLE pairing (spec 2026-09-27-ble-pairing-design.md §3).
 *
 *   short press (< 1 s)       next screen, wrapping; cancels an automatic return
 *   hold 5 s on Pairing        clear all bonds (once per hold; OLED confirms);
 *                              only a press that began on Pairing counts
 *   pairing started            jump to Pairing (unless already there)
 *   pairing ended              back to the previous screen 10 s later
 *
 * Presses that begin before enable_at_us (the role-switch window after boot,
 * app_role.h) are ignored until released. Time and the button level are
 * injected: the firmware polls every ~20 ms. Portable: no ESP-IDF. */
#ifndef LC_TERM_UI_H
#define LC_TERM_UI_H

#include <stdint.h>

#define LC_UI_SHORT_US     1000000ull  /* shorter than this: a short press */
#define LC_UI_CLEAR_US     5000000ull  /* hold on Pairing this long: clear bonds */
#define LC_UI_RETURN_US    10000000ull /* auto return after pairing ends */
#define LC_UI_CLEARED_US   3000000ull  /* "BONDS CLEARED" stays up this long */

typedef enum { LC_UI_NONE = 0, LC_UI_CLEAR_BONDS } lc_term_ui_action_t;

typedef struct {
    uint64_t enable_at;
    uint8_t  screen;
    uint8_t  return_to;    /* screen before an automatic jump to Pairing */
    uint8_t  auto_jumped;  /* on Pairing because pairing started */
    uint64_t return_at;    /* 0: no return scheduled */
    uint8_t  down;         /* button level at the last poll */
    uint8_t  ignore;       /* the current press began inside the role-switch window */
    uint8_t  hold_fired;   /* this hold already cleared the bonds */
    uint8_t  press_screen; /* screen shown when the current press began */
    uint64_t down_since;
    uint64_t cleared_until;
} lc_term_ui_t;

void    lc_term_ui_init(lc_term_ui_t *u, uint64_t enable_at_us); /* starts on Status */
/* Feed the button level (1 = pressed) at every poll. */
lc_term_ui_action_t lc_term_ui_button(lc_term_ui_t *u, int pressed, uint64_t now_us);
void    lc_term_ui_pairing_started(lc_term_ui_t *u);
void    lc_term_ui_pairing_ended(lc_term_ui_t *u, uint64_t now_us);
/* The screen to draw now (applies a due automatic return). */
uint8_t lc_term_ui_screen(lc_term_ui_t *u, uint64_t now_us);
/* 1 while the "bonds cleared" confirmation should show. */
int     lc_term_ui_cleared(const lc_term_ui_t *u, uint64_t now_us);

#endif
