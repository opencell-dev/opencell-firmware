/* oc_rxt — RX turnaround trace, for measurement builds only
 * (idf.py -DOC_RXT_TRACE=1 build). Without it every call below compiles to
 * nothing.
 *
 * A sequence opens when a poll finds an RX done and closes when the next
 * slot is launched (or skipped as late). Each point is stored as µs after
 * the LR2021's RX-done IRQ edge and aggregated by the next slot's budget
 * (its start - the IRQ): an RX slot tight (<= 1.5 ms: back to back after a
 * full packet) or loose (<= OC_RXT_B2B_US); a TX slot within
 * OC_RXT_TO_TX_US (e.g. the next frame's beacon after a RACH reception);
 * late (skipped); farther ones are only counted. The link task prints the aggregates as text lines starting
 * "@RXT" once a second while no host has spoken for 2 s (so they never
 * interleave with a running host's frames). */
#ifndef OC_RXT_H
#define OC_RXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OC_RXT_POLL = 0, /* the op_poll call that found RX done: entry */
    OC_RXT_FLAGS,    /* IRQ status read */
    OC_RXT_LEN,      /* packet length read */
    OC_RXT_DATA,     /* payload read */
    OC_RXT_PSTAT,    /* RSSI/SNR read */
    OC_RXT_PDONE,    /* op_poll returns (IRQ flags cleared) */
    OC_RXT_SINK,     /* RX report handled by the sink (sent, or queued) */
    OC_RXT_CFG0,     /* next slot: configure entry */
    OC_RXT_CFG1,     /* configure exit */
    OC_RXT_STAGE,    /* stage exit */
    OC_RXT_STEP,     /* oc_exec_step returned to the exec task */
    OC_RXT_LOCK,     /* exec task holds the app lock again (final spin) */
    OC_RXT_LAUNCH,   /* op_launch entry */
    OC_RXT_FIRE,     /* NSS edge (the SetRx/SetTx runs) */
    OC_RXT_READY,    /* BUSY low: the next slot has started */
    OC_RXT_CPRE,     /* configure: BUSY low before SetRfFrequency */
    OC_RXT_SCLR,     /* stage RX: after ClearIrq */
    OC_RXT_SFIFO,    /* stage RX: after ClearRxFifo */
    OC_RXT_SFS,      /* stage RX: after SetFs */
    OC_RXT_N
};

#define OC_RXT_B2B_US 3000
#define OC_RXT_TO_TX_US 15000

#if OC_RXT_TRACE
void oc_rxt_begin(int64_t irq_us, int64_t poll_us, int64_t prev_poll_us);
void oc_rxt_mark(int point);
void oc_rxt_launch_at(uint64_t at_us, int tx);
void oc_rxt_end_launched(void);
void oc_rxt_end_late(void);
int  oc_rxt_open(void);
/* How long another task held the app lock (the exec task may be waiting). */
void oc_rxt_hold(int32_t us);
/* RX reports dropped so far (report queue full), for the text. */
void oc_rxt_rx_drops(uint32_t n);
/* How long the exec task waited for the app lock (active: while polling a slot). */
void oc_rxt_exec_wait(int32_t us, int active);
/* Text for the link: "@RXT ...\n" lines; returns the length. */
int  oc_rxt_format(char *out, int cap);
#define OC_RXT_BEGIN(i, p, pp) oc_rxt_begin((i), (p), (pp))
#define OC_RXT_MARK(p)         oc_rxt_mark(p)
#define OC_RXT_LAUNCH_AT(t, tx) oc_rxt_launch_at((t), (tx))
#define OC_RXT_END_LAUNCHED()  oc_rxt_end_launched()
#define OC_RXT_END_LATE()      oc_rxt_end_late()
#define OC_RXT_HOLD(us)        oc_rxt_hold(us)
#define OC_RXT_EXEC_WAIT(us, a) oc_rxt_exec_wait((us), (a))
#else
#define OC_RXT_BEGIN(i, p, pp) ((void)0)
#define OC_RXT_MARK(p)         ((void)0)
#define OC_RXT_LAUNCH_AT(t, tx) ((void)0)
#define OC_RXT_END_LAUNCHED()  ((void)0)
#define OC_RXT_END_LATE()      ((void)0)
#define OC_RXT_HOLD(us)        ((void)0)
#define OC_RXT_EXEC_WAIT(us, a) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
