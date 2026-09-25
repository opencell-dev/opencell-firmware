/* lc_radio — the W12's LR2021 behind the lc_radio_ops_t interface.
 * Wraps RadioLib 7.7.1 (built with RADIOLIB_GODMODE for per-slot LoRa/FLRC
 * switching). Call only from one task. */
#ifndef LC_RADIO_H
#define LC_RADIO_H

/* lc_phy/lc_exec headers are plain C without C++ guards. */
#ifdef __cplusplus
extern "C" {
#endif

#include "lc_phy.h"
#include "lc_radio_if.h"

/* Reset and configure the LR2021 for `band`, including front-end calibration
 * at the band's edges and centre. Returns 0 or a RadioLib error code. */
int lc_radio_init(lc_band_t band);

/* Operations for lc_exec (valid after lc_radio_init). */
const lc_radio_ops_t *lc_radio_ops(void);

#ifdef __cplusplus
}
#endif

#endif
