#include "oc_vox_modes.h"

typedef struct {
    uint16_t    bps;
    uint8_t     bytes;
    uint8_t     codec_id; /* 0: none yet */
    const char *name;
} mode_row_t;

static const mode_row_t MODES[OC_VOX_MODE_COUNT] = {
    { 600, 9, 3, "OVX-600" },   { 800, 12, 4, "OVX-800" },   { 1000, 15, 5, "OVX-1000" },
    { 1200, 18, 6, "OVX-1200" }, { 1400, 21, 0, "OVX-1400" }, { 1600, 24, 0, "OVX-1600" },
    { 1800, 27, 0, "OVX-1800" }, { 2400, 36, 0, "OVX-2400" }, { 3200, 48, 0, "OVX-3200" },
};

static int valid(oc_vox_mode_t m) { return (int)m >= 0 && (int)m < (int)OC_VOX_MODE_COUNT; }

uint16_t oc_vox_mode_bps(oc_vox_mode_t m) { return valid(m) ? MODES[m].bps : 0u; }
uint8_t  oc_vox_block_bytes(oc_vox_mode_t m) { return valid(m) ? MODES[m].bytes : 0u; }
uint16_t oc_vox_block_bits(oc_vox_mode_t m) { return valid(m) ? (uint16_t)(MODES[m].bytes * 8u) : 0u; }
const char *oc_vox_mode_name(oc_vox_mode_t m) { return valid(m) ? MODES[m].name : "?"; }

int oc_vox_mode_of_block_bytes(uint8_t n)
{
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++)
        if (MODES[m].bytes == n) return m;
    return OC_VOX_MODE_NONE;
}

uint8_t oc_vox_codec_id(oc_vox_mode_t m) { return valid(m) ? MODES[m].codec_id : 0u; }

int oc_vox_mode_of_codec_id(uint8_t id)
{
    if (id == 0) return OC_VOX_MODE_NONE;
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++)
        if (MODES[m].codec_id == id) return m;
    return OC_VOX_MODE_NONE;
}

uint8_t oc_vox_caps_to_sig(oc_vox_caps_t caps)
{
    uint8_t out = 0;
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++)
        if ((caps & (1u << m)) && MODES[m].codec_id != 0) out |= (uint8_t)(1u << (MODES[m].codec_id - 1u));
    return out;
}

oc_vox_caps_t oc_vox_caps_of_sig(uint8_t codec_caps)
{
    oc_vox_caps_t out = 0;
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++)
        if (MODES[m].codec_id != 0 && (codec_caps & (1u << (MODES[m].codec_id - 1u)))) out |= (oc_vox_caps_t)(1u << m);
    return out;
}

int oc_vox_negotiate(oc_vox_caps_t local, oc_vox_caps_t remote, uint8_t max_block_bytes, int prefer_robust)
{
    oc_vox_caps_t both = local & remote & OC_VOX_CAPS_ALL;
    int pick = OC_VOX_MODE_NONE;
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++) {
        if (!(both & (1u << m)) || MODES[m].bytes > max_block_bytes) continue;
        pick = m;
        if (prefer_robust) break;
    }
    return pick;
}
