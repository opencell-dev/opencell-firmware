/* The trained envelope codebook (PLAN.md Task 13). Written by
 * tools/vox/vox_train_cb.py together with oc_vox_cb.c; this committed pair
 * is the untrained stub: OC_VOX_CB_TRAINED 0, and the envelope's stage 1
 * is the structured DCT quantizer (oc_vox_quant.c). With a trained
 * codebook stage 1 is a 1024-entry vector quantizer of the 12 band values
 * (10 bits, the same width), and stages 2-4 refine the DCT of the residual
 * exactly as before, so the layout and the FEC classes never move. */
#ifndef OC_VOX_CB_H
#define OC_VOX_CB_H

#include <stdint.h>

#define OC_VOX_CB_TRAINED 0
#define OC_VOX_CB_S1_SIZE 1024u
/* The training set's identity, for the record: "" when untrained. */
#define OC_VOX_CB_TRAINING_SET ""

#if OC_VOX_CB_TRAINED
extern const int8_t oc_vox_cb_s1[OC_VOX_CB_S1_SIZE][12]; /* 0.75 dB units, mean removed */
#endif

#endif
