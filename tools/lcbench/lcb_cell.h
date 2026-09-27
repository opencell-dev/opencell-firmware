/* lcb_cell — a minimal base-station cell for bench-testing terminals.
 *
 * Not the real scheduler (plan 4's rhu_bs is): at most LCB_CELL_MAX_TERMS
 * terminals, all on one tier and one DL/UL band pair. It follows plan 4's
 * air contract (lc_term.h) so a terminal can be exercised end to end:
 * beacon, RACH attach, AG grant, DL/UL DATA, paging, in-DL regrant, and
 * UL -> DL loopback of upper-layer payloads.
 *
 * Frame layout, radio_index 0 on each band (µs from frame start):
 *   0                   beacon, 915 EDGE, sync channel          [915]
 *   beacon_len          AG, 915 EDGE, hop slot 1 (when needed)   [915]
 *   dl_start ...        DL legs, terminal k at dl_start + k*dl   [dl band]
 *   split + turn ...    UL legs, terminal k                      [ul band]
 *   rach_off .. 120000  RACH window, 915 EDGE, hop slot 2        [915]
 * Hop slot indices: AG 1, RACH 2, terminal k DL 8+2k, UL 9+2k (as plan 4).
 * I/O-free: lcbench (and the host tests) move the messages. */
#ifndef LCB_CELL_H
#define LCB_CELL_H

#include <stdint.h>

#include "lc_air.h"
#include "lc_link.h"
#include "lc_phy.h"

#define LCB_CELL_MAX_TERMS     2u
#define LCB_CELL_SLOT_BYTES    28u  /* every leg sized for 28 air bytes (plan 4) */
#define LCB_CELL_PAYLOAD       (LCB_CELL_SLOT_BYTES - 8u)
#define LCB_CELL_RACH_PAYLOAD  8u
#define LCB_CELL_GRANT_LEAD    3u   /* effective = first frame sent + 3 */
#define LCB_CELL_GRANT_REPEATS 2u
#define LCB_CELL_KIND_FRAMES   8u   /* frames of slot bookkeeping kept per band */
#define LCB_CELL_MAX_SLOTS     8u
#define LCB_CELL_DLQ           8u   /* DL payloads queued per terminal */

typedef enum {
    LCB_SLOT_BEACON = 1,
    LCB_SLOT_AG     = 2,
    LCB_SLOT_DL     = 3,
    LCB_SLOT_UL     = 4,
    LCB_SLOT_RACH   = 5
} lcb_slot_kind_t;

/* Signalling hooks (lcbench net): UL DATA payloads and RACH UPPER payloads
 * go to these instead of the echo loop. */
typedef struct {
    void *ctx;
    void (*on_ul)(void *ctx, uint32_t tmid, const uint8_t *p, uint8_t n);
    void (*on_upper)(void *ctx, uint32_t tmid, const uint8_t *p, uint8_t n);
} lcb_cell_hooks_t;

typedef struct {
    int        used;
    uint32_t   tmid;
    uint8_t    dl_seq;
    uint8_t    loop_len;              /* payload to echo on the next DL */
    uint8_t    loop[LCB_CELL_PAYLOAD];
    int        have_cur;              /* grant in force (from cur.effective_frame) */
    lc_grant_t cur;
    int        have_next;             /* grant being delivered */
    lc_grant_t next;
    int        next_via_ag;           /* deliver in the AG slot (else in the DL slot) */
    uint8_t    next_tx_left;
    uint32_t   ul_rx;                 /* UL DATA received */
    uint32_t   loops;                 /* payloads echoed */
    uint8_t    dlq[LCB_CELL_DLQ][LCB_CELL_PAYLOAD]; /* DL payloads waiting for this terminal's DL slot */
    uint8_t    dlq_len[LCB_CELL_DLQ];
    uint8_t    dlq_head, dlq_count;
} lcb_cell_term_t;

typedef struct {
    uint32_t frame;
    uint8_t  count;
    uint8_t  kind[LCB_CELL_MAX_SLOTS];
    uint8_t  term[LCB_CELL_MAX_SLOTS];
} lcb_cell_kinds_t;

typedef struct {
    uint32_t         cell_seed;
    lc_tier_t        tier;
    lc_band_t        dl_band;
    lc_band_t        ul_band;
    int              attach_idle;  /* answer ATTACH with an empty grant (terminal stays IDLE) */
    int              fallback_915; /* a terminal re-attaching from a 2.4 GHz grant moves all
                                      grants to 915 (plan 4 re-grants a failed 2.4 link on 915) */
    int              off;          /* 1: schedule nothing (a dead cell) */
    int              part97;       /* beacons carry LC_BCN_FLAG_PART97 */
    lcb_cell_term_t  terms[LCB_CELL_MAX_TERMS];
    uint32_t         page_tmid;    /* 0: nobody paged */
    lcb_cell_hooks_t hooks;

    lcb_cell_kinds_t kinds[LC_BAND_COUNT][LCB_CELL_KIND_FRAMES];
    uint8_t          payloads[LC_BAND_COUNT][LCB_CELL_MAX_SLOTS][LC_AIR_MAX_FRAME];

    /* counters */
    uint32_t         rach_rx;
    uint32_t         attaches;
    uint32_t         page_replies;
    uint32_t         uppers;
    uint32_t         grants_sent;
} lcb_cell_t;

void lcb_cell_init(lcb_cell_t *c, uint32_t cell_seed, lc_tier_t tier, lc_band_t dl_band, lc_band_t ul_band);

/* Legs for terminal slot k on the cell's current tier and bands.
 * Returns 0, or -1 if the layout doesn't fit (legs zeroed). */
int lcb_cell_legs(const lcb_cell_t *c, uint8_t k, lc_grant_leg_t *dl, lc_grant_leg_t *ul);

/* The SCHEDULE for radio 0 on `band` for frame_number (one message, LAST
 * set). Returns 0, or -1 if the radio has nothing to do (or the cell is off). */
int lcb_cell_schedule(lcb_cell_t *c, lc_band_t band, uint32_t frame_number, lc_msg_t *out);

/* An RX_REPORT from radio 0 on `band`. */
void lcb_cell_on_rx(lcb_cell_t *c, lc_band_t band, const lc_rx_report_t *r);

/* Page tmid in every beacon until it answers with PAGE_REPLY. */
void lcb_cell_page(lcb_cell_t *c, uint32_t tmid);

/* Move every granted terminal to new bands with a GRANT sent in its DL slot
 * (like plan 4's regrant); later attaches use them too. */
void lcb_cell_set_bands(lcb_cell_t *c, lc_band_t dl_band, lc_band_t ul_band);

void lcb_cell_set_hooks(lcb_cell_t *c, const lcb_cell_hooks_t *h);
/* Queue a DL payload (<= LCB_CELL_PAYLOAD) for tmid's next DL slots. 0, or -1 (unknown terminal, full). */
int  lcb_cell_dl_push(lcb_cell_t *c, uint32_t tmid, const uint8_t *p, uint8_t n);
/* Take tmid's legs away with an empty grant (the terminal goes IDLE). */
void lcb_cell_release(lcb_cell_t *c, uint32_t tmid);
/* tmid has legs in force. */
int  lcb_cell_granted(const lcb_cell_t *c, uint32_t tmid);

#endif
