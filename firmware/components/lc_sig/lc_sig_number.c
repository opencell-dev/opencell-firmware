/* OpenCell numbers (numbering-plan.md v0.2, spec 2026-09-27-numbering-v2-design.md §4-§5). */
#include "lc_sig.h"

#include <string.h>

/* The ITU-T E.164 two-digit country codes; 1 and 7 are the one-digit ones,
 * every other code has three digits (codes are prefix-free). A trailing
 * space after the last entry keeps every triplet the same width, so the
 * scan below always finds its NUL terminator on a triplet boundary. */
static const char k_cc2[] = "20 27 30 31 32 33 34 36 39 40 41 43 44 45 46 47 48 49 51 52 53 54 55 56 57 58 "
                            "60 61 62 63 64 65 66 81 82 84 86 90 91 92 93 94 95 98 ";

static size_t cc_len(const char *d) /* d: the digits after 883 */
{
    if (d[0] == '1' || d[0] == '7') return 1;
    for (const char *p = k_cc2; *p != '\0'; p += 3) {
        if (p[0] == d[0] && p[1] == d[1]) return 2;
    }
    return 3;
}

static int nanp_code_ok(const char *c) /* NPA or NXX: 2-9 first, not N11 */
{
    return c[0] >= '2' && c[0] <= '9' && !(c[1] == '1' && c[2] == '1');
}

/* The rules every number obeys, on n digits (no '+'). */
static int digits_ok(const char *d, size_t n)
{
    if (n < 8 || n > LC_SIG_NUMBER_DIGITS || memcmp(d, "883", 3) != 0) return 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i] < '0' || d[i] > '9') return 0;
    }
    if (d[3] == '1') { /* NANP: 883 1 NPA NXX subscriber(5) */
        return n == 15 && nanp_code_ok(d + 4) && memcmp(d + 4, "883", 3) != 0 && nanp_code_ok(d + 7);
    }
    return 1;
}

static void encode(const char *d, size_t n, uint8_t bcd[LC_SIG_NUMBER_LEN])
{
    memset(bcd, 0xFF, LC_SIG_NUMBER_LEN);
    for (size_t i = 0; i < n; i++) {
        uint8_t v = (uint8_t)(d[i] - '0');
        bcd[i / 2] = (uint8_t)(i % 2 == 0 ? (v << 4) | 0x0F : (bcd[i / 2] & 0xF0) | v);
    }
}

/* The digits of bcd up to the first filler nibble; their count, 0 if a nibble
 * is A-E, a digit follows the filler, or there is no filler at all. d must
 * hold LC_SIG_NUMBER_DIGITS + 1 bytes: a canonical number has at most
 * LC_SIG_NUMBER_DIGITS digit nibbles (the 16th of 16 is always the filler,
 * or later), so a 16th digit nibble is rejected before it would overflow d. */
static size_t decode(const uint8_t bcd[LC_SIG_NUMBER_LEN], char d[LC_SIG_NUMBER_DIGITS + 1])
{
    size_t n = 0;
    int filler = 0;
    for (size_t i = 0; i < LC_SIG_NUMBER_LEN * 2; i++) {
        uint8_t v = (uint8_t)(i % 2 == 0 ? bcd[i / 2] >> 4 : bcd[i / 2] & 0x0F);
        if (v == 0x0F) {
            filler = 1;
        } else if (v > 9 || filler) {
            return 0;
        } else {
            if (n >= LC_SIG_NUMBER_DIGITS) return 0; /* 16 digit nibbles, no filler: not canonical */
            d[n++] = (char)('0' + v);
        }
    }
    d[n] = '\0';
    return filler ? n : 0;
}

int lc_sig_number_to_bcd(const char *text, size_t len, uint8_t bcd[LC_SIG_NUMBER_LEN])
{
    if (len > 0 && text[0] == '+') {
        text++;
        len--;
    }
    if (!digits_ok(text, len)) return -1;
    encode(text, len, bcd);
    return 0;
}

int lc_sig_number_valid(const uint8_t bcd[LC_SIG_NUMBER_LEN])
{
    char d[LC_SIG_NUMBER_DIGITS + 1];
    size_t n = decode(bcd, d);
    return n > 0 && digits_ok(d, n);
}

