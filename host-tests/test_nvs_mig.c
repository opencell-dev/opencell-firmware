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
static int corrupt_data;  /* 1: every non-marker set stores a flipped first byte */
static int corrupt_marker; /* 1: the marker's set stores a flipped byte */
static int list_error;
static int fail_marker_get_once;    /* 1: the next get() of the marker key returns -2, once */
static int fail_marker_get_once_nf; /* 1: the next get() of the marker key returns -1 (NOT_FOUND), once */
static int marker_get_calls;        /* count of marker-key get() calls since the last boot */
static int fail_marker_get_at_call; /* -1: never; else the Nth marker get() call this boot returns -2, once */
static int lie_on_marker_set_once;  /* 1: the marker's set() writes for real but reports failure, once */

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
    if (strcmp(key, OC_NVS_MIG_MARKER) == 0) {
        marker_get_calls++;
        if (fail_marker_get_once) {
            fail_marker_get_once = 0;
            return -2;
        }
        if (fail_marker_get_once_nf) {
            fail_marker_get_once_nf = 0;
            return -1;
        }
        if (fail_marker_get_at_call == marker_get_calls) {
            fail_marker_get_at_call = -1;
            return -2;
        }
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
    int is_marker = strcmp(key, OC_NVS_MIG_MARKER) == 0;
    if (is_marker && lie_on_marker_set_once) {
        lie_on_marker_set_once = 0;
        put(ns, key, type, val, len); /* the write actually lands... */
        return -1;                    /* ...but is reported as failed */
    }
    if (step() != 0) {
        return -1;
    }
    put(ns, key, type, val, len);
    if (len > 0) {
        if (corrupt || (is_marker && corrupt_marker) || (!is_marker && corrupt_data)) {
            find(ns, key)->val[0] ^= 0xFFu;
        }
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
    corrupt_data = 0;
    corrupt_marker = 0;
    fail_marker_get_once = 0;
    fail_marker_get_once_nf = 0;
    marker_get_calls = 0;
    fail_marker_get_at_call = -1;
    lie_on_marker_set_once = 0;
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

/* seed_id()'s three keys, written directly into `ns`: a stale copy left
 * behind by an earlier attempt that was cut short before the marker. */
static void seed_stale_copy(const char *ns)
{
    const uint8_t one = 1;
    put(ns, "ident", T_BLOB, ident, sizeof(ident));
    put(ns, "flag", T_U8, &one, 1);
    put(ns, "name", T_STR, "T2", 3);
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
    TEST_ASSERT_EQUAL_INT(0, count("oc_id")); /* the one key copied before the failure is cleaned up */
    boot();
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    assert_holds_id("oc_id");
}

/* Round 3, N1.2: an unexplained key in `to` -- one that isn't among `from`'s
 * current keys, such as a leftover from something else entirely (here,
 * "sc_done" is really an oc_ble key, not one of lc_id's) -- means `to` did
 * not come from a cut-short copy of this `from`. A cut-short copy can only
 * ever hold keys taken from `from`, so this is not that; erasing `to` on the
 * assumption that it is would not be safe. UNSURE, both namespaces left
 * exactly as found. */
static void test_an_unexplained_key_in_to_is_unsure(void)
{
    seed_id();
    put("oc_id", "ident", T_BLOB, ident, sizeof(ident));
    put("oc_id", "sc_done", T_BLOB, cfg, 14);
    int moved;
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, r);
    TEST_ASSERT_EQUAL_INT(0, steps);
    assert_holds_id("lc_id");
    TEST_ASSERT_NOT_NULL(find("oc_id", "sc_done")); /* untouched, not silently erased */
    TEST_ASSERT_NOT_NULL(find("oc_id", "ident"));
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

/* Round 3, N1.1: listing `from` erroring means its contents were never
 * observed -- FAILED ("`from` holds everything, untouched") would be a
 * claim with no basis. UNSURE. */
static void test_a_list_error_fails(void)
{
    seed_id();
    list_error = 1;
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_EQUAL_INT(0, steps);
}

/* Review Focus (identity wipe / identity loss): a marker read that errors
 * once, on an already-completed move, must not be treated as "unmarked" --
 * that would send the move down the path that erases `to`, the only
 * remaining copy of the identity. It must also not be reported as FAILED
 * ("`from` holds everything, use `from`"): `from` is empty, so that would be
 * a lie the caller could act on by creating a fresh identity. UNSURE is the
 * only honest answer: touch neither namespace, retry next boot. */
static void test_a_marker_read_error_does_not_wipe_the_moved_identity(void)
{
    seed_id();
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    boot();
    fail_marker_get_once = 1; /* -2 */
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, r);
    TEST_ASSERT_EQUAL_INT(0, steps); /* nothing written this boot */
    assert_holds_id("oc_id");
    TEST_ASSERT_TRUE(has_marker("oc_id"));
}

/* The same, but the glitch reads back NOT_FOUND (-1) instead of an I/O
 * error (-2) -- this is the review's two-boot scenario: a moved board whose
 * marker misreads as absent. `from` (already erased by the earlier move) is
 * empty and `to` isn't, so this can only be UNSURE, never FAILED: the
 * caller must write nothing this boot and try again, not read "no marker,
 * from empty" as "there is no identity". */
static void test_a_moved_board_with_marker_misread_as_absent_is_unsure(void)
{
    seed_id();
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    boot();
    fail_marker_get_once_nf = 1; /* -1, NOT_FOUND */
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, r);
    TEST_ASSERT_EQUAL_INT(0, steps); /* the caller writes nothing */
    assert_holds_id("oc_id");
    TEST_ASSERT_TRUE(has_marker("oc_id"));
    TEST_ASSERT_EQUAL_INT(0, count("lc_id"));
    boot(); /* the glitch is gone: the next boot is clean */
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_DONE, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    assert_holds_id("oc_id");
}

