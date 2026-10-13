/* The reference codec's state, complete for callers to hold (no heap). */
#ifndef OC_VOX_REF_STATE_H
#define OC_VOX_REF_STATE_H

#include "oc_vox_modes.h"
#include "oc_vox_ref_int.h"
#include "oc_vox_pipe.h"

typedef struct oc_vox_ref_enc {
    oc_vox_pipe_enc_t pipe;
    oc_vox_ref_ana_t  ana;
} oc_vox_ref_enc_t;

typedef struct oc_vox_ref_dec {
    oc_vox_pipe_dec_t pipe;
    oc_vox_ref_syn_t  syn;
} oc_vox_ref_dec_t;

#endif
