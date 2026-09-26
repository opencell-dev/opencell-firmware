#include "lc_radio.h"

#include "esp_log.h"
#include "third_party/lr20xx_pram_lr2021.h" /* Semtech LR2021 firmware patch (Clear BSD) */

#include <string.h>

#include <RadioLib.h>
#include "modules/LR2021/LR2021_registers.h" /* DC-DC workaround registers */

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
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
            snapshotDcdc();
        }
        return LR2021::standby();
    }

    /* RadioLib's setDCDCworkaround()/resetDCDCworkaround() (run by every
     * modulation setter and setPacketType) pass sizeof() = 4 as the *word*
     * count to readRegMem32/writeRegMem32: the read overruns a one-word
     * stack variable, and the write copies 12 bytes of stack into the three
     * chip-RAM words after DCDC_FREQ_LF (0x80004C). Snapshot those words
     * straight after reset, and redo the workaround correctly - restoring
     * them - after anything that went through RadioLib's version. */
    uint32_t dcdc_words[4] = { 0, 0, 0, 0 };
    bool dcdc_snap = false;
    bool dcdc_repair = true;  /* neighbours need restoring */
    uint32_t dcdc_sw = 0, dcdc_lf = 0;
    bool dcdc_known = false;

    void snapshotDcdc()
    {
        dcdc_snap = readRegMem32(RADIOLIB_LR2021_REG_DCDC_FREQ_LF, dcdc_words, 4) == RADIOLIB_ERR_NONE;
        dcdc_repair = true;
        dcdc_known = false;
    }

    /* Semtech's narrow-band DC-DC settings for the loaded modulation and band
     * (what RadioLib's workaround means to do). Writes only what changed. */
    int16_t dcdcWorkaround()
    {
        uint32_t raw = 0;
        int16_t st = readRegMem32(RADIOLIB_LR2021_REG_DCDC_ADC_CTRL, &raw, 1);
        if (st != RADIOLIB_ERR_NONE) return st;
        uint32_t ana_dec = (raw >> 8) & 0x7u;
        bool narrow = !highFreq && (ana_dec == 1 || ana_dec == 2);
        uint32_t sw = narrow ? ((11u << 20) | (13u << 16)) : ((15u << 20) | (15u << 16));
        uint32_t lf = ana_dec == 1 ? 4508876u : 2936012u; /* Semtech: 4.3 / 2.8 MHz * 1.048576 */
        bool changed = false;
        if (!dcdc_known || sw != dcdc_sw) {
            st = writeRegMemMask32(RADIOLIB_LR2021_REG_DCDC_SWITCHER, 0xFFu << 16, sw);
            if (st != RADIOLIB_ERR_NONE) return st;
            changed = true;
        }
        if (!dcdc_known || lf != dcdc_lf || (dcdc_repair && dcdc_snap)) {
            dcdc_words[0] = lf;
            st = writeRegMem32(RADIOLIB_LR2021_REG_DCDC_FREQ_LF, dcdc_words, dcdc_snap ? 4 : 1);
            if (st != RADIOLIB_ERR_NONE) return st;
            dcdc_repair = false;
            changed = true;
        }
        dcdc_sw = sw;
        dcdc_lf = lf;
        dcdc_known = true;
        /* as RadioLib/Semtech: re-apply the RF frequency after DC-DC changes */
        return changed ? setFrequency(freqMHz, true) : RADIOLIB_ERR_NONE;
    }

    /* Call after any RadioLib setter that ran its (buggy) workaround. */
    int16_t repairDcdc()
    {
        dcdc_repair = true;
        dcdc_known = false;
        return dcdcWorkaround();
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
    /* Modulation change within the current packet type: one command plus
     * RadioLib's DC-DC workaround (which follows the band), with RadioLib's
     * state kept in step for the packet params. Its per-field setters each
     * re-read the packet type and re-run the workaround: ~4.4 ms per change
     * (bench), far over the slot lead. */
    int16_t fastLora(uint8_t sf, uint8_t bwCode, float bwKhz, uint8_t cr, uint16_t preamble)
    {
        spreadingFactor = sf;
        bandwidth = bwCode;
        bandwidthKhz = bwKhz;
        codingRate = cr;
        preambleLengthLoRa = preamble;
        /* RadioLib's LDRO rule for these modes: on for symbols >= 16 ms */
        ldrOptimize = ((float)(1u << sf) / bwKhz >= 16.0f) ? RADIOLIB_LR2021_LORA_LDRO_ENABLED
                                                           : RADIOLIB_LR2021_LORA_LDRO_DISABLED;
        uint8_t buff[] = { (uint8_t)((sf << 4) | (bwCode & 0x0F)), (uint8_t)(((cr & 0x0F) << 4) | ldrOptimize) };
        int16_t st = SPIcommand(RADIOLIB_LR2021_CMD_SET_LORA_MODULATION_PARAMS, true, buff, sizeof(buff));
        return st == RADIOLIB_ERR_NONE ? dcdcWorkaround() : st;
    }
    /* Packet type change once that type has had a full RadioLib setup (its
     * RadioLib state is still valid): SetPacketType and the sync word only.
     * RadioLib's setPacketType() would run its DC-DC reset (same bug). */
    int16_t fastPacketType(uint8_t type, uint32_t flrcSync)
    {
        int16_t st = SPIcommand(RADIOLIB_LR2021_CMD_SET_PACKET_TYPE, true, &type, sizeof(type));
        if (st != RADIOLIB_ERR_NONE) return st;
        dcdc_known = false; /* the chip may reset its DC-DC settings with the type */
        return type == RADIOLIB_LR2021_PACKET_TYPE_LORA ? setLoRaSyncword(RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE)
                                                        : setFlrcSyncWord(1, flrcSync);
    }
    int16_t fastFlrc(uint8_t brCode, uint8_t cr)
    {
        bitRateFlrc = brCode;
        codingRateFlrc = cr;
        uint8_t buff[] = { brCode, (uint8_t)((cr << 4) | (pulseShape & 0x0F)) };
        int16_t st = SPIcommand(RADIOLIB_LR2021_CMD_SET_FLRC_MODULATION_PARAMS, true, buff, sizeof(buff));
        return st == RADIOLIB_ERR_NONE ? dcdcWorkaround() : st;
    }
    void setStagedRxTimeout(uint32_t t) { rxTimeout = t; }
    uint32_t stagedRxTimeout() const { return rxTimeout; }
    /* SetRxPath without RadioLib's setRxPath(), which also runs its buggy
     * DC-DC workaround; ours follows (the workaround depends on the path).
     * Bench: switching to HF right after an LF reception holds BUSY ~7 ms. */
    int16_t rxPathForBand()
    {
        uint8_t buff[] = { (uint8_t)(highFreq ? RADIOLIB_LR2021_RX_PATH_HF : RADIOLIB_LR2021_RX_PATH_LF),
                           (uint8_t)((highFreq ? gainModeHf : gainModeLf) & 0x07) };
        int16_t st = SPIcommand(RADIOLIB_LR2021_CMD_SET_RX_PATH, true, buff, sizeof(buff));
        if (st == RADIOLIB_ERR_NONE) {
            dcdc_known = false;
            st = dcdcWorkaround();
        }
        return st;
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

/* LR2021 codes for the modes lc_phy uses; -1 = not in the fast path. */
int lora_bw_code(uint32_t bw_hz)
{
    switch (bw_hz) {
    case 125000:  return RADIOLIB_LR2021_LORA_BW_125;
    case 203125:  return RADIOLIB_LR2021_LORA_BW_203;
    case 250000:  return RADIOLIB_LR2021_LORA_BW_250;
    case 406250:  return RADIOLIB_LR2021_LORA_BW_406;
    case 500000:  return RADIOLIB_LR2021_LORA_BW_500;
    case 812500:  return RADIOLIB_LR2021_LORA_BW_812;
    case 1000000: return RADIOLIB_LR2021_LORA_BW_1000;
    default:      return -1;
    }
}

int flrc_br_code(uint32_t bps)
{
    switch (bps) {
    case 260000:  return RADIOLIB_LR2021_FLRC_BR_260;
    case 325000:  return RADIOLIB_LR2021_FLRC_BR_325;
    case 520000:  return RADIOLIB_LR2021_FLRC_BR_520;
    case 650000:  return RADIOLIB_LR2021_FLRC_BR_650;
    case 1040000: return RADIOLIB_LR2021_FLRC_BR_1040;
    case 1300000: return RADIOLIB_LR2021_FLRC_BR_1300;
    case 2080000: return RADIOLIB_LR2021_FLRC_BR_2080;
    case 2600000: return RADIOLIB_LR2021_FLRC_BR_2600;
    default:      return -1;
    }
}

/* Each packet type gets one full RadioLib setup; after that its RadioLib
 * state stays valid and switching back is a few commands. */
bool s_type_ready[2];     /* [LC_MOD_LORA], [LC_MOD_FLRC] */
uint16_t s_flrc_preamble; /* the FLRC preamble RadioLib was set up with */

/* Switch modulation fast: 1 = done (st set), 0 = needs the full RadioLib
 * setup (packet type never set up, or another FLRC preamble). */
int fast_mode(const lc_mode_t *m, int16_t *st)
{
    if (!s_mode_valid || m->modulation > LC_MOD_FLRC || !s_type_ready[m->modulation]) {
        return 0;
    }
    if (m->modulation == LC_MOD_FLRC &&
        (flrc_br_code(m->bitrate_bps) < 0 || m->preamble != s_flrc_preamble)) {
        return 0;
    }
    if (m->modulation != s_mode.modulation) {
        static const uint32_t sync = ((uint32_t)k_flrc_sync[0] << 24) | ((uint32_t)k_flrc_sync[1] << 16) |
                                     ((uint32_t)k_flrc_sync[2] << 8) | k_flrc_sync[3];
        *st = s_radio->fastPacketType(m->modulation == LC_MOD_FLRC ? RADIOLIB_LR2021_PACKET_TYPE_FLRC
                                                                   : RADIOLIB_LR2021_PACKET_TYPE_LORA,
                                      sync);
        if (*st != RADIOLIB_ERR_NONE) return 1;
        /* bench: FLRC RX after a same-band LoRa slot heard nothing until the
         * RX path was re-sent (the old full setup did that via the cache) */
        s_rxpath_band = -1;
    }
    if (m->modulation == LC_MOD_LORA) {
        int bw = lora_bw_code(m->bw_hz);
        if (bw < 0 || m->sf < 5 || m->sf > 12 || m->cr < 1 || m->cr > 4) return 0;
        *st = s_radio->fastLora(m->sf, (uint8_t)bw, (float)m->bw_hz / 1000.0f, m->cr, m->preamble);
        return 1;
    }
    *st = s_radio->fastFlrc((uint8_t)flrc_br_code(m->bitrate_bps), m->cr);
    return 1;
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
    int band_changed = (int)band != s_band;
    /* A band change re-sends the modulation too: RadioLib's DC-DC workaround,
     * applied with it, depends on the band. */
    if (!s_mode_valid || band_changed || memcmp(mode, &s_mode, sizeof(s_mode)) != 0) {
        if (fast_mode(mode, &st)) {
            s_pp_len = -1; /* new preamble/header: re-send the packet params */
        } else {
            st = mode->modulation == LC_MOD_FLRC ? apply_flrc(mode) : apply_lora(mode);
            if (st == RADIOLIB_ERR_NONE) st = s_radio->repairDcdc();
            if (st == RADIOLIB_ERR_NONE && mode->modulation <= LC_MOD_FLRC) {
                s_type_ready[mode->modulation] = true;
                if (mode->modulation == LC_MOD_FLRC) s_flrc_preamble = mode->preamble;
            }
            /* RadioLib's setters rewrite packet params with their own length. */
            invalidate_stage_cache();
        }
        if (st != RADIOLIB_ERR_NONE) {
            s_mode_valid = false;
            s_pkt_type = RADIOLIB_LR2021_PACKET_TYPE_NONE;
            return st;
        }
        s_pkt_type = mode->modulation == LC_MOD_FLRC ? RADIOLIB_LR2021_PACKET_TYPE_FLRC : RADIOLIB_LR2021_PACKET_TYPE_LORA;
        s_mode = *mode;
        s_mode_valid = true;
    }
    /* SetPaConfig for the 2.4 GHz PA is refused (CMD_INVALID, -706) while the
     * LF RX path is selected after an LF reception (bench): move the RX path
     * with the band, then the PA. */
    if (band_changed) {
        st = s_radio->rxPathForBand();
        s_rxpath_band = st == RADIOLIB_ERR_NONE ? (int)band : -1;
        if (st != RADIOLIB_ERR_NONE) return st;
        st = s_radio->setOutputPower(chip_dbm(band));
        if (st != RADIOLIB_ERR_NONE) return st;
        s_band = band;
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

/* NSS rising edge to BUSY low (TX: preamble on air, RX: receiver ready),
 * learned per direction; launches fire this much before their target. */
int32_t s_start_lat16[2] = { 60 * 16, 100 * 16 }; /* 1/16 us; [0] RX, [1] TX: datasheet-ish seeds */
portMUX_TYPE s_fire_mux = portMUX_INITIALIZER_UNLOCKED;
constexpr int64_t k_fire_critical_us = 20; /* interrupts off only for the last stretch */

/* SetTx/SetRx with the SPI bytes clocked in ahead of time and NSS held low:
 * the LR2021 runs a command on NSS rising, so the start is one GPIO edge at
 * a chosen microsecond instead of an SPI transaction's variable latency
 * (RadioLib's setTx() also allocates and, with RADIOLIB_SPI_PARANOID, reads
 * the status back first). */
int16_t fire_set_mode(uint16_t cmd, uint32_t timeout, uint64_t at_us, int tx, int64_t *ready_us)
{
    int64_t idle;
    int16_t st = wait_busy_low(&idle);
    if (st != RADIOLIB_ERR_NONE) {
        return st;
    }
    uint8_t out[5] = { (uint8_t)(cmd >> 8), (uint8_t)cmd, (uint8_t)(timeout >> 16), (uint8_t)(timeout >> 8),
                       (uint8_t)timeout };
    uint8_t in[5];
    int64_t fire = (int64_t)at_us - s_start_lat16[tx] / 16;
    s_hal->spiBeginTransaction();
    s_hal->digitalWrite(W12_PIN_LORA_NSS, 0);
    s_hal->spiTransfer(out, sizeof(out), in);
    while (esp_timer_get_time() < fire - k_fire_critical_us) {
    }
    portENTER_CRITICAL(&s_fire_mux);
    int64_t edge = esp_timer_get_time();
    while (edge < fire) {
        edge = esp_timer_get_time();
    }
    gpio_set_level((gpio_num_t)W12_PIN_LORA_NSS, 1);
    portEXIT_CRITICAL(&s_fire_mux);
    s_hal->spiEndTransaction();
    st = wait_busy_low(ready_us);
    if (st == RADIOLIB_ERR_NONE) {
        int32_t lat = (int32_t)(*ready_us - edge);
        if (lat > 0 && lat < 1000) {
            s_start_lat16[tx] += (lat * 16 - s_start_lat16[tx]) / 8;
        }
    }
    return st;
}

int op_launch(void *ctx, uint64_t at_us)
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
    int16_t st = tx ? fire_set_mode(RADIOLIB_LR2021_CMD_SET_TX, RADIOLIB_LR2021_TX_TIMEOUT_NONE, at_us, 1, &ready)
                    : fire_set_mode(RADIOLIB_LR2021_CMD_SET_RX, s_radio->stagedRxTimeout(), at_us, 0, &ready);
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
    /* begin()'s setters ran RadioLib's DC-DC workaround: undo its stray writes. */
    if (st == RADIOLIB_ERR_NONE) st = s_radio->repairDcdc();
    if (st != RADIOLIB_ERR_NONE) return st;
    s_band = band;
    s_mode = *edge;
    s_mode_valid = band == LC_BAND_915;
    s_type_ready[LC_MOD_LORA] = s_type_ready[LC_MOD_FLRC] = false; /* fresh chip: full setup first */
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