int lc_sig_number_normalize(const char *dialed, size_t len, const uint8_t *home, uint8_t bcd[LC_SIG_NUMBER_LEN])
{
    char d[LC_SIG_NUMBER_DIGITS + 4]; /* room for "00" and one digit too many */
    size_t n = 0;
    int plus = 0;
    for (size_t i = 0; i < len; i++) { /* 1. strip separators; '+' only first */
        char c = dialed[i];
        if (c == ' ' || c == '-' || c == '.' || c == '(' || c == ')') continue;
        if (c == '+' && n == 0 && !plus) {
            plus = 1;
        } else if (c >= '0' && c <= '9' && n < sizeof(d) - 1) {
            d[n++] = c;
        } else {
            return -1; /* a letter, a second '+', a '+' after a digit, or too many digits */
        }
    }
    char full[LC_SIG_NUMBER_DIGITS + 4];
    size_t fn;
    const char *src = d;
    if (!plus && n >= 2 && d[0] == '0' && d[1] == '0') { /* 2. "00" international prefix */
        plus = 1;
        src += 2;
        n -= 2;
    }
    if (plus || (n >= 12 && memcmp(d, "883", 3) == 0)) { /* 2-3. international */
        if (n > LC_SIG_NUMBER_DIGITS) return -1;
        memcpy(full, src, n);
        fn = n;
    } else { /* 4. in-country, by the caller's country */
        char h[LC_SIG_NUMBER_DIGITS + 1];
        if (home == NULL || decode(home, h) == 0 || !digits_ok(h, strlen(h)) || h[3] != '1') return -1;
        if ((n == 11 || n == 12) && d[0] == '1') { /* the country code without 883 */
            src++;
            n--;
        }
        if (n != 10 && n != 11) return -1;
        memcpy(full, "8831", 4);
        memcpy(full + 4, src, n);
        fn = 4 + n;
    }
    if (fn == 14 && memcmp(full, "8831", 4) == 0) { /* 5. the subscriber's leading 0 */
        memmove(full + 11, full + 10, 4);
        full[10] = '0';
        fn = 15;
    }
    if (!digits_ok(full, fn)) return -1; /* 6. */
    encode(full, fn, bcd);
    return 0;
}

void lc_sig_number_to_text(const uint8_t bcd[LC_SIG_NUMBER_LEN], char text[LC_SIG_NUMBER_TEXT])
{
    size_t n = 0;
    text[n++] = '+';
    for (size_t i = 0; i < LC_SIG_NUMBER_DIGITS; i++) {
        uint8_t v = (uint8_t)(i % 2 == 0 ? bcd[i / 2] >> 4 : bcd[i / 2] & 0x0F);
        if (v > 9) break;
        text[n++] = (char)('0' + v);
    }
    text[n] = '\0';
}

size_t lc_sig_number_format(const uint8_t bcd[LC_SIG_NUMBER_LEN], char *out, size_t cap)
{
    char d[LC_SIG_NUMBER_DIGITS + 1], s[LC_SIG_NUMBER_SHOW];
    size_t n = decode(bcd, d), o = 0;
    if (cap > 0) out[0] = '\0';
    if (n == 0 || !digits_ok(d, n)) return 0;
    size_t cc = cc_len(d + 3);
    memcpy(s, "+883-", 5);
    o = 5;
    memcpy(s + o, d + 3, cc);
    o += cc;
    s[o++] = '-';
    if (d[3] == '1') { /* NPA-NXX-subscriber */
        memcpy(s + o, d + 4, 3);
        s[o + 3] = '-';
        memcpy(s + o + 4, d + 7, 3);
        s[o + 7] = '-';
        memcpy(s + o + 8, d + 10, 5);
        o += 13;
    } else {
        memcpy(s + o, d + 3 + cc, n - 3 - cc);
        o += n - 3 - cc;
    }
    s[o] = '\0';
    if (o + 1 > cap) {
        return 0;
    }
    memcpy(out, s, o + 1);
    return o;
}
