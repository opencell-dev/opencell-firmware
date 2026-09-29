/* oc_term — the W12 terminal's radio layer (spec §3.4, §4.1–4.6).
 *
 * Portable C: no ESP-IDF, no malloc, no floating point. The firmware calls
 * oc_term_step() from one task with the local µs clock; the radio is driven
 * through plan 2's oc_radio_ops_t, so host tests run it against a fake.
 *
 * States:
 *   SEARCH     no timing. Continuous RX on one 915 frequency from the scan
 *              list (oc_term_scan.h; channel-list spec §5) for its dwell
 *              (1.2 s, or 0.36 s for a FIXED entry), then the next. Everything
 *              heard is kept per scan pass (one round of the list): the
 *              strongest packet, CRC good or not, from any cell, and the
 *              noise floor (radio op rssi_inst, sampled once per dwell).
 *              Status shows the last full pass merged with the one in
 *              progress, so a packet stays shown for one to two passes
 *              (spec 2026-09-27-ble-pairing-design.md §3.1). on_status fires
 *              at the start of every dwell.
 *   SYNCED     frame timing from beacons; waiting for ACCEPTING_ATTACH.
 *   ATTACHING  RACH ATTACH sent in the RACH window after a random backoff;
 *              listening for a GRANT in the access-grant (AG) slot.
 *   IDLE       attached, no slots. Answers pages with PAGE_REPLY.
 *   GRANTED    DL/UL legs of the current GRANT run every frame.
 *
 * Timing: there is no PPS. Every packet received at a known frame offset
 * (beacon, AG, DL) is a timing observation that disciplines a phase +
 * frequency tracker. No observation for OC_TERM_SYNC_LOSS_FRAMES -> SEARCH.
 *
 * Air contract shared with the Pi scheduler (plan 4, rhu/include/rhu_bs.h):
 *   - Beacon: 915 band, radio 0, EDGE tier, offset 0, slot
 *     oc_term_beacon_len_us(), on oc_sync_channel_at(beacon.anchor) - or on
 *     the anchor itself in every frame when OC_BCN_FLAG_FIXED_SYNC is set,
 *     which a terminal only believes with OC_BCN_FLAG_PART97. A cell is its
 *     (cell_seed, anchor) pair. (2.4 beacons, MID tier, are ignored.)
 *   - Access grant (AG): right after the 915 beacon, EDGE tier, channel
 *     oc_hop_channel(seed, 915, 0, frame, OC_TERM_AG_SLOT_INDEX). Carries the
 *     GRANT answering a RACH ATTACH / PAGE_REPLY; the terminal listens for
 *     OC_TERM_AG_FRAMES frames after its RACH.
 *   - RACH: 915, radio 0, EDGE, channel oc_hop_channel(seed, 915, 0, frame,
 *     beacon.rach_slot_index), sent at the start of the beacon's RACH window.
 *     Payload <= OC_TERM_RACH_MAX_PAYLOAD.
 *   - Grants may repeat (sent twice); duplicates are ignored. Both legs empty
 *     = revoke. A GRANT may also arrive in the terminal's own DL slot.
 *   - The host sends DL DATA (or a GRANT) in every granted frame; the
 *     terminal sends UL DATA every granted frame (empty = keepalive).
 *   - Leg cores [offset, offset+len) include the trailing guard; DL and UL of
 *     one grant must be OC_TERM_BAND_SWITCH_US apart when on different bands.
 *   - OC_TERM_DL_LOSS_FRAMES consecutive DL misses -> the terminal drops the
 *     grant and re-attaches through 915 RACH (spec §4.6 fallback). */
#ifndef OC_TERM_H
#define OC_TERM_H

#include <stdint.h>

#include "oc_air.h"
#include "oc_phy.h"
#include "oc_radio_if.h"
#include "oc_term_scan.h"

