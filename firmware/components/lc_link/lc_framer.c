#include "lc_link.h"

#include <string.h>

#include "lc_crc.h"

size_t lc_link_write_frame(const lc_msg_t *msg, uint8_t *out, size_t cap)
{
    uint8_t plain[LC_LINK_MAX_MSG + 2u];
    size_t n = lc_msg_encode(msg, plain, LC_LINK_MAX_MSG);
    if (n == 0) {
        return 0;
    }
    uint16_t crc = lc_crc16(plain, n);
    plain[n++] = (uint8_t)(crc >> 8);
    plain[n++] = (uint8_t)crc;

    if (cap < LC_COBS_MAX_ENCODED(n) + 2u) {
        return 0;
    }
    /* Leading 0x00 terminates any partial frame or boot garbage already in the
     * receiver's buffer, so it can't be glued onto this frame. */
    out[0] = 0x00;
    size_t enc = 1u + lc_cobs_encode(plain, n, out + 1);
    out[enc++] = 0x00;
    return enc;
}

void lc_framer_init(lc_framer_t *f)
{
    memset(f, 0, sizeof(*f));
}

static int finish_frame(lc_framer_t *f, lc_msg_t *msg)
{
    int n = lc_cobs_decode(f->raw, f->raw_len, f->decoded, sizeof(f->decoded));
    if (n < 0) {
        f->cobs_errors++;
        return 0;
    }
    if (n < 4) { /* type + seq + CRC minimum */
        f->malformed++;
        return 0;
    }
    size_t body = (size_t)n - 2u;
    uint16_t rx_crc = (uint16_t)((f->decoded[body] << 8) | f->decoded[body + 1]);
    if (lc_crc16(f->decoded, body) != rx_crc) {
        f->crc_errors++;
        return 0;
    }
    if (lc_msg_decode(f->decoded, body, msg) != 0) {
        f->malformed++;
        return 0;
    }
    return 1;
}

int lc_framer_push(lc_framer_t *f, uint8_t byte, lc_msg_t *msg)
{
    if (byte != 0x00) {
        if (f->overflowed) {
            return 0;
        }
        if (f->raw_len >= sizeof(f->raw)) {
            f->overflowed = 1;
            f->overflows++;
            return 0;
        }
        f->raw[f->raw_len++] = byte;
        return 0;
    }

    /* Delimiter: an empty frame (back-to-back 0x00) is idle filler. */
    int result = 0;
    if (!f->overflowed && f->raw_len > 0) {
        result = finish_frame(f, msg);
    }
    f->raw_len = 0;
    f->overflowed = 0;
    return result;
}
