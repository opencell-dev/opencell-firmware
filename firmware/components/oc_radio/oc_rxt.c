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

/* ---- Diagnosis (2026-09-30 §13) ----
 * Times are the low 32 bits of esp_timer (µs): written on core 0, read on
 * core 1, so they must not tear (a 64-bit volatile can); compared modulo
 * 2^32, fine for windows far shorter than 71 minutes. */
static const char *const k_kinds[OC_RXT_K_N] = { "none", "sched", "time", "config", "fw", "other", "status", "oled" };

static volatile int      s_holder;
static uint32_t          s_kh_n[OC_RXT_K_N], s_kh_50[OC_RXT_K_N], s_kh_100[OC_RXT_K_N];
static int32_t           s_kh_max[OC_RXT_K_N];
static uint32_t          s_kh_rep_n;   /* holds a report-task run overlapped (the holder was preempted) */
static int32_t           s_kh_rep_max;
static uint32_t          s_kw_50[OC_RXT_K_N]; /* exec waits while polling, by the holder it found */
static int32_t           s_kw_max[OC_RXT_K_N];
static volatile int      s_prep_busy, s_rep_busy;
static volatile uint32_t s_prep_since, s_rep_since;
static volatile uint32_t s_prep_t0, s_prep_t1, s_rep_t0, s_rep_t1; /* last finished windows */
static uint32_t          s_prep_n, s_rep_n;
static int32_t           s_prep_max, s_rep_max;
static uint32_t          s_gap_10[2], s_gap_50[2]; /* [1]: while a sequence is open */
static int32_t           s_gap_max[2];
static int32_t           s_seq_gap;
static uint32_t          s_wait_end;   /* the exec task's latest lock wait */
static int32_t           s_wait_us;
static int               s_wait_kind;
static int32_t           s_seq_wait;   /* that wait, if it ended after this sequence's IRQ */
static int               s_seq_wait_kind;

typedef struct {
    uint32_t n;
    int32_t  slack_min, start_max;
} ovl_t;
static ovl_t s_ovl[4]; /* tight sequences: [0] clean, [1] prepare ran, [2] a report ran, [3] both */

typedef struct {
    int     valid;
    int32_t t[OC_RXT_N];
    int32_t budget, slack, start, gap, wait;
    int     wait_kind, prep, rep;
} worst_t;
static worst_t s_worst_start, s_worst_slack;

static uint32_t lo(int64_t t)
{
    return (uint32_t)t;
}

/* [a0, a1] and [b0, b1] share a moment (mod 2^32) */
static int overlaps(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1)
{
    return (int32_t)(a0 - b1) <= 0 && (int32_t)(b0 - a1) <= 0;
}

/* core-0 work running now or in its last window, inside [t0, t1] */
static int ran_in(int busy, uint32_t since, uint32_t w0, uint32_t w1, uint32_t t0, uint32_t t1)
{
    if (busy && (int32_t)(since - t1) <= 0) {
        return 1;
    }
    return w1 != 0 && overlaps(w0, w1, t0, t1);
}

void oc_rxt_holder(int kind)
{
    s_holder = kind;
}

int oc_rxt_holder_now(void)
{
    return s_holder;
}

void oc_rxt_hold_kind(int kind, int64_t t0, int64_t t1)
{
    int32_t us = (int32_t)(t1 - t0);
    oc_rxt_hold(us);
    if (kind < 0 || kind >= OC_RXT_K_N) kind = OC_RXT_K_OTHER;
    s_kh_n[kind]++;
    if (us > 50) s_kh_50[kind]++;
    if (us > 100) s_kh_100[kind]++;
    if (us > s_kh_max[kind]) s_kh_max[kind] = us;
    if (ran_in(s_rep_busy, s_rep_since, s_rep_t0, s_rep_t1, lo(t0), lo(t1))) {
        s_kh_rep_n++;
        if (us > s_kh_rep_max) s_kh_rep_max = us;
    }
}

void oc_rxt_exec_wait_kind(int32_t us, int active, int kind)
{
    oc_rxt_exec_wait(us, active);
    if (kind < 0 || kind >= OC_RXT_K_N) kind = OC_RXT_K_OTHER;
    if (active) {
        if (us > 50) s_kw_50[kind]++;
        if (us > s_kw_max[kind]) s_kw_max[kind] = us;
    }
    s_wait_end = lo(esp_timer_get_time());
    s_wait_us = us;
    s_wait_kind = kind;
}

void oc_rxt_prepare_begin(int64_t t0)
{
    s_prep_since = lo(t0);
    s_prep_busy = 1;
}

void oc_rxt_prepare(int64_t t0, int64_t t1)
{
    int32_t us = (int32_t)(t1 - t0);
    s_prep_n++;
    if (us > s_prep_max) s_prep_max = us;
    s_prep_t0 = lo(t0);
    s_prep_t1 = lo(t1);
    s_prep_busy = 0;
}

void oc_rxt_report_begin(int64_t t0)
{
    s_rep_since = lo(t0);
    s_rep_busy = 1;
}

void oc_rxt_report(int64_t t0, int64_t t1)
{
    int32_t us = (int32_t)(t1 - t0);
    s_rep_n++;
    if (us > s_rep_max) s_rep_max = us;
    s_rep_t0 = lo(t0);
    s_rep_t1 = lo(t1);
    s_rep_busy = 0;
}

