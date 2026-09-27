/* lc_air — over-the-air message formats between base station and terminals.
 *
 * Every air frame starts with one byte: (LC_AIR_VERSION << 4) | type.
 * Multi-byte integers are little-endian. Times are in 10 µs units from the
 * start of the 120 ms frame (max LC_FRAME_US / 10 = 12000).
 *
 * The beacon is deliberately small (<= LC_BEACON_MAX_BYTES) because it goes
 * out every frame at the edge tier (LoRa SF7/500 kHz, ~15 ms at max size).
 * Slot assignments are NOT repeated in every beacon: a GRANT is sent once in
 * the terminal's DL slot (or the paging flow) and stays in force from
 * effective_frame until replaced or revoked. */
#ifndef LC_AIR_H
#define LC_AIR_H

#include <stddef.h>
#include <stdint.h>

#include "lc_phy.h"

#define LC_AIR_VERSION      1u
#define LC_AIR_MAX_FRAME    255u
#define LC_BCN_MAX_PAGES    2u
#define LC_BEACON_MAX_BYTES (17u + 4u * LC_BCN_MAX_PAGES)
#define LC_RACH_MAX_PAYLOAD 32u
#define LC_DATA_MAX_PAYLOAD 240u
#define LC_AIR_TIME_UNIT_US 10u
#define LC_AIR_FRAME_UNITS  (LC_FRAME_US / LC_AIR_TIME_UNIT_US)

typedef enum {
    LC_AIR_BEACON = 1,
    LC_AIR_GRANT  = 2,
    LC_AIR_RACH   = 3,
    LC_AIR_DATA   = 4
} lc_air_type_t;

#define LC_BCN_FLAG_ACCEPTING_ATTACH 0x01u
#define LC_BCN_FLAG_BACKHAUL_ACTIVE  0x02u
#define LC_BCN_FLAG_PART97           0x04u /* the network runs Part 97 mode (lc_sig spec §4.3) */

typedef struct {
    uint32_t cell_seed;
    uint32_t frame_number;
    uint8_t  band;          /* band of the radio sending this beacon */
    uint8_t  flags;         /* LC_BCN_FLAG_* */
    uint16_t rach_offset;   /* RACH window, 10 µs units */
    uint16_t rach_len;
    uint8_t  rach_slot_index;
    uint8_t  page_count;    /* 0..LC_BCN_MAX_PAGES */
    uint32_t page_tmid[LC_BCN_MAX_PAGES];
} lc_beacon_t;

/* One direction of a grant. len == 0 means "no slot in this direction". */
typedef struct {
    uint8_t  band;
    uint8_t  tier;
    uint8_t  radio_index; /* base-station radio on that band, for lc_hop_channel */
    uint8_t  slot_index;  /* hop slot index for lc_hop_channel */
    uint16_t offset;      /* 10 µs units from frame start */
    uint16_t len;         /* 10 µs units */
} lc_grant_leg_t;

/* A grant with both legs len == 0 revokes the terminal's slots. */
typedef struct {
    uint32_t       tmid;
    uint32_t       effective_frame;
    lc_grant_leg_t dl;
    lc_grant_leg_t ul;
} lc_grant_t;

typedef enum {
    LC_RACH_ATTACH     = 1,
    LC_RACH_PAGE_REPLY = 2,
    LC_RACH_UPPER      = 3  /* opaque upper-layer request (call setup etc.) */
} lc_rach_kind_t;

typedef struct {
    uint32_t       tmid;
    uint8_t        kind;
    uint8_t        payload_len; /* 0..LC_RACH_MAX_PAYLOAD */
    const uint8_t *payload;
} lc_rach_t;

/* Voice/control payload in a granted slot. The payload is opaque here
 * (RADE/Codec2 frames and upper-layer control, encrypted above this layer). */
typedef struct {
    uint32_t       tmid;
    uint8_t        seq;
    uint8_t        flags;
    uint8_t        payload_len; /* 0..LC_DATA_MAX_PAYLOAD */
    const uint8_t *payload;
} lc_data_t;

typedef struct {
    uint8_t type; /* lc_air_type_t */
    union {
        lc_beacon_t beacon;
        lc_grant_t  grant;
        lc_rach_t   rach;
        lc_data_t   data;
    } u;
} lc_air_msg_t;

/* Serialize. Returns bytes written, or 0 if a field is out of range or cap is
 * too small. */
size_t lc_air_encode(const lc_air_msg_t *msg, uint8_t *out, size_t cap);

/* Parse. Returns 0 on success, -1 on wrong version, unknown type, bad length,
 * or out-of-range fields. Payload pointers alias buf. */
int lc_air_decode(const uint8_t *buf, size_t len, lc_air_msg_t *msg);

/* Channel a terminal uses for one leg of its grant in a given frame. */
uint8_t lc_grant_leg_channel(uint32_t cell_seed, const lc_grant_leg_t *leg, uint32_t frame_number);

#endif
