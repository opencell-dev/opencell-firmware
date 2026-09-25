/* lc_link — Pi <-> W12 UART protocol.
 * Wire format per frame: 0x00, COBS(message || crc16_be(message)), 0x00.
 * Message: type (u8), seq (u8), body. Multi-byte integers are little-endian. */
#ifndef LC_LINK_H
#define LC_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "lc_cobs.h"
#include "lc_phy.h"

#define LC_LINK_MAX_MSG           2048u  /* decoded message bytes, excluding CRC */
#define LC_MAX_SLOTS_PER_SCHEDULE 64u
#define LC_MAX_FW_CHUNK           1024u
#define LC_SCHED_FLAG_LAST        0x01u

typedef enum {
    LC_MSG_CONFIG    = 0x01, /* host -> W12 */
    LC_MSG_SCHEDULE  = 0x02, /* host -> W12 */
    LC_MSG_RX_REPORT = 0x03, /* W12 -> host */
    LC_MSG_STATUS    = 0x04, /* W12 -> host */
    LC_MSG_FW_CHUNK  = 0x05, /* host -> W12 */
    LC_MSG_FW_COMMIT = 0x06, /* host -> W12 */
    LC_MSG_ACK       = 0x07, /* W12 -> host */
    LC_MSG_TIME      = 0x08  /* host -> W12: unix second of the latest PPS edge */
} lc_msg_type_t;

typedef enum {
    LC_ROLE_BS_RADIO       = 0,
    LC_ROLE_TERMINAL       = 1,
    LC_ROLE_BS_RADIO_BENCH = 2  /* bs-radio with an internal 1 Hz PPS (bench only, no GPS) */
} lc_role_t;
typedef enum { LC_DIR_RX = 0, LC_DIR_TX = 1 } lc_dir_t;

typedef enum {
    LC_ACK_OK              = 0,
    LC_ACK_ERR_UNSUPPORTED = 1,
    LC_ACK_ERR_LATE        = 2, /* SCHEDULE arrived after its frame's deadline */
    LC_ACK_ERR_FLASH       = 3,
    LC_ACK_ERR_MALFORMED   = 4
} lc_ack_status_t;

typedef struct {
    uint8_t  role;
    uint8_t  band;
    uint8_t  radio_index;
    uint32_t cell_seed;
} lc_config_t;

/* payload points into the buffer the message was decoded from. */
typedef struct {
    uint32_t       offset_us; /* from frame start */
    uint32_t       length_us;
    uint32_t       freq_hz;
    lc_mode_t      mode;
    uint8_t        dir;
    uint8_t        payload_len; /* TX: bytes to send; RX: ignored (0) */
    const uint8_t *payload;
} lc_slot_t;

/* One frame's schedule may span several messages; the last has
 * LC_SCHED_FLAG_LAST set. */
typedef struct {
    uint32_t  frame_number;
    uint8_t   flags;
    uint8_t   slot_count;
    lc_slot_t slots[LC_MAX_SLOTS_PER_SCHEDULE];
} lc_schedule_t;

typedef struct {
    uint32_t       frame_number;
    uint8_t        slot_index;
    int16_t        rssi_dbm;
    int16_t        snr_qdb; /* SNR in 0.25 dB units */
    uint8_t        crc_ok;
    uint8_t        payload_len;
    const uint8_t *payload;
} lc_rx_report_t;

/* pps_locked carries an lc_clock_state_t: 0 unlocked, 1 locked, 2 holdover. */
typedef struct {
    uint32_t uptime_ms;
    uint8_t  pps_locked;
    int8_t   temp_c;
    uint16_t schedule_misses;
    uint16_t uart_crc_errors;
    uint32_t frame_number;  /* W12's current frame; 0 if its clock is unusable */
} lc_status_t;

typedef struct {
    uint32_t       offset;
    uint16_t       len;
    const uint8_t *data;
} lc_fw_chunk_t;

typedef struct {
    uint32_t image_size;
} lc_fw_commit_t;

typedef struct {
    uint8_t acked_seq;
    uint8_t status;
} lc_ack_t;

typedef struct {
    uint32_t unix_s; /* UTC second that the most recent PPS edge marked */
} lc_time_t;

typedef struct {
    uint8_t type;
    uint8_t seq;
    union {
        lc_config_t    config;
        lc_schedule_t  schedule;
        lc_rx_report_t rx_report;
        lc_status_t    status;
        lc_fw_chunk_t  fw_chunk;
        lc_fw_commit_t fw_commit;
        lc_ack_t       ack;
        lc_time_t      time;
    } u;
} lc_msg_t;

/* Serialize msg (no CRC, no COBS). Returns bytes written, or 0 if the type is
 * unknown, a count/length exceeds its limit, or cap is too small. */
size_t lc_msg_encode(const lc_msg_t *msg, uint8_t *out, size_t cap);

/* Parse a serialized message. Returns 0 on success, -1 if malformed
 * (unknown type, short/long body, counts over limits). Payload pointers in
 * *msg alias buf. */
int lc_msg_decode(const uint8_t *buf, size_t len, lc_msg_t *msg);

/* Encode msg as one wire frame (0x00 + COBS(msg + CRC) + 0x00) into out.
 * Returns total bytes, or 0 on error. */
size_t lc_link_write_frame(const lc_msg_t *msg, uint8_t *out, size_t cap);

#define LC_FRAMER_RAW_CAP LC_COBS_MAX_ENCODED(LC_LINK_MAX_MSG + 2u)

typedef struct {
    uint8_t  raw[LC_FRAMER_RAW_CAP];
    size_t   raw_len;
    int      overflowed;
    uint8_t  decoded[LC_LINK_MAX_MSG + 2u];
    uint32_t crc_errors;
    uint32_t cobs_errors;
    uint32_t overflows;
    uint32_t malformed;
} lc_framer_t;

void lc_framer_init(lc_framer_t *f);

/* Feed one received byte. Returns 1 when a 0x00 delimiter completes a valid
 * frame and *msg has been filled; 0 otherwise (bad frames bump a counter).
 * Payload pointers in *msg stay valid until the next 0x00 is pushed. */
int lc_framer_push(lc_framer_t *f, uint8_t byte, lc_msg_t *msg);

#endif
