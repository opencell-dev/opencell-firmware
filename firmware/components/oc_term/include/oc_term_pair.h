/* oc_term_pair — the terminal's BLE pairing code (spec 2026-09-27-ble-pairing-design.md §2).
 *
 * A uniformly random 6-digit passkey, drawn at boot, after every phone
 * disconnect and after every failed pairing attempt. Three failed attempts
 * within OC_PAIR_FAIL_WINDOW_US lock pairing for OC_PAIR_LOCK_US. The random
 * source is injected (esp_random on the terminal, a script in the tests).
 * Portable: no ESP-IDF. Not thread-safe: the caller serializes. */
#ifndef OC_TERM_PAIR_H
#define OC_TERM_PAIR_H

#include <stdint.h>

#define OC_PAIR_CODES          1000000u     /* 000000..999999 */
#define OC_PAIR_MAX_FAILS      3u
#define OC_PAIR_FAIL_WINDOW_US 60000000ull  /* 3 failures within 60 s ... */
#define OC_PAIR_LOCK_US        60000000ull  /* ... lock pairing for 60 s */

typedef uint32_t (*oc_pair_rand_fn)(void *ctx);

typedef struct {
    oc_pair_rand_fn rand;
    void           *rand_ctx;
    uint32_t        code;
    uint64_t        fail_at[OC_PAIR_MAX_FAILS]; /* recent failures, oldest first */
    uint8_t         fails;
    uint64_t        locked_until;               /* 0: not locked */
} oc_term_pair_t;

/* A uniform draw from 0..999999 by rejection sampling on 32-bit values. */
uint32_t oc_term_pair_draw(oc_pair_rand_fn rand, void *ctx);

void     oc_term_pair_init(oc_term_pair_t *p, oc_pair_rand_fn rand, void *ctx); /* draws the boot code */
uint32_t oc_term_pair_code(const oc_term_pair_t *p);
void     oc_term_pair_disconnected(oc_term_pair_t *p);           /* a phone disconnected: new code */
void     oc_term_pair_failed(oc_term_pair_t *p, uint64_t now_us); /* new code; may start the lock-out */
void     oc_term_pair_succeeded(oc_term_pair_t *p);               /* forgets earlier failures */
int      oc_term_pair_locked(const oc_term_pair_t *p, uint64_t now_us);
/* Seconds of lock-out left, rounded up (0 when not locked). */
uint32_t oc_term_pair_lock_left_s(const oc_term_pair_t *p, uint64_t now_us);

#endif
