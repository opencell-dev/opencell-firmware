#include "lc_sig_msg.h"

#include <string.h>

size_t lc_sig_body_encode(const lc_sig_msg_t *m, uint8_t *out, size_t cap)
{
    uint8_t b[64];
    size_t n = 0;
    switch (m->type) {
    case LC_SIG_ACT_REQ:
        memcpy(b, m->u.act_req.token_id, 8);
        memcpy(b + 8, m->u.act_req.pkt, 32);
        memcpy(b + 40, m->u.act_req.tag, 8);
        n = 48;
        break;
    case LC_SIG_ACT_ACK:
        memcpy(b, m->u.act_ack.number, LC_SIG_NUMBER_LEN);
        memcpy(b + LC_SIG_NUMBER_LEN, m->u.act_ack.confirm, 8);
        n = LC_SIG_NUMBER_LEN + 8;
        break;
    case LC_SIG_ACT_NAK:
        b[0] = m->u.act_nak.reason;
        memcpy(b + 1, m->u.act_nak.tag, 8);
        n = 9;
        break;
    case LC_SIG_REG_REQ:
        memcpy(b, m->u.reg_req.sw_version, 3);
        b[3] = m->u.reg_req.caps;
        n = 4;
        break;
    case LC_SIG_AUTH_REQ:
        memcpy(b, m->u.auth_req.rand, 16);
        memcpy(b + 16, m->u.auth_req.autn, 16);
        n = 32;
        break;
    case LC_SIG_AUTH_RSP:
        memcpy(b, m->u.auth_rsp.res, 8);
        n = 8;
        break;
    case LC_SIG_AUTH_FAIL:
        b[0] = m->u.auth_fail.cause;
        n = 1;
        if (m->u.auth_fail.cause == 2) {
            memcpy(b + 1, m->u.auth_fail.auts, 14);
            n = 15;
        }
        break;
    case LC_SIG_REG_ACK:
        b[0] = m->u.reg_ack.mode;
        lc_sig_put16(b + 1, m->u.reg_ack.period_s);
        memcpy(b + 3, m->u.reg_ack.number, LC_SIG_NUMBER_LEN);
        n = 3 + LC_SIG_NUMBER_LEN;
        break;
    case LC_SIG_REG_REJ:
        b[0] = m->u.reg_rej.cause;
        n = 1;
        break;
    case LC_SIG_CALL_SETUP:
        b[0] = m->u.call_setup.ref;
        memcpy(b + 1, m->u.call_setup.called, LC_SIG_NUMBER_LEN);
        b[1 + LC_SIG_NUMBER_LEN] = m->u.call_setup.codec_caps;
        n = 2 + LC_SIG_NUMBER_LEN;
        break;
    case LC_SIG_CALL_PROC:
        b[0] = m->u.call_proc.ref;
        lc_sig_put32(b + 1, m->u.call_proc.call_id);
        n = 5;
        break;
    case LC_SIG_ALERTING:
    case LC_SIG_CONNECT_ACK:
    case LC_SIG_RELEASE_COMPLETE:
        lc_sig_put32(b, m->u.call.call_id);
        n = 4;
        break;
    case LC_SIG_CONNECT:
        lc_sig_put32(b, m->u.connect.call_id);
        b[4] = m->u.connect.codec;
        n = 5;
        break;
    case LC_SIG_SETUP_IND:
        lc_sig_put32(b, m->u.setup_ind.call_id);
        memcpy(b + 4, m->u.setup_ind.caller, LC_SIG_NUMBER_LEN);
        b[4 + LC_SIG_NUMBER_LEN] = m->u.setup_ind.codec_caps;
        n = 5 + LC_SIG_NUMBER_LEN;
        break;
    case LC_SIG_RELEASE:
        lc_sig_put32(b, m->u.release.call_id);
        b[4] = m->u.release.cause;
        n = 5;
        break;
    default:
        return 0;
    }
    if (n > cap) {
        return 0;
    }
    memcpy(out, b, n);
    return n;
}

