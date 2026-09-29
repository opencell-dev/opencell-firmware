/* ocb_cell — a minimal base-station cell for bench-testing terminals.
 *
 * Not the real scheduler (plan 4's rhu_bs is): at most OCB_CELL_MAX_TERMS
 * terminals, all on one tier and one DL/UL band pair. It follows plan 4's
 * air contract (oc_term.h) so a terminal can be exercised end to end:
 * beacon, RACH attach, AG grant, DL/UL DATA, paging, in-DL regrant, and
 * UL -> DL loopback of upper-layer payloads.
 *
 * Frame layout, radio_index 0 on each band (µs from frame start):
 *   0                   beacon, 915 EDGE, sync channel          [915]
 *                       (oc_sync_channel_at(sync_ch), or sync_ch itself with fixed_sync)
 *   beacon_len          AG, 915 EDGE, hop slot 1 (when needed)   [915]
 *   dl_start ...        DL legs, terminal k at dl_start + k*dl   [dl band]
 *   split + turn ...    UL legs, terminal k                      [ul band]
 *   rach_off .. 120000  RACH window, 915 EDGE, hop slot 2        [915]
 * Hop slot indices: AG 1, RACH 2, terminal k DL 8+2k, UL 9+2k (as plan 4).
 * I/O-free: ocbench (and the host tests) move the messages. */
#ifndef OCB_CELL_H
#define OCB_CELL_H

#include <stdint.h>

#include "oc_air.h"
#include "oc_link.h"
#include "oc_phy.h"

#define OCB_CELL_MAX_TERMS     2u
#define OCB_CELL_SLOT_BYTES    28u  /* every leg sized for 28 air bytes (plan 4) */
#define OCB_CELL_PAYLOAD       (OCB_CELL_SLOT_BYTES - 8u)
#define OCB_CELL_RACH_PAYLOAD  8u
#define OCB_CELL_GRANT_LEAD    3u   /* effective = first frame sent + 3 */
#define OCB_CELL_GRANT_REPEATS 2u
#define OCB_CELL_KIND_FRAMES   8u   /* frames of slot bookkeeping kept per band */
#define OCB_CELL_MAX_SLOTS     8u
#define OCB_CELL_DLQ           8u   /* DL payloads queued per terminal */

typedef enum {
    OCB_SLOT_BEACON = 1,
    OCB_SLOT_AG     = 2,
    OCB_SLOT_DL     = 3,
    OCB_SLOT_UL     = 4,
    OCB_SLOT_RACH   = 5
} ocb_slot_kind_t;

/* Signalling hooks (ocbench net): UL DATA payloads and RACH UPPER payloads
 * go to these instead of the echo loop. */
typedef struct {
    void *ctx;
    void (*on_ul)(void *ctx, uint32_t tmid, const uint8_t *p, uint8_t n);
    void (*on_upper)(void *ctx, uint32_t tmid, const uint8_t *p, uint8_t n);
} ocb_cell_hooks_t;

typedef struct {
    int        used;
    uint32_t   tmid;
    uint8_t    dl_seq;
    uint8_t    loop_len;              /* payload to echo on the next DL */
    uint8_t    loop[OCB_CELL_PAYLOAD];
    int        have_cur;              /* grant in force (from cur.effective_frame) */
    oc_grant_t cur;
    int        have_next;             /* grant being delivered */
    oc_grant_t next;
    int        next_via_ag;           /* deliver in the AG slot (else in the DL slot) */
    uint8_t    next_tx_left;
    uint32_t   ul_rx;                 /* UL DATA received */
    uint32_t   loops;                 /* payloads echoed */
    uint8_t    dlq[OCB_CELL_DLQ][OCB_CELL_PAYLOAD]; /* DL payloads waiting for this terminal's DL slot */
    uint8_t    dlq_len[OCB_CELL_DLQ];
    uint8_t    dlq_head, dlq_count;
} ocb_cell_term_t;

