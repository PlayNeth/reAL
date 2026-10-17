/* reAL - ALchemy's stereo virtualizer: its channel converter's mode 3 for stereo speakers (fn 0x10036860,
 * the !headphones branch), which folds the side and rear pairs into two channels through sum/difference FIRs
 * (80/72 taps at 48 kHz), three biquads per side and a 12-sample crossfeed, then adds the front pair direct
 * and the centre at 0.7071.  Only at 22.05, 44.1 and 48 kHz; at other rates a plain fold-down.  Used by the
 * reverb on a stereo device and by CMSS-3D for stereo speakers.  Platform independent. */
#ifndef REAL_VIRT_H
#define REAL_VIRT_H

#include <stdint.h>
#include "alrv.h"

typedef struct rl_virt {                 /* stereo virtualizer state */
    const void *tab;                     /* alrv_virt_tables*, NULL = plain fold-down */
    float  *hist;                        /* 4 FIR inputs (side sum/diff, back sum/diff) with history */
    uint32_t hist_cap, hist_len;
    float   bq[10][2];                   /* biquad states w1 w2 */
    float   dly[2][16]; uint32_t dpos;   /* crossfeed delay (<= 12 samples) */
} rl_virt;

void rl_virt_init(rl_virt *v, uint32_t rate);
void rl_virt_free(rl_virt *v);
/* ch: ALRV_NOUT channel buffers in ALRV_* order (ALRV_CX unused), out: interleaved stereo, added to;
 * tmp: 2 x frames floats.  0 on allocation failure. */
int  rl_virt_process(rl_virt *v, float *const ch[ALRV_NOUT], float *out, uint32_t frames, float *tmp);

#endif