int lc_sig_body_decode(uint8_t type, const uint8_t *in, size_t len, lc_sig_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->type = type;
    switch (type) {
    case LC_SIG_ACT_REQ:
        if (len != 48) return -1;
        memcpy(m->u.act_req.token_id, in, 8);
        memcpy(m->u.act_req.pkt, in + 8, 32);
        memcpy(m->u.act_req.tag, in + 40, 8);
        return 0;
    case LC_SIG_ACT_ACK:
        if (len != LC_SIG_NUMBER_LEN + 8) return -1;
        memcpy(m->u.act_ack.number, in, LC_SIG_NUMBER_LEN);
        memcpy(m->u.act_ack.confirm, in + LC_SIG_NUMBER_LEN, 8);
        return lc_sig_number_valid(m->u.act_ack.number) ? 0 : -1;
    case LC_SIG_ACT_NAK:
        if (len != 9) return -1;
        m->u.act_nak.reason = in[0];
        memcpy(m->u.act_nak.tag, in + 1, 8);
        return 0;
    case LC_SIG_REG_REQ:
        if (len != 4) return -1;
        memcpy(m->u.reg_req.sw_version, in, 3);
        m->u.reg_req.caps = in[3];
        return 0;
    case LC_SIG_AUTH_REQ:
        if (len != 32) return -1;
        memcpy(m->u.auth_req.rand, in, 16);
        memcpy(m->u.auth_req.autn, in + 16, 16);
        return 0;
    case LC_SIG_AUTH_RSP:
        if (len != 8) return -1;
        memcpy(m->u.auth_rsp.res, in, 8);
        return 0;
    case LC_SIG_AUTH_FAIL:
        if (len < 1) return -1;
        m->u.auth_fail.cause = in[0];
        if (in[0] == 2) {
            if (len != 15) return -1;
            memcpy(m->u.auth_fail.auts, in + 1, 14);
            return 0;
        }
        return len == 1 ? 0 : -1;
    case LC_SIG_REG_ACK:
        if (len != 3 + LC_SIG_NUMBER_LEN) return -1;
        m->u.reg_ack.mode = in[0];
        m->u.reg_ack.period_s = lc_sig_get16(in + 1);
        memcpy(m->u.reg_ack.number, in + 3, LC_SIG_NUMBER_LEN);
        return lc_sig_number_valid(m->u.reg_ack.number) ? 0 : -1;
    case LC_SIG_REG_REJ:
        if (len != 1) return -1;
        m->u.reg_rej.cause = in[0];
        return 0;
    case LC_SIG_CALL_SETUP:
        if (len != 2 + LC_SIG_NUMBER_LEN) return -1;
        m->u.call_setup.ref = in[0];
        memcpy(m->u.call_setup.called, in + 1, LC_SIG_NUMBER_LEN);
        m->u.call_setup.codec_caps = in[1 + LC_SIG_NUMBER_LEN];
        return lc_sig_number_valid(m->u.call_setup.called) ? 0 : -1;
    case LC_SIG_CALL_PROC:
        if (len != 5) return -1;
        m->u.call_proc.ref = in[0];
        m->u.call_proc.call_id = lc_sig_get32(in + 1);
        return 0;
    case LC_SIG_ALERTING:
    case LC_SIG_CONNECT_ACK:
    case LC_SIG_RELEASE_COMPLETE:
        if (len != 4) return -1;
        m->u.call.call_id = lc_sig_get32(in);
        return 0;
    case LC_SIG_CONNECT:
        if (len != 5) return -1;
        m->u.connect.call_id = lc_sig_get32(in);
        m->u.connect.codec = in[4];
        return 0;
    case LC_SIG_SETUP_IND:
        if (len != 5 + LC_SIG_NUMBER_LEN) return -1;
        m->u.setup_ind.call_id = lc_sig_get32(in);
        memcpy(m->u.setup_ind.caller, in + 4, LC_SIG_NUMBER_LEN);
        m->u.setup_ind.codec_caps = in[4 + LC_SIG_NUMBER_LEN];
        return lc_sig_number_valid(m->u.setup_ind.caller) ? 0 : -1;
    case LC_SIG_RELEASE:
        if (len != 5) return -1;
        m->u.release.call_id = lc_sig_get32(in);
        m->u.release.cause = in[4];
        return 0;
    default:
        return -1;
    }
}
