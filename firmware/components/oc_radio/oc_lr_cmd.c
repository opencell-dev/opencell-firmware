#include "oc_lr_cmd.h"

#include <string.h>

#include "oc_rxt.h"

#define OC_LR_NOP 0x00u

static uint8_t s_out[2 + 255];
static uint8_t s_in[2 + 255];

int oc_lr_freq_ok(uint32_t hz)
{
    return (hz >= 150000000u && hz <= 1090000000u) || (hz >= 1900000000u && hz <= 2200000000u) ||
           (hz >= 2400000000u && hz <= 2500000000u);
}

/* A transaction, then Stat1 as RadioLib's SPItransferStream checks it. */
static int16_t xfer_stat(const oc_lr_bus_t *b, uint8_t *out, uint8_t *in, size_t len)
{
    int16_t st = b->xfer(b->ctx, out, in, len);
    if (st == 0 && b->stat != NULL) {
        st = b->stat(in[0]);
    }
    return st;
}

int16_t oc_lr_write(const oc_lr_bus_t *b, uint16_t op, const uint8_t *args, size_t n)
{
    if (n > 255u) {
        return OC_LR_ERR_SPI_CMD_INVALID;
    }
    s_out[0] = (uint8_t)(op >> 8);
    s_out[1] = (uint8_t)op;
    if (n > 0) {
        memcpy(&s_out[2], args, n);
    }
    return xfer_stat(b, s_out, s_in, 2u + n);
}

int16_t oc_lr_read(const oc_lr_bus_t *b, uint16_t op, uint8_t *resp, size_t n)
{
    uint8_t out[2 + 8] = { (uint8_t)(op >> 8), (uint8_t)op };
    uint8_t in[2 + 8] = { 0 };
    memset(resp, 0, n);
    if (n > 8u) {
        return OC_LR_ERR_SPI_CMD_INVALID;
    }
    int16_t st = xfer_stat(b, out, in, 2);
    if (st != 0) {
        return st;
    }
    memset(out, OC_LR_NOP, sizeof(out));
    memset(in, 0, sizeof(in));
    int16_t xs = b->xfer(b->ctx, out, in, 2u + n);
    if (xs != 0) {
        return xs; /* resp stays zero */
    }
    memcpy(resp, &in[2], n);
    return b->stat != NULL ? b->stat(in[0]) : 0;
}

int16_t oc_lr_get_irq(const oc_lr_bus_t *b, uint32_t *irq)
{
    uint8_t out[6] = { 0 }, in[6] = { 0 };
    int16_t st = b->xfer(b->ctx, out, in, sizeof(out));
    *irq = st == 0 ? oc_lr_irq_of(in) : 0;
    return st;
}

int16_t oc_lr_clear_irq(const oc_lr_bus_t *b)
{
    static const uint8_t all[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    return oc_lr_write(b, OC_LR_OP_CLEAR_IRQ, all, sizeof(all));
}

int16_t oc_lr_clear_rx_fifo(const oc_lr_bus_t *b)
{
    return oc_lr_write(b, OC_LR_OP_CLEAR_RX_FIFO, NULL, 0);
}

int16_t oc_lr_clear_tx_fifo(const oc_lr_bus_t *b)
{
    return oc_lr_write(b, OC_LR_OP_CLEAR_TX_FIFO, NULL, 0);
}

int16_t oc_lr_set_fs(const oc_lr_bus_t *b)
{
    return oc_lr_write(b, OC_LR_OP_SET_FS, NULL, 0);
}

int16_t oc_lr_write_tx_fifo(const oc_lr_bus_t *b, const uint8_t *data, uint8_t len)
{
    return oc_lr_write(b, OC_LR_OP_WRITE_TX_FIFO, data, len);
}

int16_t oc_lr_set_rf_frequency(const oc_lr_bus_t *b, uint32_t hz)
{
    if (!oc_lr_freq_ok(hz)) {
        return OC_LR_ERR_INVALID_FREQUENCY;
    }
    const uint8_t f[4] = { (uint8_t)(hz >> 24), (uint8_t)(hz >> 16), (uint8_t)(hz >> 8), (uint8_t)hz };
    return oc_lr_write(b, OC_LR_OP_SET_RF_FREQUENCY, f, sizeof(f));
}

int16_t oc_lr_read_rx(const oc_lr_bus_t *b, int flrc, int lora_explicit, uint32_t irq, oc_radio_event_t *ev)
{
    uint8_t lenb[2], ps[OC_LR_LORA_STATUS_LEN] = { 0 };
    int16_t st = oc_lr_read(b, OC_LR_OP_GET_RX_PKT_LENGTH, lenb, sizeof(lenb));
    uint16_t len = st == 0 ? oc_lr_rx_len(lenb) : 0;
    OC_RXT_MARK(OC_RXT_LEN);
    if (len > 0) {
        /* ReadRxFifo: one transaction, the opcode then NOPs, data from byte 2 */
        s_out[0] = (uint8_t)(OC_LR_OP_READ_RX_FIFO >> 8);
        s_out[1] = (uint8_t)OC_LR_OP_READ_RX_FIFO;
        memset(&s_out[2], OC_LR_NOP, len);
        memset(s_in, 0, 2u + len);
        int16_t fs = xfer_stat(b, s_out, s_in, 2u + len);
        memcpy(ev->data, &s_in[2], len);
        if (st == 0) st = fs;
    }
    OC_RXT_MARK(OC_RXT_DATA);
    int16_t ps_st = oc_lr_read(b, flrc ? OC_LR_OP_GET_FLRC_PACKET_STATUS : OC_LR_OP_GET_LORA_PACKET_STATUS, ps,
                               flrc ? OC_LR_FLRC_STATUS_LEN : OC_LR_LORA_STATUS_LEN);
    if (st == 0) st = ps_st;
    ev->len = (uint8_t)len;
    oc_lr_rx_quality(flrc, lora_explicit, irq, ps, ev);
    if (st != 0) ev->crc_ok = 0;
    OC_RXT_MARK(OC_RXT_PSTAT);
    return st;
}
