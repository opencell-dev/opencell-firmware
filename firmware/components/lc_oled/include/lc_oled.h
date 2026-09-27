/* lc_oled — text on the W12's 128x64 SSD1315/SSD1306 in page format (one
 * byte = 8 vertical pixels, LSB on top; page p = rows 8p..8p+7). Rows 0-1
 * are yellow glass, the rest blue. 21 columns x 8 rows of 5x7 characters;
 * lower case prints as upper case. Portable: no ESP-IDF. */
#ifndef LC_OLED_H
#define LC_OLED_H

#include <stdint.h>

#define LC_OLED_W     128u
#define LC_OLED_PAGES 8u
#define LC_OLED_FB    (LC_OLED_W * LC_OLED_PAGES)
#define LC_OLED_COLS  21u

void lc_oled_clear(uint8_t *fb);

/* Draw s at text row `page` (0..7), column `col` (0..20); clipped. */
void lc_oled_text(uint8_t *fb, uint8_t page, uint8_t col, const char *s);

/* Draw s twice the size (10x14 glyphs in 12x16 cells) on pages page and
 * page+1, starting x pixels from the left; clipped at the right edge. For
 * the pairing code: 6 digits take 72 of the 128 pixels. */
void lc_oled_text_x2(uint8_t *fb, uint8_t page, uint8_t x, const char *s);

/* Clear fb, draw lines[0] on page 0 (yellow) and lines[1..] from page 2 down.
 * Lines that don't fit are dropped. */
void lc_oled_render_lines(uint8_t *fb, const char lines[][LC_OLED_COLS + 1], uint8_t n);

#endif
