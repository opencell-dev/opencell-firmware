#include "lcb_cell.h"

#include <string.h>

#include "lc_term.h"

#define AG_SLOT_INDEX   1u
#define RACH_SLOT_INDEX 2u
#define TERM_SLOT_INDEX 8u

static uint32_t up10(uint32_t us)
{
    return (us + LC_AIR_TIME_UNIT_US - 1u) / LC_AIR_TIME_UNIT_US * LC_AIR_TIME_UNIT_US;
}

static uint32_t rach_len_us(void)
{
    return up10(lc_slot_len_us(lc_tier_mode(LC_BAND_915, LC_TIER_EDGE), (uint8_t)(7u + LCB_CELL_RACH_PAYLOAD)));
}

static uint32_t rach_off_us(void)
{
    return LC_FRAME_US - rach_len_us();
}

void lcb_cell_init(lcb_cell_t *c, uint32_t cell_seed, lc_tier_t tier, lc_band_t dl_band, lc_band_t ul_band)
{
    memset(c, 0, sizeof(*c));
    c->cell_seed = cell_seed;
    c->tier = tier;
    c->dl_band = dl_band;
    c->ul_band = ul_band;
    for (unsigned b = 0; b < LC_BAND_COUNT; b++) {
        for (unsigned i = 0; i < LCB_CELL_KIND_FRAMES; i++) {
            c->kinds[b][i].frame = 0xFFFFFFFFu;
        }
    }
}

void lcb_cell_page(lcb_cell_t *c, uint32_t tmid)
{
    c->page_tmid = tmid;
}

int lcb_cell_legs(const lcb_cell_t *c, uint8_t k, lc_grant_leg_t *dl, lc_grant_leg_t *ul)
{
    memset(dl, 0, sizeof(*dl));
    memset(ul, 0, sizeof(*ul));
    const lc_mode_t *dm = lc_tier_mode(c->dl_band, c->tier);
    const lc_mode_t *um = lc_tier_mode(c->ul_band, c->tier);
    if (dm == NULL || um == NULL || k >= LCB_CELL_MAX_TERMS) {
        return -1;
    }
    uint32_t dl_len = up10(lc_slot_len_us(dm, LCB_CELL_SLOT_BYTES));
    uint32_t ul_len = up10(lc_slot_len_us(um, LCB_CELL_SLOT_BYTES));
    uint32_t turn = LC_TERM_BAND_SWITCH_US > LC_GUARD_US ? LC_TERM_BAND_SWITCH_US : LC_GUARD_US;
    uint32_t dl_start = lc_term_beacon_len_us() + lc_term_ag_len_us();
    uint32_t split = dl_start + LCB_CELL_MAX_TERMS * dl_len;
    uint32_t ul_off = split + turn + k * ul_len;
    if (split + turn + LCB_CELL_MAX_TERMS * ul_len > rach_off_us()) {
        return -1;
    }
    *dl = (lc_grant_leg_t){ (uint8_t)c->dl_band, (uint8_t)c->tier, 0, (uint8_t)(TERM_SLOT_INDEX + 2u * k),
                            (uint16_t)((dl_start + k * dl_len) / LC_AIR_TIME_UNIT_US),
                            (uint16_t)(dl_len / LC_AIR_TIME_UNIT_US) };
    *ul = (lc_grant_leg_t){ (uint8_t)c->ul_band, (uint8_t)c->tier, 0, (uint8_t)(TERM_SLOT_INDEX + 2u * k + 1u),
                            (uint16_t)(ul_off / LC_AIR_TIME_UNIT_US), (uint16_t)(ul_len / LC_AIR_TIME_UNIT_US) };
    return 0;
}

/* Queue a grant for terminal k; delivered in the AG slot or the DL slot. */
static void queue_grant(lcb_cell_t *c, uint8_t k, int legs, int via_ag)
{
    lcb_cell_term_t *t = &c->terms[k];
    memset(&t->next, 0, sizeof(t->next));
    t->next.tmid = t->tmid;
    if (legs) {
        lcb_cell_legs(c, k, &t->next.dl, &t->next.ul);
    }
    t->next.effective_frame = 0; /* set when first sent */
    t->have_next = 1;
    t->next_via_ag = via_ag;
    t->next_tx_left = LCB_CELL_GRANT_REPEATS;
}

void lcb_cell_set_bands(lcb_cell_t *c, lc_band_t dl_band, lc_band_t ul_band)
{
    c->dl_band = dl_band;
    c->ul_band = ul_band;
    for (uint8_t k = 0; k < LCB_CELL_MAX_TERMS; k++) {
        if (c->terms[k].used && c->terms[k].have_cur && !c->terms[k].have_next) {
            queue_grant(c, k, 1, 0);
        }
    }
}

typedef struct {
    lcb_cell_t       *c;
    lc_band_t         band;
    lc_msg_t         *out;
    lcb_cell_kinds_t *kinds;
} builder_t;

