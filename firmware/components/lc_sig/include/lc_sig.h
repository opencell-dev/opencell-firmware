/* lc_sig: OpenCell signalling. Constants and shared types (spec
 * 2026-09-26-activation-registration-calls-design.md). */
#ifndef LC_SIG_H
#define LC_SIG_H

#include <stddef.h>
#include <stdint.h>

#define LC_SIG_LINK_MAX   20u /* one UP/DOWN link payload */
#define LC_SIG_FRAG_DATA  18u /* message bytes per signalling fragment */
#define LC_SIG_MAX_FRAGS  4u
#define LC_SIG_MAX_MSG    (LC_SIG_FRAG_DATA * LC_SIG_MAX_FRAGS)
#define LC_SIG_APP_MAX    18u /* app data bytes per frame */
#define LC_SIG_KIND_SIG   0x10u
#define LC_SIG_KIND_SVC   0x30u
#define LC_SIG_KIND_DATA  0x80u
#define LC_SIG_NUMBER_LEN 7u

typedef enum {
    LC_SIG_ACT_REQ = 0x01, LC_SIG_ACT_ACK = 0x02, LC_SIG_ACT_NAK = 0x03,
    LC_SIG_REG_REQ = 0x10, LC_SIG_AUTH_REQ = 0x11, LC_SIG_AUTH_RSP = 0x12, LC_SIG_AUTH_FAIL = 0x13,
    LC_SIG_REG_ACK = 0x14, LC_SIG_REG_REJ = 0x15,
    LC_SIG_CALL_SETUP = 0x20, LC_SIG_CALL_PROC = 0x21, LC_SIG_ALERTING = 0x22, LC_SIG_CONNECT = 0x23,
    LC_SIG_CONNECT_ACK = 0x24, LC_SIG_SETUP_IND = 0x25, LC_SIG_RELEASE = 0x26, LC_SIG_RELEASE_COMPLETE = 0x27
} lc_sig_type_t;

typedef enum { LC_SIG_MODE_PART15 = 1, LC_SIG_MODE_PART97 = 2 } lc_sig_mode_t;

typedef enum {
    LC_SIG_CAUSE_NORMAL = 0, LC_SIG_CAUSE_REJECTED = 1, LC_SIG_CAUSE_BUSY = 2, LC_SIG_CAUSE_NO_ANSWER = 3,
    LC_SIG_CAUSE_UNREACHABLE = 4, LC_SIG_CAUSE_NET_FAILURE = 5, LC_SIG_CAUSE_LINK_LOST = 6
} lc_sig_cause_t;

typedef enum { LC_SIG_SVC_REGISTER = 1, LC_SIG_SVC_CALL = 2, LC_SIG_SVC_REREGISTER = 3 } lc_sig_svc_t;

/* ACT_NAK / ACT_FAILED reasons: 1-4 from the network, 5-6 found by the terminal. */
typedef enum {
    LC_SIG_ACT_UNKNOWN = 1, LC_SIG_ACT_USED = 2, LC_SIG_ACT_EXPIRED = 3, LC_SIG_ACT_BAD_TAG = 4,
    LC_SIG_ACT_BAD_CONFIRM = 5, LC_SIG_ACT_TIMEOUT = 6
} lc_sig_act_reason_t;

/* REG_REJ / REG_FAILED reasons. */
typedef enum {
    LC_SIG_REG_NOT_ACTIVATED = 1, LC_SIG_REG_AUTH_FAILED = 2, LC_SIG_REG_NET_AUTH = 3, LC_SIG_REG_TIMEOUT = 4
} lc_sig_reg_reason_t;

/* Terminal states (STATUS byte 3). */
typedef enum {
    LC_SIG_ST_NOT_ACTIVATED = 0, LC_SIG_ST_ACTIVATING = 1, LC_SIG_ST_REGISTERING = 2, LC_SIG_ST_REGISTERED = 3,
    LC_SIG_ST_CALLING = 4, LC_SIG_ST_RINGING_OUT = 5, LC_SIG_ST_RINGING_IN = 6, LC_SIG_ST_IN_CALL = 7,
    LC_SIG_ST_RELEASING = 8
} lc_sig_state_t;

/* BLE EVENT codes and COMMAND ops (spec §6). */
typedef enum {
    LC_SIG_EV_ACTIVATED = 0x01, LC_SIG_EV_ACT_FAILED = 0x02, LC_SIG_EV_REGISTERED = 0x03,
    LC_SIG_EV_REG_FAILED = 0x04, LC_SIG_EV_INCOMING = 0x05, LC_SIG_EV_RINGING = 0x06,
    LC_SIG_EV_CONNECTED = 0x07, LC_SIG_EV_ENDED = 0x08, LC_SIG_EV_DEACTIVATED = 0x09
} lc_sig_ev_t;

typedef enum {
    LC_SIG_CMD_ACTIVATE = 0x01, LC_SIG_CMD_DIAL = 0x02, LC_SIG_CMD_ANSWER = 0x03, LC_SIG_CMD_REJECT = 0x04,
    LC_SIG_CMD_HANGUP = 0x05, LC_SIG_CMD_DEACTIVATE = 0x06
} lc_sig_cmd_t;

#define LC_SIG_ATT_NOT_NOW 0x80u
#define LC_SIG_ATT_BAD_LEN 0x0Du
#define LC_SIG_ATT_BAD_ARG 0x81u

/* "+8836065551234" ('+' optional): exactly 13 digits starting 883. 0 or -1. */
int  lc_sig_number_to_bcd(const char *text, size_t len, uint8_t bcd[LC_SIG_NUMBER_LEN]);
void lc_sig_number_to_text(const uint8_t bcd[LC_SIG_NUMBER_LEN], char text[16]);

static inline void lc_sig_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint32_t lc_sig_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void lc_sig_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline uint16_t lc_sig_get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* 48-bit MILENAGE sequence numbers, big-endian. */
static inline uint64_t lc_sig_sqn_get(const uint8_t s[6])
{
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | s[i];
    return v;
}
static inline void lc_sig_sqn_put(uint8_t s[6], uint64_t v)
{
    for (int i = 5; i >= 0; i--) {
        s[i] = (uint8_t)v;
        v >>= 8;
    }
}

#endif