#define OC_TERM_SEARCH_DWELL_US   OC_SCAN_DWELL_US /* 10 frames: covers the 8-frame sync cycle */
#define OC_TERM_NOISE_LEAD_US     50000u   /* noise floor sampled this long before a dwell ends */
#define OC_TERM_NO_DBM            0        /* dBm field "no reading": real readings are negative */
#define OC_TERM_SYNC_LOSS_FRAMES  25u      /* ~3 s without a timing observation */
#define OC_TERM_DL_LOSS_FRAMES    8u       /* consecutive missed DL slots -> drop grant */
#define OC_TERM_AG_FRAMES         6u       /* frames after RACH to listen in the AG slot */
#define OC_TERM_AG_SLOT_INDEX     1u       /* == RHU_AG_SLOT_INDEX (plan 4) */
#define OC_TERM_AG_BYTES          26u      /* AG slot sized like plan 4 (GRANT is 25) */
#define OC_TERM_RACH_MAX_PAYLOAD  8u       /* == RHU_RACH_MAX_PAYLOAD (plan 4) */
#define OC_TERM_DATA_MAX_PAYLOAD  20u      /* legs are sized for 28 air bytes (plan 4 RHU_SLOT_BYTES) */
#define OC_TERM_BACKOFF_MIN       4u       /* initial RACH backoff window, frames */
#define OC_TERM_BACKOFF_MAX       64u
#define OC_TERM_PAGE_TRIES        4u
#define OC_TERM_RX_MARGIN_US      300u     /* RX opens this early and closes this late */
#define OC_TERM_RESYNC_US         2000u    /* observation further off than this re-anchors */
/* The radio's reconfiguration times (bench 2026-09-26), equal to oc_exec's:
 * the terminal's ops on different bands (or LoRa vs FLRC) need this much
 * idle radio between them, and each op is configured this long ahead. */
#define OC_TERM_BAND_SWITCH_US    12000u   /* sub-GHz <-> 2.4 GHz; == OC_EXEC_BAND_SWITCH_LEAD_US */
#define OC_TERM_CONFIG_LEAD_US    1200u    /* same band and modulation; == OC_EXEC_CONFIG_LEAD_US */
#define OC_TERM_MOD_SWITCH_US     4000u    /* LoRa <-> FLRC; == OC_EXEC_MOD_SWITCH_LEAD_US */
#define OC_TERM_LATE_US           100u     /* later than this after op start: skip op */
#define OC_TERM_OVERRUN_US        2000u    /* op still busy this long after its end: stop */
#define OC_TERM_IDLE_US           10000u
#define OC_TERM_UPQ_DEPTH         4u
#define OC_TERM_MAX_OPS           5u

typedef enum {
    OC_TERM_SEARCH    = 0,
    OC_TERM_SYNCED    = 1,
    OC_TERM_ATTACHING = 2,
    OC_TERM_IDLE      = 3,
    OC_TERM_GRANTED   = 4
} oc_term_state_t;

/* Operation kinds, in priority order (lower value wins a conflict). */
typedef enum {
    OC_TOP_DL_RX     = 0,
    OC_TOP_UL_TX     = 1,
    OC_TOP_AG_RX     = 2,
    OC_TOP_RACH_TX   = 3,
    OC_TOP_BEACON_RX = 4
} oc_term_op_kind_t;

/* All times in µs from the frame start. The core is the slot as the base
 * station sees it (packet starts at nominal_us; core_len includes the
 * trailing guard). start/len are when the terminal actually runs the op:
 * TX = the core; RX = the core widened by OC_TERM_RX_MARGIN_US each side,
 * then clipped so neighbouring ops never overlap. */
typedef struct {
    uint8_t          kind;       /* oc_term_op_kind_t */
    uint8_t          band;
    int32_t          nominal_us;
    uint32_t         core_len_us;
    int32_t          start_us;
    uint32_t         len_us;
    uint32_t         freq_hz;
    const oc_mode_t *mode;
} oc_term_op_t;

