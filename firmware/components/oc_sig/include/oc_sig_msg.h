/* Signalling message bodies (spec §5); integers big-endian. */
#ifndef OC_SIG_MSG_H
#define OC_SIG_MSG_H

#include "oc_sig.h"

typedef struct {
    uint8_t type; /* oc_sig_type_t */
    union {
        struct { uint8_t token_id[8], pkt[32], tag[8]; } act_req;
        struct { uint8_t number[OC_SIG_NUMBER_LEN], confirm[8]; } act_ack;
        struct { uint8_t reason, tag[8]; } act_nak;
        struct { uint8_t sw_version[3], caps; } reg_req;
        struct { uint8_t rand[16], autn[16]; } auth_req;
        struct { uint8_t res[8]; } auth_rsp;
        struct { uint8_t cause, auts[14]; } auth_fail; /* auts only when cause == 2 */
        struct { uint8_t mode; uint16_t period_s; uint8_t number[OC_SIG_NUMBER_LEN]; } reg_ack;
        struct { uint8_t cause; } reg_rej;
        struct { uint8_t ref, called[OC_SIG_NUMBER_LEN], codec_caps; } call_setup;
        struct { uint8_t ref; uint32_t call_id; } call_proc;
        struct { uint32_t call_id; } call; /* ALERTING, CONNECT_ACK, RELEASE_COMPLETE */
        struct { uint32_t call_id; uint8_t codec; } connect;
        struct { uint32_t call_id; uint8_t caller[OC_SIG_NUMBER_LEN], codec_caps; } setup_ind;
        struct { uint32_t call_id; uint8_t cause; } release;
        oc_sig_chan_list_t chan_list;
        struct { uint8_t ver; } chan_list_ack;
    } u;
} oc_sig_msg_t;

/* Body only (no type/prot/ctr header). Returns its length, 0 for an unknown
 * type or a buffer too small. */
size_t oc_sig_body_encode(const oc_sig_msg_t *m, uint8_t *out, size_t cap);

/* Exact length required, and every number valid (oc_sig_number_valid): a
 * malformed number makes the message undecodable. *m zeroed first. 0 or -1. */
int oc_sig_body_decode(uint8_t type, const uint8_t *in, size_t len, oc_sig_msg_t *m);

#endif
