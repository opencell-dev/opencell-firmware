/* Voice codec ids and the ranking (codec negotiation spec N1-N2 as amended
 * by the OpenVoxNB spec V1-V2): the best codec of a caps byte, the block
 * size and name of each id, and the OpenVoxNB ids agreeing with oc_vox's
 * own table (oc_vox_modes.h). */
#include "unity.h"

#include "oc_codec.h"
#include "oc_vox_modes.h"

void setUp(void) {}
void tearDown(void) {}

static void test_ids_and_caps_bits(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x01, OC_CODEC_BIT(OC_CODEC_C2_1200));
    TEST_ASSERT_EQUAL_HEX8(0x02, OC_CODEC_BIT(OC_CODEC_LPCNET_1733));
    TEST_ASSERT_EQUAL_HEX8(0x04, OC_CODEC_BIT(OC_CODEC_OVX_600));
    TEST_ASSERT_EQUAL_HEX8(0x20, OC_CODEC_BIT(OC_CODEC_OVX_1200));
    TEST_ASSERT_EQUAL_HEX8(0x01, OC_CODEC_CAPS_DEFAULT);
    TEST_ASSERT_TRUE(oc_codec_in(0x21, OC_CODEC_OVX_1200));
    TEST_ASSERT_TRUE(oc_codec_in(0x21, OC_CODEC_C2_1200));
    TEST_ASSERT_FALSE(oc_codec_in(0x21, OC_CODEC_OVX_600));
    TEST_ASSERT_FALSE(oc_codec_in(0xFF, OC_CODEC_NONE)); /* 0 is never in caps */
    TEST_ASSERT_FALSE(oc_codec_in(0xFF, 9));
}

/* V2: OVX-1200 > OVX-1000 > OVX-800 > LPCNet 1733 > Codec2 1200 > OVX-600. */
static void test_the_ranking(void)
{
    static const uint8_t order[] = { OC_CODEC_OVX_1200,    OC_CODEC_OVX_1000, OC_CODEC_OVX_800,
                                     OC_CODEC_LPCNET_1733, OC_CODEC_C2_1200,  OC_CODEC_OVX_600 };
    uint8_t caps = 0x3F;
    for (unsigned i = 0; i < sizeof(order); i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(order[i], oc_codec_best(caps), "the best of what is left");
        caps &= (uint8_t)~OC_CODEC_BIT(order[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(OC_CODEC_NONE, oc_codec_best(caps));
    TEST_ASSERT_EQUAL_UINT8(OC_CODEC_C2_1200, oc_codec_best(0x05));  /* OVX-600 loses to Codec2 1200 */
    TEST_ASSERT_EQUAL_UINT8(OC_CODEC_OVX_1200, oc_codec_best(0x21)); /* the spec's example: caller 0x21 */
    TEST_ASSERT_EQUAL_UINT8(OC_CODEC_NONE, oc_codec_best(0xC0));     /* ids 7 and 8: reserved, ignored */
    TEST_ASSERT_EQUAL_UINT8(OC_CODEC_C2_1200, oc_codec_best(0xC1));
}

static void test_block_bytes_and_names(void)
{
    TEST_ASSERT_EQUAL_UINT8(18, oc_codec_block_bytes(OC_CODEC_C2_1200));
    TEST_ASSERT_EQUAL_UINT8(9, oc_codec_block_bytes(OC_CODEC_OVX_600));
    TEST_ASSERT_EQUAL_UINT8(12, oc_codec_block_bytes(OC_CODEC_OVX_800));
    TEST_ASSERT_EQUAL_UINT8(15, oc_codec_block_bytes(OC_CODEC_OVX_1000));
    TEST_ASSERT_EQUAL_UINT8(18, oc_codec_block_bytes(OC_CODEC_OVX_1200));
    TEST_ASSERT_EQUAL_UINT8(0, oc_codec_block_bytes(OC_CODEC_LPCNET_1733)); /* reserved: no frame of it yet */
    TEST_ASSERT_EQUAL_UINT8(0, oc_codec_block_bytes(OC_CODEC_NONE));
    TEST_ASSERT_EQUAL_UINT8(0, oc_codec_block_bytes(7));
    TEST_ASSERT_EQUAL_STRING("Codec2 1200", oc_codec_name(OC_CODEC_C2_1200));
    TEST_ASSERT_EQUAL_STRING("LPCNet 1733", oc_codec_name(OC_CODEC_LPCNET_1733));
    TEST_ASSERT_EQUAL_STRING("OVX-600", oc_codec_name(OC_CODEC_OVX_600));
    TEST_ASSERT_EQUAL_STRING("OVX-1200", oc_codec_name(OC_CODEC_OVX_1200));
    TEST_ASSERT_EQUAL_STRING("none", oc_codec_name(OC_CODEC_NONE));
    TEST_ASSERT_EQUAL_STRING("?", oc_codec_name(7));
}

/* oc_vox_modes.h maps its modes to ids 3-6 itself: the two tables agree. */
static void test_openvox_ids_agree_with_oc_vox(void)
{
    for (uint8_t id = OC_CODEC_OVX_600; id <= OC_CODEC_OVX_1200; id++) {
        int mode = oc_vox_mode_of_codec_id(id);
        TEST_ASSERT_NOT_EQUAL(OC_VOX_MODE_NONE, mode);
        TEST_ASSERT_EQUAL_UINT8(oc_vox_block_bytes((oc_vox_mode_t)mode), oc_codec_block_bytes(id));
        TEST_ASSERT_EQUAL_STRING(oc_vox_mode_name((oc_vox_mode_t)mode), oc_codec_name(id));
    }
    TEST_ASSERT_EQUAL_HEX8(0x3C, oc_vox_caps_to_sig(OC_VOX_CAPS_ALL));
    TEST_ASSERT_EQUAL_INT(OC_VOX_MODE_NONE, oc_vox_mode_of_codec_id(OC_CODEC_C2_1200));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ids_and_caps_bits);
    RUN_TEST(test_the_ranking);
    RUN_TEST(test_block_bytes_and_names);
    RUN_TEST(test_openvox_ids_agree_with_oc_vox);
    return UNITY_END();
}
