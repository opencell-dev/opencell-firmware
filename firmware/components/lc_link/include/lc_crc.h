#ifndef LC_CRC_H
#define LC_CRC_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout. */
uint16_t lc_crc16(const uint8_t *data, size_t len);

#endif
