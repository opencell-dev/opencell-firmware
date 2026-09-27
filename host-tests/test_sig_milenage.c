#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "lc_sig_milenage.h"

void setUp(void) {}
void tearDown(void) {}

static void hex(const char *s, uint8_t *out)
{
    for (size_t i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(&s[2 * i], "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

typedef struct {
    const char *k, *rand, *sqn, *amf, *op, *opc, *f1, *f1s, *f2, *f5, *f3, *f4, *f5s;
} vec_t;

static const vec_t sets[] = { /* 3GPP TS 35.208 test sets 1-6 */
    { "465b5ce8b199b49faa5f0a2ee238a6bc", "23553cbe9637a89d218ae64dae47bf35", "ff9bb4d0b607", "b9b9",
      "cdc202d5123e20f62b6d676ac72cb318", "cd63cb71954a9f4e48a5994e37a02baf", "4a9ffac354dfafb3",
      "01cfaf9ec4e871e9", "a54211d5e3ba50bf", "aa689c648370", "b40ba9a3c58b2a05bbf0d987b21bf8cb",
      "f769bcd751044604127672711c6d3441", "451e8beca43b" },
    { "465b5ce8b199b49faa5f0a2ee238a6bc", "23553cbe9637a89d218ae64dae47bf35", "ff9bb4d0b607", "b9b9",
      "cdc202d5123e20f62b6d676ac72cb318", "cd63cb71954a9f4e48a5994e37a02baf", "4a9ffac354dfafb3",
      "01cfaf9ec4e871e9", "a54211d5e3ba50bf", "aa689c648370", "b40ba9a3c58b2a05bbf0d987b21bf8cb",
      "f769bcd751044604127672711c6d3441", "451e8beca43b" },
    { "fec86ba6eb707ed08905757b1bb44b8f", "9f7c8d021accf4db213ccff0c7f71a6a", "9d0277595ffc", "725c",
      "dbc59adcb6f9a0ef735477b7fadf8374", "1006020f0a478bf6b699f15c062e42b3", "9cabc3e99baf7281",
      "95814ba2b3044324", "8011c48c0c214ed2", "33484dc2136b", "5dbdbb2954e8f3cde665b046179a5098",
      "59a92d3b476a0443487055cf88b2307b", "deacdd848cc6" },
    { "9e5944aea94b81165c82fbf9f32db751", "ce83dbc54ac0274a157c17f80d017bd6", "0b604a81eca8", "9e09",
      "223014c5806694c007ca1eeef57f004f", "a64a507ae1a2a98bb88eb4210135dc87", "74a58220cba84c49",
      "ac2cc74a96871837", "f365cd683cd92e96", "f0b9c08ad02e", "e203edb3971574f5a94b0d61b816345d",
      "0c4524adeac041c4dd830d20854fc46b", "6085a86c6f63" },
    { "4ab1deb05ca6ceb051fc98e77d026a84", "74b0cd6031a1c8339b2b6ce2b8c4a186", "e880a1b580b6", "9f07",
      "2d16c5cd1fdf6b22383584e3bef2a8d8", "dcf07cbd51855290b92a07a9891e523e", "49e785dd12626ef2",
      "9e85790336bb3fa2", "5860fc1bce351e7e", "31e11a609118", "7657766b373d1c2138f307e3de9242f9",
      "1c42e960d89b8fa99f2744e0708ccb53", "fe2555e54aa9" },
    { "6c38a116ac280c454f59332ee35c8c4f", "ee6466bc96202c5a557abbeff8babf63", "414b98222181", "4464",
      "1ba00a1a7c6700ac8c3ff3e96ad08725", "3803ef5363b947c6aaa225e58fae3934", "078adfb488241a57",
      "80246b8d0186bcf1", "16c8233f05a0ac28", "45b0f69ab06c", "3f8c7587fe8e4b233af676aede30ba3b",
      "a7466cc1e6b2a1337d49d3b66e95d7b4", "1f53cd2b1113" },
};

static void check_set(const vec_t *v)
{
    uint8_t k[16], rand[16], sqn[6], amf[2], op[16], opc[16], want[16];
    lc_milenage_t o;
    hex(v->k, k);
    hex(v->rand, rand);
    hex(v->sqn, sqn);
    hex(v->amf, amf);
    hex(v->op, op);
    TEST_ASSERT_EQUAL_INT(0, lc_milenage_opc(k, op, opc));
    hex(v->opc, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, opc, 16);
    TEST_ASSERT_EQUAL_INT(0, lc_milenage(k, opc, rand, sqn, amf, &o));
    hex(v->f1, want);  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, o.mac_a, 8);
    hex(v->f1s, want); TEST_ASSERT_EQUAL_HEX8_ARRAY(want, o.mac_s, 8);
    hex(v->f2, want);  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, o.res, 8);
    hex(v->f5, want);  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, o.ak, 6);
    hex(v->f3, want);  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, o.ck, 16);
    hex(v->f4, want);  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, o.ik, 16);
    hex(v->f5s, want); TEST_ASSERT_EQUAL_HEX8_ARRAY(want, o.ak_s, 6);
}

static void test_ts35208_set1(void) { check_set(&sets[0]); }
static void test_ts35208_set2(void) { check_set(&sets[1]); }
static void test_ts35208_set3(void) { check_set(&sets[2]); }
static void test_ts35208_set4(void) { check_set(&sets[3]); }
static void test_ts35208_set5(void) { check_set(&sets[4]); }
static void test_ts35208_set6(void) { check_set(&sets[5]); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ts35208_set1);
    RUN_TEST(test_ts35208_set2);
    RUN_TEST(test_ts35208_set3);
    RUN_TEST(test_ts35208_set4);
    RUN_TEST(test_ts35208_set5);
    RUN_TEST(test_ts35208_set6);
    return UNITY_END();
}