/* `from` empty, `to` already holds data but no marker: not a state a real
 * cut-short copy can produce (`from` is erased only after `to` is marked).
 * Refuse to erase it; and refuse to call it FAILED, since `from` does not in
 * fact hold everything -- it holds nothing. UNSURE. */
static void test_from_empty_to_nonempty_without_marker_is_not_erased(void)
{
    put("oc_id", "ident", T_BLOB, ident, sizeof(ident));
    int moved;
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, r);
    TEST_ASSERT_EQUAL_INT(0, steps);
    TEST_ASSERT_EQUAL_INT(1, count("oc_id"));
    TEST_ASSERT_NOT_NULL(find("oc_id", "ident"));
    TEST_ASSERT_EQUAL_INT(0, count("lc_id"));
}

/* The read-back compare (step 5) catches a corrupted data key on its own,
 * separate from the marker's own read-back check. */
static void test_a_bad_data_read_back_is_not_marked(void)
{
    seed_id();
    corrupt_data = 1;
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_FAILED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_FALSE(has_marker("oc_id"));
    assert_holds_id("lc_id");
}

/* The marker's own read-back catches a corrupted marker byte even when
 * every data key copied and compared cleanly. The write already happened
 * (to `to`), so this is UNSURE, not FAILED: `from` doesn't hold everything
 * any more (its data was already copied out and, moments from now, may or
 * may not get erased depending on what a future boot's marker check finds). */
static void test_a_bad_marker_read_back_fails(void)
{
    seed_id();
    corrupt_marker = 1;
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    TEST_ASSERT_FALSE(has_marker("oc_id"));
    assert_holds_id("lc_id");
}

/* N1's third UNSURE trigger: the marker's set() itself succeeds -- the byte
 * really is 1 in `to` -- but the confirmation read right after it fails
 * (a transient glitch, not corruption). UNSURE this boot; the next boot's
 * marker check reads cleanly and the move resolves (DONE, since the byte was
 * already there), with the data intact either way. */
