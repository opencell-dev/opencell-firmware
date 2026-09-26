#include "lc_radio.h"

#include "esp_log.h"
#include "third_party/lr20xx_pram_lr2021.h" /* Semtech LR2021 firmware patch (Clear BSD) */

#include <string.h>

#include <RadioLib.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "w12_board.h"

namespace {

/* Exposes RadioLib state the staging fast path needs; LRxxxx keeps it
 * protected (GODMODE only opens LR2021's own members). */
class LR2021Fast : public LR2021 {
public:
    using LR2021::LR2021;

    /* LR20xx datasheet 22.3: load the firmware patch (PRAM) after every reset;
     * running without it "can create performance issues and unexpected bugs"
     * (bench: 2.4 GHz FLRC below 1.3 Mb/s never passed CRC). RadioLib never
     * loads it. Its setup does findChip() (reset) then standby() before any
     * configuration, so the first standby() after a reset loads it - the same
     * order as Semtech's driver. */
    bool pram_pending = false;
    int16_t pram_status = RADIOLIB_ERR_NONE;
    bool pram_loaded = false;
    uint16_t pram_version = 0;

    int16_t standby() override
    {
        if (pram_pending) {
            pram_pending = false;
            pram_status = loadPram();
        }
        return LR2021::standby();
    }

    int16_t loadPram()
    {
        const uint32_t base = 0x801000;
        for (uint32_t i = 0; i < pram_lr2021_size; i += 32) {
            uint32_t n = pram_lr2021_size - i < 32 ? pram_lr2021_size - i : 32;
            int16_t st = writeRegMem32(base + i * 4u, &pram_lr2021[i], n);
            if (st != RADIOLIB_ERR_NONE) return st;
        }
        int16_t st = activatePram();
        if (st == RADIOLIB_ERR_NONE) st = checkPramLoaded(&pram_loaded);
        if (st == RADIOLIB_ERR_NONE) st = getPramVersion(&pram_version);
        return st;
    }
    int16_t loraPacketParams(uint8_t len)
    {
        return setLoRaPacketParams(preambleLengthLoRa, headerType, len, crcTypeLoRa, invertIQEnabled);
    }
    int16_t flrcPacketParams(uint8_t len)
    {
        return setFlrcPacketParams(preambleLengthGFSK, syncWordLength, 1, 0x01,
                                   packetType == RADIOLIB_LR2021_GFSK_OOK_PACKET_FORMAT_FIXED, crcLenGFSK, len);
    }
    void setStagedRxTimeout(uint32_t t) { rxTimeout = t; }
    uint32_t stagedRxTimeout() const { return rxTimeout; }
    int16_t rxPathForBand()
    {
        return setRxPath(highFreq ? RADIOLIB_LR2021_RX_PATH_HF : RADIOLIB_LR2021_RX_PATH_LF,
                         highFreq ? gainModeHf : gainModeLf);
    }
};

EspHal *s_hal;
LR2021Fast *s_radio;
int s_band = -1;       /* lc_band_t currently tuned, -1 = none */
lc_mode_t s_mode;      /* modulation currently loaded */
bool s_mode_valid;

/* Staging fast path: what the chip already holds, so a slot only sends what
 * changed (RadioLib's stageMode re-reads the packet type and re-sends packet
 * params, RX path and IRQ config every time: ~1.3 ms per RX stage). */
uint8_t s_pkt_type = RADIOLIB_LR2021_PACKET_TYPE_NONE; /* set by apply_lora/apply_flrc */
int s_pp_len = -1;       /* payload length in the loaded packet params, -1 = unknown */
int s_irq_dir = -1;      /* 0 RX, 1 TX, -1 unknown: IRQ-to-DIO mapping loaded */
int s_rxpath_band = -1;  /* band whose RX path is loaded */

void invalidate_stage_cache()
{
    s_pp_len = -1;
    s_irq_dir = -1;
    s_rxpath_band = -1;
}

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
#if LC_BENCH_LOW_POWER
    /* BENCH ONLY: GC1109 CPS low = PA bypass (about -10 dBm at the 915 port). */
    { LR2021::MODE_TX,    { 0, 0, 1, 0, 1 } },
#else
    { LR2021::MODE_TX,    { 0, 0, 1, 1, 1 } },
#endif
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
#if LC_BENCH_LOW_POWER
    /* Minimum chip drive per band. 915: PA bypassed -> about -10 dBm out.
     * 2.4 GHz: the RFX2402E has no bypass (+22 dB) -> about +3 dBm out. */
    return band == LC_BAND_2G4 ? -19 : -9;
#else
    return band == LC_BAND_2G4 ? W12_HF_CHIP_DBM : W12_LF_CHIP_DBM;
#endif
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
        /* RadioLib's setters rewrite packet params with their own length. */
        invalidate_stage_cache();
        if (st != RADIOLIB_ERR_NONE) {
            s_mode_valid = false;
            s_pkt_type = RADIOLIB_LR2021_PACKET_TYPE_NONE;
            return st;
        }
        s_pkt_type = mode->modulation == LC_MOD_FLRC ? RADIOLIB_LR2021_PACKET_TYPE_FLRC : RADIOLIB_LR2021_PACKET_TYPE_LORA;
        s_mode = *mode;
        s_mode_valid = true;
    }
    return 0;
}

