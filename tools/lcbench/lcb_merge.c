#include "lcb_merge.h"

#include <stdio.h>
#include <stdlib.h>

int lcb_merge_bands(lcb_cell_t *cell, uint32_t f, int drop_2g4, lcb_one_map_t maps[LCB_ONE_MAP_FRAMES], lc_msg_t *out)
{
    static lc_msg_t m915, m24;
    int have915 = lcb_cell_schedule(cell, LC_BAND_915, f, &m915) == 0;
    int have24 = !drop_2g4 && lcb_cell_schedule(cell, LC_BAND_2G4, f, &m24) == 0;
    if (!have915 && !have24) {
        return -1;
    }
    *out = have915 ? m915 : m24;
    lcb_one_map_t *map = &maps[f % LCB_ONE_MAP_FRAMES];
    map->frame = f;
    map->n = 0;
    lc_schedule_t *s = &out->u.schedule;
    s->slot_count = 0;
    const lc_schedule_t *src[2] = { have915 ? &m915.u.schedule : NULL, have24 ? &m24.u.schedule : NULL };
    uint8_t pos[2] = { 0, 0 };
    for (;;) { /* merge two offset-sorted lists */
        int pick = -1;
        for (int b = 0; b < 2; b++) {
            if (src[b] != NULL && pos[b] < src[b]->slot_count &&
                (pick < 0 || src[b]->slots[pos[b]].offset_us < src[pick]->slots[pos[pick]].offset_us)) {
                pick = b;
            }
        }
        if (pick < 0 || s->slot_count >= LC_MAX_SLOTS_PER_SCHEDULE) {
            break;
        }
        map->band[s->slot_count] = (uint8_t)pick; /* 0 = LC_BAND_915, 1 = LC_BAND_2G4 */
        map->offset_us[s->slot_count] = src[pick]->slots[pos[pick]].offset_us;
        map->mode[s->slot_count] = src[pick]->slots[pos[pick]].mode;
        map->idx[s->slot_count] = pos[pick];
        s->slots[s->slot_count++] = src[pick]->slots[pos[pick]++];
    }
    map->n = s->slot_count;
    if (getenv("LCB_DEBUG")) {
        printf("frame %u:", f);
        for (uint8_t i = 0; i < s->slot_count; i++)
            printf(" [%u+%u f%u d%u len%u mod%u]", s->slots[i].offset_us, s->slots[i].length_us, s->slots[i].freq_hz / 1000000u,
                   s->slots[i].dir, s->slots[i].payload_len, s->slots[i].mode.modulation);
        printf("\n");
    }
    return 0;
}
