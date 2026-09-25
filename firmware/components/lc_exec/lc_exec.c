#include "lc_exec.h"

#include <stddef.h>
#include <string.h>

void lc_exec_init(lc_exec_t *e, const lc_radio_ops_t *radio, const lc_exec_sink_t *sink)
{
    memset(e, 0, sizeof(*e));
    e->radio = *radio;
    e->sink = *sink;
    e->tx_enabled = 1;
}

void lc_exec_set_tx_enabled(lc_exec_t *e, int enabled)
{
    e->tx_enabled = enabled;
}

static lc_exec_frame_t *find_any(lc_exec_t *e, uint32_t frame_number)
{
    for (unsigned i = 0; i < LC_EXEC_FRAMES; i++) {
        lc_exec_frame_t *b = &e->frames[i];
        if (b->state != LC_EXEC_BUF_EMPTY && b->frame_number == frame_number) {
            return b;
        }
    }
    return NULL;
}

/* A buffer that is empty, or holds a frame that can never run again. */
static lc_exec_frame_t *free_buffer(lc_exec_t *e, uint32_t now_frame)
{
    for (unsigned i = 0; i < LC_EXEC_FRAMES; i++) {
        lc_exec_frame_t *b = &e->frames[i];
        if (b->state == LC_EXEC_BUF_EMPTY ||
            (b->state != LC_EXEC_BUF_RUNNING && b->frame_number <= now_frame)) {
            return b;
        }
    }
    return NULL;
}

static int add_slot(lc_exec_frame_t *b, const lc_slot_t *s)
{
    if (b->slot_count >= LC_EXEC_MAX_SLOTS || s->length_us == 0 ||
        (uint64_t)s->offset_us + s->length_us > LC_FRAME_US || s->freq_hz == 0 ||
        (s->dir != LC_DIR_RX && s->dir != LC_DIR_TX)) {
        return -1;
    }
    if (b->slot_count > 0) {
        const lc_exec_slot_t *prev = &b->slots[b->slot_count - 1];
        if (s->offset_us < prev->offset_us + prev->length_us) {
            return -1; /* unsorted or overlapping */
        }
    }
    uint8_t len = s->dir == LC_DIR_TX ? s->payload_len : 0;
    uint32_t airtime = lc_airtime_us(&s->mode, len);
    if (airtime == 0) {
        return -1; /* invalid mode */
    }
    if (s->dir == LC_DIR_TX) {
        if (len == 0 || s->payload == NULL || airtime > s->length_us ||
            b->pool_used + len > LC_EXEC_PAYLOAD_POOL) {
            return -1;
        }
        memcpy(&b->pool[b->pool_used], s->payload, len);
    }
    lc_exec_slot_t *d = &b->slots[b->slot_count++];
    d->offset_us = s->offset_us;
    d->length_us = s->length_us;
    d->freq_hz = s->freq_hz;
    d->mode = s->mode;
    d->dir = s->dir;
    d->payload_len = len;
    d->payload_off = b->pool_used;
    b->pool_used = (uint16_t)(b->pool_used + len);
    return 0;
}

uint8_t lc_exec_add_part(lc_exec_t *e, const lc_schedule_t *part, const lc_clock_t *clk, uint64_t now_us)
{
    uint32_t now_frame;
    uint64_t start_us;
    if (lc_clock_frame_at(clk, now_us, &now_frame) != 0 ||
        lc_clock_frame_start_us(clk, part->frame_number, &start_us) != 0) {
        return LC_ACK_ERR_LATE; /* no usable time: can't meet any deadline */
    }
    lc_exec_frame_t *b = find_any(e, part->frame_number);
    if (part->frame_number <= now_frame || start_us < now_us + LC_EXEC_SETUP_US) {
        if (b != NULL && b->state == LC_EXEC_BUF_ASSEMBLING) {
            b->state = LC_EXEC_BUF_EMPTY;
        }
        return LC_ACK_ERR_LATE;
    }
    if (part->frame_number - now_frame > LC_EXEC_MAX_AHEAD || part->slot_count > LC_MAX_SLOTS_PER_SCHEDULE) {
        return LC_ACK_ERR_MALFORMED;
    }
    if (b != NULL && b->state != LC_EXEC_BUF_ASSEMBLING) {
        return LC_ACK_ERR_MALFORMED; /* frame already complete */
    }
    if (b == NULL) {
        b = free_buffer(e, now_frame);
        if (b == NULL) {
            return LC_ACK_ERR_MALFORMED;
        }
        b->frame_number = part->frame_number;
        b->state = LC_EXEC_BUF_ASSEMBLING;
        b->slot_count = 0;
        b->pool_used = 0;
    }
    for (uint8_t i = 0; i < part->slot_count; i++) {
        if (add_slot(b, &part->slots[i]) != 0) {
            b->state = LC_EXEC_BUF_EMPTY;
            return LC_ACK_ERR_MALFORMED;
        }
    }
    if (part->flags & LC_SCHED_FLAG_LAST) {
        b->state = LC_EXEC_BUF_READY;
        if (!e->active) {
            e->active = 1;
            e->first_frame = part->frame_number;
        }
    }
    return LC_ACK_OK;
}

uint64_t lc_exec_step(lc_exec_t *e, const lc_clock_t *clk, uint64_t now_us)
{
    (void)e;
    (void)clk;
    return now_us + LC_EXEC_IDLE_US; /* slot execution is added in the next task */
}
