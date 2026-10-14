/* The block pipeline both codecs share (PLAN.md §1.6): analysed frames ->
 * quantize -> pack -> protect -> block, and block -> recover -> unpack ->
 * dequantize (or conceal) -> frames to synthesise, with the decoder's
 * statistics. The two implementations supply only analysis and synthesis. */
#ifndef OC_VOX_PIPE_H
#define OC_VOX_PIPE_H

#include "oc_vox_conceal.h"
#include "oc_vox_int.h"
#include "oc_vox_modes.h"
#include "oc_vox_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    oc_vox_mode_t   mode;
    oc_vox_pframe_t last;      /* the decoder's view of the previous block's last frame */
    uint8_t         have_last;
    uint8_t         toggle;
} oc_vox_pipe_enc_t;

typedef struct {
    oc_vox_mode_t      mode;
    oc_vox_conceal_t   conceal;
    uint8_t            toggle, have_toggle;
    oc_vox_dec_stats_t stats;
} oc_vox_pipe_dec_t;

int  oc_vox_pipe_enc_init(oc_vox_pipe_enc_t *p, oc_vox_mode_t mode);
void oc_vox_pipe_encode(oc_vox_pipe_enc_t *p, const oc_vox_pframe_t frames[OC_VOX_FRAMES], uint8_t *block);

int  oc_vox_pipe_dec_init(oc_vox_pipe_dec_t *p, oc_vox_mode_t mode);
/* block NULL: lost. The six frames to synthesise, always. */
void oc_vox_pipe_decode(oc_vox_pipe_dec_t *p, const uint8_t *block, oc_vox_pframe_t frames[OC_VOX_FRAMES]);

#ifdef __cplusplus
}
#endif

#endif
