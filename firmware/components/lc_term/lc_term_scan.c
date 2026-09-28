#include "lc_term_scan.h"

#include <string.h>

#include "lc_crc.h"
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

/* ------------------------------------------------------------- changes */

static int same(const lc_scan_ent_t *a, const lc_scan_ent_t *b)
{
    return a->freq_hz == b->freq_hz && a->flags == b->flags;
}

void lc_term_scan_set_mode(lc_term_scan_t *s, uint8_t mode)
{
    if ((mode == LC_PHY_MODE_PART15 || mode == LC_PHY_MODE_PART97) && mode != s->mode) {
        s->mode = mode;
        s->dirty = 1;
    }
}

/* Drops learned entries equal to e. */
static void learn_remove(lc_term_scan_t *s, const lc_scan_ent_t *e)
{
    uint8_t k = 0;
    for (uint8_t i = 0; i < s->n_learn; i++) {
        if (!same(&s->learn[i], e)) {
            s->learn[k++] = s->learn[i];
        }
    }
    s->n_learn = k;
}

void lc_term_scan_serving(lc_term_scan_t *s, uint32_t anchor_hz, int fixed)
{
    const lc_scan_ent_t e = { anchor_hz, fixed ? LC_SCAN_F_FIXED : 0u };
    if (anchor_hz == 0 || same(&s->last, &e)) {
        return;
    }
    learn_remove(s, &e);
    if (s->last.freq_hz != 0) {
        learn_remove(s, &s->last);
        uint8_t n = s->n_learn < LC_SCAN_MAX_LEARN ? s->n_learn : (uint8_t)(LC_SCAN_MAX_LEARN - 1u);
        memmove(&s->learn[1], &s->learn[0], n * sizeof(s->learn[0]));
        s->learn[0] = s->last;
        s->n_learn = (uint8_t)(n + 1u);
    }
    s->last = e;
    s->dirty = 1;
}

void lc_term_scan_set_net(lc_term_scan_t *s, uint8_t ver, uint8_t count, const lc_scan_ent_t *e)
{
    if (count > LC_SCAN_MAX_NET) {
        count = LC_SCAN_MAX_NET;
    }
    /* The network pushes its list after every registration: the same version
     * with the same entries is no change (no NVS write). */
    int same = ver == s->net_ver && count == s->n_net;
    for (uint8_t i = 0; same && i < count; i++) {
        same = e[i].freq_hz == s->net[i].freq_hz && (e[i].flags & LC_SCAN_F_FIXED) == s->net[i].flags;
    }
    if (same) {
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        s->net[i].freq_hz = e[i].freq_hz;
        s->net[i].flags = e[i].flags & LC_SCAN_F_FIXED;
    }
    s->n_net = count;
    s->net_ver = ver;
    s->dirty = 1;
}

int lc_term_scan_set_user(lc_term_scan_t *s, uint8_t count, const lc_scan_ent_t *e)
{
    if (count > LC_SCAN_MAX_USER) {
        return -1;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (lc_channel_of_freq(LC_BAND_915, e[i].freq_hz) == LC_INVALID_CHANNEL) {
            return -1;
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        s->user[i].freq_hz = e[i].freq_hz;
        s->user[i].flags = e[i].flags & LC_SCAN_F_FIXED;
    }
    s->n_user = count;
    s->dirty = 1;
    return 0;
}

int lc_term_scan_set_fallback(lc_term_scan_t *s, uint8_t after, uint8_t chunk)
{
    if (after > LC_SCAN_NEVER || chunk == 0 || chunk > lc_num_channels(LC_BAND_915)) {
        return -1;
    }
    s->fallback_after = after;
    s->fallback_chunk = chunk;
    s->dirty = 1;
    return 0;
}

void lc_term_scan_forget_learned(lc_term_scan_t *s)
{
    s->n_learn = 0;
    s->dirty = 1;
}

void lc_term_scan_deactivate(lc_term_scan_t *s)
{
    s->n_net = 0;
    s->net_ver = 0;
    s->n_learn = 0;
    s->dirty = 1;
}

/* ---------------------------------------------------------------- blob */

static uint8_t *put_ent(uint8_t *p, const lc_scan_ent_t *e)
{
    p[0] = (uint8_t)e->freq_hz;
    p[1] = (uint8_t)(e->freq_hz >> 8);
    p[2] = (uint8_t)(e->freq_hz >> 16);
    p[3] = (uint8_t)(e->freq_hz >> 24);
    p[4] = e->flags;
    return p + 5;
}

static const uint8_t *get_ent(const uint8_t *p, lc_scan_ent_t *e)
{
    e->freq_hz = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    e->flags = p[4] & LC_SCAN_F_FIXED;
    return p + 5;
}

size_t lc_term_scan_pack(const lc_term_scan_t *s, uint8_t out[LC_SCAN_BLOB_MAX])
{
    uint8_t nu = s->n_user < LC_SCAN_MAX_USER ? s->n_user : LC_SCAN_MAX_USER;
    uint8_t nn = s->n_net < LC_SCAN_MAX_NET ? s->n_net : LC_SCAN_MAX_NET;
    uint8_t nl = s->n_learn < LC_SCAN_MAX_LEARN ? s->n_learn : LC_SCAN_MAX_LEARN;
    out[0] = LC_SCAN_BLOB_VER;
    out[1] = s->mode;
    out[2] = s->fallback_after;
    out[3] = s->fallback_chunk;
    out[4] = s->net_ver;
    out[5] = nu;
    out[6] = nn;
    out[7] = nl;
    uint8_t *p = put_ent(out + 8, &s->last);
    for (uint8_t i = 0; i < nu; i++) p = put_ent(p, &s->user[i]);
    for (uint8_t i = 0; i < nn; i++) p = put_ent(p, &s->net[i]);
    for (uint8_t i = 0; i < nl; i++) p = put_ent(p, &s->learn[i]);
    size_t n = (size_t)(p - out);
    uint16_t crc = lc_crc16(out, n);
    out[n] = (uint8_t)crc;
    out[n + 1] = (uint8_t)(crc >> 8);
    return n + 2;
}

int lc_term_scan_unpack(lc_term_scan_t *s, const uint8_t *in, size_t len)
{
    if (len < 15 || in[0] != LC_SCAN_BLOB_VER || in[5] > LC_SCAN_MAX_USER || in[6] > LC_SCAN_MAX_NET ||
        in[7] > LC_SCAN_MAX_LEARN || len != 15u + 5u * (size_t)(in[5] + in[6] + in[7]) ||
        lc_crc16(in, len - 2) != (uint16_t)(in[len - 2] | (in[len - 1] << 8)) ||
        (in[1] != LC_PHY_MODE_PART15 && in[1] != LC_PHY_MODE_PART97) || in[2] > LC_SCAN_NEVER || in[3] == 0 ||
        in[3] > lc_num_channels(LC_BAND_915)) {
        return -1;
    }
    s->mode = in[1];
    s->fallback_after = in[2];
    s->fallback_chunk = in[3];
    s->net_ver = in[4];
    s->n_user = in[5];
    s->n_net = in[6];
    s->n_learn = in[7];
    const uint8_t *p = get_ent(in + 8, &s->last);
    for (uint8_t i = 0; i < s->n_user; i++) p = get_ent(p, &s->user[i]);
    for (uint8_t i = 0; i < s->n_net; i++) p = get_ent(p, &s->net[i]);
    for (uint8_t i = 0; i < s->n_learn; i++) p = get_ent(p, &s->learn[i]);
    s->dirty = 0;
    return 0;
}