static void test_a_marker_set_but_unconfirmed_converges_next_boot(void)
{
    seed_id();
    fail_marker_get_at_call = 2; /* the confirmation read right after the marker's set() */
    int moved;
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, r);
    boot();
    oc_nvs_mig_result_t r2 = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_NOT_EQUAL(OC_NVS_MIG_FAILED, r2);
    TEST_ASSERT_NOT_EQUAL(OC_NVS_MIG_UNSURE, r2);
    assert_holds_id("oc_id");
    TEST_ASSERT_TRUE(has_marker("oc_id"));
    TEST_ASSERT_EQUAL_INT(0, count("lc_id"));
}

/* fail_clearing(): if the marker's set() reports failure but the write
 * actually landed (a lying write, or a success report lost after a real
 * commit), a fresh marker check finds it present after all -- finish the
 * move (MOVED) instead of erasing `to` and reporting FAILED. */
static void test_a_lying_marker_set_failure_still_finishes_the_move(void)
{
    seed_id();
    lie_on_marker_set_once = 1;
    int moved;
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, r);
    TEST_ASSERT_EQUAL_INT(3, moved);
    assert_holds_id("oc_id");
    TEST_ASSERT_TRUE(has_marker("oc_id"));
    TEST_ASSERT_EQUAL_INT(0, count("lc_id"));
}

/* A key literally named like the marker in `from`: refuse instead of
 * copying it in, which would otherwise make `to` look already-moved. */
static void test_a_key_named_like_the_marker_in_from_fails(void)
{
    const uint8_t one = 1;
    put("lc", "term", T_U8, &one, 1);
    put("lc", OC_NVS_MIG_MARKER, T_U8, &one, 1);
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_FAILED, oc_nvs_mig_move(&ops, "lc", "oc", &moved));
    TEST_ASSERT_EQUAL_INT(0, steps);
    TEST_ASSERT_EQUAL_INT(2, count("lc"));
    TEST_ASSERT_EQUAL_INT(0, count("oc"));
}

/* Review Focus 1, extended: the same power-loss sweep, but `to` already
 * holds a 3-key stale copy (an earlier attempt cut short before the
 * marker), so erase_all(to) itself now takes steps and can be cut too. */
static void test_power_lost_with_a_stale_copy_already_in_to(void)
{
    seed_id();
    seed_stale_copy("oc_id");
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    const int total = steps;
    for (int k = 0; k < total; k++) {
        memset(st, 0, sizeof(st));
        seed_id();
        seed_stale_copy("oc_id");
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

/* Round 3, "Loss 1": erase_all(from) was cut after erasing "ident" -- `to`
 * holds the complete move {ident,flag,name,oc_moved}, `from` holds only
 * {flag,name}. The next boot's marker read then misreads NOT_FOUND once.
 * Without the from/to cross-check, the top-level misread alone would send
 * this down the unmarked path with `from` looking like a 2-key namespace to
 * copy from -- exactly wrong, since `to` (4 keys) has far more than `from`
 * (2) could explain. UNSURE, nothing written, identity intact in `oc_id`. */
static void test_a_half_erased_from_with_a_marker_misread_is_unsure(void)
{
    const uint8_t one = 1;
    put("oc_id", "ident", T_BLOB, ident, sizeof(ident));
    put("oc_id", "flag", T_U8, &one, 1);
    put("oc_id", "name", T_STR, "T2", 3);
    put("oc_id", OC_NVS_MIG_MARKER, T_U8, &one, 1);
    put("lc_id", "flag", T_U8, &one, 1);
    put("lc_id", "name", T_STR, "T2", 3);
    fail_marker_get_once_nf = 1;
    int moved;
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, r);
    TEST_ASSERT_EQUAL_INT(0, steps);
    assert_holds_id("oc_id");
    TEST_ASSERT_TRUE(has_marker("oc_id"));
}

/* Round 3, "Loss 2": an already-moved board (identity fully in `oc_id`,
 * `lc_id` fully erased), a NOT_FOUND marker misread, and this time list()
 * on `from` also errors. N1.1 (list(from) < 0 is UNSURE, not FAILED) and
 * N1.2 (`to`'s own listing must check out) both independently steer this
 * away from FAILED; either alone would have been enough. UNSURE, not
 * FAILED, identity intact. */
static void test_a_moved_board_with_marker_misread_and_list_from_error_is_unsure(void)
{
    seed_id();
    int moved;
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_MOVED, oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved));
    boot();
    fail_marker_get_once_nf = 1;
    list_error = 1;
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, r);
    TEST_ASSERT_EQUAL_INT(0, steps);
    assert_holds_id("oc_id");
    TEST_ASSERT_TRUE(has_marker("oc_id"));
}

