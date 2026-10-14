/* OpenVoxNB modes (docs/openvox/PLAN.md §1.5): the nine bitrates, their
 * 120 ms block sizes, the OpenCell codec ids they map to, capability
 * bitmaps and the negotiation rule.
 *
 * A block is six 20 ms frames: bit/s x 0.12 bits, always whole bytes, and
 * every mode's block has a different size, so a transport that keeps
 * payload lengths (oc_air DATA, the GATT UP/DOWN frames, MEDIA) implies the
 * mode. The OpenCell codec byte (CONNECT.codec, voice spec §4) names a
 * mode outright: ids 3-6 are the modes whose block fits today's 18-byte app
 * data frame (OC_SIG_APP_MAX); the others get ids with voice spec v2. */
#ifndef OC_VOX_MODES_H
#define OC_VOX_MODES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OC_VOX_600 = 0, OC_VOX_800 = 1, OC_VOX_1000 = 2, OC_VOX_1200 = 3, OC_VOX_1400 = 4,
    OC_VOX_1600 = 5, OC_VOX_1800 = 6, OC_VOX_2400 = 7, OC_VOX_3200 = 8,
    OC_VOX_MODE_COUNT = 9
} oc_vox_mode_t;
#define OC_VOX_MODE_NONE (-1)

#define OC_VOX_BLOCK_MAX_BYTES 48u /* OVX-3200 */
#define OC_VOX_BLOCK_MAX_BITS  384u

/* Codec ids in CALL_SETUP/SETUP_IND codec_caps (bit id-1) and CONNECT codec:
 * 1 is Codec2 1200, 2 is reserved (the app's CodecId), 3-6 are OpenVoxNB. */
#define OC_VOX_CODEC_ID_FIRST 3u
#define OC_VOX_CODEC_ID_LAST  6u

/* 600 ... 3200; 0 for a value that is not a mode. */
uint16_t oc_vox_mode_bps(oc_vox_mode_t m);
/* 9 ... 48; 0 for a value that is not a mode. */
uint8_t  oc_vox_block_bytes(oc_vox_mode_t m);
/* 72 ... 384; 0 for a value that is not a mode. */
uint16_t oc_vox_block_bits(oc_vox_mode_t m);
/* The mode whose block is n bytes, or OC_VOX_MODE_NONE. */
int      oc_vox_mode_of_block_bytes(uint8_t n);
/* "OVX-1200" etc.; "?" for a value that is not a mode. */
const char *oc_vox_mode_name(oc_vox_mode_t m);

/* The OpenCell codec id of a mode: 3-6 for OVX-600...OVX-1200, 0 for the
 * modes that have none yet (and for a value that is not a mode). */
uint8_t  oc_vox_codec_id(oc_vox_mode_t m);
/* The mode of an OpenCell codec id, or OC_VOX_MODE_NONE (1, 2, 0, 7...). */
int      oc_vox_mode_of_codec_id(uint8_t id);

/* A set of modes: bit m for mode m. */
typedef uint16_t oc_vox_caps_t;
#define OC_VOX_CAPS_ALL ((oc_vox_caps_t)((1u << OC_VOX_MODE_COUNT) - 1u))
/* The codec_caps bits (bit id-1) of the modes in caps that have an id: a
 * caller ORs it into its own codec_caps next to Codec2's 0x01. */
uint8_t       oc_vox_caps_to_sig(oc_vox_caps_t caps);
/* The modes a codec_caps byte advertises (its OpenVoxNB bits only). */
oc_vox_caps_t oc_vox_caps_of_sig(uint8_t codec_caps);

/* The mode both sides have whose block fits max_block_bytes: the highest
 * such mode, or with prefer_robust the lowest. OC_VOX_MODE_NONE if none. */
int oc_vox_negotiate(oc_vox_caps_t local, oc_vox_caps_t remote, uint8_t max_block_bytes, int prefer_robust);

#ifdef __cplusplus
}
#endif

#endif
