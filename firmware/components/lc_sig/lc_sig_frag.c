#include "lc_sig_frag.h"

#include <string.h>

uint8_t lc_sig_fragment(const uint8_t *msg, size_t len, uint8_t seq, uint8_t out[LC_SIG_MAX_FRAGS][LC_SIG_LINK_MAX],
                        uint8_t out_len[LC_SIG_MAX_FRAGS])
{
    if (len == 0 || len > LC_SIG_MAX_MSG) {
        return 0;
    }
    uint8_t count = (uint8_t)((len + LC_SIG_FRAG_DATA - 1u) / LC_SIG_FRAG_DATA);
    for (uint8_t i = 0; i < count; i++) {
        size_t off = (size_t)i * LC_SIG_FRAG_DATA;
        size_t n = len - off < LC_SIG_FRAG_DATA ? len - off : LC_SIG_FRAG_DATA;
        out[i][0] = (uint8_t)(LC_SIG_KIND_SIG | (unsigned)(i << 2) | (i + 1u == count ? 0x02u : 0u));
        out[i][1] = seq;
        memcpy(&out[i][2], msg + off, n);
        out_len[i] = (uint8_t)(n + 2u);
    }
    return count;
}

void lc_sig_reasm_init(lc_sig_reasm_t *r)
{
    memset(r, 0, sizeof(*r));
}

int lc_sig_reasm_push(lc_sig_reasm_t *r, const uint8_t *p, uint8_t n, uint8_t *msg, size_t *len, uint8_t *seq)
{
    if (n < 3 || n > LC_SIG_LINK_MAX || (p[0] & 0xF1u) != LC_SIG_KIND_SIG) {
        return -1;
    }
    uint8_t idx = (uint8_t)((p[0] >> 2) & 0x03u);
    int last = (p[0] >> 1) & 1;
    uint8_t dn = (uint8_t)(n - 2u);
    if (!last && dn != LC_SIG_FRAG_DATA) {
        return -1;
    }
    if (!r->active || r->seq != p[1]) {
        r->active = 1;
        r->seq = p[1];
        r->have = 0;
        r->total = 0;
    }
    memcpy(r->buf + (size_t)idx * LC_SIG_FRAG_DATA, p + 2, dn);
    r->have |= (uint8_t)(1u << idx);
    if (last) {
        r->total = (uint8_t)(idx + 1u);
        r->last_len = dn;
    }
    if (r->total != 0 && r->have == (uint8_t)((1u << r->total) - 1u)) {
        *len = (size_t)(r->total - 1u) * LC_SIG_FRAG_DATA + r->last_len;
        memcpy(msg, r->buf, *len);
        *seq = r->seq;
        r->active = 0;
        return 1;
    }
    return 0;
}
