/* oc_sig: OpenCell signalling. Constants and shared types (spec
 * 2026-09-26-activation-registration-calls-design.md). */
#ifndef OC_SIG_H
#define OC_SIG_H

#include <stddef.h>
#include <stdint.h>

#define OC_SIG_LINK_MAX   20u /* one UP/DOWN link payload */
#define OC_SIG_FRAG_DATA  18u /* message bytes per signalling fragment */
#define OC_SIG_MAX_FRAGS  4u
#define OC_SIG_MAX_MSG    (OC_SIG_FRAG_DATA * OC_SIG_MAX_FRAGS)
#define OC_SIG_APP_MAX    18u /* app data bytes per frame */
#define OC_SIG_KIND_SIG   0x10u
#define OC_SIG_KIND_SVC   0x30u
#define OC_SIG_KIND_DATA  0x80u
#define OC_SIG_NUMBER_LEN    8u  /* BCD bytes (numbering-plan.md v0.2) */
#define OC_SIG_NUMBER_DIGITS 15u
#define OC_SIG_NUMBER_TEXT   17u /* '+', 15 digits, NUL */
#define OC_SIG_NUMBER_SHOW   21u /* "+883-1-606-555-01234", NUL */

typedef enum {
    OC_SIG_ACT_REQ = 0x01, OC_SIG_ACT_ACK = 0x02, OC_SIG_ACT_NAK = 0x03,
    OC_SIG_REG_REQ = 0x10, OC_SIG_AUTH_REQ = 0x11, OC_SIG_AUTH_RSP = 0x12, OC_SIG_AUTH_FAIL = 0x13,
    OC_SIG_REG_ACK = 0x14, OC_SIG_REG_REJ = 0x15,
    OC_SIG_CHAN_LIST = 0x16, OC_SIG_CHAN_LIST_ACK = 0x17, /* channel-list spec §7 */
    OC_SIG_CALL_SETUP = 0x20, OC_SIG_CALL_PROC = 0x21, OC_SIG_ALERTING = 0x22, OC_SIG_CONNECT = 0x23,
    OC_SIG_CONNECT_ACK = 0x24, OC_SIG_SETUP_IND = 0x25, OC_SIG_RELEASE = 0x26, OC_SIG_RELEASE_COMPLETE = 0x27
} oc_sig_type_t;

typedef enum { OC_SIG_MODE_PART15 = 1, OC_SIG_MODE_PART97 = 2 } oc_sig_mode_t;

typedef enum {
    OC_SIG_CAUSE_NORMAL = 0, OC_SIG_CAUSE_REJECTED = 1, OC_SIG_CAUSE_BUSY = 2, OC_SIG_CAUSE_NO_ANSWER = 3,
    OC_SIG_CAUSE_UNREACHABLE = 4, OC_SIG_CAUSE_NET_FAILURE = 5, OC_SIG_CAUSE_LINK_LOST = 6
} oc_sig_cause_t;

typedef enum { OC_SIG_SVC_REGISTER = 1, OC_SIG_SVC_CALL = 2, OC_SIG_SVC_REREGISTER = 3, OC_SIG_SVC_CONFIG = 4 } oc_sig_svc_t;

/* ACT_NAK / ACT_FAILED reasons: 1-4 from the network, 5-6 found by the terminal. */
typedef enum {
    OC_SIG_ACT_UNKNOWN = 1, OC_SIG_ACT_USED = 2, OC_SIG_ACT_EXPIRED = 3, OC_SIG_ACT_BAD_TAG = 4,
    OC_SIG_ACT_BAD_CONFIRM = 5, OC_SIG_ACT_TIMEOUT = 6
} oc_sig_act_reason_t;

/* REG_REJ / REG_FAILED reasons. */
typedef enum {
    OC_SIG_REG_NOT_ACTIVATED = 1, OC_SIG_REG_AUTH_FAILED = 2, OC_SIG_REG_NET_AUTH = 3, OC_SIG_REG_TIMEOUT = 4
} oc_sig_reg_reason_t;

/* Terminal states (STATUS byte 3). */
typedef enum {
    OC_SIG_ST_NOT_ACTIVATED = 0, OC_SIG_ST_ACTIVATING = 1, OC_SIG_ST_REGISTERING = 2, OC_SIG_ST_REGISTERED = 3,
    OC_SIG_ST_CALLING = 4, OC_SIG_ST_RINGING_OUT = 5, OC_SIG_ST_RINGING_IN = 6, OC_SIG_ST_IN_CALL = 7,
    OC_SIG_ST_RELEASING = 8
} oc_sig_state_t;

