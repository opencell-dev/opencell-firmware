/* Shared fixture for lc_exec tests: a locked clock and a fake radio that
 * records every call. Frame 25000 starts at local T0. */
#ifndef EXEC_FIXTURE_H
#define EXEC_FIXTURE_H

#include <string.h>

#include "lc_clock.h"
#include "lc_exec.h"

#define T0   5000000ull
#define UTC0 (LC_EPOCH_UNIX_S + 3000u) /* frame 25000 */
#define F0   25000u

typedef enum { CALL_CONFIGURE, CALL_STAGE_TX, CALL_STAGE_RX, CALL_LAUNCH, CALL_STANDBY } call_kind_t;

typedef struct {
    call_kind_t kind;
    uint64_t    at_us;
    uint32_t    arg;   /* freq for configure, len for stage_tx, timeout for stage_rx */
    uint64_t    target_us; /* launch: when the operation must start */
} call_t;

typedef struct {
    call_t           calls[256];
    int              n;
    uint64_t         now;          /* set by the test before each step */
    lc_radio_event_t queue[8];     /* events returned by poll(), in order */
    int              qn;
    int              qi;
    int              fail_configure;
    /* rx sink */
    int              rx_count;
    uint32_t         rx_frame;
    uint8_t          rx_slot;
    lc_radio_event_t rx_ev;
} fake_t;

static fake_t fake;
static lc_clock_t clk;
static lc_exec_t exec_;

static void rec(call_kind_t k, uint32_t arg)
{
    if (fake.n < 256) {
        fake.calls[fake.n++] = (call_t){ k, fake.now, arg, 0 };
    }
}

static int f_configure(void *ctx, uint32_t freq_hz, const lc_mode_t *mode)
{
    (void)ctx;
    (void)mode;
    rec(CALL_CONFIGURE, freq_hz);
    return fake.fail_configure ? -1 : 0;
}

static int f_stage_tx(void *ctx, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    (void)data;
    rec(CALL_STAGE_TX, len);
    return 0;
}

static int f_stage_rx(void *ctx, uint32_t timeout_us)
{
    (void)ctx;
    rec(CALL_STAGE_RX, timeout_us);
    return 0;
}

static int f_launch(void *ctx, uint64_t at_us)
{
    (void)ctx;
    rec(CALL_LAUNCH, 0);
    fake.calls[fake.n - 1].target_us = at_us;
    return 0;
}

static int f_poll(void *ctx, lc_radio_event_t *ev)
{
    (void)ctx;
    if (fake.qi >= fake.qn) {
        return 0; /* still busy */
    }
    *ev = fake.queue[fake.qi++];
    return 1;
}

static void f_standby(void *ctx)
{
    (void)ctx;
    rec(CALL_STANDBY, 0);
}

static void f_on_rx(void *ctx, uint32_t frame, uint8_t slot, const lc_radio_event_t *ev)
{
    (void)ctx;
    fake.rx_count++;
    fake.rx_frame = frame;
    fake.rx_slot = slot;
    fake.rx_ev = *ev;
}

static inline void fixture_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    lc_clock_init(&clk, 30000000u);
    for (int i = 0; i < 3; i++) {
        lc_clock_on_pps(&clk, T0 - 2000000ull + (uint64_t)i * 1000000u); /* last edge at T0 */
    }
    lc_clock_on_time(&clk, UTC0, T0 + 1000);
    const lc_radio_ops_t ops = { NULL, f_configure, f_stage_tx, f_stage_rx, f_launch, f_poll, f_standby };
    const lc_exec_sink_t sink = { NULL, f_on_rx };
    lc_exec_init(&exec_, &ops, &sink);
}

static inline lc_slot_t tx_slot(uint32_t offset_us, uint32_t length_us, const uint8_t *payload, uint8_t len)
{
    lc_slot_t s = { offset_us, length_us, 915250000u, *lc_tier_mode(LC_BAND_915, LC_TIER_EDGE),
                    LC_DIR_TX, len, payload };
    return s;
}

static inline lc_slot_t rx_slot(uint32_t offset_us, uint32_t length_us)
{
    lc_slot_t s = { offset_us, length_us, 903750000u, *lc_tier_mode(LC_BAND_915, LC_TIER_EDGE),
                    LC_DIR_RX, 0, NULL };
    return s;
}

static inline void queue_event(uint8_t type, int16_t rssi, int16_t snr_qdb)
{
    lc_radio_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.crc_ok = 1;
    ev.len = 4;
    ev.rssi_dbm = rssi;
    ev.snr_qdb = snr_qdb;
    fake.queue[fake.qn++] = ev;
}

/* Call lc_exec_step at every wake time it asks for, from start to end. */
static inline void run_until(uint64_t start, uint64_t end)
{
    uint64_t now = start;
    while (now < end) {
        fake.now = now;
        uint64_t next = lc_exec_step(&exec_, &clk, now);
        now = next > now ? next : now + 1;
    }
}

static inline int find_call(call_kind_t k, int nth)
{
    for (int i = 0; i < fake.n; i++) {
        if (fake.calls[i].kind == k && nth-- == 0) {
            return i;
        }
    }
    return -1;
}

static inline int count_calls(call_kind_t k)
{
    int c = 0;
    for (int i = 0; i < fake.n; i++) {
        c += fake.calls[i].kind == k;
    }
    return c;
}

#endif
