#include "unity.h"

#include <string.h>

#include "lc_oled.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t fb[LC_OLED_FB];

static void test_glyph_lands_in_page_format(void)
{
    lc_oled_clear(fb);
    lc_oled_text(fb, 2, 1, "A");
    static const uint8_t a[6] = { 0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, fb + 2 * LC_OLED_W + 6, 6);
    TEST_ASSERT_EQUAL_HEX8(0, fb[2 * LC_OLED_W]); /* column 0 untouched */
}

static void test_lower_case_and_unknown_characters(void)
{
    uint8_t fb2[LC_OLED_FB];
    lc_oled_clear(fb);
    lc_oled_clear(fb2);
    lc_oled_text(fb, 0, 0, "cell");
    lc_oled_text(fb2, 0, 0, "CELL");
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fb2, fb, LC_OLED_FB);
    lc_oled_text(fb, 1, 0, "~");
    lc_oled_text(fb2, 1, 0, "?");
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fb2, fb, LC_OLED_FB);
}

static void test_text_clips_at_edges(void)
{
    lc_oled_clear(fb);
    lc_oled_text(fb, 0, 18, "ABCDEF");             /* only columns 18..20 */
    lc_oled_text(fb, 8, 0, "X");                   /* page 8 doesn't exist */
    for (size_t i = LC_OLED_W; i < LC_OLED_FB; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, fb[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x7E, fb[18 * 6]);      /* 'A' */
    TEST_ASSERT_EQUAL_HEX8(0x3E, fb[20 * 6]);      /* 'C' */
}

static void test_render_lines_title_yellow_rest_from_page_2(void)
{
    const char lines[3][LC_OLED_COLS + 1] = { "T", "A", "B" };
    uint8_t expect[LC_OLED_FB];
    lc_oled_clear(expect);
    lc_oled_text(expect, 0, 0, "T");
    lc_oled_text(expect, 2, 0, "A");
    lc_oled_text(expect, 3, 0, "B");
    memset(fb, 0xAA, sizeof(fb)); /* render clears first */
    lc_oled_render_lines(fb, lines, 3);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, fb, LC_OLED_FB);
}

static void test_render_lines_ignores_lines_past_the_screen(void)
{
    char lines[9][LC_OLED_COLS + 1];
    for (int i = 0; i < 9; i++) {
        strcpy(lines[i], "X");
    }
    lc_oled_render_lines(fb, (const char (*)[LC_OLED_COLS + 1])lines, 9); /* 7 fit: pages 0, 2..7 */
    TEST_ASSERT_EQUAL_HEX8(0, fb[1 * LC_OLED_W]);         /* page 1 stays blank */
    TEST_ASSERT_EQUAL_HEX8(0x63, fb[7 * LC_OLED_W]);      /* 'X' on the last page */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_glyph_lands_in_page_format);
    RUN_TEST(test_lower_case_and_unknown_characters);
    RUN_TEST(test_text_clips_at_edges);
    RUN_TEST(test_render_lines_title_yellow_rest_from_page_2);
    RUN_TEST(test_render_lines_ignores_lines_past_the_screen);
    return UNITY_END();
}
