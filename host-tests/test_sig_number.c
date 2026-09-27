/* Numbering v2 (numbering-plan.md v0.2; spec 2026-09-27-numbering-v2-design.md
 * §4-§5): the 8-byte BCD form, the dial plan (vectors/numbers.txt, shared with
 * the Android app) and the display form. */
#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "lc_sig.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t k_own[LC_SIG_NUMBER_LEN] = { 0x88, 0x31, 0x60, 0x65, 0x55, 0x01, 0x23, 0x4F };

static void test_full_form_to_bcd_and_back(void)
{
    uint8_t bcd[LC_SIG_NUMBER_LEN];
    char text[LC_SIG_NUMBER_TEXT];
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+883160655501234", 16, bcd));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_own, bcd, LC_SIG_NUMBER_LEN);
    lc_sig_number_to_text(bcd, text);
    TEST_ASSERT_EQUAL_STRING("+883160655501234", text);
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("883160655501234", 15, bcd)); /* '+' optional */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_own, bcd, LC_SIG_NUMBER_LEN);

    static const uint8_t uk[LC_SIG_NUMBER_LEN] = { 0x88, 0x34, 0x42, 0x07, 0x94, 0x60, 0x00, 0x0F };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+883442079460000", 16, bcd));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(uk, bcd, LC_SIG_NUMBER_LEN);
    static const uint8_t short8[LC_SIG_NUMBER_LEN] = { 0x88, 0x34, 0x41, 0x23, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+88344123", 9, bcd)); /* 8 digits: the shortest */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(short8, bcd, LC_SIG_NUMBER_LEN);
    lc_sig_number_to_text(bcd, text);
    TEST_ASSERT_EQUAL_STRING("+88344123", text);

    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+883-1-606-555-01234", 20, bcd)); /* full form only */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+88316065551234", 15, bcd));      /* CC 1, 14 digits */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+884160655501234", 16, bcd));     /* not 883 */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+8831606555012345", 17, bcd));    /* 16 digits */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+8834412", 8, bcd));              /* 7 digits */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+883188355501234", 16, bcd));     /* NPA 883 */
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("", 0, bcd));
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_to_bcd("+", 1, bcd));
}

static void test_valid_accepts_only_canonical_numbers(void)
{
    uint8_t b[LC_SIG_NUMBER_LEN];
    TEST_ASSERT_EQUAL_INT(1, lc_sig_number_valid(k_own));
    memcpy(b, k_own, sizeof(b));
    b[0] = 0x98; /* 983...: not 883 */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(b));
    memcpy(b, k_own, sizeof(b));
    b[3] = 0x6A; /* a nibble A-E */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(b));
    memcpy(b, k_own, sizeof(b));
    b[7] = 0x40; /* no filler at all: 16 digits */
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(b));
    static const uint8_t after_filler[LC_SIG_NUMBER_LEN] = { 0x88, 0x34, 0x42, 0x07, 0x94, 0x60, 0x0F, 0x0F };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(after_filler)); /* a digit after the filler */
    static const uint8_t zero_tail[LC_SIG_NUMBER_LEN] = { 0x88, 0x34, 0x42, 0x07, 0x94, 0x60, 0x0F, 0x00 };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(zero_tail));
    static const uint8_t cc1_14[LC_SIG_NUMBER_LEN] = { 0x88, 0x31, 0x60, 0x65, 0x55, 0x12, 0x34, 0xFF };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(cc1_14)); /* CC 1 with 14 digits */
    static const uint8_t zeros[LC_SIG_NUMBER_LEN] = { 0 };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(zeros)); /* a not-activated identity's number */
    static const uint8_t blank[LC_SIG_NUMBER_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(blank));
}

