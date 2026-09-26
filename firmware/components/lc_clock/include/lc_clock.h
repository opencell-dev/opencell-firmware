/* lc_clock — maps GPS time to the local microsecond timer.
 *
 * Frame numbering (shared by W12 firmware and the Pi daemon):
 *   frame_number = (unix_ms - LC_EPOCH_UNIX_S * 1000) / 120
 * LC_EPOCH_UNIX_S is a multiple of 3 s, so frame boundaries fall on PPS
 * edges every third second.
 *
 * The W12 feeds every PPS edge (local µs) to lc_clock_on_pps() and every
 * host TIME message to lc_clock_on_time(). Once 3 consecutive 1-second edges
 * are seen and one edge is labelled with its UTC second, frame start times
 * can be computed. If PPS stops, the clock free-runs on its last measured
 * period (HOLDOVER) until holdover_us elapses, then goes UNLOCKED. */
#ifndef LC_CLOCK_H
#define LC_CLOCK_H

#include <stdint.h>

#define LC_EPOCH_UNIX_S      1767225600u /* 2026-01-01T00:00:00Z */
#define LC_PPS_NOMINAL_US    1000000u
#define LC_PPS_TOLERANCE_US  500u        /* per elapsed second */
#define LC_PPS_LOCK_EDGES    3u
#define LC_PPS_MISSING_US    1500000u    /* no edge for this long -> HOLDOVER */
#define LC_TIME_LABEL_MAX_US 900000u     /* TIME must arrive within this of the edge */
#define LC_TIME_RELABEL_COUNT 3u         /* consecutive disagreeing labels before re-anchoring */
#define LC_FRAME_WINDOW_US   3600000000LL /* frame_start only answers within ±1 h of the anchor */

typedef enum {
    LC_CLOCK_UNLOCKED = 0,
    LC_CLOCK_LOCKED   = 1,
    LC_CLOCK_HOLDOVER = 2
} lc_clock_state_t;

typedef struct {
    uint64_t last_edge_us;  /* local time of the last accepted edge */
    uint32_t period_us;     /* local µs per true second (0 = unknown) */
    uint8_t  good_edges;    /* consecutive 1-second edges */
    uint8_t  state;         /* lc_clock_state_t */
    uint8_t  have_edge;
    uint8_t  have_time;     /* anchor_unix_s is valid */
    uint32_t anchor_unix_s; /* UTC second of last_edge_us */
    uint8_t  bad_labels;    /* consecutive TIME labels that disagreed with the anchor */
    uint32_t holdover_us;
} lc_clock_t;

void lc_clock_init(lc_clock_t *c, uint32_t holdover_us);

/* A PPS edge was captured at local time edge_us. Glitches (edges early by
 * more than the tolerance) are ignored. */
void lc_clock_on_pps(lc_clock_t *c, uint64_t edge_us);

/* Host says the most recent edge marked UTC second unix_s. Returns 0 if
 * applied, -1 if there is no edge within LC_TIME_LABEL_MAX_US of now_us. */
int lc_clock_on_time(lc_clock_t *c, uint32_t unix_s, uint64_t now_us);

/* Advance LOCKED -> HOLDOVER -> UNLOCKED as time passes without edges. */
void lc_clock_tick(lc_clock_t *c, uint64_t now_us);

/* Local µs at which frame_number starts. Returns 0 on success, -1 if the
 * clock is UNLOCKED, unlabelled, or the frame is more than 1 h away. */
int lc_clock_frame_start_us(const lc_clock_t *c, uint32_t frame_number, uint64_t *out_us);

/* Frame in progress at local time now_us. Returns 0 on success, -1 as above. */
int lc_clock_frame_at(const lc_clock_t *c, uint64_t now_us, uint32_t *frame_number);

/* Pure time helpers (used by the Pi daemon and bench tools). */
uint32_t lc_frame_from_unix_us(uint64_t unix_us);
uint64_t lc_frame_start_unix_us(uint32_t frame_number);

#endif