static void add_slot(builder_t *b, uint8_t kind, uint8_t term, uint32_t off, uint32_t len, lc_band_t band,
                     uint8_t channel, lc_tier_t tier, uint8_t dir, const lc_air_msg_t *tx)
{
    lc_schedule_t *s = &b->out->u.schedule;
    uint8_t i = s->slot_count;
    if (i >= LCB_CELL_MAX_SLOTS) {
        return;
    }
    lc_slot_t *sl = &s->slots[i];
    memset(sl, 0, sizeof(*sl));
    sl->offset_us = off;
    sl->length_us = len;
    sl->freq_hz = lc_channel_freq_hz(band, channel);
    sl->mode = *lc_tier_mode(band, tier);
    sl->dir = dir;
    if (tx != NULL) {
        size_t n = lc_air_encode(tx, b->c->payloads[b->band][i], LC_AIR_MAX_FRAME);
        if (n == 0) {
            return;
        }
        sl->payload_len = (uint8_t)n;
        sl->payload = b->c->payloads[b->band][i];
    }
    b->kinds->kind[i] = kind;
    b->kinds->term[i] = term;
    b->kinds->count = (uint8_t)(i + 1u);
    s->slot_count++;
}

/* Promote grants taking effect at f; stamp and count grants sent in f. */
static void grant_housekeeping(lcb_cell_t *c, uint32_t f)
{
    for (uint8_t k = 0; k < LCB_CELL_MAX_TERMS; k++) {
        lcb_cell_term_t *t = &c->terms[k];
        if (t->used && t->have_next && t->next_tx_left == 0 && (int32_t)(f - t->next.effective_frame) >= 0) {
            t->cur = t->next;
            t->have_cur = t->cur.dl.len != 0 || t->cur.ul.len != 0;
            t->have_next = 0;
        }
    }
}

static int send_grant_now(lcb_cell_term_t *t, uint32_t f, lc_air_msg_t *m)
{
    if (t->next.effective_frame == 0) {
        t->next.effective_frame = f + LCB_CELL_GRANT_LEAD;
    }
    memset(m, 0, sizeof(*m));
    m->type = LC_AIR_GRANT;
    m->u.grant = t->next;
    t->next_tx_left--;
    return 1;
}

int lcb_cell_schedule(lcb_cell_t *c, lc_band_t band, uint32_t f, lc_msg_t *out)
{
    if (c->off || (unsigned)band >= LC_BAND_COUNT) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->type = LC_MSG_SCHEDULE;
    out->u.schedule.frame_number = f;
    out->u.schedule.flags = LC_SCHED_FLAG_LAST;
    lcb_cell_kinds_t *kd = &c->kinds[band][f % LCB_CELL_KIND_FRAMES];
    memset(kd, 0, sizeof(*kd));
    kd->frame = f;
    builder_t b = { c, band, out, kd };
    lc_air_msg_t m;

    if (band == LC_BAND_915) {
        grant_housekeeping(c, f);
        memset(&m, 0, sizeof(m));
        m.type = LC_AIR_BEACON;
        m.u.beacon.cell_seed = c->cell_seed;
        m.u.beacon.frame_number = f;
        m.u.beacon.band = LC_BAND_915;
        m.u.beacon.flags = LC_BCN_FLAG_ACCEPTING_ATTACH;
        m.u.beacon.rach_offset = (uint16_t)(rach_off_us() / LC_AIR_TIME_UNIT_US);
        m.u.beacon.rach_len = (uint16_t)(rach_len_us() / LC_AIR_TIME_UNIT_US);
        m.u.beacon.rach_slot_index = RACH_SLOT_INDEX;
        if (c->page_tmid != 0) {
            m.u.beacon.page_count = 1;
            m.u.beacon.page_tmid[0] = c->page_tmid;
        }
        add_slot(&b, LCB_SLOT_BEACON, 0xFF, 0, lc_term_beacon_len_us(), LC_BAND_915,
                 lc_sync_channel(c->cell_seed, LC_BAND_915, f), LC_TIER_EDGE, LC_DIR_TX, &m);

        for (uint8_t k = 0; k < LCB_CELL_MAX_TERMS; k++) { /* one AG grant per frame */
            lcb_cell_term_t *t = &c->terms[k];
            if (t->used && t->have_next && t->next_via_ag && t->next_tx_left > 0 && send_grant_now(t, f, &m)) {
                add_slot(&b, LCB_SLOT_AG, k, lc_term_beacon_len_us(), lc_term_ag_len_us(), LC_BAND_915,
                         lc_hop_channel(c->cell_seed, LC_BAND_915, 0, f, AG_SLOT_INDEX), LC_TIER_EDGE,
                         LC_DIR_TX, &m);
                c->grants_sent++;
                break;
            }
        }
    }

    for (uint8_t k = 0; k < LCB_CELL_MAX_TERMS; k++) {
        lcb_cell_term_t *t = &c->terms[k];
        if (!t->used || !t->have_cur || (int32_t)(f - t->cur.effective_frame) < 0) {
            continue;
        }
        const lc_grant_leg_t *dl = &t->cur.dl, *ul = &t->cur.ul;
        if (dl->len != 0 && dl->band == band) {
            if (t->have_next && !t->next_via_ag && t->next_tx_left > 0) {
                send_grant_now(t, f, &m);
                c->grants_sent++;
            } else {
                memset(&m, 0, sizeof(m));
                m.type = LC_AIR_DATA;
                m.u.data = (lc_data_t){ t->tmid, t->dl_seq++, 0, t->loop_len, t->loop };
                if (t->loop_len > 0) {
                    t->loops++;
                }
                t->loop_len = 0;
            }
            add_slot(&b, LCB_SLOT_DL, k, dl->offset * LC_AIR_TIME_UNIT_US, dl->len * LC_AIR_TIME_UNIT_US,
                     (lc_band_t)dl->band, lc_grant_leg_channel(c->cell_seed, dl, f), (lc_tier_t)dl->tier,
                     LC_DIR_TX, &m);
        }
        if (ul->len != 0 && ul->band == band) {
            /* Like plan 4: the RX window is exactly the leg. */
            add_slot(&b, LCB_SLOT_UL, k, ul->offset * LC_AIR_TIME_UNIT_US, ul->len * LC_AIR_TIME_UNIT_US,
                     (lc_band_t)ul->band, lc_grant_leg_channel(c->cell_seed, ul, f), (lc_tier_t)ul->tier,
                     LC_DIR_RX, NULL);
        }
    }

    if (band == LC_BAND_915) {
        add_slot(&b, LCB_SLOT_RACH, 0xFF, rach_off_us(), rach_len_us(), LC_BAND_915,
                 lc_hop_channel(c->cell_seed, LC_BAND_915, 0, f, RACH_SLOT_INDEX), LC_TIER_EDGE, LC_DIR_RX,
                 NULL);
    }
    return out->u.schedule.slot_count > 0 ? 0 : -1;
}