typedef struct {
    uint32_t frame;
    uint8_t  count;
    uint8_t  kind[OCB_CELL_MAX_SLOTS];
    uint8_t  term[OCB_CELL_MAX_SLOTS];
} ocb_cell_kinds_t;

typedef struct {
    uint32_t         cell_seed;
    oc_tier_t        tier;
    oc_band_t        dl_band;
    oc_band_t        ul_band;
    int              attach_idle;  /* answer ATTACH with an empty grant (terminal stays IDLE) */
    int              fallback_915; /* a terminal re-attaching from a 2.4 GHz grant moves all
                                      grants to 915 (plan 4 re-grants a failed 2.4 link on 915) */
    int              off;          /* 1: schedule nothing (a dead cell) */
    int              part97;       /* beacons carry OC_BCN_FLAG_PART97 */
    uint8_t          sync_ch;      /* anchor: the 915 sync channel of frames f % 8 == 0 (default seed % 6) */
    int              fixed_sync;   /* every beacon on sync_ch (Part 97 only; see ocb_cell_set_sync) */
    uint8_t          cfg_ver;      /* channel-list version mod 4, in every beacon */
    ocb_cell_term_t  terms[OCB_CELL_MAX_TERMS];
    uint32_t         page_tmid;    /* 0: nobody paged */
    ocb_cell_hooks_t hooks;

    ocb_cell_kinds_t kinds[OC_BAND_COUNT][OCB_CELL_KIND_FRAMES];
    uint8_t          payloads[OC_BAND_COUNT][OCB_CELL_MAX_SLOTS][OC_AIR_MAX_FRAME];

    /* counters */
    uint32_t         rach_rx;
    uint32_t         attaches;
    uint32_t         page_replies;
    uint32_t         uppers;
    uint32_t         grants_sent;
} ocb_cell_t;

void ocb_cell_init(ocb_cell_t *c, uint32_t cell_seed, oc_tier_t tier, oc_band_t dl_band, oc_band_t ul_band);

/* Operator-set anchor and sync pattern (channel-list spec §4.1). Checked with
 * oc_sync_anchor_ok against the cell's mode (part97), so set part97 first.
 * 0, or -1 (refused: the cell keeps its sync). */
int ocb_cell_set_sync(ocb_cell_t *c, uint8_t sync_ch, int fixed);

/* Legs for terminal slot k on the cell's current tier and bands.
 * Returns 0, or -1 if the layout doesn't fit (legs zeroed). */
int ocb_cell_legs(const ocb_cell_t *c, uint8_t k, oc_grant_leg_t *dl, oc_grant_leg_t *ul);

/* The SCHEDULE for radio 0 on `band` for frame_number (one message, LAST
 * set). Returns 0, or -1 if the radio has nothing to do (or the cell is off). */
int ocb_cell_schedule(ocb_cell_t *c, oc_band_t band, uint32_t frame_number, oc_msg_t *out);

/* An RX_REPORT from radio 0 on `band`. */
void ocb_cell_on_rx(ocb_cell_t *c, oc_band_t band, const oc_rx_report_t *r);

/* Page tmid in every beacon until it answers with PAGE_REPLY. */
void ocb_cell_page(ocb_cell_t *c, uint32_t tmid);

/* Move every granted terminal to new bands with a GRANT sent in its DL slot
 * (like plan 4's regrant); later attaches use them too. */
void ocb_cell_set_bands(ocb_cell_t *c, oc_band_t dl_band, oc_band_t ul_band);

void ocb_cell_set_hooks(ocb_cell_t *c, const ocb_cell_hooks_t *h);
/* Queue a DL payload (<= OCB_CELL_PAYLOAD) for tmid's next DL slots. 0, or -1 (unknown terminal, full). */
int  ocb_cell_dl_push(ocb_cell_t *c, uint32_t tmid, const uint8_t *p, uint8_t n);
/* Take tmid's legs away with an empty grant (the terminal goes IDLE). */
void ocb_cell_release(ocb_cell_t *c, uint32_t tmid);
/* tmid has legs in force. */
int  ocb_cell_granted(const ocb_cell_t *c, uint32_t tmid);

#endif
