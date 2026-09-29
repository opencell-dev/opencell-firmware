#include "oc_sig_milenage.h"

#include <string.h>

#include "oc_sig_crypto.h"

static void xor16(uint8_t *out, const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(a[i] ^ b[i]);
}

/* Cyclic left rotation by r bits (r a multiple of 8). */
static void rot(uint8_t *out, const uint8_t *in, int r)
{
    for (int i = 0; i < 16; i++) out[i] = in[(i + r / 8) % 16];
}

int oc_milenage_opc(const uint8_t k[16], const uint8_t op[16], uint8_t opc[16])
{
    uint8_t e[16];
    if (oc_sig_aes128_block(k, op, e) != 0) return -1;
    xor16(opc, e, op);
    return 0;
}

int oc_milenage(const uint8_t k[16], const uint8_t opc[16], const uint8_t rand[16], const uint8_t sqn[6],
                const uint8_t amf[2], oc_milenage_t *o)
{
    static const int r[5] = { 64, 0, 32, 64, 96 };
    uint8_t temp[16], x[16], y[16], out[16], in1[16];
    xor16(x, rand, opc);
    if (oc_sig_aes128_block(k, x, temp) != 0) return -1;

    /* out1 = E_K(TEMP xor rot(IN1 xor OPc, r1) xor c1) xor OPc, c1 = 0 */
    memcpy(in1, sqn, 6);
    memcpy(in1 + 6, amf, 2);
    memcpy(in1 + 8, sqn, 6);
    memcpy(in1 + 14, amf, 2);
    xor16(x, in1, opc);
    rot(y, x, r[0]);
    xor16(x, y, temp);
    if (oc_sig_aes128_block(k, x, out) != 0) return -1;
    xor16(out, out, opc);
    memcpy(o->mac_a, out, 8);
    memcpy(o->mac_s, out + 8, 8);

    /* out2..out5 = E_K(rot(TEMP xor OPc, ri) xor ci) xor OPc, ci = 1, 2, 4, 8 in the last byte */
    for (int i = 1; i <= 4; i++) {
        xor16(x, temp, opc);
        rot(y, x, r[i]);
        y[15] ^= (uint8_t)(1u << (i - 1));
        if (oc_sig_aes128_block(k, y, out) != 0) return -1;
        xor16(out, out, opc);
        if (i == 1) {
            memcpy(o->ak, out, 6);
            memcpy(o->res, out + 8, 8);
        } else if (i == 2) {
            memcpy(o->ck, out, 16);
        } else if (i == 3) {
            memcpy(o->ik, out, 16);
        } else {
            memcpy(o->ak_s, out, 6);
        }
    }
    return 0;
}
