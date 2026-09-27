#include "unity.h"

#include "lc_term_screen.h"
#include "lc_term_ui.h"

void setUp(void) {}
void tearDown(void) {}

#define S(x) ((uint64_t)(x) * 1000000ull)
#define MS(x) ((uint64_t)(x) * 1000ull)
#define ENABLE S(10)

static lc_term_ui_t ui;

/* Press at t, release at t + held_ms, polling every 20 ms like the firmware.
 * Returns how many times LC_UI_CLEAR_BONDS came back. */
static int press(uint64_t t, uint64_t held_ms)
{
    int clears = 0;
    for (uint64_t at = t; at < t + MS(held_ms); at += MS(20)) {
        clears += lc_term_ui_button(&ui, 1, at) == LC_UI_CLEAR_BONDS;
    }
    clears += lc_term_ui_button(&ui, 0, t + MS(held_ms)) == LC_UI_CLEAR_BONDS;
    return clears;
}

static void test_short_presses_step_through_the_four_screens_and_wrap(void)
{
    lc_term_ui_init(&ui, ENABLE);
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_STATUS, lc_term_ui_screen(&ui, S(11)));
    press(S(11), 200);
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(12)));
    press(S(12), 200);
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_SUBSCRIBER, lc_term_ui_screen(&ui, S(13)));
    press(S(13), 200);
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_RADIO, lc_term_ui_screen(&ui, S(14)));
    press(S(14), 200);
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_STATUS, lc_term_ui_screen(&ui, S(15)));
}

static void test_the_screen_changes_on_release_not_on_press(void)
{
    lc_term_ui_init(&ui, ENABLE);
    lc_term_ui_button(&ui, 1, S(11));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_STATUS, lc_term_ui_screen(&ui, S(11)));
    lc_term_ui_button(&ui, 0, S(11) + MS(100));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(11) + MS(100)));
}

static void test_presses_in_the_role_switch_window_are_ignored(void)
{
    lc_term_ui_init(&ui, ENABLE);
    press(S(2), 200);
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_STATUS, lc_term_ui_screen(&ui, S(3)));
    /* begun in the window, released after it (a role-switch hold): still ignored */
    press(S(9) + MS(800), 500);
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_STATUS, lc_term_ui_screen(&ui, S(11)));
    press(S(11), 200); /* the first press begun after the window counts */
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(11)));
}

static void test_a_press_of_one_to_five_seconds_does_nothing(void)
{
    lc_term_ui_init(&ui, ENABLE);
    press(S(11), 200); /* Pairing */
    TEST_ASSERT_EQUAL_INT(0, press(S(12), 3000));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(16)));
    TEST_ASSERT_FALSE(lc_term_ui_cleared(&ui, S(16)));
}

static void test_holding_5_s_on_pairing_clears_bonds_once(void)
{
    lc_term_ui_init(&ui, ENABLE);
    press(S(11), 200); /* Pairing */
    TEST_ASSERT_EQUAL_INT(1, press(S(12), 8000));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(20))); /* no step on release */
    TEST_ASSERT_TRUE(lc_term_ui_cleared(&ui, S(17) + MS(100)));  /* fired at 17 s, shown for 3 s */
    TEST_ASSERT_FALSE(lc_term_ui_cleared(&ui, S(20) + MS(100)));
}

static void test_holding_on_other_screens_clears_nothing(void)
{
    lc_term_ui_init(&ui, ENABLE);
    TEST_ASSERT_EQUAL_INT(0, press(S(11), 6000)); /* Status */
    press(S(20), 200);
    press(S(21), 200); /* Subscriber */
    TEST_ASSERT_EQUAL_INT(0, press(S(22), 6000));
}

static void test_a_hold_that_began_elsewhere_does_not_clear_after_a_jump_to_pairing(void)
{
    lc_term_ui_init(&ui, ENABLE);
    lc_term_ui_button(&ui, 1, S(11)); /* held on Status ... */
    lc_term_ui_pairing_started(&ui);  /* ... when a phone starts pairing */
    int clears = 0;
    for (uint64_t at = S(11); at < S(18); at += MS(20)) {
        clears += lc_term_ui_button(&ui, 1, at) == LC_UI_CLEAR_BONDS;
    }
    TEST_ASSERT_EQUAL_INT(0, clears);
}

static void test_pairing_jumps_to_the_pairing_screen_and_returns_10_s_after_it_ends(void)
{
    lc_term_ui_init(&ui, ENABLE);
    press(S(11), 200);
    press(S(12), 200); /* Subscriber */
    lc_term_ui_pairing_started(&ui);
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(20)));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(60))); /* no return while pairing */
    lc_term_ui_pairing_ended(&ui, S(61));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(70)));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_SUBSCRIBER, lc_term_ui_screen(&ui, S(71)));
}

static void test_a_press_during_the_jump_cancels_the_return(void)
{
    lc_term_ui_init(&ui, ENABLE);
    lc_term_ui_pairing_started(&ui);
    lc_term_ui_pairing_ended(&ui, S(20));
    press(S(21), 200); /* Pairing -> Subscriber, the user's choice */
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_SUBSCRIBER, lc_term_ui_screen(&ui, S(40)));
}

static void test_no_return_when_the_user_was_already_on_pairing(void)
{
    lc_term_ui_init(&ui, ENABLE);
    press(S(11), 200); /* Pairing */
    lc_term_ui_pairing_started(&ui);
    lc_term_ui_pairing_ended(&ui, S(20));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(40)));
}

static void test_a_new_attempt_before_the_return_keeps_the_original_screen(void)
{
    lc_term_ui_init(&ui, ENABLE);
    press(S(11), 200);
    press(S(12), 200);
    press(S(13), 200); /* Radio */
    lc_term_ui_pairing_started(&ui);
    lc_term_ui_pairing_ended(&ui, S(20));  /* failed */
    lc_term_ui_pairing_started(&ui);       /* tried again at 25 s */
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_PAIRING, lc_term_ui_screen(&ui, S(31)));
    lc_term_ui_pairing_ended(&ui, S(40));
    TEST_ASSERT_EQUAL_UINT8(LC_SCREEN_RADIO, lc_term_ui_screen(&ui, S(50)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_presses_step_through_the_four_screens_and_wrap);
    RUN_TEST(test_the_screen_changes_on_release_not_on_press);
    RUN_TEST(test_presses_in_the_role_switch_window_are_ignored);
    RUN_TEST(test_a_press_of_one_to_five_seconds_does_nothing);
    RUN_TEST(test_holding_5_s_on_pairing_clears_bonds_once);
    RUN_TEST(test_holding_on_other_screens_clears_nothing);
    RUN_TEST(test_a_hold_that_began_elsewhere_does_not_clear_after_a_jump_to_pairing);
    RUN_TEST(test_pairing_jumps_to_the_pairing_screen_and_returns_10_s_after_it_ends);
    RUN_TEST(test_a_press_during_the_jump_cancels_the_return);
    RUN_TEST(test_no_return_when_the_user_was_already_on_pairing);
    RUN_TEST(test_a_new_attempt_before_the_return_keeps_the_original_screen);
    return UNITY_END();
}