int16_t load_packet_params(int len)
{
    if (len == s_pp_len) {
        return RADIOLIB_ERR_NONE;
    }
    int16_t st;
    if (s_pkt_type == RADIOLIB_LR2021_PACKET_TYPE_LORA) {
        st = s_radio->loraPacketParams((uint8_t)len);
    } else if (s_pkt_type == RADIOLIB_LR2021_PACKET_TYPE_FLRC) {
        st = s_radio->flrcPacketParams((uint8_t)len);
    } else {
        return RADIOLIB_ERR_WRONG_MODEM;
    }
    s_pp_len = st == RADIOLIB_ERR_NONE ? len : -1;
    return st;
}

/* Same end state as RadioLib's stageMode(TX), sending only what changed. */
int op_stage_tx(void *ctx, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    int16_t st = load_packet_params(len);
    if (st == RADIOLIB_ERR_NONE && s_irq_dir != 1) {
        st = s_radio->setDioIrqConfig(s_radio->irqDioNum, RADIOLIB_LR2021_IRQ_TX_DONE | RADIOLIB_LR2021_IRQ_TIMEOUT);
        s_irq_dir = st == RADIOLIB_ERR_NONE ? 1 : -1;
    }
    /* Re-sending packet params used to reset the FIFOs as a side effect; with
     * them cached, stale bytes leaked into the next packet (bench: bad payloads). */
    if (st == RADIOLIB_ERR_NONE) st = s_radio->clearTxFifo();
    if (st == RADIOLIB_ERR_NONE) st = s_radio->writeRadioTxFifo(data, len);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->clearIrqState(RADIOLIB_LR2021_IRQ_ALL);
    /* Wait for the slot with the PLL locked (FS): SetTx from standby spent
     * ~280 us before the preamble (bench), from FS only the PA ramp remains. */
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setFs();
    if (st == RADIOLIB_ERR_NONE) s_radio->stagedMode = RADIOLIB_RADIO_MODE_TX;
    return st;
}

/* Same end state as RadioLib's stageMode(RX), sending only what changed. */
int op_stage_rx(void *ctx, uint32_t timeout_us)
{
    (void)ctx;
    int16_t st = RADIOLIB_ERR_NONE;
    if (s_rxpath_band != s_band) {
        st = s_radio->rxPathForBand();
        s_rxpath_band = st == RADIOLIB_ERR_NONE ? s_band : -1;
    }
    if (st == RADIOLIB_ERR_NONE && s_irq_dir != 0) {
        uint32_t flags = RADIOLIB_IRQ_RX_DEFAULT_FLAGS & (RADIOLIB_IRQ_RX_DEFAULT_MASK | (1UL << RADIOLIB_IRQ_TIMEOUT));
        st = s_radio->setDioIrqConfig(s_radio->irqDioNum, s_radio->getIrqMapped(flags));
        s_irq_dir = st == RADIOLIB_ERR_NONE ? 0 : -1;
    }
    if (st == RADIOLIB_ERR_NONE) st = s_radio->clearIrqState(RADIOLIB_LR2021_IRQ_ALL);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->clearRxFifo();
    if (st == RADIOLIB_ERR_NONE) st = load_packet_params(RADIOLIB_LR2021_MAX_PACKET_LENGTH);
    if (st == RADIOLIB_ERR_NONE) st = s_radio->setFs(); /* as for TX: launch from a locked PLL */
    if (st == RADIOLIB_ERR_NONE) {
        s_radio->setStagedRxTimeout((uint32_t)s_radio->calculateRxTimeout(timeout_us));
        s_radio->stagedMode = RADIOLIB_RADIO_MODE_RX;
    }
    return st;
}

