/* oc_lr_rx — decoding of the LR2021's RX readout for oc_radio's fast path.
 * Pure C (host-tested): the same results as the RadioLib 7.7.1 calls it
 * replaces (getIrqFlags, readData, getRSSI, getSNR). */
#ifndef OC_LR_RX_H
#define OC_LR_RX_H

#include <stdint.h>

#include "oc_radio_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LR2021 IRQ flags (datasheet; RadioLib's RADIOLIB_LR2021_IRQ_*) */
#define OC_LR_IRQ_LORA_HEADER_VALID (1UL << 6)
#define OC_LR_IRQ_ERROR             (1UL << 16)
#define OC_LR_IRQ_CMD_ERROR         (1UL << 17)
#define OC_LR_IRQ_RX_DONE           (1UL << 18)
#define OC_LR_IRQ_TX_DONE           (1UL << 19)
#define OC_LR_IRQ_TIMEOUT           (1UL << 21)
#define OC_LR_IRQ_CRC_ERROR         (1UL << 22)

#define OC_LR_LORA_STATUS_LEN 6 /* GetLoRaPacketStatus response */
#define OC_LR_FLRC_STATUS_LEN 5 /* GetFlrcPacketStatus response */

/* Every SPI transaction clocks out Stat1, Stat2 and IrqStatus[31:0] first:
 * the IRQ flags from those 6 bytes. */
uint32_t oc_lr_irq_of(const uint8_t status[6]);

/* The event that ends an operation with these flags, or OC_RADIO_EV_NONE
 * while it runs (preamble/header flags only). RX done first, then TX done,
 * timeout, error. */
uint8_t oc_lr_event(uint32_t irq);

/* GetRxPktLength response (big-endian), capped to oc_radio_event_t.data. */
uint16_t oc_lr_rx_len(const uint8_t resp[2]);

/* crc_ok, rssi_dbm and snr_qdb of a received packet from its IRQ flags and
 * packet status response (LoRa: 6 bytes, packet RSSI and SNR, an explicit
 * header must have been valid; FLRC: 5 bytes, average RSSI, SNR 0). */
void oc_lr_rx_quality(int flrc, int lora_explicit, uint32_t irq, const uint8_t *pstat, oc_radio_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif
