/* The terminal's OLED status screen as text lines (drawn with lc_oled_render_lines:
 * line 0 on the yellow rows, the rest from page 2). Portable: no ESP-IDF. */
#ifndef LC_TERM_SCREEN_H
#define LC_TERM_SCREEN_H

#include "lc_term.h"

#define LC_TERM_SCREEN_LINES 5
#define LC_TERM_SCREEN_COLS  21

void lc_term_status_lines(const lc_term_status_t *st, char lines[LC_TERM_SCREEN_LINES][LC_TERM_SCREEN_COLS + 1]);

#endif
