/* Meshnology W12 (ESP32-S3R8 + LR2021) board definitions.
 * Source: github.com/spangap/hw-meshnology-w12 (from the board's Meshtastic
 * and MeshCore variants). */
#ifndef W12_BOARD_H
#define W12_BOARD_H

/* LR2021 on SPI2 */
#define W12_PIN_LORA_SCK   9
#define W12_PIN_LORA_MOSI  10
#define W12_PIN_LORA_MISO  11
#define W12_PIN_LORA_NSS   8
#define W12_PIN_LORA_RST   12
#define W12_PIN_LORA_BUSY  13
#define W12_PIN_LORA_IRQ   14 /* LR2021 DIO8 (DIO5 is not bonded out) */
#define W12_LORA_IRQ_DIO   8

/* Front-end supplies (drive high to power the PA/LNA) */
#define W12_PIN_FEM_LF_PWR 4  /* GC1109, 860-930 MHz, 30 dB gain */
#define W12_PIN_FEM_HF_PWR 3  /* RFX2402E, 2.4 GHz, 22 dB gain */

/* Chip drive for full antenna power: 0 dBm -> ~30 dBm (sub-GHz),
 * -2 dBm -> ~20 dBm (2.4 GHz). Never exceed these. */
#define W12_LF_CHIP_DBM    0
#define W12_HF_CHIP_DBM    (-2)

/* GNSS header, reused on base stations for the Pi link and PPS */
#define W12_PIN_HDR_RX     39 /* into the ESP32 (Pi TX / GNSS TX): verified on hardware, the GNSS NMEA arrives here */
#define W12_PIN_HDR_TX     38 /* out of the ESP32 (Pi RX / GNSS RX) */
#define W12_PIN_HDR_PPS    41
/* GNSS module on the header (when fitted, e.g. Quectel L76K) */
#define W12_PIN_GNSS_FORCE  40 /* high: keep the receiver awake */
#define W12_PIN_GNSS_RESET  42 /* active low */
#define W12_PIN_GNSS_SUPPLY 48 /* active low: gates the GNSS supply */

/* Misc */
#define W12_PIN_VEXT_EN    45 /* active low: powers OLED + GNSS header 3V3 */
#define W12_PIN_BOOT_BTN   0

/* Power the RF front ends and Vext. GNSS-header control pins (40, 42, 48)
 * are left as inputs so nothing drives into a Pi wired to the header. */
void w12_board_init(void);

#endif
