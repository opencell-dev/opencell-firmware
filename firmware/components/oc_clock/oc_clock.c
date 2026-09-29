#include "oc_clock.h"

#include "oc_phy.h"

void oc_clock_init(oc_clock_t *c, uint32_t holdover_us)
{
    *c = (oc_clock_t){ 0 };
    c->holdover_us = holdover_us;
}

void oc_clock_on_pps(oc_clock_t *c, uint64_t edge_us)
{
    if (!c->have_edge) {
        c->last_edge_us = edge_us;
        c->have_edge = 1;
        c->good_edges = 1;
        return;
    }
    if (edge_us <= c->last_edge_us) {
        return;
    }
    uint64_t delta = edge_us - c->last_edge_us;
    uint32_t nominal = c->period_us ? c->period_us : OC_PPS_NOMINAL_US;
    uint64_t n = (delta + nominal / 2) / nominal; /* whole seconds elapsed */

    if (n == 0) {
        return; /* glitch: early edge */
    }
    uint64_t expected = n * nominal;
    uint64_t err = delta > expected ? delta - expected : expected - delta;
    if (err > n * OC_PPS_TOLERANCE_US) {
        if (delta < expected) {
            return; /* glitch between real edges */
        }
        /* Edge doesn't fit the second grid: restart, time label is stale. */
        c->last_edge_us = edge_us;
        c->good_edges = 1;
        c->have_time = 0;
        if (c->state == OC_CLOCK_LOCKED) {
            c->state = OC_CLOCK_HOLDOVER;
        }
        return;
    }

    if (n == 1) {
        c->period_us = c->period_us ? (uint32_t)((3ull * c->period_us + delta) / 4u) : (uint32_t)delta;
        if (c->good_edges < 255) {
            c->good_edges++;
        }
    } else {
        c->good_edges = 1; /* gap: keep period, need fresh consecutive edges */
    }
    if (c->have_time) {
        c->anchor_unix_s += (uint32_t)n;
    }
    c->last_edge_us = edge_us;
    if (c->good_edges >= OC_PPS_LOCK_EDGES) {
        c->state = OC_CLOCK_LOCKED;
    }
}

int oc_clock_on_time(oc_clock_t *c, uint32_t unix_s, uint64_t now_us)
{
    if (!c->have_edge || now_us < c->last_edge_us || now_us - c->last_edge_us > OC_TIME_LABEL_MAX_US) {
        return -1;
    }
    /* Once labelled, a disagreeing label is most likely stale (processed after
     * the next edge): jumping the timebase by a second would run slots at the
     * wrong time. Re-anchor only if the host keeps disagreeing. */
    if (c->have_time && unix_s != c->anchor_unix_s) {
        if (++c->bad_labels < OC_TIME_RELABEL_COUNT) {
            return -1;
        }
    }
    c->bad_labels = 0;
    c->anchor_unix_s = unix_s;
    c->have_time = 1;
    return 0;
}

void oc_clock_tick(oc_clock_t *c, uint64_t now_us)
{
    if (!c->have_edge || now_us < c->last_edge_us) {
        return;
    }
    uint64_t since = now_us - c->last_edge_us;
    if (c->state == OC_CLOCK_LOCKED && since > OC_PPS_MISSING_US) {
        c->state = OC_CLOCK_HOLDOVER;
        c->good_edges = 0;
    }
    if (c->state == OC_CLOCK_HOLDOVER && since > c->holdover_us) {
        c->state = OC_CLOCK_UNLOCKED;
        c->have_time = 0;
    }
}

static int usable(const oc_clock_t *c)
{
    return c->state != OC_CLOCK_UNLOCKED && c->have_time && c->period_us != 0 &&
           c->anchor_unix_s >= OC_EPOCH_UNIX_S;
}

int oc_clock_frame_start_us(const oc_clock_t *c, uint32_t frame_number, uint64_t *out_us)
{
    if (!usable(c)) {
        return -1;
    }
    /* True µs from the anchor edge to the frame start. */
    int64_t anchor_true = (int64_t)(c->anchor_unix_s - OC_EPOCH_UNIX_S) * 1000000LL;
    int64_t dt = (int64_t)frame_number * (int64_t)OC_FRAME_US - anchor_true;
    if (dt > OC_FRAME_WINDOW_US || dt < -OC_FRAME_WINDOW_US) {
        return -1;
    }
    int64_t local_dt = dt * (int64_t)c->period_us / 1000000LL;
    int64_t t = (int64_t)c->last_edge_us + local_dt;
    if (t < 0) {
        return -1;
    }
    *out_us = (uint64_t)t;
    return 0;
}

int oc_clock_frame_at(const oc_clock_t *c, uint64_t now_us, uint32_t *frame_number)
{
    if (!usable(c)) {
        return -1;
    }
    int64_t local_dt = (int64_t)now_us - (int64_t)c->last_edge_us;
    if (local_dt > OC_FRAME_WINDOW_US || local_dt < -OC_FRAME_WINDOW_US) {
        return -1;
    }
    int64_t true_dt = local_dt * 1000000LL / (int64_t)c->period_us;
    int64_t t = (int64_t)(c->anchor_unix_s - OC_EPOCH_UNIX_S) * 1000000LL + true_dt;
    if (t < 0) {
        return -1;
    }
    *frame_number = (uint32_t)(t / (int64_t)OC_FRAME_US);
    return 0;
}

uint32_t oc_frame_from_unix_us(uint64_t unix_us)
{
    uint64_t epoch_us = (uint64_t)OC_EPOCH_UNIX_S * 1000000u;
    if (unix_us < epoch_us) {
        return 0;
    }
    return (uint32_t)((unix_us - epoch_us) / OC_FRAME_US);
}

uint64_t oc_frame_start_unix_us(uint32_t frame_number)
{
    return (uint64_t)OC_EPOCH_UNIX_S * 1000000u + (uint64_t)frame_number * OC_FRAME_US;
}
