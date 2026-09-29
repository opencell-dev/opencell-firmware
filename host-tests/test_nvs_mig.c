/* lc2oc: keep-file (this file names the legacy NVS namespaces on purpose) */
/* oc_nvs_mig: moving an NVS namespace (lc -> oc rename), with power lost at
 * every step. The fake store counts each write (a set, or one key erased by
 * erase_all) as a step; "power lost at step k" lets k steps happen and fails
 * everything after, until the next "boot". */
#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "oc_nvs_mig.h"

#define T_U8   0x01u /* nvs_type_t codes */
#define T_STR  0x21u
#define T_BLOB 0x42u
#define MAX_E  64
#define MAX_V  600

typedef struct {
    int     used;
    char    ns[16];
    char    key[16];
    uint8_t type;
    size_t  len;
    uint8_t val[MAX_V];
} ent_t;

static ent_t st[MAX_E];
static int steps;        /* writes done since the last boot */
static int lose_power_at; /* -1: never */
static int fail_once_at;  /* -1: never; that one write fails, later ones work */
static int corrupt;       /* 1: every set stores a flipped first byte */
static int list_error;

static int dead(void) { return lose_power_at >= 0 && steps >= lose_power_at; }

/* A write step: 0 to go ahead, -1 when it fails. */
static int step(void)
{
    if (dead()) {
        return -1;
    }
    if (fail_once_at >= 0 && steps == fail_once_at) {
        fail_once_at = -1;
        return -1;
    }
    steps++;
    return 0;
}

static ent_t *find(const char *ns, const char *key)
{
    for (int i = 0; i < MAX_E; i++) {
        if (st[i].used && strcmp(st[i].ns, ns) == 0 && strcmp(st[i].key, key) == 0) {
            return &st[i];
        }
    }
    return NULL;
}

static void put(const char *ns, const char *key, uint8_t type, const void *val, size_t len)
{
    ent_t *e = find(ns, key);
    for (int i = 0; e == NULL && i < MAX_E; i++) {
        if (!st[i].used) {
            e = &st[i];
        }
    }
    TEST_ASSERT_NOT_NULL(e);
    e->used = 1;
    snprintf(e->ns, sizeof(e->ns), "%s", ns);
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->type = type;
    e->len = len;
    memcpy(e->val, val, len);
}

static int f_list(void *ctx, const char *ns, oc_nvs_mig_key_t *keys, int max)
{
    (void)ctx;
    if (dead() || list_error) {
        return -1;
    }
    int n = 0;
    for (int i = 0; i < MAX_E; i++) {
        if (st[i].used && strcmp(st[i].ns, ns) == 0) {
            if (n < max) {
                snprintf(keys[n].key, sizeof(keys[n].key), "%s", st[i].key);
                keys[n].type = st[i].type;
            }
            n++;
        }
    }
    return n;
}

static int f_get(void *ctx, const char *ns, const char *key, uint8_t type, uint8_t *buf, size_t cap)
{
    (void)ctx;
    if (dead()) {
        return -2;
    }
    ent_t *e = find(ns, key);
    if (e == NULL) {
        return -1;
    }
    if (e->type != type || e->len > cap) {
        return -2;
    }
    memcpy(buf, e->val, e->len);
    return (int)e->len;
}

static int f_set(void *ctx, const char *ns, const char *key, uint8_t type, const uint8_t *val, size_t len)
{
    (void)ctx;
    if (step() != 0) {
        return -1;
    }
    put(ns, key, type, val, len);
    if (corrupt && len > 0) {
        find(ns, key)->val[0] ^= 0xFFu;
    }
    return 0;
}

static int f_erase_all(void *ctx, const char *ns)
{
    (void)ctx;
    for (int i = 0; i < MAX_E; i++) {
        if (st[i].used && strcmp(st[i].ns, ns) == 0) {
            if (step() != 0) {
                return -1; /* keys erased so far stay erased */
            }
            st[i].used = 0;
        }
    }
    return 0;
}

static const oc_nvs_mig_ops_t ops = { NULL, T_U8, f_list, f_get, f_set, f_erase_all };

