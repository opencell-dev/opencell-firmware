/* oc_link — Pi <-> W12 UART protocol.
 * Wire format per frame: 0x00, COBS(message || crc16_be(message)), 0x00.
 * Message: type (u8), seq (u8), body. Multi-byte integers are little-endian. */
#ifndef OC_LINK_H
#define OC_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "oc_cobs.h"
#include "oc_phy.h"

#define OC_LINK_MAX_MSG           2048u  /* decoded message bytes, excluding CRC */
#define OC_MAX_SLOTS_PER_SCHEDULE 64u
#define OC_MAX_FW_CHUNK           1024u
#define OC_SCHED_FLAG_LAST        0x01u
#define OC_SCHED_FLAG_FIRST       0x02u  /* first part of a frame: opens (or restarts) its buffer */

typedef enum {
    OC_MSG_CONFIG    = 0x01, /* host -> W12 */
    OC_MSG_SCHEDULE  = 0x02, /* host -> W12 */
    OC_MSG_RX_REPORT = 0x03, /* W12 -> host */
    OC_MSG_STATUS    = 0x04, /* W12 -> host */
    OC_MSG_FW_CHUNK  = 0x05, /* host -> W12 */
    OC_MSG_FW_COMMIT = 0x06, /* host -> W12 */
    OC_MSG_ACK       = 0x07, /* W12 -> host */
    OC_MSG_TIME      = 0x08  /* host -> W12: unix second of the latest PPS edge */
} oc_msg_type_t;

typedef enum {
    OC_ROLE_BS_RADIO       = 0,
    OC_ROLE_TERMINAL       = 1,
    OC_ROLE_BS_RADIO_BENCH = 2  /* bs-radio with an internal 1 Hz PPS (bench only, no GPS) */
} oc_role_t;
typedef enum { OC_DIR_RX = 0, OC_DIR_TX = 1 } oc_dir_t;

typedef enum {
    OC_ACK_OK              = 0,
    OC_ACK_ERR_UNSUPPORTED = 1,
    OC_ACK_ERR_LATE        = 2, /* SCHEDULE arrived after its frame's deadline */
    OC_ACK_ERR_FLASH       = 3,
    OC_ACK_ERR_MALFORMED   = 4
} oc_ack_status_t;

typedef struct {
    uint8_t  role;
    uint8_t  band;
    uint8_t  radio_index;
    uint32_t cell_seed;
} oc_config_t;

/* payload points into the buffer the message was decoded from. */
typedef struct {
    uint32_t       offset_us; /* from frame start */
    uint32_t       length_us;
    uint32_t       freq_hz;
    oc_mode_t      mode;
    uint8_t        dir;
    uint8_t        payload_len; /* TX: bytes to send; RX: ignored (0) */
    const uint8_t *payload;
} oc_slot_t;

/* One frame's schedule may span several messages; the last has
 * OC_SCHED_FLAG_LAST set. */
typedef struct {
    uint32_t  frame_number;
    uint8_t   flags;
    uint8_t   slot_count;
    oc_slot_t slots[OC_MAX_SLOTS_PER_SCHEDULE];
} oc_schedule_t;

typedef struct {
    uint32_t       frame_number;
    uint8_t        slot_index;
    int16_t        rssi_dbm;
    int16_t        snr_qdb; /* SNR in 0.25 dB units */
    uint8_t        crc_ok;
    uint8_t        payload_len;
    const uint8_t *payload;
    int32_t        end_us; /* packet end from the RX board's frame start; OC_RX_END_UNKNOWN if not measured */
} oc_rx_report_t;

#define OC_RX_END_UNKNOWN INT32_MIN

/* pps_locked carries an oc_clock_state_t: 0 unlocked, 1 locked, 2 holdover. */
typedef struct {
    uint32_t uptime_ms;
    uint8_t  pps_locked;
    int8_t   temp_c;
    uint16_t schedule_misses;
    uint16_t uart_crc_errors;
    uint32_t frame_number;  /* W12's current frame; 0 if its clock is unusable */
    int32_t  last_tx_end_us;   /* latest TX done from its frame start; OC_RX_END_UNKNOWN if none */
    int32_t  last_tx_start_us; /* and its preamble start */
    uint16_t late_slots;       /* executor counters since boot, saturating */
    uint16_t radio_errors;
    int16_t  last_radio_err;   /* latest failing radio call's code (RadioLib error) */
    uint8_t  last_radio_op;    /* which call: 1 configure, 2 stage, 3 launch, 4 error event */
} oc_status_t;

typedef struct {
    uint32_t       offset;
    uint16_t       len;
    const uint8_t *data;
} oc_fw_chunk_t;

typedef struct {
    uint32_t image_size;
} oc_fw_commit_t;

typedef struct {
    uint8_t acked_seq;
    uint8_t status;
} oc_ack_t;

typedef struct {
    uint32_t unix_s; /* UTC second that the most recent PPS edge marked */
} oc_time_t;

typedef struct {
    uint8_t type;
    uint8_t seq;
    union {
        oc_config_t    config;
        oc_schedule_t  schedule;
        oc_rx_report_t rx_report;
        oc_status_t    status;
        oc_fw_chunk_t  fw_chunk;
        oc_fw_commit_t fw_commit;
        oc_ack_t       ack;
        oc_time_t      time;
    } u;
} oc_msg_t;

/* Serialize msg (no CRC, no COBS). Returns bytes written, or 0 if the type is
 * unknown, a count/length exceeds its limit, or cap is too small. */
size_t oc_msg_encode(const oc_msg_t *msg, uint8_t *out, size_t cap);

/* Parse a serialized message. Returns 0 on success, -1 if malformed
 * (unknown type, short/long body, counts over limits). Payload pointers in
 * *msg alias buf. */
int oc_msg_decode(const uint8_t *buf, size_t len, oc_msg_t *msg);

/* Encode msg as one wire frame (0x00 + COBS(msg + CRC) + 0x00) into out.
 * Returns total bytes, or 0 on error. */
size_t oc_link_write_frame(const oc_msg_t *msg, uint8_t *out, size_t cap);

#define OC_FRAMER_RAW_CAP OC_COBS_MAX_ENCODED(OC_LINK_MAX_MSG + 2u)

typedef struct {
    uint8_t  raw[OC_FRAMER_RAW_CAP];
    size_t   raw_len;
    int      overflowed;
    uint8_t  decoded[OC_LINK_MAX_MSG + 2u];
    uint32_t crc_errors;
    uint32_t cobs_errors;
    uint32_t overflows;
    uint32_t malformed;
} oc_framer_t;

void oc_framer_init(oc_framer_t *f);

/* Feed one received byte. Returns 1 when a 0x00 delimiter completes a valid
 * frame and *msg has been filled; 0 otherwise (bad frames bump a counter).
 * Payload pointers in *msg stay valid until the next 0x00 is pushed. */
int oc_framer_push(oc_framer_t *f, uint8_t byte, oc_msg_t *msg);

#endif
