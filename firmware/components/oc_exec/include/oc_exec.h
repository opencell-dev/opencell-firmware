/* oc_exec — the bs-radio slot executor.
 *
 * The host sends one frame's slots as one or more SCHEDULE messages (the last
 * has OC_SCHED_FLAG_LAST). oc_exec_add_part() validates and stores them,
 * copying payloads (message payload pointers don't outlive the UART frame).
 * oc_exec_step() is called by the firmware's executor task; it configures,
 * stages and launches each slot at its time and reports received packets. */
#ifndef OC_EXEC_H
#define OC_EXEC_H

#include <stdint.h>

#include "oc_clock.h"
#include "oc_link.h"
#include "oc_phy.h"
#include "oc_radio_if.h"

#define OC_EXEC_FRAMES        4u
#define OC_EXEC_MAX_SLOTS     64u
#define OC_EXEC_PAYLOAD_POOL  4096u
#define OC_EXEC_SETUP_US      1500u /* a frame's schedule must be complete this long before it starts */
#define OC_EXEC_MAX_AHEAD     3u    /* frames ahead of the current one that may be scheduled (a host that only
                                        * estimates the board frame schedules 3 ahead: 2-3 frames of real lead) */
/* A slot on the other band than the last configured one: the LR2021 moves
 * RX path, PA and DC-DC settings. SetRxPath after a reception on the other
 * path holds BUSY ~7.5 ms, either direction; the whole switch plus staging
 * is ~8.5 ms (bench 2026-09-26). Schedules must leave the radio idle this
 * long before such a slot. */
#define OC_EXEC_BAND_SWITCH_LEAD_US 12000u
/* LoRa <-> FLRC on the same band: SetPacketType, sync word, modulation and
 * DC-DC settings, then staging: 3.1-3.2 ms from the previous slot's end to
 * the launch, either direction (bench 2026-09-26, 915 near <-> mid). */
#define OC_EXEC_MOD_SWITCH_LEAD_US 4000u
#define OC_EXEC_CONFIG_LEAD_US 1200u /* configure+stage before slot start: bench 2026-09-25 worst ~1 ms (fast-path staging, 16 MHz SPI) */
#define OC_EXEC_LATE_US       100u  /* launching later than this after slot start skips the slot */
/* Poll cadence while a slot runs. An RX done must be noticed fast: after a
 * full packet the next back-to-back slot starts ~1 ms later (guard minus the
 * RX-done lag). The W12 poll reads the IRQ line first, so polling is cheap. */
#define OC_EXEC_POLL_US       50u
#define OC_EXEC_IDLE_US       10000u
#define OC_EXEC_MAX_PARTS     16u   /* parts remembered per frame for duplicate (resend) detection */

typedef struct {
    uint32_t  offset_us;
    uint32_t  length_us;
    uint32_t  freq_hz;
    oc_mode_t mode;
    uint8_t   dir;
    uint8_t   payload_len;
    uint16_t  payload_off;
} oc_exec_slot_t;

typedef enum {
    OC_EXEC_BUF_EMPTY      = 0,
    OC_EXEC_BUF_ASSEMBLING = 1,
    OC_EXEC_BUF_READY      = 2,
    OC_EXEC_BUF_RUNNING    = 3
} oc_exec_buf_state_t;

typedef struct {
    uint32_t       frame_number;
    uint8_t        state;
    uint8_t        slot_count;
    uint16_t       pool_used;
    uint8_t        parts;                        /* parts added (hashes kept up to OC_EXEC_MAX_PARTS) */
    uint32_t       part_hash[OC_EXEC_MAX_PARTS];
    oc_exec_slot_t slots[OC_EXEC_MAX_SLOTS];
    uint8_t        pool[OC_EXEC_PAYLOAD_POOL];
} oc_exec_frame_t;

typedef struct {
    void *ctx;
    void (*on_rx)(void *ctx, uint32_t frame_number, uint8_t slot_index, const oc_radio_event_t *ev);
} oc_exec_sink_t;

/* Which radio step failed last (oc_exec_t.last_radio_op). */
enum { OC_EXEC_OP_NONE = 0, OC_EXEC_OP_CONFIGURE, OC_EXEC_OP_STAGE, OC_EXEC_OP_LAUNCH, OC_EXEC_OP_EVENT };

typedef enum {
    OC_EXEC_PH_CONFIG = 0,
    OC_EXEC_PH_LAUNCH = 1,
    OC_EXEC_PH_ACTIVE = 2
} oc_exec_phase_t;

typedef struct {
    oc_exec_frame_t  frames[OC_EXEC_FRAMES];
    oc_radio_ops_t   radio;
    oc_exec_sink_t   sink;
    int              tx_enabled;
    int              active;      /* a schedule has been accepted at least once */
    uint32_t         first_frame; /* first frame ever scheduled; misses count after it */
    /* run state */
    int              have_frame;
    uint32_t         cur_frame;
    uint64_t         cur_start_us;
    oc_exec_frame_t *run;         /* NULL: nothing scheduled for cur_frame */
    uint8_t          slot;
    uint8_t          phase;
    oc_radio_event_t ev;
    /* counters */
    uint16_t         schedule_misses;
    uint32_t         tx_blocked;
    uint32_t         late_slots;
    uint32_t         overruns;
    uint32_t         radio_errors;
    int32_t          last_tx_end_us;   /* latest TX done from its frame start; OC_RX_END_UNKNOWN if none */
    int32_t          last_tx_start_us; /* and its preamble start */
    int8_t           radio_band;       /* band of the last configured slot; -1 unknown */
    int8_t           radio_mod;        /* and its oc_modulation_t; -1 unknown */
    int16_t          last_radio_err;   /* the latest failing radio call's return value */
    uint8_t          last_radio_op;    /* OC_EXEC_OP_*: which call it was */
} oc_exec_t;

void oc_exec_init(oc_exec_t *e, const oc_radio_ops_t *radio, const oc_exec_sink_t *sink);

/* Accept one SCHEDULE message. Returns an oc_ack_status_t. */
uint8_t oc_exec_add_part(oc_exec_t *e, const oc_schedule_t *part, const oc_clock_t *clk, uint64_t now_us);

/* Do everything due at now_us. Returns the local µs at which to call again. */
uint64_t oc_exec_step(oc_exec_t *e, const oc_clock_t *clk, uint64_t now_us);

/* TX is refused (slots skipped, tx_blocked counted) while disabled. */
void oc_exec_set_tx_enabled(oc_exec_t *e, int enabled);

#endif