static void test_format_for_people(void)
{
    char s[LC_SIG_NUMBER_SHOW];
    uint8_t b[LC_SIG_NUMBER_LEN];
    TEST_ASSERT_EQUAL_size_t(20, lc_sig_number_format(k_own, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("+883-1-606-555-01234", s);
    TEST_ASSERT_EQUAL_size_t(0, lc_sig_number_format(k_own, s, 20)); /* no room for the NUL */
    TEST_ASSERT_EQUAL_STRING("", s);
    lc_sig_number_to_bcd("+883442079460000", 16, b);
    TEST_ASSERT_EQUAL_size_t(18, lc_sig_number_format(b, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("+883-44-2079460000", s);
    lc_sig_number_to_bcd("+883712345678", 13, b);
    lc_sig_number_format(b, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("+883-7-12345678", s);
    lc_sig_number_to_bcd("+8833801234567", 14, b); /* 380: a three-digit code */
    lc_sig_number_format(b, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("+883-380-1234567", s);
    static const uint8_t zeros[LC_SIG_NUMBER_LEN] = { 0 };
    TEST_ASSERT_EQUAL_size_t(0, lc_sig_number_format(zeros, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("", s);
}

/* ASan/UBSan regressions found in review of the brief's code:
 * (1) decode() must not write past its LC_SIG_NUMBER_DIGITS+1 buffer when all
 *     16 nibbles are digit nibbles (no filler at all: not canonical, must be
 *     rejected before the write, not after).
 * (2) cc_len()'s country-code table scan must not read past its NUL when the
 *     looked-up code is not in the table (forcing the scan past the table's
 *     last entry, "98"). */
static void test_asan_regressions(void)
{
    static const uint8_t all_digits[LC_SIG_NUMBER_LEN] = { 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56 };
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_valid(all_digits)); /* 16 digit nibbles, no filler */
    char s[LC_SIG_NUMBER_SHOW];
    TEST_ASSERT_EQUAL_size_t(0, lc_sig_number_format(all_digits, s, sizeof(s)));

    uint8_t b[LC_SIG_NUMBER_LEN];
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+883981234567", 13, b)); /* CC "98": the table's last entry */
    TEST_ASSERT_EQUAL_size_t(15, lc_sig_number_format(b, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("+883-98-1234567", s);

    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_to_bcd("+88399123456", 12, b)); /* CC "99": not in the table */
    TEST_ASSERT_EQUAL_size_t(14, lc_sig_number_format(b, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("+883-991-23456", s);
}

/* Every row of vectors/numbers.txt: "dialled|home|expected". */
static void test_dial_plan_vectors(void)
{
    FILE *f = fopen(LC_NUMBER_VECTORS, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, LC_NUMBER_VECTORS);
    char line[160], msg[200];
    int rows = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;
        char *bar1 = strchr(line, '|'), *bar2 = bar1 != NULL ? strchr(bar1 + 1, '|') : NULL;
        TEST_ASSERT_NOT_NULL_MESSAGE(bar2, line);
        *bar1 = *bar2 = '\0';
        const char *dialed = line, *home_s = bar1 + 1, *want = bar2 + 1;
        uint8_t home[LC_SIG_NUMBER_LEN], got[LC_SIG_NUMBER_LEN];
        const uint8_t *hp = NULL;
        if (strcmp(home_s, "-") != 0) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, lc_sig_number_to_bcd(home_s, strlen(home_s), home), home_s);
            hp = home;
        }
        int r = lc_sig_number_normalize(dialed, strlen(dialed), hp, got);
        snprintf(msg, sizeof(msg), "dialled \"%s\" home %s", dialed, home_s);
        if (strcmp(want, "reject") == 0) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(-1, r, msg);
        } else {
            char text[LC_SIG_NUMBER_TEXT];
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r, msg);
            lc_sig_number_to_text(got, text);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(want, text, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, lc_sig_number_valid(got), msg);
        }
        rows++;
    }
    fclose(f);
    TEST_ASSERT_TRUE(rows >= 50);
}

/* A home number that is not a valid number never completes a national form. */
static void test_normalize_needs_a_valid_home(void)
{
    uint8_t got[LC_SIG_NUMBER_LEN];
    static const uint8_t zeros[LC_SIG_NUMBER_LEN] = { 0 };
    TEST_ASSERT_EQUAL_INT(-1, lc_sig_number_normalize("606-555-1235", 12, zeros, got));
    TEST_ASSERT_EQUAL_INT(0, lc_sig_number_normalize("+883 1 606 555 1235", 19, zeros, got));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_full_form_to_bcd_and_back);
    RUN_TEST(test_valid_accepts_only_canonical_numbers);
    RUN_TEST(test_format_for_people);
    RUN_TEST(test_asan_regressions);
    RUN_TEST(test_dial_plan_vectors);
    RUN_TEST(test_normalize_needs_a_valid_home);
    return UNITY_END();
}
