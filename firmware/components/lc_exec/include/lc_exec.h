/* lc_exec — the bs-radio slot executor.
 *
 * The host sends one frame's slots as one or more SCHEDULE messages (the last
 * has LC_SCHED_FLAG_LAST). lc_exec_add_part() validates and stores them,
 * copying payloads (message payload pointers don't outlive the UART frame).
 * lc_exec_step() is called by the firmware's executor task; it configures,
 * stages and launches each slot at its time and reports received packets. */
#ifndef LC_EXEC_H
#define LC_EXEC_H

#include <stdint.h>

#include "lc_clock.h"
#include "lc_link.h"
#include "lc_phy.h"
#include "lc_radio_if.h"

#define LC_EXEC_FRAMES        3u
#define LC_EXEC_MAX_SLOTS     64u
#define LC_EXEC_PAYLOAD_POOL  4096u
#define LC_EXEC_SETUP_US      1500u /* a frame's schedule must be complete this long before it starts */
#define LC_EXEC_MAX_AHEAD     2u    /* frames ahead of the current one that may be scheduled */
#define LC_EXEC_CONFIG_LEAD_US 400u /* configure+stage this long before slot start; bench-calibrated */
#define LC_EXEC_LATE_US       100u  /* launching later than this after slot start skips the slot */
#define LC_EXEC_POLL_US       200u
#define LC_EXEC_IDLE_US       10000u

typedef struct {
    uint32_t  offset_us;
    uint32_t  length_us;
    uint32_t  freq_hz;
    lc_mode_t mode;
    uint8_t   dir;
    uint8_t   payload_len;
    uint16_t  payload_off;
} lc_exec_slot_t;

typedef enum {
    LC_EXEC_BUF_EMPTY      = 0,
    LC_EXEC_BUF_ASSEMBLING = 1,
    LC_EXEC_BUF_READY      = 2,
    LC_EXEC_BUF_RUNNING    = 3
} lc_exec_buf_state_t;

typedef struct {
    uint32_t       frame_number;
    uint8_t        state;
    uint8_t        slot_count;
    uint16_t       pool_used;
    lc_exec_slot_t slots[LC_EXEC_MAX_SLOTS];
    uint8_t        pool[LC_EXEC_PAYLOAD_POOL];
} lc_exec_frame_t;

typedef struct {
    void *ctx;
    void (*on_rx)(void *ctx, uint32_t frame_number, uint8_t slot_index, const lc_radio_event_t *ev);
} lc_exec_sink_t;

typedef enum {
    LC_EXEC_PH_CONFIG = 0,
    LC_EXEC_PH_LAUNCH = 1,
    LC_EXEC_PH_ACTIVE = 2
} lc_exec_phase_t;

typedef struct {
    lc_exec_frame_t  frames[LC_EXEC_FRAMES];
    lc_radio_ops_t   radio;
    lc_exec_sink_t   sink;
    int              tx_enabled;
    int              active;      /* a schedule has been accepted at least once */
    uint32_t         first_frame; /* first frame ever scheduled; misses count after it */
    /* run state */
    int              have_frame;
    uint32_t         cur_frame;
    uint64_t         cur_start_us;
    lc_exec_frame_t *run;         /* NULL: nothing scheduled for cur_frame */
    uint8_t          slot;
    uint8_t          phase;
    lc_radio_event_t ev;
    /* counters */
    uint16_t         schedule_misses;
    uint32_t         tx_blocked;
    uint32_t         late_slots;
    uint32_t         overruns;
    uint32_t         radio_errors;
} lc_exec_t;

void lc_exec_init(lc_exec_t *e, const lc_radio_ops_t *radio, const lc_exec_sink_t *sink);

/* Accept one SCHEDULE message. Returns an lc_ack_status_t. */
uint8_t lc_exec_add_part(lc_exec_t *e, const lc_schedule_t *part, const lc_clock_t *clk, uint64_t now_us);

/* Do everything due at now_us. Returns the local µs at which to call again. */
uint64_t lc_exec_step(lc_exec_t *e, const lc_clock_t *clk, uint64_t now_us);

/* TX is refused (slots skipped, tx_blocked counted) while disabled. */
void lc_exec_set_tx_enabled(lc_exec_t *e, int enabled);

#endif