/* BUSY bound for launch: a stuck radio must not hang the exec task. */
constexpr uint32_t k_busy_max_us = 2000;

/* Local time of the first IRQ edge since the last launch; 0 = none yet. */
volatile int64_t s_irq_us;
/* TX: when BUSY fell after SetTx - the datasheet's preamble start (5.3). */
int64_t s_tx_start_us;

void IRAM_ATTR irq_stamp_isr(void *arg)
{
    (void)arg;
    if (s_irq_us == 0) {
        s_irq_us = esp_timer_get_time();
    }
}

/* Spin until BUSY is low; its local time in *low_us. */
int16_t wait_busy_low(int64_t *low_us)
{
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        int64_t t = esp_timer_get_time();
        if (!s_hal->digitalRead(W12_PIN_LORA_BUSY)) {
            *low_us = t;
            return RADIOLIB_ERR_NONE;
        }
        if (t - t0 > (int64_t)k_busy_max_us) {
            return RADIOLIB_ERR_SPI_CMD_TIMEOUT;
        }
    }
}

/* SetTx/SetRx as one bare SPI transfer. RadioLib's setTx() allocates two
 * buffers and, with RADIOLIB_SPI_PARANOID, reads the status back in a second
 * transaction; the preamble timing then carries all of that. */
int16_t raw_set_mode(uint16_t cmd, uint32_t timeout, int64_t *ready_us)
{
    int64_t idle;
    int16_t st = wait_busy_low(&idle);
    if (st != RADIOLIB_ERR_NONE) {
        return st;
    }
    uint8_t out[5] = { (uint8_t)(cmd >> 8), (uint8_t)cmd, (uint8_t)(timeout >> 16), (uint8_t)(timeout >> 8),
                       (uint8_t)timeout };
    uint8_t in[5];
    s_hal->spiBeginTransaction();
    s_hal->digitalWrite(W12_PIN_LORA_NSS, 0);
    s_hal->spiTransfer(out, sizeof(out), in);
    s_hal->digitalWrite(W12_PIN_LORA_NSS, 1);
    s_hal->spiEndTransaction();
    /* BUSY rose with NSS; it falls when RX is ready or TX starts its preamble. */
    return wait_busy_low(ready_us);
}

int op_launch(void *ctx)
{
    (void)ctx;
    s_irq_us = 0;
    s_tx_start_us = 0;
    int tx = s_radio->stagedMode == RADIOLIB_RADIO_MODE_TX;
    if (!tx && s_radio->stagedMode != RADIOLIB_RADIO_MODE_RX) {
        return RADIOLIB_ERR_UNSUPPORTED;
    }
    s_radio->getMod()->setRfSwitchState(tx ? Module::MODE_TX : Module::MODE_RX);
    int64_t ready;
    int16_t st = tx ? raw_set_mode(RADIOLIB_LR2021_CMD_SET_TX, RADIOLIB_LR2021_TX_TIMEOUT_NONE, &ready)
                    : raw_set_mode(RADIOLIB_LR2021_CMD_SET_RX, s_radio->stagedRxTimeout(), &ready);
    s_radio->stagedMode = RADIOLIB_RADIO_MODE_NONE;
    if (st == RADIOLIB_ERR_NONE && tx) {
        s_tx_start_us = ready;
    }
    return st;
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
    ev->irq_us = (uint64_t)s_irq_us;
    ev->start_us = ev->type == LC_RADIO_EV_TX_DONE ? (uint64_t)s_tx_start_us : 0;
    return 1;
}

void op_standby(void *ctx)
{
    (void)ctx;
    s_radio->standby();
}

const lc_radio_ops_t k_ops = { nullptr, op_configure, op_stage_tx, op_stage_rx, op_launch, op_poll, op_standby };

/* Front-end calibration points (MHz): both band edges and the centre. */
/* Calibrate both front ends, whatever the configured band: receiving on a
 * band whose front end isn't calibrated fails with RXFREQ_NO_FE_CAL_ERR
 * (bench 2026-09-25: a 915 bs-radio could TX at 2.4 GHz but every RX errored).
 * Each point carries its own path flag, so one call covers both bands. */
