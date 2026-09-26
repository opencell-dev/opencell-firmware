#include "lc_exec.h"

#include <stddef.h>
#include <string.h>

void lc_exec_init(lc_exec_t *e, const lc_radio_ops_t *radio, const lc_exec_sink_t *sink)
{
    memset(e, 0, sizeof(*e));
    e->radio = *radio;
    e->sink = *sink;
    e->tx_enabled = 1;
    e->last_tx_end_us = LC_RX_END_UNKNOWN;
    e->last_tx_start_us = LC_RX_END_UNKNOWN;
    e->radio_band = -1;
    e->radio_mod = -1;
}

static int8_t band_of(uint32_t freq_hz)
{
    return freq_hz >= 1500000000u ? (int8_t)LC_BAND_2G4 : (int8_t)LC_BAND_915;
}

static void note_radio_error(lc_exec_t *e, uint8_t op, int err)
{
    e->radio_errors++;
    e->last_radio_op = op;
    e->last_radio_err = (int16_t)err;
}

/* µs from the current frame's start to a radio IRQ; LC_RX_END_UNKNOWN if the
 * IRQ wasn't timestamped or lies outside the frame. */
static int32_t frame_offset(const lc_exec_t *e, uint64_t irq_us)
{
    int64_t off = (int64_t)(irq_us - e->cur_start_us);
    return (irq_us == 0 || off < 0 || off > (int64_t)LC_FRAME_US) ? LC_RX_END_UNKNOWN : (int32_t)off;
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

/* FNV-1a over everything that defines a part, so a resend after a lost ACK
 * is recognised and acknowledged without being added twice. */
static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static uint32_t part_hash(const lc_schedule_t *part)
{
    uint32_t h = 2166136261u;
    h = fnv(h, &part->frame_number, sizeof(part->frame_number));
    h = fnv(h, &part->flags, 1);
    h = fnv(h, &part->slot_count, 1);
    for (uint8_t i = 0; i < part->slot_count && i < LC_MAX_SLOTS_PER_SCHEDULE; i++) {
        const lc_slot_t *s = &part->slots[i];
        h = fnv(h, &s->offset_us, sizeof(s->offset_us));
        h = fnv(h, &s->length_us, sizeof(s->length_us));
        h = fnv(h, &s->freq_hz, sizeof(s->freq_hz));
        h = fnv(h, &s->mode.modulation, 1);
        h = fnv(h, &s->mode.sf, 1);
        h = fnv(h, &s->mode.cr, 1);
        h = fnv(h, &s->mode.preamble, sizeof(s->mode.preamble));
        h = fnv(h, &s->mode.bw_hz, sizeof(s->mode.bw_hz));
        h = fnv(h, &s->mode.bitrate_bps, sizeof(s->mode.bitrate_bps));
        h = fnv(h, &s->dir, 1);
        if (s->dir == LC_DIR_TX && s->payload != NULL) {
            h = fnv(h, &s->payload_len, 1);
            h = fnv(h, s->payload, s->payload_len);
        }
    }
    return h;
}

static int part_seen(const lc_exec_frame_t *b, uint32_t h)
{
    uint8_t n = b->parts < LC_EXEC_MAX_PARTS ? b->parts : (uint8_t)LC_EXEC_MAX_PARTS;
    for (uint8_t i = 0; i < n; i++) {
        if (b->part_hash[i] == h) {
            return 1;
        }
    }
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
    if (b != NULL && b->state == LC_EXEC_BUF_RUNNING) {
        return LC_ACK_ERR_LATE;
    }
    uint32_t h = part_hash(part);
    if (b != NULL && part_seen(b, h)) {
        return LC_ACK_OK; /* resend after a lost ACK: already applied */
    }
    if (part->flags & LC_SCHED_FLAG_FIRST) {
        if (b == NULL) {
            b = free_buffer(e, now_frame);
            if (b == NULL) {
                return LC_ACK_ERR_MALFORMED;
            }
        }
        /* A FIRST part opens the frame, or restarts it if the host re-sends it. */
        b->frame_number = part->frame_number;
        b->state = LC_EXEC_BUF_ASSEMBLING;
        b->slot_count = 0;
        b->pool_used = 0;
        b->parts = 0;
    } else if (b == NULL || b->state != LC_EXEC_BUF_ASSEMBLING) {
        return LC_ACK_ERR_MALFORMED; /* continuation with no open frame, or after LAST */
    }
    for (uint8_t i = 0; i < part->slot_count; i++) {
        if (add_slot(b, &part->slots[i]) != 0) {
            b->state = LC_EXEC_BUF_EMPTY;
            return LC_ACK_ERR_MALFORMED;
        }
    }
    if (b->parts < LC_EXEC_MAX_PARTS) {
        b->part_hash[b->parts] = h;
    }
    b->parts++;
    if (part->flags & LC_SCHED_FLAG_LAST) {
        b->state = LC_EXEC_BUF_READY;
        if (!e->active) {
            e->active = 1;
            e->first_frame = part->frame_number;
        }
    }
    return LC_ACK_OK;
}

static lc_exec_frame_t *find_frame(lc_exec_t *e, uint32_t frame_number, uint8_t state)
{
    for (unsigned i = 0; i < LC_EXEC_FRAMES; i++) {
        lc_exec_frame_t *b = &e->frames[i];
        if (b->state == state && b->frame_number == frame_number) {
            return b;
        }
    }
    return NULL;
}

static void finish_frame(lc_exec_t *e)
{
    if (e->run != NULL) {
        if (e->phase == LC_EXEC_PH_ACTIVE || e->phase == LC_EXEC_PH_LAUNCH) {
            e->radio.standby(e->radio.ctx);
        }
        e->run->state = LC_EXEC_BUF_EMPTY;
        e->run = NULL;
    }
}

static void next_slot(lc_exec_t *e)
{
    e->slot++;
    e->phase = LC_EXEC_PH_CONFIG;
}

/* Wake one configure-lead before the next frame so its first slot can be
 * configured in time. */
static uint64_t next_frame_wake(const lc_exec_t *e, const lc_clock_t *clk, uint64_t now_us)
{
    uint64_t t;
    if (lc_clock_frame_start_us(clk, e->cur_frame + 1, &t) != 0 ||
        t <= now_us + LC_EXEC_CONFIG_LEAD_US) {
        return now_us + LC_EXEC_POLL_US;
    }
    return t - LC_EXEC_CONFIG_LEAD_US;
}

uint64_t lc_exec_step(lc_exec_t *e, const lc_clock_t *clk, uint64_t now_us)
{
    uint32_t f;
    if (lc_clock_frame_at(clk, now_us, &f) != 0) {
        finish_frame(e);
        e->have_frame = 0;
        return now_us + LC_EXEC_IDLE_US;
    }
    /* We may have entered cur_frame a configure-lead early (below): until the
     * boundary, or with a fast crystal right at it, frame_at still reports the
     * frame before. That is not a frame change. */
    if (e->have_frame && (uint32_t)(e->cur_frame - f) == 1u) {
        f = e->cur_frame;
    }
    /* Once this frame's slots are done, enter the next frame a configure-lead
     * early so its first slot is staged before the boundary. */
    if (e->have_frame && f == e->cur_frame && (e->run == NULL || e->slot >= e->run->slot_count)) {
        uint64_t next_start;
        if (lc_clock_frame_start_us(clk, f + 1, &next_start) == 0 &&
            now_us + LC_EXEC_CONFIG_LEAD_US >= next_start) {
            f = f + 1;
        }
    }
    if (!e->have_frame || f != e->cur_frame) {
        finish_frame(e);
        uint64_t start;
        if (lc_clock_frame_start_us(clk, f, &start) != 0) {
            e->have_frame = 0;
            return now_us + LC_EXEC_IDLE_US;
        }
        e->have_frame = 1;
        e->cur_frame = f;
        e->cur_start_us = start;
        e->run = find_frame(e, f, LC_EXEC_BUF_READY);
        if (e->run != NULL) {
            e->run->state = LC_EXEC_BUF_RUNNING;
            e->slot = 0;
            e->phase = LC_EXEC_PH_CONFIG;
        } else if (e->active && f > e->first_frame) {
            e->schedule_misses++;
        }
    }
    if (e->run == NULL) {
        return next_frame_wake(e, clk, now_us);
    }

    while (e->slot < e->run->slot_count) {
        const lc_exec_slot_t *s = &e->run->slots[e->slot];
        uint64_t slot_start = e->cur_start_us + s->offset_us;
        uint64_t slot_end = slot_start + s->length_us;

        switch (e->phase) {
        case LC_EXEC_PH_CONFIG:
            {
                uint32_t lead = LC_EXEC_CONFIG_LEAD_US;
                if (e->radio_band >= 0 && e->radio_band != band_of(s->freq_hz)) {
                    lead = LC_EXEC_BAND_SWITCH_LEAD_US;
                } else if (e->radio_mod >= 0 && e->radio_mod != (int8_t)s->mode.modulation) {
                    lead = LC_EXEC_MOD_SWITCH_LEAD_US;
                }
                if (now_us + lead < slot_start) {
                    return slot_start - lead;
                }
            }
            if (now_us > slot_start + LC_EXEC_LATE_US) {
                e->late_slots++;
                next_slot(e);
                continue;
            }
            if (s->dir == LC_DIR_TX && !e->tx_enabled) {
                e->tx_blocked++;
                next_slot(e);
                continue;
            }
            {
                uint8_t op = LC_EXEC_OP_CONFIGURE;
                int err = e->radio.configure(e->radio.ctx, s->freq_hz, &s->mode);
                e->radio_band = err == 0 ? band_of(s->freq_hz) : (int8_t)-1;
                e->radio_mod = err == 0 ? (int8_t)s->mode.modulation : (int8_t)-1;
                if (err == 0) {
                    op = LC_EXEC_OP_STAGE;
                    err = s->dir == LC_DIR_TX
                              ? e->radio.stage_tx(e->radio.ctx, &e->run->pool[s->payload_off], s->payload_len)
                              : e->radio.stage_rx(e->radio.ctx, s->length_us - LC_GUARD_US);
                }
                if (err != 0) {
                    note_radio_error(e, op, err);
                    e->radio.standby(e->radio.ctx);
                    next_slot(e);
                    continue;
                }
            }
            e->phase = LC_EXEC_PH_LAUNCH;
            /* fall through */
        case LC_EXEC_PH_LAUNCH:
            if (now_us + LC_RADIO_ARM_US < slot_start) {
                return slot_start - LC_RADIO_ARM_US;
            }
            if (now_us > slot_start + LC_EXEC_LATE_US) {
                e->late_slots++;
                e->radio.standby(e->radio.ctx);
                next_slot(e);
                continue;
            }
            int lerr = e->radio.launch(e->radio.ctx, slot_start);
            if (lerr != 0) {
                note_radio_error(e, LC_EXEC_OP_LAUNCH, lerr);
                e->radio.standby(e->radio.ctx);
                next_slot(e);
                continue;
            }
            e->phase = LC_EXEC_PH_ACTIVE;
            /* fall through */
        case LC_EXEC_PH_ACTIVE:
        default:
            if (e->radio.poll(e->radio.ctx, &e->ev)) {
                if (e->ev.type == LC_RADIO_EV_RX_DONE && e->sink.on_rx != NULL) {
                    e->ev.frame_offset_us = frame_offset(e, e->ev.irq_us);
                    e->sink.on_rx(e->sink.ctx, e->cur_frame, e->slot, &e->ev);
                } else if (e->ev.type == LC_RADIO_EV_TX_DONE && e->ev.irq_us != 0) {
                    e->last_tx_end_us = frame_offset(e, e->ev.irq_us);
                    e->last_tx_start_us = frame_offset(e, e->ev.start_us);
                } else if (e->ev.type == LC_RADIO_EV_ERROR) {
                    note_radio_error(e, LC_EXEC_OP_EVENT, 0);
                }
                next_slot(e);
                continue;
            }
            if (now_us >= slot_end) {
                e->overruns++;
                e->radio.standby(e->radio.ctx);
                next_slot(e);
                continue;
            }
            return (now_us + LC_EXEC_POLL_US < slot_end) ? now_us + LC_EXEC_POLL_US : slot_end;
        }
    }
    return next_frame_wake(e, clk, now_us);
}
