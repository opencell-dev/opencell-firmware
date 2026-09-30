#include "oc_lr_rx.h"

uint32_t oc_lr_irq_of(const uint8_t status[6])
{
    return ((uint32_t)status[2] << 24) | ((uint32_t)status[3] << 16) | ((uint32_t)status[4] << 8) | status[5];
}

uint8_t oc_lr_event(uint32_t irq)
{
    if (irq & OC_LR_IRQ_RX_DONE) return OC_RADIO_EV_RX_DONE;
    if (irq & OC_LR_IRQ_TX_DONE) return OC_RADIO_EV_TX_DONE;
    if (irq & OC_LR_IRQ_TIMEOUT) return OC_RADIO_EV_RX_TIMEOUT;
    if (irq & (OC_LR_IRQ_ERROR | OC_LR_IRQ_CMD_ERROR)) return OC_RADIO_EV_ERROR;
    return OC_RADIO_EV_NONE;
}

uint16_t oc_lr_rx_len(const uint8_t resp[2])
{
    uint16_t len = (uint16_t)(((uint16_t)resp[0] << 8) | resp[1]);
    uint16_t cap = (uint16_t)sizeof(((oc_radio_event_t *)0)->data);
    return len > cap ? cap : len;
}

/* RadioLib: raw / -2.0f dBm, truncated toward zero by the int16 cast. */
static int16_t half_db_neg(uint16_t raw)
{
    return (int16_t)-(int16_t)(raw / 2u);
}

void oc_lr_rx_quality(int flrc, int lora_explicit, uint32_t irq, const uint8_t *pstat, oc_radio_event_t *ev)
{
    (void)lora_explicit;
    if (flrc) {
        ev->rssi_dbm = half_db_neg((uint16_t)(((uint16_t)pstat[2] << 1) | ((pstat[4] & 0x04u) >> 2)));
        ev->snr_qdb = 0;
        ev->crc_ok = (irq & OC_LR_IRQ_CRC_ERROR) == 0;
        return;
    }
    ev->rssi_dbm = half_db_neg((uint16_t)(((uint16_t)pstat[3] << 1) | ((pstat[5] & 0x02u) >> 1)));
    ev->snr_qdb = (int8_t)pstat[2];
    ev->crc_ok = (irq & OC_LR_IRQ_CRC_ERROR) == 0 && (irq & OC_LR_IRQ_LORA_HEADER_VALID) != 0;
}