int16_t calibrate()
{
    const uint16_t f[3] = {
        (uint16_t)((904 / 4) | RADIOLIB_LR2021_CALIBRATE_FE_LF_PATH), /* units of 4 MHz */
        (uint16_t)((924 / 4) | RADIOLIB_LR2021_CALIBRATE_FE_LF_PATH),
        (uint16_t)((2440 / 4) | RADIOLIB_LR2021_CALIBRATE_FE_HF_PATH),
    };
    return s_radio->calibrateFrontEnd(f);
}

} // namespace

extern "C" int lc_radio_init(lc_band_t band)
{
#if LC_BENCH_LOW_POWER
    ESP_LOGW("lc_radio", "*** BENCH LOW-POWER BUILD: 915 PA bypassed + chip -9 dBm; 2.4 GHz chip -19 dBm ***");
#endif
    if (s_radio == nullptr) {
        /* 16 MHz (default 2 MHz): FIFO writes and the per-slot mode switch must fit
         * LC_EXEC_CONFIG_LEAD_US. Bench Task 12 can raise it toward the LR2021 max. */
        s_hal = new EspHal(W12_PIN_LORA_SCK, W12_PIN_LORA_MISO, W12_PIN_LORA_MOSI, SPI2_HOST, 16000000);
        s_radio = new LR2021Fast(new Module(s_hal, W12_PIN_LORA_NSS, W12_PIN_LORA_IRQ, W12_PIN_LORA_RST,
                                        W12_PIN_LORA_BUSY));
        s_radio->irqDioNum = W12_LORA_IRQ_DIO;
    }
    const lc_mode_t *edge = lc_tier_mode(LC_BAND_915, LC_TIER_EDGE);
    float mhz = band == LC_BAND_2G4 ? 2440.0f : 915.0f;
    /* No TCXO on the W12: tcxoVoltage 0 selects the crystal. */
    s_radio->pram_pending = true; /* loaded right after begin()'s reset */
    int16_t st = s_radio->begin(mhz, (float)edge->bw_hz / 1000.0f, edge->sf, (uint8_t)(edge->cr + 4),
                                RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, chip_dbm(band), edge->preamble, 0.0f);
    ESP_LOGI("lc_radio", "LR2021 PRAM: load %d, loaded %d, version 0x%04x", s_radio->pram_status,
             s_radio->pram_loaded, s_radio->pram_version);
    if (st == RADIOLIB_ERR_NONE && (s_radio->pram_status != RADIOLIB_ERR_NONE || !s_radio->pram_loaded)) {
        st = s_radio->pram_status != RADIOLIB_ERR_NONE ? s_radio->pram_status : RADIOLIB_ERR_CHIP_NOT_FOUND;
    }
    if (st == RADIOLIB_ERR_NONE) {
        /* On the LR2021 setRfSwitchTable sends SetDioFunction commands at once, so it
         * must follow begin(), which resets the chip (and clears DIO config). */
        s_radio->setRfSwitchTable(k_rfsw_pins, k_rfsw_table);
        st = calibrate();
    }
    if (st != RADIOLIB_ERR_NONE) return st;
    s_band = band;
    s_mode = *edge;
    s_mode_valid = band == LC_BAND_915;
    s_pkt_type = RADIOLIB_LR2021_PACKET_TYPE_LORA; /* begin() loaded LoRa */
    invalidate_stage_cache();
    /* Init (reset, calibration) is done: per-slot commands finish in well under a
     * millisecond, so don't let a stuck BUSY block the exec task for 1 s each. */
    s_radio->getMod()->spiConfig.timeout = 20;
    return 0;
}

extern "C" int lc_radio_init_terminal(void)
{
    /* lc_radio_init now calibrates both bands for every role. */
    return lc_radio_init(LC_BAND_915);
}


extern "C" const lc_radio_ops_t *lc_radio_ops(void)
{
    return &k_ops;
}

extern "C" void lc_radio_stamp_irq(void)
{
    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << W12_PIN_LORA_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&in);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM); /* may already be installed */
    gpio_isr_handler_add((gpio_num_t)W12_PIN_LORA_IRQ, irq_stamp_isr, nullptr);
}
