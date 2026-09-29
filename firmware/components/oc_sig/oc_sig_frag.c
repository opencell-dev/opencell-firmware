#include "oc_sig_frag.h"

#include <string.h>

uint8_t oc_sig_fragment(const uint8_t *msg, size_t len, uint8_t seq, uint8_t out[OC_SIG_MAX_FRAGS][OC_SIG_LINK_MAX],
                        uint8_t out_len[OC_SIG_MAX_FRAGS])
{
    if (len == 0 || len > OC_SIG_MAX_MSG) {
        return 0;
    }
    uint8_t count = (uint8_t)((len + OC_SIG_FRAG_DATA - 1u) / OC_SIG_FRAG_DATA);
    for (uint8_t i = 0; i < count; i++) {
        size_t off = (size_t)i * OC_SIG_FRAG_DATA;
        size_t n = len - off < OC_SIG_FRAG_DATA ? len - off : OC_SIG_FRAG_DATA;
        out[i][0] = (uint8_t)(OC_SIG_KIND_SIG | (unsigned)(i << 2) | (i + 1u == count ? 0x02u : 0u));
        out[i][1] = seq;
        memcpy(&out[i][2], msg + off, n);
        out_len[i] = (uint8_t)(n + 2u);
    }
    return count;
}

void oc_sig_reasm_init(oc_sig_reasm_t *r)
{
    memset(r, 0, sizeof(*r));
}

int oc_sig_reasm_push(oc_sig_reasm_t *r, const uint8_t *p, uint8_t n, uint8_t *msg, size_t *len, uint8_t *seq)
{
    if (n < 3 || n > OC_SIG_LINK_MAX || (p[0] & 0xF1u) != OC_SIG_KIND_SIG) {
        return -1;
    }
    uint8_t idx = (uint8_t)((p[0] >> 2) & 0x03u);
    int last = (p[0] >> 1) & 1;
    uint8_t dn = (uint8_t)(n - 2u);
    if (!last && dn != OC_SIG_FRAG_DATA) {
        return -1;
    }
    if (!r->active || r->seq != p[1]) {
        r->active = 1;
        r->seq = p[1];
        r->have = 0;
        r->total = 0;
    }
    memcpy(r->buf + (size_t)idx * OC_SIG_FRAG_DATA, p + 2, dn);
    r->have |= (uint8_t)(1u << idx);
    if (last) {
        r->total = (uint8_t)(idx + 1u);
        r->last_len = dn;
    }
    if (r->total != 0 && r->have == (uint8_t)((1u << r->total) - 1u)) {
        *len = (size_t)(r->total - 1u) * OC_SIG_FRAG_DATA + r->last_len;
        memcpy(msg, r->buf, *len);
        *seq = r->seq;
        r->active = 0;
        return 1;
    }
    return 0;
}
