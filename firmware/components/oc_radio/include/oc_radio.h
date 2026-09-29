/* oc_radio — the W12's LR2021 behind the oc_radio_ops_t interface.
 * Wraps RadioLib 7.7.1 (built with RADIOLIB_GODMODE for per-slot LoRa/FLRC
 * switching). Call only from one task. */
#ifndef OC_RADIO_H
#define OC_RADIO_H

/* oc_phy/oc_exec headers are plain C without C++ guards. */
#ifdef __cplusplus
extern "C" {
#endif

#include "oc_phy.h"
#include "oc_radio_if.h"

/* Reset and configure the LR2021 for `band`, including front-end calibration
 * at the band's edges and centre. Returns 0 or a RadioLib error code. */
int oc_radio_init(oc_band_t band);

/* Terminal: oc_radio_init(OC_BAND_915), then calibrate the front end at two
 * sub-GHz points and one 2.4 GHz point so either band can be used per slot.
 * Returns 0 or a RadioLib error code. */
int oc_radio_init_terminal(void);

/* Timestamp the LR2021 IRQ line (DIO8) in an ISR so poll() fills
 * oc_radio_event_t.irq_us with the first done edge after each launch. The
 * terminal installs its own handler on that pin: bs-radio only. */
void oc_radio_stamp_irq(void);

/* Operations for oc_exec (valid after oc_radio_init). */
const oc_radio_ops_t *oc_radio_ops(void);

#ifdef __cplusplus
}
#endif

#endif
