/* ocb_merge — --one-board: one W12 carries both bands' slots (it switches
 * band per slot, with the cell leaving OC_EXEC_BAND_SWITCH_LEAD_US around
 * each change). The merged schedule is sorted by offset; RX reports are
 * mapped back to the band and per-band slot index ocb_cell used. */
#ifndef OCB_MERGE_H
#define OCB_MERGE_H

#include <stdint.h>

#include "oc_link.h"
#include "oc_phy.h"
#include "ocb_cell.h"

#define OCB_ONE_MAP_FRAMES 16u
typedef struct {
    uint32_t frame;
    uint8_t  n;
    uint8_t  band[OC_MAX_SLOTS_PER_SCHEDULE];
    uint8_t  idx[OC_MAX_SLOTS_PER_SCHEDULE];
    uint32_t offset_us[OC_MAX_SLOTS_PER_SCHEDULE];
    oc_mode_t mode[OC_MAX_SLOTS_PER_SCHEDULE];
} ocb_one_map_t;

/* One SCHEDULE for frame f carrying both bands' slots (2.4 GHz left out when
 * drop_2g4); fills maps[f % OCB_ONE_MAP_FRAMES]. 0, or -1 if nothing to do. */
int ocb_merge_bands(ocb_cell_t *cell, uint32_t f, int drop_2g4, ocb_one_map_t maps[OCB_ONE_MAP_FRAMES], oc_msg_t *out);

#endif
