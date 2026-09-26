#include "lc_term.h"

#include <string.h>

#define DATA_HDR 8u /* lc_air DATA header bytes */

static const lc_mode_t *edge_mode(void)
{
    return lc_tier_mode(LC_BAND_915, LC_TIER_EDGE);
}

static uint32_t up10(uint32_t us)
{
    return (us + LC_AIR_TIME_UNIT_US - 1u) / LC_AIR_TIME_UNIT_US * LC_AIR_TIME_UNIT_US;
}

/* a is at or after b, modulo 2^32 */
static int frame_ge(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

/* ---------------------------------------------------------------- tracker */

uint64_t lc_term_trk_frame_start(const lc_term_tracker_t *k, uint32_t frame_number)
{
    int64_t d = (int32_t)(frame_number - k->anchor_frame);
    return (uint64_t)((int64_t)k->anchor_us + (d * k->period_q16) / 65536);
}

uint32_t lc_term_trk_frame_at(const lc_term_tracker_t *k, uint64_t now_us)
{
    int64_t dt = (int64_t)(now_us - k->anchor_us);
    int64_t n = (dt * 65536) / k->period_q16;
    if (dt < 0 && (n * k->period_q16) / 65536 != dt) {
        n -= 1; /* floor for times before the anchor */
    }
    return k->anchor_frame + (uint32_t)n;
}

void lc_term_trk_observe(lc_term_tracker_t *k, uint32_t frame_number, uint64_t start_us)
{
    if (!k->valid) {
        k->valid = 1;
        k->anchor_frame = frame_number;
        k->anchor_us = start_us;
        k->period_q16 = (int64_t)LC_FRAME_US * 65536;
        k->last_obs_frame = frame_number;
        return;
    }
    uint64_t pred = lc_term_trk_frame_start(k, frame_number);
    int64_t err = (int64_t)(start_us - pred);
    uint32_t df = frame_number - k->last_obs_frame;
    if (err > (int64_t)LC_TERM_RESYNC_US || err < -(int64_t)LC_TERM_RESYNC_US || df == 0 ||
        df > LC_TERM_SYNC_LOSS_FRAMES) {
        /* Too far off (wrong frame label, long gap), or a second observation
         * in the same frame: re-anchor, keep the period. */
        k->anchor_frame = frame_number;
        k->anchor_us = df == 0 ? (uint64_t)((int64_t)pred + err / 2) : start_us;
        k->last_obs_frame = frame_number;
        return;
    }
    /* PI loop: half the phase error now, 1/8 of the rate error per frame. */
    k->period_q16 += (err * 65536) / (int64_t)df / 8;
    k->anchor_frame = frame_number;
    k->anchor_us = (uint64_t)((int64_t)pred + err / 2);
    k->last_obs_frame = frame_number;
}

/* ------------------------------------------------------------- utilities */

uint32_t lc_term_tmid_from_mac(const uint8_t mac[6])
{
    uint32_t v = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    if (v == 0u || v == 0xFFFFFFFFu) {
        v ^= 0x5A5A5A5Au;
    }
    return v;
}

uint32_t lc_term_beacon_len_us(void)
{
    return up10(lc_slot_len_us(edge_mode(), LC_BEACON_MAX_BYTES));
}

uint32_t lc_term_ag_len_us(void)
{
    return up10(lc_slot_len_us(edge_mode(), LC_TERM_AG_BYTES));
}

/* Idle radio needed between two ops: a band change, or LoRa <-> FLRC. */
static uint32_t switch_us(const lc_term_op_t *a, const lc_term_op_t *b)
{
    if (a->band != b->band) return LC_TERM_BAND_SWITCH_US;
    if (a->mode != NULL && b->mode != NULL && a->mode->modulation != b->mode->modulation) return LC_TERM_MOD_SWITCH_US;
    return 0u;
}

/* Cores don't overlap, with the switch time between them where needed. */
static int cores_separated(const lc_term_op_t *a, const lc_term_op_t *b)
{
    int64_t gap = switch_us(a, b);
    int64_t a_end = (int64_t)a->nominal_us + a->core_len_us;
    int64_t b_end = (int64_t)b->nominal_us + b->core_len_us;
    return a_end + gap <= b->nominal_us || b_end + gap <= a->nominal_us;
}

static void make_op(lc_term_op_t *op, uint8_t kind, uint8_t band, int32_t nominal, uint32_t core_len,
                    uint32_t freq_hz, const lc_mode_t *mode)
{
    op->kind = kind;
    op->band = band;
    op->nominal_us = nominal;
    op->core_len_us = core_len;
    op->freq_hz = freq_hz;
    op->mode = mode;
    if (kind == LC_TOP_UL_TX || kind == LC_TOP_RACH_TX) {
        op->start_us = nominal;
        op->len_us = core_len;
    } else {
        /* The peer's packet ends at least one guard before the core ends. */
        op->start_us = nominal - (int32_t)LC_TERM_RX_MARGIN_US;
        op->len_us = core_len - LC_GUARD_US + 2u * LC_TERM_RX_MARGIN_US;
    }
}

static int leg_op(uint32_t cell_seed, const lc_grant_leg_t *leg, uint32_t frame_number, int rx,
                  lc_term_op_t *op)
{
    const lc_mode_t *mode = lc_tier_mode((lc_band_t)leg->band, (lc_tier_t)leg->tier);
    uint8_t ch = lc_grant_leg_channel(cell_seed, leg, frame_number);
    uint32_t len = (uint32_t)leg->len * LC_AIR_TIME_UNIT_US;
    if (mode == NULL || ch == LC_INVALID_CHANNEL || len <= LC_GUARD_US) {
        return -1;
    }
    make_op(op, rx ? LC_TOP_DL_RX : LC_TOP_UL_TX, leg->band, (int32_t)leg->offset * (int32_t)LC_AIR_TIME_UNIT_US,
            len, lc_channel_freq_hz((lc_band_t)leg->band, ch), mode);
    return 0;
}

int lc_term_grant_ok(const lc_grant_t *g)
{
    lc_term_op_t dl, ul;
    int have_dl = g->dl.len != 0, have_ul = g->ul.len != 0;
    if (have_dl && leg_op(0, &g->dl, 0, 1, &dl) != 0) {
        return 0;
    }
    if (have_ul && leg_op(0, &g->ul, 0, 0, &ul) != 0) {
        return 0;
    }
    if (have_ul && lc_slot_len_us(ul.mode, DATA_HDR) > ul.core_len_us) {
        return 0; /* UL slot can't even carry an empty DATA */
    }
    return !(have_dl && have_ul) || cores_separated(&dl, &ul);
}

/* The grant in force for frame_number, or NULL. */
static const lc_grant_t *grant_for(const lc_term_t *t, uint32_t frame_number)
{
    if (t->have_next && frame_ge(frame_number, t->next.effective_frame)) {
        return &t->next;
    }
    return t->have_grant ? &t->grant : NULL;
}

uint8_t lc_term_build_plan(const lc_term_t *t, uint32_t frame_number, lc_term_op_t *ops)
{
    if (t->state == LC_TERM_SEARCH) {
        return 0;
    }
    lc_term_op_t cand[LC_TERM_MAX_OPS];
    uint8_t nc = 0;
    const lc_mode_t *edge = edge_mode();

    /* Candidates in priority order: DL, UL, AG, RACH, beacon. */
    const lc_grant_t *g = (t->state == LC_TERM_GRANTED || t->state == LC_TERM_IDLE) ? grant_for(t, frame_number)
                                                                                      : NULL;
    if (g != NULL && g->dl.len != 0 && leg_op(t->cell_seed, &g->dl, frame_number, 1, &cand[nc]) == 0) {
        nc++;
    }
    if (g != NULL && g->ul.len != 0 && leg_op(t->cell_seed, &g->ul, frame_number, 0, &cand[nc]) == 0) {
        nc++;
    }
    if (t->ag_waiting && frame_ge(t->ag_until, frame_number) && frame_number != t->rach_frame) {
        uint8_t ch = lc_hop_channel(t->cell_seed, LC_BAND_915, 0, frame_number, LC_TERM_AG_SLOT_INDEX);
        make_op(&cand[nc++], LC_TOP_AG_RX, LC_BAND_915, (int32_t)lc_term_beacon_len_us(), lc_term_ag_len_us(),
                lc_channel_freq_hz(LC_BAND_915, ch), edge);
    }
    if (t->rach_pending && frame_number == t->rach_frame && t->beacon.rach_len != 0) {
        uint32_t need = lc_slot_len_us(edge, (uint8_t)(7u + t->rach_len));
        uint32_t win = (uint32_t)t->beacon.rach_len * LC_AIR_TIME_UNIT_US;
        uint8_t ch = lc_hop_channel(t->cell_seed, LC_BAND_915, 0, frame_number, t->beacon.rach_slot_index);
        if (need <= win && ch != LC_INVALID_CHANNEL) {
            make_op(&cand[nc++], LC_TOP_RACH_TX, LC_BAND_915,
                    (int32_t)t->beacon.rach_offset * (int32_t)LC_AIR_TIME_UNIT_US, need,
                    lc_channel_freq_hz(LC_BAND_915, ch), edge);
        }
    }
    make_op(&cand[nc++], LC_TOP_BEACON_RX, LC_BAND_915, 0, lc_term_beacon_len_us(),
            lc_channel_freq_hz(LC_BAND_915, lc_sync_channel(t->cell_seed, LC_BAND_915, frame_number)), edge);

    /* Keep each candidate only if its core clears every core already kept. */
    uint8_t n = 0;
    for (uint8_t i = 0; i < nc; i++) {
        int ok = 1;
        for (uint8_t j = 0; j < n && ok; j++) {
            ok = cores_separated(&cand[i], &ops[j]);
        }
        if (ok) {
            ops[n++] = cand[i];
        }
    }
    /* Sort by core start (insertion sort, n <= LC_TERM_MAX_OPS). */
    for (uint8_t i = 1; i < n; i++) {
        lc_term_op_t tmp = ops[i];
        uint8_t j = i;
        while (j > 0 && ops[j - 1].nominal_us > tmp.nominal_us) {
            ops[j] = ops[j - 1];
            j--;
        }
        ops[j] = tmp;
    }
    /* Clip RX windows: close before the next op's core (minus a band
     * switch), open after the previous op's window (plus a band switch). */
    for (uint8_t i = 0; i + 1 < n; i++) {
        if (ops[i].kind == LC_TOP_UL_TX || ops[i].kind == LC_TOP_RACH_TX) {
            continue;
        }
        int64_t limit = (int64_t)ops[i + 1].nominal_us - switch_us(&ops[i], &ops[i + 1]);
        if ((int64_t)ops[i].start_us + ops[i].len_us > limit) {
            ops[i].len_us = (uint32_t)(limit - ops[i].start_us);
        }
    }
    for (uint8_t i = 1; i < n; i++) {
        if (ops[i].kind == LC_TOP_UL_TX || ops[i].kind == LC_TOP_RACH_TX) {
            continue;
        }
        int64_t earliest = (int64_t)ops[i - 1].start_us + ops[i - 1].len_us + switch_us(&ops[i - 1], &ops[i]);
        if (ops[i].start_us < earliest) {
            ops[i].len_us -= (uint32_t)(earliest - ops[i].start_us);
            ops[i].start_us = (int32_t)earliest;
        }
    }
    return n;
}
