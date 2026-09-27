/* lcb_merge — --one-board: one W12 carries both bands' slots (it switches
 * band per slot, with the cell leaving LC_EXEC_BAND_SWITCH_LEAD_US around
 * each change). The merged schedule is sorted by offset; RX reports are
 * mapped back to the band and per-band slot index lcb_cell used. */
#ifndef LCB_MERGE_H
#define LCB_MERGE_H

#include <stdint.h>

#include "lc_link.h"
#include "lc_phy.h"
#include "lcb_cell.h"

#define LCB_ONE_MAP_FRAMES 16u
typedef struct {
    uint32_t frame;
    uint8_t  n;
    uint8_t  band[LC_MAX_SLOTS_PER_SCHEDULE];
    uint8_t  idx[LC_MAX_SLOTS_PER_SCHEDULE];
    uint32_t offset_us[LC_MAX_SLOTS_PER_SCHEDULE];
    lc_mode_t mode[LC_MAX_SLOTS_PER_SCHEDULE];
} lcb_one_map_t;

/* One SCHEDULE for frame f carrying both bands' slots (2.4 GHz left out when
 * drop_2g4); fills maps[f % LCB_ONE_MAP_FRAMES]. 0, or -1 if nothing to do. */
int lcb_merge_bands(lcb_cell_t *cell, uint32_t f, int drop_2g4, lcb_one_map_t maps[LCB_ONE_MAP_FRAMES], lc_msg_t *out);

#endif
