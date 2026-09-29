/* MILENAGE (3GPP TS 35.206) on AES-128. */
#ifndef OC_SIG_MILENAGE_H
#define OC_SIG_MILENAGE_H

#include <stdint.h>

typedef struct {
    uint8_t mac_a[8]; /* f1 */
    uint8_t mac_s[8]; /* f1* (resynchronization) */
    uint8_t res[8];   /* f2 */
    uint8_t ck[16];   /* f3 */
    uint8_t ik[16];   /* f4 */
    uint8_t ak[6];    /* f5 */
    uint8_t ak_s[6];  /* f5* (resynchronization) */
} oc_milenage_t;

/* OPc = E_K(OP) xor OP */
int oc_milenage_opc(const uint8_t k[16], const uint8_t op[16], uint8_t opc[16]);

/* All functions for one RAND/SQN/AMF. 0 on success. */
int oc_milenage(const uint8_t k[16], const uint8_t opc[16], const uint8_t rand[16], const uint8_t sqn[6],
                const uint8_t amf[2], oc_milenage_t *o);

#endif