typedef struct {
    void *ctx;
    /* Opaque upper-layer payload from a DL DATA (empty payloads are not passed up). */
    void (*on_downlink)(void *ctx, const uint8_t *data, uint8_t len);
    /* State, band, tier or link quality changed. */
    void (*on_status)(void *ctx);
    uint32_t (*rand32)(void *ctx);
} oc_term_sink_t;

typedef struct {
    int      valid;
    uint32_t anchor_frame;
    uint64_t anchor_us;
    int64_t  period_q16; /* local µs per frame, Q16 */
    uint32_t last_obs_frame;
} oc_term_tracker_t;

typedef struct {
    uint8_t len;
    uint8_t data[OC_TERM_DATA_MAX_PAYLOAD];
} oc_term_upmsg_t;

typedef struct {
    uint8_t  state;
    uint8_t  band;      /* band of the DL leg (or 915 when not granted) */
    uint8_t  tier;      /* tier of the DL leg (edge when not granted) */
    int16_t  rssi_dbm;  /* last good packet; SEARCH: the strongest heard (0 none) */
    int16_t  snr_qdb;   /* with rssi_dbm (0.25 dB); FLRC reports 0 */
    uint32_t tmid;
    uint32_t frame;
    uint32_t cell_seed;
    /* SEARCH only, otherwise 0 (spec 2026-09-27 §3.1). Not in BLE STATUS. */
    uint8_t  heard;       /* 1: rssi_dbm/snr_qdb are a packet the scan heard */
    uint16_t heard_age_s; /* seconds since the scan last heard a packet */
    int16_t  noise_dbm;   /* lowest instantaneous RSSI of the scan; OC_TERM_NO_DBM none */
    /* The scan (channel-list spec §9). SEARCH: the dwell in progress; otherwise
     * scan_* are 0 and freq_khz is the serving cell's anchor. */
    uint8_t  scan_pos;    /* 1-based position in the round; 0 not searching */
    uint8_t  scan_len;    /* the round's length (list + this round's sweep) */
    uint8_t  scan_src;    /* OC_SCAN_SRC_* */
    uint8_t  scan_pass;   /* 1-based round of this search (not in BLE STATUS) */
    uint32_t freq_khz;    /* 0 before the first dwell */
} oc_term_status_t;

/* What one search pass heard (see SEARCH above). */
typedef struct {
    uint8_t heard;
    int16_t rssi_dbm;  /* strongest packet */
    int16_t snr_qdb;   /* that packet's SNR */
    int16_t noise_dbm; /* lowest rssi_inst sample; OC_TERM_NO_DBM none */
} oc_term_heard_t;

