/* reAL - ALchemy's stereo virtualizer: its channel converter's mode 3 for stereo speakers (fn 0x10036860,
 * the !headphones branch), which folds the side and rear pairs into two channels through sum/difference FIRs
 * (80/72 taps at 48 kHz), three biquads per side and a 12-sample crossfeed, then adds the front pair direct
 * and the centre at 0.7071.  On headphones (modes 0..2, used by CMSS-3D; ALchemy's reverb always takes the
 * speaker version) the front pair goes through FIRs too, early reflections are added and there is no
 * crossfeed.  Only at 22.05, 44.1 and 48 kHz; at other rates a plain fold-down.  Platform independent. */
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
    struct rl_virt_hp *hp;               /* headphones (modes 0..2): early reflections; NULL = speakers */
} rl_virt;

void rl_virt_init(rl_virt *v, uint32_t rate);
/* the headphone version (converter mode 0: the front pair through FIRs too, early reflections, no
 * crossfeed); 0 on allocation failure */
int  rl_virt_init_hp(rl_virt *v, uint32_t rate);
/* ALchemy reads each block's early-reflection taps before writing the block into their rings, so a tap
 * shorter than the block (taps are 139 to 703 frames at 48 kHz) would land a ring length late for the rest of
 * the block.  ALchemy's own blocks are 128 frames, so that never happens there; reAL reads per sample, which
 * gives the same output with blocks of any size.  on = ALchemy's block-wise reads, for the tests. */
void rl_virt_hp_alchemy_er(rl_virt *v, int on);
void rl_virt_free(rl_virt *v);
/* clear the filter histories (a restarted source) */
void rl_virt_reset(rl_virt *v);
/* ch: ALRV_NOUT channel buffers in ALRV_* order (ALRV_CX unused), out: interleaved stereo, added to;
 * tmp: 2 x frames floats.  0 on allocation failure. */
int  rl_virt_process(rl_virt *v, float *const ch[ALRV_NOUT], float *out, uint32_t frames, float *tmp);

#endif
