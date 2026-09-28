#include "lc_phy.h"

#include <stddef.h>

#define LC_915_NUM_CHANNELS 52u
#define LC_915_BASE_HZ      902250000u
#define LC_915_SPACING_HZ   500000u
#define LC_2G4_NUM_CHANNELS 40u
#define LC_2G4_BASE_HZ      2402000000u
#define LC_2G4_SPACING_HZ   2000000u
#define LC_MAX_CHANNELS     52u

/* Every radio's slots map to permutation indices congruent to its radio_index
 * mod LC_MAX_RADIOS_PER_BAND; that only partitions the channels if the count
 * divides evenly. */
_Static_assert(LC_915_NUM_CHANNELS % LC_MAX_RADIOS_PER_BAND == 0, "915 channel count");
_Static_assert(LC_2G4_NUM_CHANNELS % LC_MAX_RADIOS_PER_BAND == 0, "2.4 channel count");

uint8_t lc_num_channels(lc_band_t band)
{
    switch (band) {
    case LC_BAND_915: return LC_915_NUM_CHANNELS;
    case LC_BAND_2G4: return LC_2G4_NUM_CHANNELS;
    default:          return 0;
    }
}

uint32_t lc_channel_freq_hz(lc_band_t band, uint8_t channel)
{
    if (channel >= lc_num_channels(band)) {
        return 0;
    }
    if (band == LC_BAND_915) {
        return LC_915_BASE_HZ + (uint32_t)channel * LC_915_SPACING_HZ;
    }
    return LC_2G4_BASE_HZ + (uint32_t)channel * LC_2G4_SPACING_HZ;
}

/* "lowbias32" integer hash (Chris Wellons). */
static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Fisher–Yates shuffle of 0..n-1, seeded per (cell, band, frame). */
static void frame_permutation(uint32_t cell_seed, lc_band_t band, uint32_t frame_number,
                              uint8_t *perm, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        perm[i] = i;
    }
    uint32_t state = mix32(cell_seed ^ mix32(frame_number ^ ((uint32_t)band << 31)));
    if (state == 0) {
        state = 1;
    }
    for (uint8_t i = n - 1; i > 0; i--) {
        uint32_t j = xorshift32(&state) % (uint32_t)(i + 1);
        uint8_t tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }
}

uint8_t lc_hop_channel(uint32_t cell_seed, lc_band_t band, uint8_t radio_index,
                       uint32_t frame_number, uint8_t slot_index)
{
    uint8_t n = lc_num_channels(band);
    if (n == 0 || radio_index >= LC_MAX_RADIOS_PER_BAND) {
        return LC_INVALID_CHANNEL;
    }
    uint8_t perm[LC_MAX_CHANNELS];
    frame_permutation(cell_seed, band, frame_number, perm, n);
    uint32_t idx = ((uint32_t)slot_index * LC_MAX_RADIOS_PER_BAND + radio_index) % n;
    return perm[idx];
}

uint8_t lc_sync_channel_at(uint8_t anchor, lc_band_t band, uint32_t frame_number)
{
    uint8_t n = lc_num_channels(band);
    if (n == 0 || anchor >= n) {
        return LC_INVALID_CHANNEL;
    }
    uint32_t i = frame_number % LC_NUM_SYNC_CHANNELS;
    return (uint8_t)((anchor + (i * n) / LC_NUM_SYNC_CHANNELS) % n);
}

uint8_t lc_sync_channel(uint32_t cell_seed, lc_band_t band, uint32_t frame_number)
{
    uint8_t n = lc_num_channels(band);
    if (n == 0) {
        return LC_INVALID_CHANNEL;
    }
    return lc_sync_channel_at((uint8_t)(cell_seed % (n / LC_NUM_SYNC_CHANNELS)), band, frame_number);
}

uint8_t lc_channel_of_freq(lc_band_t band, uint32_t freq_hz)
{
    uint8_t n = lc_num_channels(band);
    for (uint8_t ch = 0; ch < n; ch++) {
        if (lc_channel_freq_hz(band, ch) == freq_hz) {
            return ch;
        }
    }
    return LC_INVALID_CHANNEL;
}

/* Where each mode may put an anchor, and with which patterns (spec §3.3). */
typedef struct {
    uint8_t  mode;
    uint32_t lo_hz, hi_hz, step_hz;
    uint8_t  patterns; /* bit per LC_SYNC_* */
} anchor_rule_t;

static const anchor_rule_t k_anchor_rules[] = {
    { LC_PHY_MODE_PART15, LC_915_BASE_HZ, LC_915_BASE_HZ + 51u * LC_915_SPACING_HZ, LC_915_SPACING_HZ,
      1u << LC_SYNC_CYCLE },
    { LC_PHY_MODE_PART97, LC_915_BASE_HZ, LC_915_BASE_HZ + 51u * LC_915_SPACING_HZ, LC_915_SPACING_HZ,
      (1u << LC_SYNC_CYCLE) | (1u << LC_SYNC_FIXED) },
};

int lc_sync_anchor_ok(uint8_t mode, uint32_t freq_hz, uint8_t pattern)
{
    if (pattern > LC_SYNC_FIXED) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(k_anchor_rules) / sizeof(k_anchor_rules[0]); i++) {
        const anchor_rule_t *r = &k_anchor_rules[i];
        if (r->mode == mode && freq_hz >= r->lo_hz && freq_hz <= r->hi_hz && (freq_hz - r->lo_hz) % r->step_hz == 0 &&
            (r->patterns & (1u << pattern)) != 0) {
            return 1;
        }
    }
    return 0;
}
