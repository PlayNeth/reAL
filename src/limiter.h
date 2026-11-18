/* reAL - ALchemy's output limiter (CLimiter / CLimiterChan), platform independent.
 *
 * The last stage of ALchemy's engine before the samples are converted for the device: mixer -> limiter -> clipper
 * (the conversion, which saturates).  Each speaker channel on its own, 3 ms of look-ahead: the output is the mix
 * 3 ms late, and while a peak above full scale is in those 3 ms (or in the block coming in) the gain goes down
 * towards 0.98 / peak, by 1 / (5 ms) a sample at most; with nothing above full scale in sight it comes back by
 * 1 / (50 ms) a sample.  Below full scale and at gain 1 it is only the delay.
 *
 * Translated from the DLL (0x1002cb30, set up by 0x1002afb0 and 0x1002b040) and run against it in an emulator:
 * the same output and gain, bit for bit (tools/limcmp.py, this code on random blocks; tests/test_limiter.c holds
 * the hashes of 270 runs).  What is the DLL's is the block function: ALchemy gives it its renderer's blocks one
 * after the other, reAL starts a block at every mixing call (rl_limiter_process), so the two do not come back up
 * in quite the same steps.
 *
 * What it lets through, and reAL's fixes do not (`fixed`):
 * - It makes one line per block, to the highest peak in sight (or to the highest of the samples going out now),
 *   and never steeper than 1 / (5 ms) a sample.  A lower peak that comes before the highest, and any sudden one
 *   of more than about 2.5 times full scale, still come out above full scale and are clipped.
 * - Each channel has its own gain: a sound loud in one speaker moves over to the other while it is held down.
 * - How it comes back depends on the size of the blocks it is given.
 * Fixed: every sample above full scale has its own line, from the moment it comes in sight, 3 ms ahead, down to
 * where it comes out at 0.98; the gain is the lowest of them, the same for every channel, and comes back by
 * 1 / (50 ms) a sample.  Nothing comes out above full scale, on the device's speakers either when the mix is
 * folded to them afterwards (rl_limiter_fold; as long as the channels themselves are within some 100,000 times
 * full scale: beyond, channels that cancel in the fold do so less exactly than the limiter reckons).  An infinity
 * or a NaN is left out, and a peak too high for a float gain to bring to 0.98 (from some 340,000 times full scale)
 * is silence. */
#ifndef REAL_LIMITER_H
#define REAL_LIMITER_H

#include <stdint.h>

#define RL_LIM_MAXDELAY 576         /* ALchemy's ring: 3 ms at up to 192 kHz */
#define RL_LIM_MAXCH    12          /* one per speaker bit, as ALchemy */

typedef struct rl_limiter_chan {
    float    ring[RL_LIM_MAXDELAY];
    uint32_t at;                    /* the oldest sample: the next one out */
    float    gain;
} rl_limiter_chan;

typedef struct rl_limiter {
    uint32_t delay;                 /* frames: rate * 3 / 1000 */
    uint32_t block;                 /* frames ALchemy's renderer mixes at a time (128 at 44.1 and 48 kHz) */
    float    down, up;              /* the most the gain moves in a sample, either way */
    uint32_t nch;
    uint32_t limited;               /* frames put out at a gain below 1 since the caller last cleared it (a block's
                                     * frames all, in ALchemy's) */
    float    lowest;                /* and the lowest gain among them */
    rl_limiter_chan ch[RL_LIM_MAXCH];
    /* fixed: by how much each frame in the rings has to come down (1 - 0.98 / its peak; 0: not at all), how many
     * of them do, and the one gain (the rings and `at` of ch[0] are the frames') */
    int      fixed;
    float    need[RL_LIM_MAXDELAY];
    float    weight[RL_LIM_MAXDELAY];   /* how much of its need a frame asks for, from the oldest on: 1 - i / delay */
    uint32_t needed;
    float    gain;
    uint32_t fold_out;              /* rl_limiter_fold: outputs the channels are folded to after the limiter (0: none) */
    float    fold[RL_LIM_MAXCH][RL_LIM_MAXCH];
} rl_limiter;

void rl_limiter_init(rl_limiter *l, uint32_t rate, uint32_t nch, int fixed);
/* fixed: the channels are folded to `nout` others after the limiter (m[c * stride + o]: from channel c to output
 * o; gains within -1 to 1), and it is those that must stay within full scale.  NULL: they go out as they are.  Kept until the next
 * rl_limiter_init. */
void rl_limiter_fold(rl_limiter *l, const float *m, uint32_t stride, uint32_t nout);
/* `frames` frames of `nch` interleaved channels (RL_LIM_MAXCH at most), in place.  ALchemy's takes a call a block
 * of ALchemy's at a time, what is left over as a short one. */
void rl_limiter_process(rl_limiter *l, float *buf, uint32_t frames);
/* one channel, one block (the DLL's function; `stride` floats between its samples) */
void rl_limiter_chan_process(rl_limiter *l, rl_limiter_chan *c, float *buf, uint32_t n, uint32_t stride);

#endif