typedef struct {
    oc_radio_ops_t    radio;
    oc_term_sink_t    sink;
    uint32_t          tmid;
    uint8_t           state;

    /* cell */
    uint32_t          cell_seed;
    uint8_t           anchor;       /* the cell's anchor channel (beacon.anchor) */
    uint8_t           fixed_sync;   /* 1: its beacons are all on the anchor */
    oc_beacon_t       beacon;       /* last beacon (RACH window, flags) */
    oc_term_tracker_t trk;

    /* search */
    oc_term_scan_t    scan;         /* the scan list and where the walk is */
    uint32_t          search_freq;  /* the dwell in progress */
    uint64_t          search_until_us;
    int               search_active;
    int               noise_sampled; /* in this dwell */
    oc_term_heard_t   scan_cur;      /* the pass in progress */
    oc_term_heard_t   scan_prev;     /* the last full pass */
    uint64_t          heard_us;      /* local time the scan last heard a packet */
    uint64_t          last_step_us;  /* now_us of the last oc_term_step (for ages) */

    /* RACH procedure */
    int               rach_pending;
    uint8_t           rach_kind;
    uint32_t          rach_frame;    /* frame to transmit in */
    uint32_t          ag_until;      /* listen in the AG slot through this frame */
    int               ag_waiting;
    uint32_t          backoff;       /* current window, frames */
    uint8_t           page_tries;
    uint8_t           rach_len;
    uint8_t           rach_payload[OC_TERM_RACH_MAX_PAYLOAD];

    /* grants */
    int               have_grant;
    oc_grant_t        grant;         /* in force */
    int               have_next;
    oc_grant_t        next;          /* takes effect at next.effective_frame */
    uint32_t          dl_miss;

    /* uplink queue */
    oc_term_upmsg_t   upq[OC_TERM_UPQ_DEPTH];
    uint8_t           upq_head;
    uint8_t           upq_count;
    uint8_t           ul_seq;

    /* executor */
    int               have_frame;
    uint32_t          cur_frame;
    uint64_t          cur_start_us;
    oc_term_op_t      ops[OC_TERM_MAX_OPS];
    uint8_t           op_count;
    uint8_t           op_idx;
    uint8_t           phase;         /* 0 config, 1 launch, 2 active */
    uint64_t          irq_us;        /* set by oc_term_note_irq(); 0 = none */
    uint8_t           tx_buf[OC_AIR_MAX_FRAME];
    oc_radio_event_t  ev;

    /* link quality + counters */
    int16_t           rssi_dbm;
    int16_t           snr_qdb;
    uint32_t          beacons;
    uint32_t          bad_beacons;  /* FIXED without PART97: never followed */
    uint32_t          bad_grants;
    uint32_t          skipped_ops;
    int8_t            radio_band;   /* band and modulation last configured; -1 unknown */
    int8_t            radio_mod;
    uint32_t          overruns;
    uint32_t          radio_errors;
    uint32_t          sync_losses;
} oc_term_t;

void oc_term_init(oc_term_t *t, const oc_radio_ops_t *radio, const oc_term_sink_t *sink, uint32_t tmid);

/* Run everything due at local time now_us. Returns the local µs at which to
 * call again. */
uint64_t oc_term_step(oc_term_t *t, uint64_t now_us);

/* The radio IRQ line rose at local time irq_us (called from the ISR glue).
 * The next event polled uses it as its timestamp. */
void oc_term_note_irq(oc_term_t *t, uint64_t irq_us);

/* Queue an opaque upper-layer payload. GRANTED with an UL leg: sent as UL
 * DATA (len <= OC_TERM_DATA_MAX_PAYLOAD and it fits the slot). Otherwise, if attached (IDLE) and len <=
 * OC_TERM_RACH_MAX_PAYLOAD: sent as RACH UPPER. Returns 0 if queued, -1 if
 * not possible now. */
int oc_term_send_upper(oc_term_t *t, const uint8_t *data, uint8_t len);

void oc_term_status(const oc_term_t *t, oc_term_status_t *out);

/* ---- pieces exposed for tests and the firmware glue ---- */

/* 915 beacon slot length (the AG slot starts here) and AG slot length. */
uint32_t oc_term_beacon_len_us(void);
uint32_t oc_term_ag_len_us(void);

/* Build the operations for frame_number, conflict-resolved and sorted by
 * start. Returns the count. */
uint8_t oc_term_build_plan(const oc_term_t *t, uint32_t frame_number, oc_term_op_t *ops);

/* Validate a grant: legs well-formed, UL fits an empty DATA, and the leg
 * cores don't overlap (OC_TERM_BAND_SWITCH_US apart across bands). */
int oc_term_grant_ok(const oc_grant_t *g);

/* Tracker. */
void     oc_term_trk_observe(oc_term_tracker_t *k, uint32_t frame_number, uint64_t start_us);
uint64_t oc_term_trk_frame_start(const oc_term_tracker_t *k, uint32_t frame_number);
uint32_t oc_term_trk_frame_at(const oc_term_tracker_t *k, uint64_t now_us);

/* TMID from the ESP32-S3 factory MAC: the low 32 bits, big-endian; 0 and
 * 0xFFFFFFFF (reserved) are remapped. */
uint32_t oc_term_tmid_from_mac(const uint8_t mac[6]);

#endif