static uint8_t ident[114];
static uint8_t cfg[40];

static void boot(void)
{
    steps = 0;
    lose_power_at = -1;
    fail_once_at = -1;
    corrupt = 0;
    list_error = 0;
}

void setUp(void)
{
    memset(st, 0, sizeof(st));
    boot();
    for (unsigned i = 0; i < sizeof(ident); i++) {
        ident[i] = (uint8_t)(0xA5u ^ i);
    }
    for (unsigned i = 0; i < sizeof(cfg); i++) {
        cfg[i] = (uint8_t)i;
    }
}
void tearDown(void) {}

/* The terminal's lc_id as boards hold it, plus two more types. */
static void seed_id(void)
{
    const uint8_t one = 1;
    put("lc_id", "ident", T_BLOB, ident, sizeof(ident));
    put("lc_id", "flag", T_U8, &one, 1);
    put("lc_id", "name", T_STR, "T2", 3);
}

static int count(const char *ns)
{
    int n = 0;
    for (int i = 0; i < MAX_E; i++) {
        n += st[i].used && strcmp(st[i].ns, ns) == 0;
    }
    return n;
}

static int has_marker(const char *ns)
{
    ent_t *e = find(ns, OC_NVS_MIG_MARKER);
    return e != NULL && e->type == T_U8 && e->len == 1 && e->val[0] == 1;
}

/* ns holds exactly seed_id()'s three keys (and, if marked, the marker). */
static void assert_holds_id(const char *ns)
{
    ent_t *e = find(ns, "ident");
    TEST_ASSERT_NOT_NULL_MESSAGE(e, ns);
    TEST_ASSERT_EQUAL_UINT8(T_BLOB, e->type);
    TEST_ASSERT_EQUAL_size_t(sizeof(ident), e->len);
    TEST_ASSERT_EQUAL_MEMORY(ident, e->val, sizeof(ident));
    e = find(ns, "flag");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(1, e->val[0]);
    e = find(ns, "name");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("T2", (const char *)e->val);
    TEST_ASSERT_EQUAL_INT(3 + has_marker(ns), count(ns));
}

static void test_moves_every_key_and_leaves_other_namespaces_alone(void)
{
    const uint8_t one = 1;
    put("lc", "term", T_U8, &one, 1);
    put("lc", "cfg", T_BLOB, cfg, sizeof(cfg));
    seed_id();
    put("nimble_bond", "our_sec_1", T_BLOB, cfg, 8);
    int moved = -1;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc", "oc", &moved));
    TEST_ASSERT_EQUAL_INT(2, moved);
    TEST_ASSERT_EQUAL_INT(0, count("lc"));
    TEST_ASSERT_TRUE(has_marker("oc"));
    TEST_ASSERT_EQUAL_UINT8(1, find("oc", "term")->val[0]);
    TEST_ASSERT_EQUAL_MEMORY(cfg, find("oc", "cfg")->val, sizeof(cfg));
    TEST_ASSERT_EQUAL_INT(3, count("oc"));
    assert_holds_id("lc_id"); /* another namespace: its own move */
    TEST_ASSERT_EQUAL_INT(1, count("nimble_bond"));
}

static void test_a_second_boot_writes_nothing(void)
{
    seed_id();
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_EQUAL_INT(3, moved);
    boot();
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_DONE, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_EQUAL_INT(0, moved);
    TEST_ASSERT_EQUAL_INT(0, steps);
    assert_holds_id("oc_id");
    TEST_ASSERT_EQUAL_INT(0, count("lc_id"));
}

static void test_a_fresh_board_is_only_marked(void)
{
    int moved = -1;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_scan", "oc_scan", &moved));
    TEST_ASSERT_EQUAL_INT(0, moved);
    TEST_ASSERT_EQUAL_INT(1, count("oc_scan"));
    TEST_ASSERT_TRUE(has_marker("oc_scan"));
}

/* Review Focus 1: whatever step the power goes at, the namespace the next
 * boot would use (marked ? new : old) holds the identity, and that boot
 * finishes the move. */
