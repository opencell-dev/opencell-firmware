#include "lc_term_scan.h"

#include <string.h>

#include "lc_phy.h"

void lc_term_scan_init(lc_term_scan_t *s)
{
    memset(s, 0, sizeof(*s));
    s->mode = LC_PHY_MODE_PART15;
    s->fallback_after = LC_SCAN_DEF_AFTER;
    s->fallback_chunk = LC_SCAN_DEF_CHUNK;
    s->sweep_next = LC_SCAN_N_DEFAULT;
}

static int entry_active(const lc_term_scan_t *s, const lc_scan_ent_t *e)
{
    uint8_t pattern = (e->flags & LC_SCAN_F_FIXED) ? LC_SYNC_FIXED : LC_SYNC_CYCLE;
    return lc_sync_anchor_ok(s->mode, e->freq_hz, pattern);
}

/* Appends e with its source unless an active entry with the same frequency
 * and FIXED flag is already there. */
static void add(const lc_term_scan_t *s, lc_scan_ent_t *out, uint8_t *n, const lc_scan_ent_t *e, uint8_t src)
{
    uint8_t fixed = e->flags & LC_SCAN_F_FIXED;
    int active = entry_active(s, e);
    if (active) {
        for (uint8_t i = 0; i < *n; i++) {
            if ((out[i].flags & LC_SCAN_F_ACTIVE) && out[i].freq_hz == e->freq_hz &&
                (out[i].flags & LC_SCAN_F_FIXED) == fixed) {
                return;
            }
        }
    }
    out[*n].freq_hz = e->freq_hz;
    out[*n].flags = (uint8_t)(fixed | (src << LC_SCAN_F_SRC_SHIFT) | (active ? LC_SCAN_F_ACTIVE : 0u));
    (*n)++;
}

uint8_t lc_term_scan_list(const lc_term_scan_t *s, lc_scan_ent_t out[LC_SCAN_MAX])
{
    uint8_t n = 0;
    if (s->last.freq_hz != 0) {
        add(s, out, &n, &s->last, LC_SCAN_SRC_LAST);
    }
    for (uint8_t i = 0; i < s->n_user && i < LC_SCAN_MAX_USER; i++) {
        add(s, out, &n, &s->user[i], LC_SCAN_SRC_USER);
    }
    for (uint8_t i = 0; i < s->n_net && i < LC_SCAN_MAX_NET; i++) {
        add(s, out, &n, &s->net[i], LC_SCAN_SRC_NET);
    }
    for (uint8_t i = 0; i < s->n_learn && i < LC_SCAN_MAX_LEARN; i++) {
        add(s, out, &n, &s->learn[i], LC_SCAN_SRC_LEARN);
    }
    for (uint8_t ch = 0; ch < LC_SCAN_N_DEFAULT; ch++) {
        const lc_scan_ent_t d = { lc_channel_freq_hz(LC_BAND_915, ch), 0 };
        add(s, out, &n, &d, LC_SCAN_SRC_DEFAULT);
    }
    return n;
}

void lc_term_scan_restart(lc_term_scan_t *s)
{
    s->pos = 0;
    s->passes = 0;
    s->cur_pos = 0;
    s->cur_len = 0;
    s->cur_src = LC_SCAN_SRC_NONE;
    s->cur_freq = 0;
}

/* A grid channel a full (CYCLE) dwell on an active entry already covers. */
static int covered(const lc_scan_ent_t *l, uint8_t n, uint32_t freq_hz)
{
    for (uint8_t i = 0; i < n; i++) {
        if ((l[i].flags & LC_SCAN_F_ACTIVE) && !(l[i].flags & LC_SCAN_F_FIXED) && l[i].freq_hz == freq_hz) {
            return 1;
        }
    }
    return 0;
}

/* Channels this round sweeps: 0 until fallback_after rounds are done. */
static uint8_t sweep_len(const lc_term_scan_t *s, const lc_scan_ent_t *l, uint8_t n)
{
    if (s->fallback_after >= LC_SCAN_NEVER || s->passes < s->fallback_after) {
        return 0;
    }
    uint8_t free_ch = 0, grid = lc_num_channels(LC_BAND_915);
    for (uint8_t ch = 0; ch < grid; ch++) {
        free_ch += !covered(l, n, lc_channel_freq_hz(LC_BAND_915, ch));
    }
    uint8_t chunk = s->fallback_chunk == 0 ? 1u : s->fallback_chunk;
    return free_ch < chunk ? free_ch : chunk;
}

void lc_term_scan_next(lc_term_scan_t *s, uint32_t *freq_hz, uint32_t *dwell_us)
{
    lc_scan_ent_t l[LC_SCAN_MAX];
    uint8_t n = lc_term_scan_list(s, l);
    uint8_t act[LC_SCAN_MAX], na = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (l[i].flags & LC_SCAN_F_ACTIVE) {
            act[na++] = i;
        }
    }
    uint8_t len = (uint8_t)(na + sweep_len(s, l, n));
    if (s->pos >= len) {
        s->pos = 0; /* the list shrank under the walk */
    }
    s->cur_len = len;
    s->cur_pos = (uint8_t)(s->pos + 1u);
    if (s->pos < na) {
        const lc_scan_ent_t *e = &l[act[s->pos]];
        s->cur_freq = e->freq_hz;
        s->cur_src = (uint8_t)((e->flags & LC_SCAN_F_SRC_MASK) >> LC_SCAN_F_SRC_SHIFT);
        *dwell_us = (e->flags & LC_SCAN_F_FIXED) ? LC_SCAN_FIXED_DWELL_US : LC_SCAN_DWELL_US;
    } else {
        uint8_t grid = lc_num_channels(LC_BAND_915), ch = s->sweep_next % grid;
        while (covered(l, n, lc_channel_freq_hz(LC_BAND_915, ch))) {
            ch = (uint8_t)((ch + 1u) % grid); /* sweep_len > 0: some channel is free */
        }
        s->sweep_ch = ch;
        s->cur_freq = lc_channel_freq_hz(LC_BAND_915, ch);
        s->cur_src = LC_SCAN_SRC_SWEEP;
        *dwell_us = LC_SCAN_DWELL_US;
    }
    *freq_hz = s->cur_freq;
}

int lc_term_scan_advance(lc_term_scan_t *s)
{
    if (s->cur_src == LC_SCAN_SRC_SWEEP) {
        s->sweep_next = (uint8_t)((s->sweep_ch + 1u) % lc_num_channels(LC_BAND_915));
    }
    s->pos++;
    if (s->cur_len == 0 || s->pos >= s->cur_len) {
        s->pos = 0;
        if (s->passes < 0xFFu) {
            s->passes++;
        }
        return 1;
    }
    return 0;
}
