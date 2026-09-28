#include "lc_term.h"

#include <string.h>

#define PH_CONFIG 0u
#define PH_LAUNCH 1u
#define PH_ACTIVE 2u
#define POLL_US   200u
#define DATA_HDR  8u /* lc_air DATA header bytes */

static const lc_mode_t *edge_mode(void)
{
    return lc_tier_mode(LC_BAND_915, LC_TIER_EDGE);
}

/* a is at or after b, modulo 2^32 */
static int frame_ge(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

/* ----------------------------------------------------------- state logic */

static void notify(lc_term_t *t)
{
    if (t->sink.on_status != NULL) {
        t->sink.on_status(t->sink.ctx);
    }
}

/* The serving cell's anchor frequency. */
static uint32_t anchor_hz(const lc_term_t *t)
{
    return lc_channel_freq_hz(LC_BAND_915, t->anchor);
}

static void set_state(lc_term_t *t, uint8_t state)
{
    if (t->state != state) {
        t->state = state;
        if (state == LC_TERM_IDLE) {
            /* attached: this cell is the one to look for first next time */
            lc_term_scan_serving(&t->scan, anchor_hz(t), t->fixed_sync);
        }
        notify(t);
    }
}

static uint32_t rnd(lc_term_t *t)
{
    return t->sink.rand32 != NULL ? t->sink.rand32(t->sink.ctx) : 0u;
}

static void rach_schedule(lc_term_t *t)
{
    t->rach_frame = t->cur_frame + 1u + rnd(t) % t->backoff;
}

static void rach_start(lc_term_t *t, uint8_t kind, const uint8_t *payload, uint8_t len)
{
    t->rach_pending = 1;
    t->ag_waiting = 0;
    t->rach_kind = kind;
    t->rach_len = len;
    if (len > 0) {
        memcpy(t->rach_payload, payload, len);
    }
    t->backoff = LC_TERM_BACKOFF_MIN;
    t->page_tries = 0;
    rach_schedule(t);
}

static void drop_grants(lc_term_t *t)
{
    t->have_grant = 0;
    t->have_next = 0;
    t->dl_miss = 0;
    t->upq_count = 0;
}

static void enter_search(lc_term_t *t)
{
    drop_grants(t);
    t->rach_pending = 0;
    t->ag_waiting = 0;
    t->trk.valid = 0;
    t->have_frame = 0;
    t->search_active = 0;
    t->search_until_us = 0;
    /* a fresh scan from the top of the list: what was heard before the cell
     * was lost is not shown */
    lc_term_scan_restart(&t->scan);
    t->noise_sampled = 0;
    memset(&t->scan_cur, 0, sizeof(t->scan_cur));
    memset(&t->scan_prev, 0, sizeof(t->scan_prev));
    t->heard_us = 0;
    t->radio.standby(t->radio.ctx);
    set_state(t, LC_TERM_SEARCH);
}

static void start_attach(lc_term_t *t)
{
    drop_grants(t);
    rach_start(t, LC_RACH_ATTACH, NULL, 0);
    set_state(t, LC_TERM_ATTACHING);
}

/* Per-frame housekeeping before the frame's plan is built. */
static void frame_tick(lc_term_t *t, uint32_t f)
{
    if ((int32_t)(f - t->trk.last_obs_frame) > (int32_t)LC_TERM_SYNC_LOSS_FRAMES) {
        t->sync_losses++;
        enter_search(t);
        return;
    }
    if (t->have_next && frame_ge(f, t->next.effective_frame)) {
        t->grant = t->next;
        t->have_grant = 1;
        t->have_next = 0;
        t->dl_miss = 0;
        uint8_t st = (t->grant.dl.len != 0 || t->grant.ul.len != 0) ? LC_TERM_GRANTED : LC_TERM_IDLE;
        if (st == t->state) {
            notify(t); /* band/tier may have changed */
        }
        set_state(t, st);
    }
    if (t->ag_waiting && !frame_ge(t->ag_until, f)) {
        /* No grant: back off and retry (pages give up after LC_TERM_PAGE_TRIES). */
        t->ag_waiting = 0;
        if (t->rach_kind == LC_RACH_PAGE_REPLY && ++t->page_tries >= LC_TERM_PAGE_TRIES) {
            return;
        }
        t->backoff = t->backoff * 2u > LC_TERM_BACKOFF_MAX ? LC_TERM_BACKOFF_MAX : t->backoff * 2u;
        t->rach_pending = 1;
        rach_schedule(t);
    }
    if (t->rach_pending && !frame_ge(t->rach_frame, f)) {
        rach_schedule(t); /* missed its frame (op skipped, no RACH window): try again */
    }
}

static int same_grant(const lc_grant_t *a, const lc_grant_t *b)
{
    return a->tmid == b->tmid && a->effective_frame == b->effective_frame &&
           memcmp(&a->dl, &b->dl, sizeof(a->dl)) == 0 && memcmp(&a->ul, &b->ul, sizeof(a->ul)) == 0;
}

static void on_grant(lc_term_t *t, const lc_grant_t *g)
{
    if (g->tmid != t->tmid) {
        return;
    }
    if ((t->have_next && same_grant(g, &t->next)) || (t->have_grant && same_grant(g, &t->grant))) {
        return; /* the host sends every grant more than once */
    }
    if (!lc_term_grant_ok(g)) {
        t->bad_grants++;
        return;
    }
    t->ag_waiting = 0;
    t->rach_pending = 0;
    if (g->dl.len == 0 && g->ul.len == 0) {
        drop_grants(t);
        set_state(t, LC_TERM_IDLE);
        return;
    }
    t->next = *g;
    t->have_next = 1;
    if (t->state == LC_TERM_ATTACHING) {
        set_state(t, LC_TERM_IDLE); /* attached; GRANTED once the grant takes effect */
    }
}

/* A 915 beacon. Returns 1 if it is the serving cell's (or, searching, the
 * cell just found), else 0. */
static int on_beacon(lc_term_t *t, const lc_beacon_t *b, uint64_t start_us)
{
    if (b->band != LC_BAND_915) {
        return 0;
    }
    int fixed = (b->flags & LC_BCN_FLAG_FIXED_SYNC) != 0;
    if (fixed && !(b->flags & LC_BCN_FLAG_PART97)) {
        t->bad_beacons++; /* FIXED is Part 97 only: a mis-set Part 15 cell is never followed */
        return 0;
    }
    if (t->state == LC_TERM_SEARCH) {
        /* the beacon names its anchor, so a sync on another cell's cycle
         * crossing this frequency still follows the right channels */
        t->cell_seed = b->cell_seed;
        t->anchor = b->anchor;
        t->fixed_sync = (uint8_t)fixed;
        t->trk.valid = 0;
        lc_term_trk_observe(&t->trk, b->frame_number, start_us);
        t->have_frame = 0;
        set_state(t, LC_TERM_SYNCED);
    } else {
        if (b->cell_seed != t->cell_seed || b->anchor != t->anchor) {
            return 0; /* a neighbour's beacon on the same channel, or another cell with this seed */
        }
        t->fixed_sync = (uint8_t)fixed;
        lc_term_trk_observe(&t->trk, b->frame_number, start_us);
    }
    t->beacon = *b;
    t->beacons++;
    if (t->state == LC_TERM_SYNCED && (b->flags & LC_BCN_FLAG_ACCEPTING_ATTACH)) {
        t->cur_frame = b->frame_number;
        start_attach(t);
        return 1;
    }
    if (t->state == LC_TERM_IDLE && !t->have_next && !t->rach_pending && !t->ag_waiting) {
        for (uint8_t i = 0; i < b->page_count; i++) {
            if (b->page_tmid[i] == t->tmid) {
                rach_start(t, LC_RACH_PAGE_REPLY, NULL, 0);
                break;
            }
        }
    }
    return 1;
}

/* A frame's op finished (ev == NULL: skipped or abandoned). */
/* How long before an op its configuration must start (as lc_exec). */
static uint32_t op_lead_us(const lc_term_t *t, const lc_term_op_t *op)
{
    if (t->radio_band >= 0 && t->radio_band != (int8_t)op->band) return LC_TERM_BAND_SWITCH_US;
    if (t->radio_mod >= 0 && t->radio_mod != (int8_t)op->mode->modulation) return LC_TERM_MOD_SWITCH_US;
    return LC_TERM_CONFIG_LEAD_US;
}

static void op_done(lc_term_t *t, const lc_term_op_t *op, const lc_radio_event_t *ev, uint64_t ev_us)
{
    int good = 0; /* a packet for us (or our cell's beacon) was decoded */
    if (ev != NULL && ev->type == LC_RADIO_EV_RX_DONE && ev->crc_ok) {
        lc_air_msg_t m;
        /* The IRQ comes lc_rx_done_lag_us after the packet's end; its start
         * is where the peer put it in the frame. */
        uint64_t start = ev_us - lc_airtime_us(op->mode, ev->len) - lc_rx_done_lag_us(op->mode) -
                         (uint64_t)(int64_t)op->nominal_us;
        if (lc_air_decode(ev->data, ev->len, &m) == 0) {
            if (op->kind == LC_TOP_BEACON_RX && m.type == LC_AIR_BEACON) {
                good = on_beacon(t, &m.u.beacon, start);
            } else if ((op->kind == LC_TOP_DL_RX || op->kind == LC_TOP_AG_RX) &&
                       ((m.type == LC_AIR_GRANT && m.u.grant.tmid == t->tmid) ||
                        (m.type == LC_AIR_DATA && m.u.data.tmid == t->tmid))) {
                lc_term_trk_observe(&t->trk, t->cur_frame, start);
                if (m.type == LC_AIR_GRANT) {
                    on_grant(t, &m.u.grant);
                } else if (m.u.data.payload_len > 0 && t->sink.on_downlink != NULL) {
                    t->sink.on_downlink(t->sink.ctx, m.u.data.payload, m.u.data.payload_len);
                }
                good = 1;
            }
        }
        if (good) {
            t->rssi_dbm = ev->rssi_dbm;
            t->snr_qdb = ev->snr_qdb;
        }
    }

    switch (op->kind) {
    case LC_TOP_DL_RX:
        if (good) {
            t->dl_miss = 0;
        } else if (t->state == LC_TERM_GRANTED && ++t->dl_miss >= LC_TERM_DL_LOSS_FRAMES) {
            /* Spec §4.6: DL lost (e.g. 2.4 GHz out of range) -> back to 915 RACH. */
            start_attach(t);
        }
        break;
    case LC_TOP_RACH_TX:
        if (ev != NULL && ev->type == LC_RADIO_EV_TX_DONE) {
            t->rach_pending = 0;
            if (t->rach_kind != LC_RACH_UPPER) {
                t->ag_waiting = 1;
                t->ag_until = t->cur_frame + LC_TERM_AG_FRAMES;
            }
        } else {
            rach_schedule(t);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- executor */

static size_t build_tx(lc_term_t *t, const lc_term_op_t *op)
{
    lc_air_msg_t m;
    memset(&m, 0, sizeof(m));
    if (op->kind == LC_TOP_RACH_TX) {
        m.type = LC_AIR_RACH;
        m.u.rach = (lc_rach_t){ t->tmid, t->rach_kind, t->rach_len, t->rach_payload };
    } else {
        m.type = LC_AIR_DATA;
        m.u.data.tmid = t->tmid;
        m.u.data.seq = t->ul_seq++;
        if (t->upq_count > 0) {
            const lc_term_upmsg_t *u = &t->upq[t->upq_head];
            if (lc_slot_len_us(op->mode, (uint8_t)(DATA_HDR + u->len)) <= op->core_len_us) {
                m.u.data.payload_len = u->len;
                m.u.data.payload = u->data;
            }
            t->upq_head = (uint8_t)((t->upq_head + 1u) % LC_TERM_UPQ_DEPTH);
            t->upq_count--; /* sent, or dropped if the grant shrank below it */
        }
    }
    return lc_air_encode(&m, t->tx_buf, sizeof(t->tx_buf));
}

static void start_frame(lc_term_t *t, uint32_t f, uint64_t now_us)
{
    if (lc_term_trk_frame_start(&t->trk, f) + LC_FRAME_US < now_us) {
        f = lc_term_trk_frame_at(&t->trk, now_us); /* stalled: jump to the present */
    }
    t->have_frame = 1;
    t->cur_frame = f;
    t->cur_start_us = lc_term_trk_frame_start(&t->trk, f);
    frame_tick(t, f);
    t->op_count = t->state == LC_TERM_SEARCH ? 0 : lc_term_build_plan(t, f, t->ops);
    t->op_idx = 0;
    t->phase = PH_CONFIG;
}

/* ------------------------------------------- what the search hears */

/* Any packet on a search channel: CRC good or not, any cell. */
static void scan_packet(lc_term_t *t, const lc_radio_event_t *ev, uint64_t now_us)
{
    if (ev->rssi_dbm >= 0) {
        return; /* getRSSI() returns 0 on a failed read (lc_radio.cpp): not a reading */
    }
    int was = t->scan_cur.heard || t->scan_prev.heard;
    lc_term_heard_t *s = &t->scan_cur;
    if (!s->heard || ev->rssi_dbm > s->rssi_dbm) {
        s->heard = 1;
        s->rssi_dbm = ev->rssi_dbm;
        s->snr_qdb = ev->snr_qdb;
    }
    t->heard_us = now_us;
    if (!was) {
        notify(t); /* NO SIGNAL -> a signal */
    }
}

/* Once per dwell, while the receiver runs. */
static void scan_noise(lc_term_t *t)
{
    int16_t dbm;
    t->noise_sampled = 1;
    if (t->radio.rssi_inst == NULL || t->radio.rssi_inst(t->radio.ctx, &dbm) != 0 || dbm >= 0) {
        return; /* no such op, a failed read, or not a reading */
    }
    if (t->scan_cur.noise_dbm == LC_TERM_NO_DBM || dbm < t->scan_cur.noise_dbm) {
        t->scan_cur.noise_dbm = dbm;
    }
}

/* The dwell is over: on to the next entry. After a round, the pass in
 * progress becomes the last full pass and a new pass starts (the next
 * dwell's notify shows it). */
static void next_candidate(lc_term_t *t)
{
    t->search_until_us = 0;
    t->noise_sampled = 0;
    if (lc_term_scan_advance(&t->scan)) {
        t->scan_prev = t->scan_cur;
        memset(&t->scan_cur, 0, sizeof(t->scan_cur));
    }
}

/* The last full pass merged with the pass in progress. */
static void scan_merged(const lc_term_t *t, lc_term_heard_t *out)
{
    const lc_term_heard_t *c = &t->scan_cur;
    *out = t->scan_prev;
    if (c->heard && (!out->heard || c->rssi_dbm > out->rssi_dbm)) {
        out->heard = 1;
        out->rssi_dbm = c->rssi_dbm;
        out->snr_qdb = c->snr_qdb;
    }
    if (c->noise_dbm != LC_TERM_NO_DBM && (out->noise_dbm == LC_TERM_NO_DBM || c->noise_dbm < out->noise_dbm)) {
        out->noise_dbm = c->noise_dbm;
    }
}

static uint64_t search_step(lc_term_t *t, uint64_t now_us)
{
    if (!t->search_active) {
        /* A new dwell on the scan list's next entry; within a dwell the
         * receiver re-arms on the same frequency after every packet. */
        if (t->search_until_us == 0 || now_us >= t->search_until_us) {
            uint32_t dwell;
            lc_term_scan_next(&t->scan, &t->search_freq, &dwell);
            t->search_until_us = now_us + dwell;
            notify(t); /* STATUS shows the entry being scanned */
        }
        uint32_t remain = (uint32_t)(t->search_until_us - now_us);
        t->radio_band = LC_BAND_915; /* the search listens on 915 edge */
        t->radio_mod = (int8_t)edge_mode()->modulation;
        if (t->radio.configure(t->radio.ctx, t->search_freq, edge_mode()) != 0 ||
            t->radio.stage_rx(t->radio.ctx, remain) != 0 || t->radio.launch(t->radio.ctx, 0) != 0) {
            t->radio_errors++;
            t->radio_band = t->radio_mod = -1;
            return now_us + LC_TERM_IDLE_US;
        }
        t->search_active = 1;
        t->irq_us = 0;
        return now_us + POLL_US;
    }
    if (!t->radio.poll(t->radio.ctx, &t->ev)) {
        /* May land on a packet in flight (poll() returns 0 through preamble/header
         * too); harmless: scan_noise keeps the pass's lowest reading. */
        if (!t->noise_sampled && now_us + LC_TERM_NOISE_LEAD_US >= t->search_until_us) {
            scan_noise(t);
        }
        if (now_us > t->search_until_us + LC_TERM_OVERRUN_US) {
            t->radio.standby(t->radio.ctx);
            t->overruns++;
            t->search_active = 0;
            next_candidate(t);
        }
        return now_us + POLL_US;
    }
    t->search_active = 0;
    uint64_t ev_us = (t->irq_us != 0 && t->irq_us <= now_us) ? t->irq_us : now_us;
    if (t->ev.type == LC_RADIO_EV_RX_DONE) {
        scan_packet(t, &t->ev, now_us);
    }
    if (t->ev.type == LC_RADIO_EV_RX_DONE && t->ev.crc_ok) {
        lc_air_msg_t m;
        if (lc_air_decode(t->ev.data, t->ev.len, &m) == 0 && m.type == LC_AIR_BEACON &&
            on_beacon(t, &m.u.beacon,
                      ev_us - lc_airtime_us(edge_mode(), t->ev.len) - lc_rx_done_lag_us(edge_mode()))) {
            t->rssi_dbm = t->ev.rssi_dbm;
            t->snr_qdb = t->ev.snr_qdb;
            t->search_until_us = 0;
            return now_us;
        }
    }
    if (now_us >= t->search_until_us) {
        next_candidate(t); /* dwell over */
    }
    return now_us; /* re-arm (same candidate for the rest of its dwell) */
}

uint64_t lc_term_step(lc_term_t *t, uint64_t now_us)
{
    t->last_step_us = now_us;
    for (;;) {
        if (t->state == LC_TERM_SEARCH) {
            return search_step(t, now_us);
        }
        if (!t->have_frame) {
            start_frame(t, lc_term_trk_frame_at(&t->trk, now_us + LC_TERM_CONFIG_LEAD_US + LC_TERM_RX_MARGIN_US),
                        now_us);
            continue;
        }
        if (t->op_idx >= t->op_count) {
            start_frame(t, t->cur_frame + 1u, now_us);
            if (t->op_count == 0 && t->state != LC_TERM_SEARCH) {
                uint64_t next = t->cur_start_us + LC_FRAME_US - LC_TERM_CONFIG_LEAD_US - LC_TERM_RX_MARGIN_US;
                return next > now_us ? next : now_us + LC_TERM_IDLE_US;
            }
            continue;
        }
        const lc_term_op_t *op = &t->ops[t->op_idx];
        uint64_t start = (uint64_t)((int64_t)t->cur_start_us + op->start_us);

        if (t->phase == PH_CONFIG) {
            uint32_t lead = op_lead_us(t, op);
            if (now_us + lead < start) {
                return start - lead;
            }
            if (now_us > start + LC_TERM_LATE_US) {
                t->skipped_ops++;
                op_done(t, op, NULL, now_us);
                t->op_idx++;
                continue;
            }
            int err = t->radio.configure(t->radio.ctx, op->freq_hz, op->mode);
            t->radio_band = err == 0 ? (int8_t)op->band : (int8_t)-1;
            t->radio_mod = err == 0 ? (int8_t)op->mode->modulation : (int8_t)-1;
            if (err == 0) {
                if (op->kind == LC_TOP_UL_TX || op->kind == LC_TOP_RACH_TX) {
                    size_t n = build_tx(t, op);
                    err = n == 0 ? -1 : t->radio.stage_tx(t->radio.ctx, t->tx_buf, (uint8_t)n);
                } else {
                    err = t->radio.stage_rx(t->radio.ctx, op->len_us);
                }
            }
            if (err != 0) {
                t->radio_errors++;
                op_done(t, op, NULL, now_us);
                t->op_idx++;
                continue;
            }
            t->phase = PH_LAUNCH;
        }
        if (t->phase == PH_LAUNCH) {
            if (now_us + LC_RADIO_ARM_US < start) {
                return start - LC_RADIO_ARM_US;
            }
            if (now_us > start + LC_TERM_LATE_US || t->radio.launch(t->radio.ctx, start) != 0) {
                t->radio.standby(t->radio.ctx);
                if (now_us > start + LC_TERM_LATE_US) {
                    t->skipped_ops++;
                } else {
                    t->radio_errors++;
                }
                op_done(t, op, NULL, now_us);
                t->op_idx++;
                t->phase = PH_CONFIG;
                continue;
            }
            t->irq_us = 0;
            t->phase = PH_ACTIVE;
            return now_us + POLL_US;
        }
        /* PH_ACTIVE */
        if (t->radio.poll(t->radio.ctx, &t->ev)) {
            uint64_t ev_us = (t->irq_us != 0 && t->irq_us <= now_us) ? t->irq_us : now_us;
            if (t->ev.type == LC_RADIO_EV_ERROR) {
                t->radio_errors++;
            }
            op_done(t, op, &t->ev, ev_us);
            t->op_idx++;
            t->phase = PH_CONFIG;
            continue;
        }
        if (now_us > start + op->len_us + LC_TERM_OVERRUN_US) {
            t->radio.standby(t->radio.ctx);
            t->overruns++;
            op_done(t, op, NULL, now_us);
            t->op_idx++;
            t->phase = PH_CONFIG;
            continue;
        }
        return now_us + POLL_US;
    }
}

/* ------------------------------------------------------------- public */

void lc_term_init(lc_term_t *t, const lc_radio_ops_t *radio, const lc_term_sink_t *sink, uint32_t tmid)
{
    memset(t, 0, sizeof(*t));
    t->radio = *radio;
    if (sink != NULL) {
        t->sink = *sink;
    }
    t->tmid = tmid;
    t->state = LC_TERM_SEARCH;
    lc_term_scan_init(&t->scan);
    t->backoff = LC_TERM_BACKOFF_MIN;
    t->radio_band = -1;
    t->radio_mod = -1;
}


void lc_term_note_irq(lc_term_t *t, uint64_t irq_us)
{
    t->irq_us = irq_us;
}

int lc_term_send_upper(lc_term_t *t, const uint8_t *data, uint8_t len)
{
    if (len > LC_TERM_DATA_MAX_PAYLOAD) {
        return -1;
    }
    if (t->state == LC_TERM_GRANTED && t->have_grant && t->grant.ul.len != 0) {
        const lc_mode_t *m = lc_tier_mode((lc_band_t)t->grant.ul.band, (lc_tier_t)t->grant.ul.tier);
        uint32_t slot = (uint32_t)t->grant.ul.len * LC_AIR_TIME_UNIT_US;
        if (t->upq_count >= LC_TERM_UPQ_DEPTH || lc_slot_len_us(m, (uint8_t)(DATA_HDR + len)) > slot) {
            return -1; /* queue full, or it could never fit this UL slot */
        }
        lc_term_upmsg_t *u = &t->upq[(t->upq_head + t->upq_count) % LC_TERM_UPQ_DEPTH];
        u->len = len;
        memcpy(u->data, data, len);
        t->upq_count++;
        return 0;
    }
    if (t->state == LC_TERM_IDLE && len <= LC_TERM_RACH_MAX_PAYLOAD && !t->rach_pending && !t->ag_waiting) {
        rach_start(t, LC_RACH_UPPER, data, len);
        return 0;
    }
    return -1;
}

void lc_term_status(const lc_term_t *t, lc_term_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = t->state;
    out->band = LC_BAND_915;
    out->tier = LC_TIER_EDGE;
    if (t->have_grant) {
        const lc_grant_leg_t *leg = t->grant.dl.len != 0 ? &t->grant.dl : &t->grant.ul;
        out->band = leg->band;
        out->tier = leg->tier;
    }
    out->rssi_dbm = t->rssi_dbm;
    out->snr_qdb = t->snr_qdb;
    out->tmid = t->tmid;
    out->frame = t->state == LC_TERM_SEARCH ? 0u : t->cur_frame;
    out->cell_seed = t->cell_seed;
    if (t->state == LC_TERM_SEARCH) {
        /* No cell: what the scan hears, not the last packet of a lost cell. */
        lc_term_heard_t s;
        scan_merged(t, &s);
        out->heard = s.heard;
        out->rssi_dbm = s.heard ? s.rssi_dbm : 0;
        out->snr_qdb = s.heard ? s.snr_qdb : 0;
        out->noise_dbm = s.noise_dbm;
        if (s.heard) {
            uint64_t age_s = (t->last_step_us - t->heard_us) / 1000000u;
            out->heard_age_s = (uint16_t)(age_s > 0xFFFFu ? 0xFFFFu : age_s);
        }
        out->scan_pos = t->scan.cur_pos;
        out->scan_len = t->scan.cur_len;
        out->scan_src = t->scan.cur_src;
        out->scan_pass = (uint8_t)(t->scan.passes < 0xFFu ? t->scan.passes + 1u : 0xFFu);
        out->freq_khz = t->scan.cur_freq / 1000u;
    } else {
        out->freq_khz = anchor_hz(t) / 1000u;
    }
}