static void test_power_lost_at_every_step(void)
{
    seed_id();
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    const int total = steps; /* 3 sets, the marker, 3 erases */
    TEST_ASSERT_EQUAL_INT(7, total);
    for (int k = 0; k < total; k++) {
        memset(st, 0, sizeof(st));
        seed_id();
        boot();
        lose_power_at = k;
        (void)oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
        boot(); /* power back */
        assert_holds_id(has_marker("oc_id") ? "oc_id" : "lc_id");
        oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
        TEST_ASSERT_NOT_EQUAL(OC_NVS_MIG_FAILED, r);
        assert_holds_id("oc_id");
        TEST_ASSERT_TRUE(has_marker("oc_id"));
        TEST_ASSERT_EQUAL_INT(0, count("lc_id"));
    }
}

static void test_a_failed_write_keeps_the_old_namespace(void)
{
    seed_id();
    fail_once_at = 1; /* the second key's copy */
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_FAILED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    assert_holds_id("lc_id");
    TEST_ASSERT_FALSE(has_marker("oc_id"));
    boot();
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    assert_holds_id("oc_id");
}

/* A copy cut short, then the old firmware's fallback boot erased sc_done:
 * the next move must not bring it back. */
static void test_a_stale_copy_does_not_come_back(void)
{
    seed_id();
    put("oc_id", "ident", T_BLOB, ident, sizeof(ident));
    put("oc_id", "sc_done", T_BLOB, cfg, 14);
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_NULL(find("oc_id", "sc_done"));
    assert_holds_id("oc_id");
}

/* After a downgrade the old firmware found no lc_id and made a new identity;
 * back on this firmware, the moved (activated) one wins. */
static void test_the_marked_namespace_wins_over_leftovers(void)
{
    const uint8_t one = 1, other[114] = { 0x11 };
    put("oc_id", "ident", T_BLOB, ident, sizeof(ident));
    put("oc_id", "flag", T_U8, &one, 1);
    put("oc_id", "name", T_STR, "T2", 3);
    put("oc_id", OC_NVS_MIG_MARKER, T_U8, &one, 1);
    put("lc_id", "ident", T_BLOB, other, sizeof(other));
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_DONE, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    assert_holds_id("oc_id");
    TEST_ASSERT_EQUAL_INT(0, count("lc_id"));
}

static void test_a_bad_read_back_is_not_marked(void)
{
    seed_id();
    corrupt = 1;
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_FAILED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_FALSE(has_marker("oc_id"));
    assert_holds_id("lc_id");
}

static void test_too_many_keys_fail_before_any_write(void)
{
    char key[16];
    for (int i = 0; i < OC_NVS_MIG_KEYS_MAX + 1; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        put("lc", key, T_U8, &i, 1);
    }
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_FAILED, oc_nvs_mig_move(&ops, "lc", "oc", &moved));
    TEST_ASSERT_EQUAL_INT(0, steps);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_KEYS_MAX + 1, count("lc"));
}

static void test_a_value_too_long_fails(void)
{
    static const uint8_t big[OC_NVS_MIG_VALUE_MAX + 1];
    seed_id();
    put("lc_id", "big", T_BLOB, big, sizeof(big));
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_FAILED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_FALSE(has_marker("oc_id"));
    TEST_ASSERT_EQUAL_INT(4, count("lc_id"));
}

static void test_a_list_error_fails(void)
{
    seed_id();
    list_error = 1;
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_FAILED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_EQUAL_INT(0, steps);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_moves_every_key_and_leaves_other_namespaces_alone);
    RUN_TEST(test_a_second_boot_writes_nothing);
    RUN_TEST(test_a_fresh_board_is_only_marked);
    RUN_TEST(test_power_lost_at_every_step);
    RUN_TEST(test_a_failed_write_keeps_the_old_namespace);
    RUN_TEST(test_a_stale_copy_does_not_come_back);
    RUN_TEST(test_the_marked_namespace_wins_over_leftovers);
    RUN_TEST(test_a_bad_read_back_is_not_marked);
    RUN_TEST(test_too_many_keys_fail_before_any_write);
    RUN_TEST(test_a_value_too_long_fails);
    RUN_TEST(test_a_list_error_fails);
    return UNITY_END();
}