static int find_term(lcb_cell_t *c, uint32_t tmid)
{
    for (uint8_t k = 0; k < LCB_CELL_MAX_TERMS; k++) {
        if (c->terms[k].used && c->terms[k].tmid == tmid) {
            return k;
        }
    }
    for (uint8_t k = 0; k < LCB_CELL_MAX_TERMS; k++) {
        if (!c->terms[k].used) {
            memset(&c->terms[k], 0, sizeof(c->terms[k]));
            c->terms[k].used = 1;
            c->terms[k].tmid = tmid;
            return k;
        }
    }
    return -1;
}

void lcb_cell_on_rx(lcb_cell_t *c, lc_band_t band, const lc_rx_report_t *r)
{
    if ((unsigned)band >= LC_BAND_COUNT || !r->crc_ok) {
        return;
    }
    const lcb_cell_kinds_t *kd = &c->kinds[band][r->frame_number % LCB_CELL_KIND_FRAMES];
    lc_air_msg_t m;
    if (kd->frame != r->frame_number || r->slot_index >= kd->count ||
        lc_air_decode(r->payload, r->payload_len, &m) != 0) {
        return;
    }
    uint8_t kind = kd->kind[r->slot_index];
    if (kind == LCB_SLOT_RACH && m.type == LC_AIR_RACH) {
        c->rach_rx++;
        if (m.u.rach.kind == LC_RACH_UPPER) {
            c->uppers++;
            return;
        }
        int k = find_term(c, m.u.rach.tmid);
        if (k < 0) {
            return; /* full: the terminal backs off and retries */
        }
        lcb_cell_term_t *t = &c->terms[k];
        int legs = 1;
        if (c->fallback_915 && t->have_cur && (t->cur.dl.band == LC_BAND_2G4 || t->cur.ul.band == LC_BAND_2G4)) {
            c->dl_band = LC_BAND_915;
            c->ul_band = LC_BAND_915;
        }
        if (m.u.rach.kind == LC_RACH_ATTACH) {
            c->attaches++;
            legs = !c->attach_idle;
        } else {
            c->page_replies++;
            if (c->page_tmid == t->tmid) {
                c->page_tmid = 0;
            }
        }
        t->have_cur = 0; /* a (re)attaching terminal has no working grant */
        queue_grant(c, (uint8_t)k, legs, 1);
    } else if (kind == LCB_SLOT_UL && m.type == LC_AIR_DATA) {
        lcb_cell_term_t *t = &c->terms[kd->term[r->slot_index]];
        if (t->used && t->tmid == m.u.data.tmid) {
            t->ul_rx++;
            if (m.u.data.payload_len > 0) {
                t->loop_len = m.u.data.payload_len > LCB_CELL_PAYLOAD ? LCB_CELL_PAYLOAD : m.u.data.payload_len;
                memcpy(t->loop, m.u.data.payload, t->loop_len);
            }
        }
    }
}