/* BLE EVENT codes and COMMAND ops (spec §6). */
typedef enum {
    OC_SIG_EV_ACTIVATED = 0x01, OC_SIG_EV_ACT_FAILED = 0x02, OC_SIG_EV_REGISTERED = 0x03,
    OC_SIG_EV_REG_FAILED = 0x04, OC_SIG_EV_INCOMING = 0x05, OC_SIG_EV_RINGING = 0x06,
    OC_SIG_EV_CONNECTED = 0x07, OC_SIG_EV_ENDED = 0x08, OC_SIG_EV_DEACTIVATED = 0x09
} oc_sig_ev_t;

typedef enum {
    OC_SIG_CMD_ACTIVATE = 0x01, OC_SIG_CMD_DIAL = 0x02, OC_SIG_CMD_ANSWER = 0x03, OC_SIG_CMD_REJECT = 0x04,
    OC_SIG_CMD_HANGUP = 0x05, OC_SIG_CMD_DEACTIVATE = 0x06,
    OC_SIG_CMD_SCAN = 0x07 /* the scan list (oc_term_gatt.h), not signalling: oc_sig_term refuses it */
} oc_sig_cmd_t;

/* CHAN_LIST (channel-list spec §7): the network's scan-list entries, in its
 * order of preference. Body: ver (1) || count (1, 0..12) || count x
 * { freq_hz (4, BE) || flags (1) }, at most 62 bytes. count 0 clears them. */
#define OC_SIG_CHAN_MAX   12u
#define OC_SIG_CHAN_FIXED 0x01u /* the cell there sends every beacon on its anchor (Part 97) */

typedef struct {
    uint8_t  ver;
    uint8_t  count;
    uint32_t freq_hz[OC_SIG_CHAN_MAX];
    uint8_t  flags[OC_SIG_CHAN_MAX];
} oc_sig_chan_list_t;

#define OC_SIG_ATT_NOT_NOW 0x80u
#define OC_SIG_ATT_BAD_LEN 0x0Du
#define OC_SIG_ATT_BAD_ARG 0x81u

/* Numbers (numbering-plan.md v0.2): 883 . country code . national number,
 * 8-15 digits; CC 1 (NANP) is exactly 15: NPA (3) NXX (3) subscriber (5).
 * On the air: BCD, high nibble first, then 0xF in every remaining nibble.
 * Only canonical encodings are valid, so equal numbers are equal bytes. */

/* Full form only ("+" optional, no separators): "+883160655501234". 0 or -1. */
int    oc_sig_number_to_bcd(const char *text, size_t len, uint8_t bcd[OC_SIG_NUMBER_LEN]);
/* Any dialled form (numbering-plan.md "Dial Plan"). home = the caller's own
 * number, or NULL (then only international forms). 0 or -1. */
int    oc_sig_number_normalize(const char *dialed, size_t len, const uint8_t *home,
                               uint8_t bcd[OC_SIG_NUMBER_LEN]);
/* 1 if bcd is a canonical, valid number, else 0. */
int    oc_sig_number_valid(const uint8_t bcd[OC_SIG_NUMBER_LEN]);
/* "+883160655501234": '+' and the digits up to the first filler nibble. */
void   oc_sig_number_to_text(const uint8_t bcd[OC_SIG_NUMBER_LEN], char text[OC_SIG_NUMBER_TEXT]);
/* For people: "+883-1-606-555-01234", or "+883-44-2079460000" for a country
 * without a national plan. Returns the length, 0 if cap is too small or the
 * number is not valid (out is then "" when cap > 0). */
size_t oc_sig_number_format(const uint8_t bcd[OC_SIG_NUMBER_LEN], char *out, size_t cap);

static inline void oc_sig_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint32_t oc_sig_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void oc_sig_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline uint16_t oc_sig_get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* 48-bit MILENAGE sequence numbers, big-endian. */
static inline uint64_t oc_sig_sqn_get(const uint8_t s[6])
{
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | s[i];
    return v;
}
static inline void oc_sig_sqn_put(uint8_t s[6], uint64_t v)
{
    for (int i = 5; i >= 0; i--) {
        s[i] = (uint8_t)v;
        v >>= 8;
    }
}

#endif
