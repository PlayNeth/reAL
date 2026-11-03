/* reAL - the built-in HRTF (hrtf=builtin): the MIT KEMAR set, see hrtf_kemar_data.h for its origin and licence.
 * Unfolded here into the reAL_hrtf.bin format and loaded as a file would be. */
#include "hrtf.h"
#include <stdlib.h>
#include <string.h>

#include "hrtf_kemar_data.h"

static uint8_t *put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); return p + 4; }
static uint8_t *putf(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); return put32(p, u); }

int rl_hrtf_load_builtin(rl_hrtf_set *set, uint32_t out_rate)
{
    uint32_t n = 0;
    for (int r = 0; r < KEMAR_RINGS; r++) n += kemar_rings[r].n_az;
    uint32_t size = 20 + 8 * KEMAR_RINGS + n * (8 + 8 * KEMAR_TAPS);
    uint8_t *blob = malloc(size), *p = blob;
    memset(set, 0, sizeof(*set));
    if (!blob) return 0;
    memcpy(p, "RLHRTF01", 8); p += 8;
    p = put32(p, KEMAR_RATE);
    p = put32(p, KEMAR_TAPS);
    p = put32(p, KEMAR_RINGS);
    for (int r = 0; r < KEMAR_RINGS; r++) {
        p = putf(p, kemar_rings[r].el);
        p = put32(p, kemar_rings[r].n_az);
    }
    uint32_t first = 0;                     /* the ring's first stored (right half) direction */
    for (int r = 0; r < KEMAR_RINGS; r++) {
        uint32_t naz = kemar_rings[r].n_az;
        for (uint32_t k = 0; k < naz; k++) {
            /* the left half (k > naz / 2) is the mirror image of azimuth 360 - az, ears swapped */
            int mirror = k > naz / 2;
            uint32_t idx = first + (mirror ? naz - k : k);
            for (int ch = 0; ch < 2; ch++) p = putf(p, kemar_delay[idx][ch ^ mirror]);
            for (int ch = 0; ch < 2; ch++)
                for (int t = 0; t < KEMAR_TAPS; t++) p = putf(p, kemar_coef[idx][ch ^ mirror][t] / KEMAR_Q);
        }
        first += naz / 2 + 1;
    }
    int ok = first == KEMAR_HALF && (uint32_t)(p - blob) == size && rl_hrtf_load(set, blob, size, out_rate);
    free(blob);
    return ok;
}
