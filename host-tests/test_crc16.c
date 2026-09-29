#include "unity.h"

#include "oc_crc.h"

void setUp(void) {}
void tearDown(void) {}

static void test_crc16_check_value(void)
{
    const uint8_t data[] = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29B1, oc_crc16(data, 9));
}

static void test_crc16_empty_is_init(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, oc_crc16(NULL, 0));
}

static void test_crc16_detects_single_bit_flip(void)
{
    uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };
    uint16_t before = oc_crc16(data, sizeof(data));
    data[2] ^= 0x10;
    TEST_ASSERT_NOT_EQUAL(before, oc_crc16(data, sizeof(data)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc16_check_value);
    RUN_TEST(test_crc16_empty_is_init);
    RUN_TEST(test_crc16_detects_single_bit_flip);
    return UNITY_END();
}