/* Round 3, N1.3: fail_clearing()'s own marker check can read UNKNOWN too --
 * a marker key corrupted independent of this attempt (a "CRC-bad" byte),
 * discovered right as a mid-copy failure (over a `from` already missing one
 * of its original keys, as if an earlier erase_all(from) had been cut) asks
 * "is it safe to erase `to`?". UNSURE, touching neither namespace, not
 * FAILED-with-an-erase. */
static void test_a_crc_bad_marker_during_a_copy_failure_is_unsure(void)
{
    const uint8_t one = 1;
    put("lc_id", "flag", T_U8, &one, 1);
    put("lc_id", "name", T_STR, "T2", 3);
    fail_once_at = 0;            /* the first key's copy fails */
    fail_marker_get_at_call = 2; /* fail_clearing()'s check, not the top-level one */
    int moved;
    oc_nvs_mig_result_t r = oc_nvs_mig_move(&ops, "lc_id", "oc_id", &moved);
    TEST_ASSERT_EQUAL_INT(OC_NVS_MIG_UNSURE, r);
    TEST_ASSERT_EQUAL_INT(0, count("oc_id")); /* never erased on UNSURE */
    TEST_ASSERT_EQUAL_INT(2, count("lc_id")); /* from untouched */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_moves_every_key_and_leaves_other_namespaces_alone);
    RUN_TEST(test_a_second_boot_writes_nothing);
    RUN_TEST(test_a_fresh_board_is_only_marked);
    RUN_TEST(test_power_lost_at_every_step);
    RUN_TEST(test_a_failed_write_keeps_the_old_namespace);
    RUN_TEST(test_an_unexplained_key_in_to_is_unsure);
    RUN_TEST(test_the_marked_namespace_wins_over_leftovers);
    RUN_TEST(test_a_bad_read_back_is_not_marked);
    RUN_TEST(test_too_many_keys_fail_before_any_write);
    RUN_TEST(test_a_value_too_long_fails);
    RUN_TEST(test_a_list_error_fails);
    RUN_TEST(test_a_marker_read_error_does_not_wipe_the_moved_identity);
    RUN_TEST(test_a_moved_board_with_marker_misread_as_absent_is_unsure);
    RUN_TEST(test_from_empty_to_nonempty_without_marker_is_not_erased);
    RUN_TEST(test_a_bad_data_read_back_is_not_marked);
    RUN_TEST(test_a_bad_marker_read_back_fails);
    RUN_TEST(test_a_marker_set_but_unconfirmed_converges_next_boot);
    RUN_TEST(test_a_lying_marker_set_failure_still_finishes_the_move);
    RUN_TEST(test_a_key_named_like_the_marker_in_from_fails);
    RUN_TEST(test_power_lost_with_a_stale_copy_already_in_to);
    RUN_TEST(test_a_half_erased_from_with_a_marker_misread_is_unsure);
    RUN_TEST(test_a_moved_board_with_marker_misread_and_list_from_error_is_unsure);
    RUN_TEST(test_a_crc_bad_marker_during_a_copy_failure_is_unsure);
    return UNITY_END();
}
