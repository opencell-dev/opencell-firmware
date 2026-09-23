#include "lc_cobs.h"

size_t lc_cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t out_pos = 1;
    size_t code_pos = 0;
    uint8_t code = 1;

    for (size_t i = 0; i < len; i++) {
        if (in[i] == 0) {
            out[code_pos] = code;
            code_pos = out_pos++;
            code = 1;
        } else {
            out[out_pos++] = in[i];
            code++;
            if (code == 0xFF && i + 1 < len) {
                out[code_pos] = code;
                code_pos = out_pos++;
                code = 1;
            }
        }
    }
    out[code_pos] = code;
    return out_pos;
}

int lc_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap)
{
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < len) {
        uint8_t code = in[in_pos++];
        if (code == 0 || in_pos + code - 1 > len) {
            return -1;
        }
        for (uint8_t i = 1; i < code; i++) {
            if (in[in_pos] == 0 || out_pos >= out_cap) {
                return -1;
            }
            out[out_pos++] = in[in_pos++];
        }
        if (code != 0xFF && in_pos < len) {
            if (out_pos >= out_cap) {
                return -1;
            }
            out[out_pos++] = 0;
        }
    }
    return (int)out_pos;
}
