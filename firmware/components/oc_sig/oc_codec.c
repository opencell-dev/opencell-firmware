#include "oc_codec.h"

/* Best first (OpenVoxNB spec V2): the only copy of the ranking here. */
static const uint8_t k_rank[] = { OC_CODEC_OVX_1200,    OC_CODEC_OVX_1000, OC_CODEC_OVX_800,
                                  OC_CODEC_LPCNET_1733, OC_CODEC_C2_1200,  OC_CODEC_OVX_600 };

int oc_codec_in(uint8_t caps, uint8_t id)
{
    return id >= 1u && id <= OC_CODEC_ID_MAX && (caps & OC_CODEC_BIT(id)) != 0;
}

uint8_t oc_codec_best(uint8_t caps)
{
    for (unsigned i = 0; i < sizeof(k_rank); i++) {
        if (oc_codec_in(caps, k_rank[i])) return k_rank[i];
    }
    return OC_CODEC_NONE;
}

uint8_t oc_codec_block_bytes(uint8_t id)
{
    switch (id) {
    case OC_CODEC_C2_1200:  return 18u; /* three 6-byte 40 ms frames */
    case OC_CODEC_OVX_600:  return 9u;
    case OC_CODEC_OVX_800:  return 12u;
    case OC_CODEC_OVX_1000: return 15u;
    case OC_CODEC_OVX_1200: return 18u;
    default:                return 0u;
    }
}

const char *oc_codec_name(uint8_t id)
{
    switch (id) {
    case OC_CODEC_NONE:        return "none";
    case OC_CODEC_C2_1200:     return "Codec2 1200";
    case OC_CODEC_LPCNET_1733: return "LPCNet 1733";
    case OC_CODEC_OVX_600:     return "OVX-600";
    case OC_CODEC_OVX_800:     return "OVX-800";
    case OC_CODEC_OVX_1000:    return "OVX-1000";
    case OC_CODEC_OVX_1200:    return "OVX-1200";
    default:                   return "?";
    }
}
