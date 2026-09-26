#include "lc_radio.h"

#include <string.h>

#include <RadioLib.h>

#include "driver/gpio.h"
#include "w12_board.h"

namespace {

EspHal *s_hal;
LR2021 *s_radio;
int s_band = -1;       /* lc_band_t currently tuned, -1 = none */
lc_mode_t s_mode;      /* modulation currently loaded */
bool s_mode_valid;

/* RF switch on the LR2021's own DIOs: DIOx(n) is DIO(5+n). */
const uint32_t k_rfsw_pins[Module::RFSWITCH_MAX_PINS] = {
    RADIOLIB_LRXXXX_DIOx(0), /* DIO5  RFX2402E TX_EN */
    RADIOLIB_LRXXXX_DIOx(1), /* DIO6  RFX2402E RX_EN */
    RADIOLIB_LRXXXX_DIOx(4), /* DIO9  GC1109 CTX */
    RADIOLIB_LRXXXX_DIOx(5), /* DIO10 GC1109 CPS */
    RADIOLIB_LRXXXX_DIOx(6), /* DIO11 GC1109 CSD */
};
const Module::RfSwitchMode_t k_rfsw_table[] = {
    { LR2021::MODE_STBY,  { 0, 0, 0, 0, 0 } },
    { LR2021::MODE_RX,    { 0, 0, 0, 0, 1 } },
    { LR2021::MODE_TX,    { 0, 0, 1, 1, 1 } },
    { LR2021::MODE_RX_HF, { 0, 1, 0, 0, 0 } },
    { LR2021::MODE_TX_HF, { 1, 0, 0, 0, 0 } },
    END_OF_MODE_TABLE,
};

const uint8_t k_flrc_sync[4] = { 0x2D, 0x01, 0x4B, 0x1D };

lc_band_t band_of(uint32_t freq_hz)
{
    return freq_hz >= 1500000000u ? LC_BAND_2G4 : LC_BAND_915;
}

int8_t chip_dbm(lc_band_t band)
{
    return band == LC_BAND_2G4 ? W12_HF_CHIP_DBM : W12_LF_CHIP_DBM;
}

int16_t apply_lora(const lc_mode_t *m)
{
    int16_t st = s_radio->setPacketType(RADIOLIB_LR2021_PACKET_TYPE_LORA);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setSpreadingFactor(m->sf);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setBandwidth((float)m->bw_hz / 1000.0f);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setCodingRate((uint8_t)(m->cr + 4)); /* 4/5..4/8 */
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setPreambleLength(m->preamble);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setSyncWord(RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->explicitHeader();
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setCRC(2);
    return st;
}

int16_t apply_flrc(const lc_mode_t *m)
{
    int16_t st = s_radio->setPacketType(RADIOLIB_LR2021_PACKET_TYPE_FLRC);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setBitRate((float)m->bitrate_bps / 1000.0f);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setCodingRate(m->cr); /* LC_FLRC_CR_* == RADIOLIB codes */
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setPreambleLength(m->preamble);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setDataShaping(RADIOLIB_SHAPING_0_5);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setSyncWord(const_cast<uint8_t *>(k_flrc_sync), 4);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->variablePacketLengthMode(RADIOLIB_LR2021_MAX_PACKET_LENGTH);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setCRC(2);
    return st;
}

int op_configure(void *ctx, uint32_t freq_hz, const lc_mode_t *mode)
{
    (void)ctx;
    lc_band_t band = band_of(freq_hz);
    float mhz = (float)freq_hz / 1e6f;
    /* Never recalibrate per hop (milliseconds): the front end is calibrated
     * once in lc_radio_init. A band change also switches the PA table. */
    int16_t st = s_radio->setFrequency(mhz, true);
    if (st != RADIOLIB_ERR_NONE) return st;
    if ((int)band != s_band) {
        st = s_radio->setOutputPower(chip_dbm(band));
        if (st != RADIOLIB_ERR_NONE) return st;
        s_band = band;
    }
    if (!s_mode_valid || memcmp(mode, &s_mode, sizeof(s_mode)) != 0) {
        st = mode->modulation == LC_MOD_FLRC ? apply_flrc(mode) : apply_lora(mode);
        if (st != RADIOLIB_ERR_NONE) {
            s_mode_valid = false;
            return st;
        }
        s_mode = *mode;
        s_mode_valid = true;
    }
    return 0;
}

int op_stage_tx(void *ctx, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    RadioModeConfig_t cfg = {};
    cfg.transmit.data = data;
    cfg.transmit.len = len;
    return s_radio->stageMode(RADIOLIB_RADIO_MODE_TX, &cfg);
}

int op_stage_rx(void *ctx, uint32_t timeout_us)
{
    (void)ctx;
    RadioModeConfig_t cfg = {};
    cfg.receive.timeout = (uint32_t)s_radio->calculateRxTimeout(timeout_us);
    cfg.receive.irqFlags = RADIOLIB_IRQ_RX_DEFAULT_FLAGS;
    cfg.receive.irqMask = RADIOLIB_IRQ_RX_DEFAULT_MASK;
    return s_radio->stageMode(RADIOLIB_RADIO_MODE_RX, &cfg);
}

/* RadioLib's launchMode() spins on BUSY after SetTx with no timeout; a stuck
 * radio would hang the exec task. Same steps, bounded wait (needs GODMODE). */
constexpr uint32_t k_tx_busy_max_us = 2000;

int op_launch(void *ctx)
{
    (void)ctx;
    if (s_radio->stagedMode != RADIOLIB_RADIO_MODE_TX) {
        return s_radio->launchMode(); /* RX: no unbounded wait */
    }
    s_radio->getMod()->setRfSwitchState(Module::MODE_TX);
    int16_t st = s_radio->setTx(RADIOLIB_LR2021_TX_TIMEOUT_NONE);
    s_radio->stagedMode = RADIOLIB_RADIO_MODE_NONE;
    if (st != RADIOLIB_ERR_NONE) {
        return st;
    }
    RadioLibTime_t t0 = s_hal->micros();
    while (s_hal->digitalRead(W12_PIN_LORA_BUSY)) {
        if (s_hal->micros() - t0 > k_tx_busy_max_us) {
            return RADIOLIB_ERR_SPI_CMD_TIMEOUT;
        }
    }
    return RADIOLIB_ERR_NONE;
}

int op_poll(void *ctx, lc_radio_event_t *ev)
{
    (void)ctx;
    uint32_t irq = s_radio->getIrqFlags();
    if (irq == 0) {
        return 0;
    }
    memset(ev, 0, sizeof(*ev));
    if (irq & RADIOLIB_LR2021_IRQ_RX_DONE) {
        size_t len = s_radio->getPacketLength();
        if (len > sizeof(ev->data)) len = sizeof(ev->data);
        int16_t st = s_radio->readData(ev->data, len);
        ev->type = LC_RADIO_EV_RX_DONE;
        ev->len = (uint8_t)len;
        ev->crc_ok = st == RADIOLIB_ERR_NONE;
        ev->rssi_dbm = (int16_t)s_radio->getRSSI();
        ev->snr_qdb = (int16_t)(s_radio->getSNR() * 4.0f); /* 0 for FLRC */
    } else if (irq & RADIOLIB_LR2021_IRQ_TX_DONE) {
        ev->type = LC_RADIO_EV_TX_DONE;
    } else if (irq & RADIOLIB_LR2021_IRQ_TIMEOUT) {
        ev->type = LC_RADIO_EV_RX_TIMEOUT;
    } else if (irq & (RADIOLIB_LR2021_IRQ_ERROR | RADIOLIB_LR2021_IRQ_CMD_ERROR)) {
        ev->type = LC_RADIO_EV_ERROR;
    } else {
        /* e.g. preamble detected / LoRa header valid: keep waiting, and leave the
         * flags set - readData() needs HEADER_VALID to accept a LoRa packet. */
        return 0;
    }
    s_radio->clearIrqFlags(RADIOLIB_LR2021_IRQ_ALL);
    return 1;
}

void op_standby(void *ctx)
{
    (void)ctx;
    s_radio->standby();
}

const lc_radio_ops_t k_ops = { nullptr, op_configure, op_stage_tx, op_stage_rx, op_launch, op_poll, op_standby };

/* Front-end calibration points (MHz): both band edges and the centre. */
int16_t calibrate(lc_band_t band)
{
    const uint16_t path = band == LC_BAND_2G4 ? RADIOLIB_LR2021_CALIBRATE_FE_HF_PATH
                                              : RADIOLIB_LR2021_CALIBRATE_FE_LF_PATH;
    const uint16_t mhz[2][3] = { { 904, 915, 926 }, { 2404, 2440, 2476 } };
    uint16_t f[3];
    for (int i = 0; i < 3; i++) {
        f[i] = (uint16_t)((mhz[band][i] / 4) | path); /* units of 4 MHz */
    }
    return s_radio->calibrateFrontEnd(f);
}

} // namespace

extern "C" int lc_radio_init(lc_band_t band)
{
    if (s_radio == nullptr) {
        /* 8 MHz (default 2 MHz): FIFO writes and the per-slot mode switch must fit
         * LC_EXEC_CONFIG_LEAD_US. Bench Task 12 can raise it toward the LR2021 max. */
        s_hal = new EspHal(W12_PIN_LORA_SCK, W12_PIN_LORA_MISO, W12_PIN_LORA_MOSI, SPI2_HOST, 8000000);
        s_radio = new LR2021(new Module(s_hal, W12_PIN_LORA_NSS, W12_PIN_LORA_IRQ, W12_PIN_LORA_RST,
                                        W12_PIN_LORA_BUSY));
        s_radio->irqDioNum = W12_LORA_IRQ_DIO;
    }
    const lc_mode_t *edge = lc_tier_mode(LC_BAND_915, LC_TIER_EDGE);
    float mhz = band == LC_BAND_2G4 ? 2440.0f : 915.0f;
    /* No TCXO on the W12: tcxoVoltage 0 selects the crystal. */
    int16_t st = s_radio->begin(mhz, (float)edge->bw_hz / 1000.0f, edge->sf, (uint8_t)(edge->cr + 4),
                                RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, chip_dbm(band), edge->preamble, 0.0f);
    if (st == RADIOLIB_ERR_NONE) {
        /* On the LR2021 setRfSwitchTable sends SetDioFunction commands at once, so it
         * must follow begin(), which resets the chip (and clears DIO config). */
        s_radio->setRfSwitchTable(k_rfsw_pins, k_rfsw_table);
        st = calibrate(band);
    }
    if (st != RADIOLIB_ERR_NONE) return st;
    s_band = band;
    s_mode = *edge;
    s_mode_valid = band == LC_BAND_915;
    /* Init (reset, calibration) is done: per-slot commands finish in well under a
     * millisecond, so don't let a stuck BUSY block the exec task for 1 s each. */
    s_radio->getMod()->spiConfig.timeout = 20;
    return 0;
}

extern "C" int lc_radio_init_terminal(void)
{
    int st = lc_radio_init(LC_BAND_915);
    if (st != 0) {
        return st;
    }
    /* Each point carries its own path flag, so one call covers both bands. */
    const uint16_t f[3] = {
        (uint16_t)((904 / 4) | RADIOLIB_LR2021_CALIBRATE_FE_LF_PATH),
        (uint16_t)((924 / 4) | RADIOLIB_LR2021_CALIBRATE_FE_LF_PATH),
        (uint16_t)((2440 / 4) | RADIOLIB_LR2021_CALIBRATE_FE_HF_PATH),
    };
    /* lc_radio_init lowered the per-command BUSY timeout for slot-time commands;
     * a 3-point front-end calibration takes longer than that (-705 otherwise). */
    s_radio->getMod()->spiConfig.timeout = 1000;
    st = s_radio->calibrateFrontEnd(f);
    s_radio->getMod()->spiConfig.timeout = 20;
    return st;
}


extern "C" const lc_radio_ops_t *lc_radio_ops(void)
{
    return &k_ops;
}
