/* Quantization of a block of parameter frames into layout fields and back
 * (PLAN.md §1.2-1.3): absolute and delta coding of energy and pitch, the
 * voicing class and mask, the DCT envelope stages, and the interpolation
 * of frames a mode does not transmit. Integer only, shared by both
 * implementations: the encoder's reconstruction is the decoder's. */
#ifndef OC_VOX_QUANT_H
#define OC_VOX_QUANT_H

#include "oc_vox_int.h"

/* f0 x 16 in Hz of pitch index p (50 Hz x 2^(p / 84)): 800 .. 6560. */
int32_t oc_vox_pitch_hz_x16(uint8_t p);
/* The nearest pitch index of f0 x 16 Hz (clamped to the pitch range). */
uint8_t oc_vox_pitch_index(int32_t hz_x16);

/* Voicing: the 2-bit class of a mask and the mask of a class. */
uint8_t oc_vox_vclass_of_mask(uint8_t v);
uint8_t oc_vox_vmask_of_class(uint8_t cls);

/* The envelope's 12-point DCT (coefficients 1..11; c[0] is the mean, which
 * e carries, and is set to 0) and its inverse. Coefficient units: 0.75 dB
 * summed over bands (a coefficient of 60 swings a band by 10 units). */
void oc_vox_env_dct(const int8_t s[OC_VOX_BANDS], int32_t c[OC_VOX_BANDS]);
void oc_vox_env_idct(const int32_t c[OC_VOX_BANDS], int8_t s[OC_VOX_BANDS]);

/* Delta tables: entries of the e (0.75 dB) and p (1/84 octave) tables of a
 * field width, and the nearest index of a difference. */
const int16_t *oc_vox_e_delta_table(unsigned bits);
const int16_t *oc_vox_p_delta_table(unsigned bits);
unsigned       oc_vox_delta_index(const int16_t *table, unsigned bits, int32_t d);

/* Quantize six analysed frames. prev: the previous block's last
 * reconstructed frame (the decoder's), or NULL for the first block.
 * toggle: the T field. recon (optional): what the decoder will see. */
void oc_vox_quant_block(oc_vox_mode_t m, const oc_vox_pframe_t in[OC_VOX_FRAMES], const oc_vox_pframe_t *prev,
                        uint8_t toggle, oc_vox_qblock_t *q, oc_vox_pframe_t *recon);

/* Reconstruct six frames from the fields, interpolating what the mode did
 * not transmit (frame 0 from prev when it is there). *toggle gets T.
 * OC_VOX_DQ_DROP_FINE: the S3, S4 and E_MID fields are ignored (a CRC
 * failure over classes B and C: the fine detail is what a bit error there
 * hurts most for least loss when dropped). */
#define OC_VOX_DQ_DROP_FINE 1u
void oc_vox_dequant_block(oc_vox_mode_t m, const oc_vox_qblock_t *q, const oc_vox_pframe_t *prev, unsigned flags,
                          oc_vox_pframe_t out[OC_VOX_FRAMES], uint8_t *toggle);

#endif
