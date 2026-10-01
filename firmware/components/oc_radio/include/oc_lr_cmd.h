/* oc_lr_cmd — the lean LR2021 commands oc_radio sends in the time-critical
 * path (an RX readout, then configuring and staging a back-to-back slot),
 * framed as the LR20xx datasheet Rev 2.2 and RadioLib 7.7.1 (with
 * RADIOLIB_SPI_PARANOID=0) put them on the wire. Pure C over a transaction
 * callback, so the byte framing is host-tested; oc_radio supplies the SPI,
 * NSS and BUSY handling. Not reentrant (static buffers): one task. */
#ifndef OC_LR_CMD_H
#define OC_LR_CMD_H

#include <stddef.h>
#include <stdint.h>

#include "oc_lr_rx.h"
#include "oc_radio_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes (RadioLib's RADIOLIB_LR2021_CMD_*; oc_radio asserts they match) */
#define OC_LR_OP_READ_RX_FIFO             0x0001u
#define OC_LR_OP_WRITE_TX_FIFO            0x0002u
#define OC_LR_OP_CLEAR_IRQ                0x0116u
#define OC_LR_OP_CLEAR_RX_FIFO            0x011Eu
#define OC_LR_OP_CLEAR_TX_FIFO            0x011Fu
#define OC_LR_OP_SET_FS                   0x0129u
#define OC_LR_OP_SET_RF_FREQUENCY         0x0200u
#define OC_LR_OP_GET_RX_PKT_LENGTH        0x0212u
#define OC_LR_OP_GET_LORA_PACKET_STATUS   0x022Au
#define OC_LR_OP_GET_FLRC_PACKET_STATUS   0x024Bu

/* Error codes: RadioLib's values (oc_radio asserts they match). */
#define OC_LR_ERR_INVALID_FREQUENCY (-12)
#define OC_LR_ERR_SPI_CMD_TIMEOUT   (-705)
#define OC_LR_ERR_SPI_CMD_INVALID   (-706)

typedef struct {
    void *ctx;
    /* One transaction: BUSY low, NSS low, clock len bytes out (and in),
     * NSS high, BUSY low again once the command has run. 0, or an error
     * (BUSY timeout) with in[] left as it was. */
    int16_t (*xfer)(void *ctx, uint8_t *out, uint8_t *in, size_t len);
    /* Stat1 (the previous command's result) as 0 or an error; NULL: none. */
    int16_t (*stat)(uint8_t stat1);
} oc_lr_bus_t;

/* 1 if the chip can tune to hz (RadioLib's ranges: 150-1090, 1900-2200,
 * 2400-2500 MHz). */
int oc_lr_freq_ok(uint32_t hz);

/* A write command, one transaction: opcode, then n (<= 255) argument bytes.
 * Returns the transfer's error, else Stat1's. */
int16_t oc_lr_write(const oc_lr_bus_t *b, uint16_t op, const uint8_t *args, size_t n);

/* A read command: the opcode, then a transaction of 2 + n NOPs whose bytes
 * after Stat1/Stat2 are the response (n <= 8). resp is all zeros unless
 * both transactions went through. */
int16_t oc_lr_read(const oc_lr_bus_t *b, uint16_t op, uint8_t *resp, size_t n);

/* The IRQ flags: 6 NOPs clock out Stat1, Stat2, IrqStatus. Stat1 is ignored
 * (as RadioLib's getIrqStatus; a failed command raises CMD_ERROR). */
int16_t oc_lr_get_irq(const oc_lr_bus_t *b, uint32_t *irq);

int16_t oc_lr_clear_irq(const oc_lr_bus_t *b); /* all flags */
int16_t oc_lr_clear_rx_fifo(const oc_lr_bus_t *b);
int16_t oc_lr_clear_tx_fifo(const oc_lr_bus_t *b);
int16_t oc_lr_set_fs(const oc_lr_bus_t *b);
int16_t oc_lr_write_tx_fifo(const oc_lr_bus_t *b, const uint8_t *data, uint8_t len);
/* SetRfFrequency with the exact Hz; OC_LR_ERR_INVALID_FREQUENCY (nothing
 * sent) outside oc_lr_freq_ok. */
int16_t oc_lr_set_rf_frequency(const oc_lr_bus_t *b, uint32_t hz);

/* After RX done with these IRQ flags: the packet (GetRxPktLength, then
 * ReadRxFifo in one transaction) and its status (Get{Lora,Flrc}PacketStatus)
 * into ev: len, data, crc_ok, rssi_dbm, snr_qdb. Any failure clears crc_ok.
 * The RX FIFO is left for the next RX stage to clear. */
int16_t oc_lr_read_rx(const oc_lr_bus_t *b, int flrc, int lora_explicit, uint32_t irq, oc_radio_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif
