/* oc_codec: the voice codec ids of CALL_SETUP/SETUP_IND's codec_caps and
 * CONNECT's codec (codec negotiation spec N1-N2, as amended by the
 * OpenVoxNB spec V1-V2), and the one fixed ranking that picks a call's
 * codec. codec_caps is a bitmap, bit id - 1. The app has the same table
 * (CodecId); a new codec gets an id and a place in the ranking in both. */
#ifndef OC_CODEC_H
#define OC_CODEC_H

#include <stdint.h>

#define OC_CODEC_NONE         0u /* no codec: ANSWER_CODEC's "no shared codec" */
#define OC_CODEC_C2_1200      1u /* Codec2 1200 */
#define OC_CODEC_LPCNET_1733  2u /* reserved: no code yet */
#define OC_CODEC_OVX_600      3u /* OpenVoxNB, the four modes that fit the 18-byte app data frame */
#define OC_CODEC_OVX_800      4u
#define OC_CODEC_OVX_1000     5u
#define OC_CODEC_OVX_1200     6u
#define OC_CODEC_ID_MAX       8u /* 7 and 8 are reserved */
#define OC_CODEC_CAPS_DEFAULT 0x01u /* what an old terminal, app or link means: Codec2 1200 only */
#define OC_CODEC_BIT(id)      ((uint8_t)(1u << ((id) - 1u)))

/* 1 if id (1..OC_CODEC_ID_MAX) is in caps, else 0 (0 never is). */
int         oc_codec_in(uint8_t caps, uint8_t id);
/* The best codec in caps by the ranking OVX-1200 > OVX-1000 > OVX-800 >
 * LPCNet 1733 > Codec2 1200 > OVX-600 (reserved bits ignored), or
 * OC_CODEC_NONE. */
uint8_t     oc_codec_best(uint8_t caps);
/* Bytes per 120 ms block (one app data frame): 18 for Codec2 1200, 9 to
 * 18 for OVX-600...1200; 0 for none, LPCNet (no frame yet) and reserved. */
uint8_t     oc_codec_block_bytes(uint8_t id);
/* "Codec2 1200", "LPCNet 1733", "OVX-600" ... "OVX-1200"; "none" for 0;
 * "?" for a reserved or unknown id. */
const char *oc_codec_name(uint8_t id);

#endif
