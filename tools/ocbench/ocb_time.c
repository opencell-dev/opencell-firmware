#include "ocb_time.h"

#include <string.h>

#include "oc_link.h"

void ocb_time_init(ocb_time_t *t, int internal)
{
    memset(t, 0, sizeof(*t));
    t->internal = internal;
}

uint32_t ocb_time_due(ocb_time_t *t, uint64_t now_us, int board_has_time)
{
    uint32_t s = (uint32_t)(now_us / 1000000u);
    if (!t->internal) {
        uint32_t into = (uint32_t)(now_us % 1000000u);
        if (s == t->last_s || into <= 100000u || into >= 800000u) return 0;
        t->last_s = s;
        return s;
    }
    if (board_has_time) {
        t->pending = 0;
        t->next_us = 0;
        return 0;
    }
    if (t->pending && now_us - t->sent_us >= OCB_TIME_ACK_US) {
        t->pending = 0; /* the ACK was lost: try again now */
        t->next_us = 0;
    }
    if (t->pending || now_us < t->next_us) return 0;
    return s;
}

void ocb_time_sent(ocb_time_t *t, uint8_t seq, uint64_t now_us)
{
    t->sent++;
    if (!t->internal) return;
    t->pending = 1;
    t->seq = seq;
    t->sent_us = now_us;
}

void ocb_time_ack(ocb_time_t *t, uint8_t seq, uint8_t status, uint64_t now_us)
{
    if (!t->internal || !t->pending || seq != t->seq) return;
    t->pending = 0;
    if (status == OC_ACK_OK) {
        t->accepted++;
        t->next_us = now_us + OCB_TIME_HOLD_US;
    } else if (status == OC_ACK_ERR_LATE) {
        t->late++;
        t->next_us = now_us + OCB_TIME_RETRY_US;
    } else {
        t->next_us = now_us + OCB_TIME_REFUSED_US;
    }
}
