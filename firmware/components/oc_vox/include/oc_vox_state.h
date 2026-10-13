/* The encoder's and decoder's state, complete so a caller can hold them
 * (no heap anywhere). Their fields are the codec's own: read nothing here,
 * use oc_vox.h. Sizes are pinned by _Static_assert in oc_vox.c. */
#ifndef OC_VOX_STATE_H
#define OC_VOX_STATE_H

#include <stdint.h>

#include "oc_vox_modes.h"

typedef struct oc_vox_enc {
    oc_vox_mode_t mode;
    uint8_t       toggle;
} oc_vox_enc_t;

typedef struct oc_vox_dec {
    oc_vox_mode_t mode;
    uint8_t       toggle;
    struct {
        uint32_t blocks, lost_blocks, concealed_frames, toggle_gaps;
        uint32_t golay_fixed, golay_failed, hamming_fixed, crc_failed;
    } stats;
} oc_vox_dec_t;

#endif
