#include "lc_sig.h"

int lc_sig_number_to_bcd(const char *text, size_t len, uint8_t bcd[LC_SIG_NUMBER_LEN])
{
    if (len > 0 && text[0] == '+') {
        text++;
        len--;
    }
    if (len != 13 || text[0] != '8' || text[1] != '8' || text[2] != '3') {
        return -1;
    }
    for (size_t i = 0; i < 13; i++) {
        if (text[i] < '0' || text[i] > '9') return -1;
    }
    for (size_t i = 0; i < LC_SIG_NUMBER_LEN; i++) {
        uint8_t hi = (uint8_t)(text[2 * i] - '0');
        uint8_t lo = 2 * i + 1 < 13 ? (uint8_t)(text[2 * i + 1] - '0') : 0x0F;
        bcd[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

void lc_sig_number_to_text(const uint8_t bcd[LC_SIG_NUMBER_LEN], char text[16])
{
    size_t n = 0;
    text[n++] = '+';
    for (size_t i = 0; i < 14; i++) {
        uint8_t d = (uint8_t)((i % 2 == 0) ? (bcd[i / 2] >> 4) : (bcd[i / 2] & 0x0F));
        if (d > 9) break;
        text[n++] = (char)('0' + d);
    }
    text[n] = '\0';
}
