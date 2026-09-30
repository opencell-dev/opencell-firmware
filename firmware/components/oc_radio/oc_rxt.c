#include "oc_rxt.h"

#if OC_RXT_TRACE

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

static const char *const k_names[OC_RXT_N] = { "poll", "flags", "len", "data", "pstat", "pdone", "sink", "cfg0",
                                                "cfg1", "stage", "step", "lock", "launch", "fire", "ready",
                                                "cpre", "sclr", "sfifo", "sfs" };

static int     s_open;
static int64_t s_irq;
static int64_t s_at;
static int     s_to_tx;
static int32_t s_t[OC_RXT_N];

/* Sequences by budget (next slot start - IRQ): RX slots [0] tight, <=
 * OC_RXT_TIGHT_US (back to back after a full packet), [1] up to
 * OC_RXT_B2B_US; [2] TX slots up to OC_RXT_TO_TX_US; [3] the next slot was
 * skipped as late (budget unknown). */
#define OC_RXT_TIGHT_US 1500
typedef struct {
    uint32_t n;
    uint32_t cnt[OC_RXT_N];
    int64_t  sum[OC_RXT_N];
    int32_t  min[OC_RXT_N];
    int32_t  max[OC_RXT_N];
    int64_t  budget_sum;
    int32_t  budget_min, budget_max;
    int32_t  slack_min;  /* budget - staged: how early the slot was ready to launch */
    int32_t  start_max;  /* ready - budget: the start's error (> 0 late) */
    int32_t  start_min;
} agg_t;

static agg_t    s_agg[4];
static uint32_t s_late, s_far, s_dropped, s_no_irq;
static int32_t  s_hold_max;
static uint32_t s_rx_drops;

void oc_rxt_rx_drops(uint32_t n)
{
    s_rx_drops = n;
}
static uint32_t s_hold_n, s_hold_over100;
static int32_t  s_wait_max[2];
static uint32_t s_wait_over50[2], s_wait_over200[2];

void oc_rxt_exec_wait(int32_t us, int active)
{
    active = active ? 1 : 0;
    if (us > s_wait_max[active]) s_wait_max[active] = us;
    if (us > 50) s_wait_over50[active]++;
    if (us > 200) s_wait_over200[active]++;
}

void oc_rxt_hold(int32_t us)
{
    s_hold_n++;
    if (us > 100) s_hold_over100++;
    if (us > s_hold_max) s_hold_max = us;
}
static int32_t  s_last[OC_RXT_N];
static int32_t  s_last_budget;
static int      s_last_late;

void oc_rxt_begin(int64_t irq_us, int64_t poll_us, int64_t prev_poll_us)
{
    (void)prev_poll_us;
    if (s_open) {
        s_dropped++;
    }
    if (irq_us == 0) {
        s_no_irq++;
        s_open = 0;
        return;
    }
    s_open = 1;
    s_irq = irq_us;
    s_at = 0;
    for (int i = 0; i < OC_RXT_N; i++) {
        s_t[i] = -1;
    }
    s_t[OC_RXT_POLL] = (int32_t)(poll_us - irq_us);
}

int oc_rxt_open(void)
{
    return s_open;
}

void oc_rxt_mark(int point)
{
    if (s_open && point >= 0 && point < OC_RXT_N) {
        s_t[point] = (int32_t)(esp_timer_get_time() - s_irq);
    }
}

void oc_rxt_launch_at(uint64_t at_us, int tx)
{
    if (s_open) {
        s_at = (int64_t)at_us;
        s_to_tx = tx;
    }
}

static void keep_last(int32_t budget, int late)
{
    memcpy(s_last, s_t, sizeof(s_last));
    s_last_budget = budget;
    s_last_late = late;
    s_open = 0;
}

