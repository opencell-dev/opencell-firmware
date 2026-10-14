/* Loading oc_vox's test vectors (host-tests/vectors/vox, OC_VOX_VECTORS):
 * raw s16le 8 kHz audio and the generator's per-frame labels. */
#ifndef VOX_VECTORS_H
#define VOX_VECTORS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOX_MAX_SAMPLES (8000 * 8)
#define VOX_MAX_FRAMES  (VOX_MAX_SAMPLES / 160)

typedef struct {
    int16_t  pcm[VOX_MAX_SAMPLES];
    unsigned n;       /* samples */
    float    f0[VOX_MAX_FRAMES];
    uint8_t  kind[VOX_MAX_FRAMES]; /* 0 silence, 1 voiced, 2 noise, 3 tone */
    unsigned frames;
} vox_vector_t;

static int vox_vector_load(const char *name, vox_vector_t *v)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s.raw", OC_VOX_VECTORS, name);
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    v->n = (unsigned)fread(v->pcm, sizeof(int16_t), VOX_MAX_SAMPLES, f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/%s.lab", OC_VOX_VECTORS, name);
    f = fopen(path, "r");
    v->frames = 0;
    if (f != NULL) {
        float f0;
        unsigned k;
        while (v->frames < VOX_MAX_FRAMES && fscanf(f, "%f %u", &f0, &k) == 2) {
            v->f0[v->frames] = f0;
            v->kind[v->frames] = (uint8_t)k;
            v->frames++;
        }
        fclose(f);
    }
    return 0;
}

#endif
