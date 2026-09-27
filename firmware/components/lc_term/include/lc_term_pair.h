/* lc_term_pair — the terminal's BLE pairing code (spec 2026-09-27-ble-pairing-design.md §2).
 *
 * A uniformly random 6-digit passkey, drawn at boot, after every phone
 * disconnect and after every failed pairing attempt. Three failed attempts
 * within LC_PAIR_FAIL_WINDOW_US lock pairing for LC_PAIR_LOCK_US. The random
 * source is injected (esp_random on the terminal, a script in the tests).
 * Portable: no ESP-IDF. Not thread-safe: the caller serializes. */
#ifndef LC_TERM_PAIR_H
#define LC_TERM_PAIR_H

#include <stdint.h>

#define LC_PAIR_CODES          1000000u     /* 000000..999999 */
#define LC_PAIR_MAX_FAILS      3u
#define LC_PAIR_FAIL_WINDOW_US 60000000ull  /* 3 failures within 60 s ... */
#define LC_PAIR_LOCK_US        60000000ull  /* ... lock pairing for 60 s */

typedef uint32_t (*lc_pair_rand_fn)(void *ctx);

typedef struct {
    lc_pair_rand_fn rand;
    void           *rand_ctx;
    uint32_t        code;
    uint64_t        fail_at[LC_PAIR_MAX_FAILS]; /* recent failures, oldest first */
    uint8_t         fails;
    uint64_t        locked_until;               /* 0: not locked */
} lc_term_pair_t;

/* A uniform draw from 0..999999 by rejection sampling on 32-bit values. */
uint32_t lc_term_pair_draw(lc_pair_rand_fn rand, void *ctx);

void     lc_term_pair_init(lc_term_pair_t *p, lc_pair_rand_fn rand, void *ctx); /* draws the boot code */
uint32_t lc_term_pair_code(const lc_term_pair_t *p);
void     lc_term_pair_disconnected(lc_term_pair_t *p);           /* a phone disconnected: new code */
void     lc_term_pair_failed(lc_term_pair_t *p, uint64_t now_us); /* new code; may start the lock-out */
void     lc_term_pair_succeeded(lc_term_pair_t *p);               /* forgets earlier failures */
int      lc_term_pair_locked(const lc_term_pair_t *p, uint64_t now_us);
/* Seconds of lock-out left, rounded up (0 when not locked). */
uint32_t lc_term_pair_lock_left_s(const lc_term_pair_t *p, uint64_t now_us);

#endif