static void close_seq(int late)
{
    int32_t budget = s_at != 0 ? (int32_t)(s_at - s_irq) : -1;
    if (late) {
        s_late++;
    } else if (budget > (s_to_tx ? OC_RXT_TO_TX_US : OC_RXT_B2B_US)) {
        s_far++;
        s_open = 0;
        return;
    }
    agg_t *a = &s_agg[late ? 3 : s_to_tx ? 2 : budget <= OC_RXT_TIGHT_US ? 0 : 1];
    int first = a->n == 0;
    a->n++;
    a->budget_sum += budget;
    int32_t slack = late ? 0 : budget - s_t[OC_RXT_STAGE];
    int32_t start = late ? 0 : s_t[OC_RXT_READY] - budget;
    if (first || budget < a->budget_min) a->budget_min = budget;
    if (first || budget > a->budget_max) a->budget_max = budget;
    if (first || slack < a->slack_min) a->slack_min = slack;
    if (first || start > a->start_max) a->start_max = start;
    if (first || start < a->start_min) a->start_min = start;
    for (int i = 0; i < OC_RXT_N; i++) {
        if (s_t[i] < 0) continue;
        if (a->cnt[i] == 0 || s_t[i] > a->max[i]) a->max[i] = s_t[i];
        if (a->cnt[i] == 0 || s_t[i] < a->min[i]) a->min[i] = s_t[i];
        a->cnt[i]++;
        a->sum[i] += s_t[i];
    }
    keep_last(budget, late);
}

void oc_rxt_end_launched(void)
{
    if (s_open) close_seq(0);
}

void oc_rxt_end_late(void)
{
    if (s_open) close_seq(1);
}

int oc_rxt_format(char *out, int cap)
{
    int n = snprintf(out, cap,
                     "@RXT late=%u far=%u drop=%u noirq=%u rx_report_drops=%u link_lock_holds=%u >100us=%u max=%d | exec_lock_wait "
                     "polling >50us=%u >200us=%u max=%d, other >50us=%u >200us=%u max=%d\n",
                     (unsigned)s_late, (unsigned)s_far, (unsigned)s_dropped, (unsigned)s_no_irq, (unsigned)s_rx_drops,
                     (unsigned)s_hold_n,
                     (unsigned)s_hold_over100, (int)s_hold_max, (unsigned)s_wait_over50[1], (unsigned)s_wait_over200[1],
                     (int)s_wait_max[1], (unsigned)s_wait_over50[0], (unsigned)s_wait_over200[0], (int)s_wait_max[0]);
    static const char *const cls[4] = { "tight", "loose", "to_tx", "late" };
    for (int k = 0; k < 4 && n < cap; k++) {
        const agg_t *a = &s_agg[k];
        n += snprintf(out + n, cap - n,
                      "@RXT %s n=%u budget %d/%d/%d slack_min=%d start_err %d..%d | us-after-irq cnt/min/avg/max:",
                      cls[k], (unsigned)a->n, a->n ? (int)a->budget_min : 0,
                      a->n ? (int)(a->budget_sum / a->n) : 0, a->n ? (int)a->budget_max : 0, (int)a->slack_min,
                      (int)a->start_min, (int)a->start_max);
        for (int i = 0; i < OC_RXT_N && n < cap; i++) {
            n += snprintf(out + n, cap - n, " %s %u/%d/%d/%d", k_names[i], (unsigned)a->cnt[i],
                          a->cnt[i] ? (int)a->min[i] : 0, a->cnt[i] ? (int)(a->sum[i] / a->cnt[i]) : 0,
                          a->cnt[i] ? (int)a->max[i] : 0);
        }
        if (n < cap) n += snprintf(out + n, cap - n, "\n");
    }
    if (n < cap) n += snprintf(out + n, cap - n, "@RXT last%s budget=%d:", s_last_late ? "(late)" : "", (int)s_last_budget);
    for (int i = 0; i < OC_RXT_N && n < cap; i++) {
        n += snprintf(out + n, cap - n, " %s=%d", k_names[i], (int)s_last[i]);
    }
    if (n < cap) n += snprintf(out + n, cap - n, "\n");
    return n < cap ? n : cap - 1;
}

#endif
