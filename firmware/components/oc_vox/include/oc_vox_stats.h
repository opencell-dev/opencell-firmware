/* The decoder's counters since init: what it saw and did. */
#ifndef OC_VOX_STATS_H
#define OC_VOX_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t blocks;           /* decode calls */
    uint32_t lost_blocks;      /* NULL blocks, plus blocks whose class A failed */
    uint32_t concealed_frames; /* frames synthesised from extrapolated parameters */
    uint32_t toggle_gaps;      /* the toggle bit repeated: a block went missing unannounced */
    uint32_t golay_fixed;      /* Golay words with 1-3 bit errors corrected */
    uint32_t golay_failed;     /* Golay words with more (the block counts as lost) */
    uint32_t hamming_fixed;    /* Hamming words with a bit error corrected */
    uint32_t crc_failed;       /* class C discarded (OVX-2400, OVX-3200) */
} oc_vox_dec_stats_t;

#ifdef __cplusplus
}
#endif

#endif
