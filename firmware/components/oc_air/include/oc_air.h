/* oc_air — over-the-air message formats between base station and terminals.
 *
 * Every air frame starts with one byte: (OC_AIR_VERSION << 4) | type.
 * Multi-byte integers are little-endian. Times are in 10 µs units from the
 * start of the 120 ms frame (max OC_FRAME_US / 10 = 12000).
 *
 * The beacon is deliberately small (<= OC_BEACON_MAX_BYTES) because it goes
 * out every frame at the edge tier (LoRa SF7/500 kHz, ~15 ms at max size).
 * Beacon layout (v2): hdr | cell_seed 4 | frame 4 | band | flags | rach_offset 2 |
 * rach_len 2 | rach_slot_index | page_count | sync | page_tmid 4 x page_count,
 * sync = anchor (bits 0-5) | cfg_ver << 6. 18 + 4 x pages bytes; the sync byte
 * costs no airtime at SF7/500 kHz (it fits the last symbol block).
 * Slot assignments are NOT repeated in every beacon: a GRANT is sent once in
 * the terminal's DL slot (or the paging flow) and stays in force from
 * effective_frame until replaced or revoked. */
#ifndef OC_AIR_H
#define OC_AIR_H

#include <stddef.h>
#include <stdint.h>

#include "oc_phy.h"

#define OC_AIR_VERSION      2u   /* 2: the beacon's sync byte (channel-list spec §4.2) */
#define OC_AIR_MAX_FRAME    255u
#define OC_BCN_MAX_PAGES    2u
#define OC_BEACON_MAX_BYTES (18u + 4u * OC_BCN_MAX_PAGES)
#define OC_BCN_MAX_ANCHOR   51u  /* the 915 grid's last channel */
#define OC_BCN_MAX_CFG_VER  3u
#define OC_RACH_MAX_PAYLOAD 32u
#define OC_DATA_MAX_PAYLOAD 240u
#define OC_AIR_TIME_UNIT_US 10u
#define OC_AIR_FRAME_UNITS  (OC_FRAME_US / OC_AIR_TIME_UNIT_US)

typedef enum {
    OC_AIR_BEACON = 1,
    OC_AIR_GRANT  = 2,
    OC_AIR_RACH   = 3,
    OC_AIR_DATA   = 4
} oc_air_type_t;

#define OC_BCN_FLAG_ACCEPTING_ATTACH 0x01u
#define OC_BCN_FLAG_BACKHAUL_ACTIVE  0x02u
#define OC_BCN_FLAG_PART97           0x04u /* the network runs Part 97 mode (oc_sig spec §4.3) */
#define OC_BCN_FLAG_FIXED_SYNC       0x08u /* every beacon on the anchor (Part 97 only, channel-list spec §3.2) */

typedef struct {
    uint32_t cell_seed;
    uint32_t frame_number;
    uint8_t  band;          /* band of the radio sending this beacon */
    uint8_t  flags;         /* OC_BCN_FLAG_* */
    uint16_t rach_offset;   /* RACH window, 10 µs units */
    uint16_t rach_len;
    uint8_t  rach_slot_index;
    uint8_t  page_count;    /* 0..OC_BCN_MAX_PAGES */
    uint8_t  anchor;        /* the cell's 915 anchor channel, 0..OC_BCN_MAX_ANCHOR */
    uint8_t  cfg_ver;       /* the cell's channel-list version mod 4 */
    uint32_t page_tmid[OC_BCN_MAX_PAGES];
} oc_beacon_t;

/* One direction of a grant. len == 0 means "no slot in this direction". */
typedef struct {
    uint8_t  band;
    uint8_t  tier;
    uint8_t  radio_index; /* base-station radio on that band, for oc_hop_channel */
    uint8_t  slot_index;  /* hop slot index for oc_hop_channel */
    uint16_t offset;      /* 10 µs units from frame start */
    uint16_t len;         /* 10 µs units */
} oc_grant_leg_t;

/* A grant with both legs len == 0 revokes the terminal's slots. */
typedef struct {
    uint32_t       tmid;
    uint32_t       effective_frame;
    oc_grant_leg_t dl;
    oc_grant_leg_t ul;
} oc_grant_t;

typedef enum {
    OC_RACH_ATTACH     = 1,
    OC_RACH_PAGE_REPLY = 2,
    OC_RACH_UPPER      = 3  /* opaque upper-layer request (call setup etc.) */
} oc_rach_kind_t;

typedef struct {
    uint32_t       tmid;
    uint8_t        kind;
    uint8_t        payload_len; /* 0..OC_RACH_MAX_PAYLOAD */
    const uint8_t *payload;
} oc_rach_t;

/* Voice/control payload in a granted slot. The payload is opaque here
 * (RADE/Codec2 frames and upper-layer control, encrypted above this layer). */
typedef struct {
    uint32_t       tmid;
    uint8_t        seq;
    uint8_t        flags;
    uint8_t        payload_len; /* 0..OC_DATA_MAX_PAYLOAD */
    const uint8_t *payload;
} oc_data_t;

typedef struct {
    uint8_t type; /* oc_air_type_t */
    union {
        oc_beacon_t beacon;
        oc_grant_t  grant;
        oc_rach_t   rach;
        oc_data_t   data;
    } u;
} oc_air_msg_t;

/* Serialize. Returns bytes written, or 0 if a field is out of range or cap is
 * too small. */
size_t oc_air_encode(const oc_air_msg_t *msg, uint8_t *out, size_t cap);

/* Parse. Returns 0 on success, -1 on wrong version, unknown type, bad length,
 * or out-of-range fields. Payload pointers alias buf. */
int oc_air_decode(const uint8_t *buf, size_t len, oc_air_msg_t *msg);

/* Channel a terminal uses for one leg of its grant in a given frame. */
uint8_t oc_grant_leg_channel(uint32_t cell_seed, const oc_grant_leg_t *leg, uint32_t frame_number);

#endif
