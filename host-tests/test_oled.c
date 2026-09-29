#include "unity.h"

#include <string.h>

#include "oc_oled.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t fb[OC_OLED_FB];

static void test_glyph_lands_in_page_format(void)
{
    oc_oled_clear(fb);
    oc_oled_text(fb, 2, 1, "A");
    static const uint8_t a[6] = { 0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, fb + 2 * OC_OLED_W + 6, 6);
    TEST_ASSERT_EQUAL_HEX8(0, fb[2 * OC_OLED_W]); /* column 0 untouched */
}

static void test_lower_case_and_unknown_characters(void)
{
    uint8_t fb2[OC_OLED_FB];
    oc_oled_clear(fb);
    oc_oled_clear(fb2);
    oc_oled_text(fb, 0, 0, "cell");
    oc_oled_text(fb2, 0, 0, "CELL");
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fb2, fb, OC_OLED_FB);
    oc_oled_text(fb, 1, 0, "~");
    oc_oled_text(fb2, 1, 0, "?");
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fb2, fb, OC_OLED_FB);
}

static void test_text_clips_at_edges(void)
{
    oc_oled_clear(fb);
    oc_oled_text(fb, 0, 18, "ABCDEF");             /* only columns 18..20 */
    oc_oled_text(fb, 8, 0, "X");                   /* page 8 doesn't exist */
    for (size_t i = OC_OLED_W; i < OC_OLED_FB; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, fb[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x7E, fb[18 * 6]);      /* 'A' */
    TEST_ASSERT_EQUAL_HEX8(0x3E, fb[20 * 6]);      /* 'C' */
}

static void test_render_lines_title_yellow_rest_from_page_2(void)
{
    const char lines[3][OC_OLED_COLS + 1] = { "T", "A", "B" };
    uint8_t expect[OC_OLED_FB];
    oc_oled_clear(expect);
    oc_oled_text(expect, 0, 0, "T");
    oc_oled_text(expect, 2, 0, "A");
    oc_oled_text(expect, 3, 0, "B");
    memset(fb, 0xAA, sizeof(fb)); /* render clears first */
    oc_oled_render_lines(fb, lines, 3);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, fb, OC_OLED_FB);
}

static void test_render_lines_ignores_lines_past_the_screen(void)
{
    char lines[9][OC_OLED_COLS + 1];
    for (int i = 0; i < 9; i++) {
        strcpy(lines[i], "X");
    }
    oc_oled_render_lines(fb, (const char (*)[OC_OLED_COLS + 1])lines, 9); /* 7 fit: pages 0, 2..7 */
    TEST_ASSERT_EQUAL_HEX8(0, fb[1 * OC_OLED_W]);         /* page 1 stays blank */
    TEST_ASSERT_EQUAL_HEX8(0x63, fb[7 * OC_OLED_W]);      /* 'X' on the last page */
}

static void test_x2_doubles_each_pixel_over_two_pages(void)
{
    oc_oled_clear(fb);
    oc_oled_text_x2(fb, 2, 10, "1");
    /* '1' is { 0x00, 0x42, 0x7F, 0x40, 0x00 }: 0x42 -> rows 2,3,12,13 = 0x300C */
    static const uint8_t top[12] = { 0x00, 0x00, 0x0C, 0x0C, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t bot[12] = { 0x00, 0x00, 0x30, 0x30, 0x3F, 0x3F, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(top, fb + 2 * OC_OLED_W + 10, 12);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bot, fb + 3 * OC_OLED_W + 10, 12);
    TEST_ASSERT_EQUAL_HEX8(0, fb[1 * OC_OLED_W + 14]); /* nothing above or below */
    TEST_ASSERT_EQUAL_HEX8(0, fb[4 * OC_OLED_W + 14]);
}

static void test_x2_clips_at_the_right_edge_and_last_page(void)
{
    oc_oled_clear(fb);
    oc_oled_text_x2(fb, 0, 120, "88"); /* only 8 columns of the first '8' fit */
    TEST_ASSERT_EQUAL_HEX8(0x3C, fb[120]);  /* '8' column 0 = 0x36: rows 1,2,4,5 -> 0x0F3C */
    TEST_ASSERT_EQUAL_HEX8(0x0F, fb[OC_OLED_W + 120]);
    oc_oled_text_x2(fb, 7, 0, "8");        /* needs pages 7 and 8: not drawn */
    for (size_t i = 7 * OC_OLED_W; i < OC_OLED_FB; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, fb[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_glyph_lands_in_page_format);
    RUN_TEST(test_lower_case_and_unknown_characters);
    RUN_TEST(test_text_clips_at_edges);
    RUN_TEST(test_render_lines_title_yellow_rest_from_page_2);
    RUN_TEST(test_render_lines_ignores_lines_past_the_screen);
    RUN_TEST(test_x2_doubles_each_pixel_over_two_pages);
    RUN_TEST(test_x2_clips_at_the_right_edge_and_last_page);
    return UNITY_END();
}