void oc_rxt_gap(int32_t us)
{
    int k = s_open ? 1 : 0;
    if (us > 10) s_gap_10[k]++;
    if (us > 50) s_gap_50[k]++;
    if (us > s_gap_max[k]) s_gap_max[k] = us;
    if (s_open && us > s_seq_gap) s_seq_gap = us;
}

static void keep_worst(worst_t *w, int32_t budget, int32_t slack, int32_t start, int prep, int rep)
{
    w->valid = 1;
    memcpy(w->t, s_t, sizeof(w->t));
    w->budget = budget;
    w->slack = slack;
    w->start = start;
    w->gap = s_seq_gap;
    w->wait = s_seq_wait;
    w->wait_kind = s_seq_wait_kind;
    w->prep = prep;
    w->rep = rep;
}

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
    s_seq_gap = 0;
    int ended_after_irq = s_wait_end != 0 && (int32_t)(s_wait_end - lo(irq_us)) >= 0;
    s_seq_wait = ended_after_irq ? s_wait_us : 0;
    s_seq_wait_kind = ended_after_irq ? s_wait_kind : OC_RXT_K_NONE;
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
    if (!late && a == &s_agg[0]) {
        uint32_t t0 = lo(s_irq), t1 = lo(esp_timer_get_time());
        int prep = ran_in(s_prep_busy, s_prep_since, s_prep_t0, s_prep_t1, t0, t1);
        int rep = ran_in(s_rep_busy, s_rep_since, s_rep_t0, s_rep_t1, t0, t1);
        ovl_t *o = &s_ovl[(prep ? 1 : 0) + (rep ? 2 : 0)];
        if (o->n == 0 || slack < o->slack_min) o->slack_min = slack;
        if (o->n == 0 || start > o->start_max) o->start_max = start;
        o->n++;
        if (!s_worst_start.valid || start > s_worst_start.start) keep_worst(&s_worst_start, budget, slack, start, prep, rep);
        if (!s_worst_slack.valid || slack < s_worst_slack.slack) keep_worst(&s_worst_slack, budget, slack, start, prep, rep);
    }
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
    if (n < cap) n += snprintf(out + n, cap - n, "@RXT holds n/>50/>100/max:");
    for (int k = 1; k < OC_RXT_K_N && n < cap; k++) {
        n += snprintf(out + n, cap - n, " %s %u/%u/%u/%d", k_kinds[k], (unsigned)s_kh_n[k], (unsigned)s_kh_50[k],
                      (unsigned)s_kh_100[k], (int)s_kh_max[k]);
    }
    if (n < cap) n += snprintf(out + n, cap - n, " | with_report_run n=%u max=%d\n", (unsigned)s_kh_rep_n, (int)s_kh_rep_max);
    if (n < cap) n += snprintf(out + n, cap - n, "@RXT exec_wait_polling by_holder >50/max:");
    for (int k = 0; k < OC_RXT_K_N && n < cap; k++) {
        n += snprintf(out + n, cap - n, " %s %u/%d", k_kinds[k], (unsigned)s_kw_50[k], (int)s_kw_max[k]);
    }
    if (n < cap) n += snprintf(out + n, cap - n, "\n");
    if (n < cap)
        n += snprintf(out + n, cap - n,
                      "@RXT core0 prepare n=%u max=%d report_run n=%u max=%d | core1 spin_gap in_seq >10=%u >50=%u "
                      "max=%d, other >10=%u >50=%u max=%d\n",
                      (unsigned)s_prep_n, (int)s_prep_max, (unsigned)s_rep_n, (int)s_rep_max, (unsigned)s_gap_10[1],
                      (unsigned)s_gap_50[1], (int)s_gap_max[1], (unsigned)s_gap_10[0], (unsigned)s_gap_50[0],
                      (int)s_gap_max[0]);
    static const char *const ovl[4] = { "clean", "prepare", "report", "both" };
    if (n < cap) n += snprintf(out + n, cap - n, "@RXT tight_by_core0 n/slack_min/start_max:");
    for (int k = 0; k < 4 && n < cap; k++) {
        n += snprintf(out + n, cap - n, " %s %u/%d/%d", ovl[k], (unsigned)s_ovl[k].n, (int)s_ovl[k].slack_min,
                      (int)s_ovl[k].start_max);
    }
    if (n < cap) n += snprintf(out + n, cap - n, "\n");
    const worst_t *const ws[2] = { &s_worst_start, &s_worst_slack };
    static const char *const wn[2] = { "worst_start", "worst_slack" };
    for (int k = 0; k < 2 && n < cap; k++) {
        const worst_t *w = ws[k];
        n += snprintf(out + n, cap - n, "@RXT %s budget=%d slack=%d start_err=%d spin_gap=%d lock_wait=%d(%s) prepare=%d report=%d:",
                      wn[k], (int)w->budget, (int)w->slack, (int)w->start, (int)w->gap, (int)w->wait,
                      k_kinds[w->wait_kind], w->prep, w->rep);
        for (int i = 0; i < OC_RXT_N && n < cap; i++) {
            n += snprintf(out + n, cap - n, " %s=%d", k_names[i], (int)w->t[i]);
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
