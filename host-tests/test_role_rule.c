#include "unity.h"

#include "app_role_rule.h"

void setUp(void) {}
void tearDown(void) {}

static void test_hold_toggles_terminal_and_bs_radio(void)
{
    TEST_ASSERT_EQUAL_INT(APP_ROLE_TERMINAL, app_role_after_hold(APP_ROLE_BS_RADIO));
    TEST_ASSERT_EQUAL_INT(APP_ROLE_BS_RADIO, app_role_after_hold(APP_ROLE_TERMINAL));
}

static void test_hold_never_enters_cell_and_leaves_it_for_bs_radio(void)
{
    TEST_ASSERT_EQUAL_INT(APP_ROLE_BS_RADIO, app_role_after_hold(APP_ROLE_CELL));
}

static void test_nvs_values(void)
{
    TEST_ASSERT_EQUAL_INT(APP_ROLE_BS_RADIO, app_role_from_nvs(0));
    TEST_ASSERT_EQUAL_INT(APP_ROLE_TERMINAL, app_role_from_nvs(1));
    TEST_ASSERT_EQUAL_INT(APP_ROLE_CELL, app_role_from_nvs(2));
    TEST_ASSERT_EQUAL_INT(APP_ROLE_BS_RADIO, app_role_from_nvs(7));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hold_toggles_terminal_and_bs_radio);
    RUN_TEST(test_hold_never_enters_cell_and_leaves_it_for_bs_radio);
    RUN_TEST(test_nvs_values);
    return UNITY_END();
}
