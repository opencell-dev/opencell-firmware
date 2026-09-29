/* ocb_time: when to send a W12 its TIME label, and which second to put in it.
 *
 * GPS PPS on the board (role bs): the edge is the true second, so the label
 * is the host second, sent once per second 0.1-0.8 s after it.
 *
 * Internal PPS (role bench, --internal): the edge falls wherever the board's
 * boot put it, and the board accepts a label only within
 * OC_TIME_LABEL_MAX_US (900 ms) after its latest edge (oc_clock_on_time).
 * Sending at a fixed point of the host second is refused for ever when the
 * edge lands just after it (opencell-firmware#2). So in this mode a label
 * goes out only while the board has no timebase (its STATUS frame is 0):
 * any second will do, since the board then counts on by itself. A label
 * refused as late means the next edge is under 100 ms away: the same label
 * is tried again 300 ms later, after that edge. Once one is accepted,
 * nothing more is sent until the board shows its frame (or 3 s pass), so a
 * second label can never disagree with the board's count.
 *
 * I/O-free: the caller sends the message and feeds back its ACK. */
#ifndef OCB_TIME_H
#define OCB_TIME_H

#include <stdint.h>

#define OCB_TIME_RETRY_US   300000u  /* after a "late" ACK */
#define OCB_TIME_ACK_US     500000u  /* no ACK by then: lost, send again */
#define OCB_TIME_HOLD_US    3000000u /* after an accepted label: wait this long for STATUS to show a frame */
#define OCB_TIME_REFUSED_US 1000000u /* any other refusal (the board is not configured yet) */

typedef struct {
    int      internal;
    uint32_t last_s;  /* GPS: the host second last labelled */
    int      pending; /* internal: a label is out, its ACK not in */
    uint8_t  seq;
    uint64_t sent_us;
    uint64_t next_us; /* internal: nothing before this */
    uint32_t sent, accepted, late; /* counters */
} ocb_time_t;

void ocb_time_init(ocb_time_t *t, int internal);
/* The unix second to send in a TIME label now, or 0: nothing to send.
 * board_has_time: the board's latest STATUS carried a frame number. */
uint32_t ocb_time_due(ocb_time_t *t, uint64_t now_us, int board_has_time);
/* The label ocb_time_due gave was sent with this seq. */
void ocb_time_sent(ocb_time_t *t, uint8_t seq, uint64_t now_us);
/* An ACK came in (any seq: those not of the pending label are ignored).
 * status is an oc_ack_status_t. */
void ocb_time_ack(ocb_time_t *t, uint8_t seq, uint8_t status, uint64_t now_us);

#endif
