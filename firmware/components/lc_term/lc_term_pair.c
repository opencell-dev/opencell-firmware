#include "lc_term_pair.h"

#include <string.h>

/* 4294 * 10^6, the largest multiple of 10^6 below 2^32: values at or above it
 * are redrawn so that every code is equally likely. */
#define ACCEPT_BELOW 4294000000u

uint32_t lc_term_pair_draw(lc_pair_rand_fn rand, void *ctx)
{
    uint32_t r;
    do {
        r = rand(ctx);
    } while (r >= ACCEPT_BELOW);
    return r % LC_PAIR_CODES;
}

void lc_term_pair_init(lc_term_pair_t *p, lc_pair_rand_fn rand, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->rand = rand;
    p->rand_ctx = ctx;
    p->code = lc_term_pair_draw(rand, ctx);
}

uint32_t lc_term_pair_code(const lc_term_pair_t *p)
{
    return p->code;
}

void lc_term_pair_disconnected(lc_term_pair_t *p)
{
    p->code = lc_term_pair_draw(p->rand, p->rand_ctx);
}

void lc_term_pair_failed(lc_term_pair_t *p, uint64_t now_us)
{
    p->code = lc_term_pair_draw(p->rand, p->rand_ctx);
    /* Keep only the failures inside the window, then add this one. */
    uint8_t kept = 0;
    for (uint8_t i = 0; i < p->fails; i++) {
        if (now_us - p->fail_at[i] < LC_PAIR_FAIL_WINDOW_US) {
            p->fail_at[kept++] = p->fail_at[i];
        }
    }
    p->fails = kept; /* < LC_PAIR_MAX_FAILS: reaching it locks and resets */
    p->fail_at[p->fails++] = now_us;
    if (p->fails >= LC_PAIR_MAX_FAILS) {
        p->locked_until = now_us + LC_PAIR_LOCK_US;
        p->fails = 0; /* the lock-out is the penalty; afterwards the count starts over */
    }
}

void lc_term_pair_succeeded(lc_term_pair_t *p)
{
    p->fails = 0;
}

int lc_term_pair_locked(const lc_term_pair_t *p, uint64_t now_us)
{
    return p->locked_until != 0 && now_us < p->locked_until;
}

uint32_t lc_term_pair_lock_left_s(const lc_term_pair_t *p, uint64_t now_us)
{
    if (!lc_term_pair_locked(p, now_us)) {
        return 0;
    }
    return (uint32_t)((p->locked_until - now_us + 999999u) / 1000000u);
}
