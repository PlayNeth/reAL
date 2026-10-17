/* reAL - CMSS-3D: ALchemy's stereo-to-surround upmixer (CCMSS3Upmix, fns 0x1003a050..0x1003fbd0).
 *
 * ALchemy gives every 2D stereo buffer its own upmixer when CMSS-3D is on for the current speaker mode.
 * The upmixer works on blocks of 512 frames at a base rate of at most 24 kHz (higher rates are split into
 * half-bands first and only the low band is analysed), with a 1024-point STFT:
 *   - centre: bins where left and right are about equally loud are moved from the front pair to the centre,
 *     above ~120 Hz;
 *   - rear and side: bins whose left/right correlation is low (ambience) are copied to the rear pair, which
 *     is then rotated, delayed by 15 ms and given a -10 dB shelf at 8 kHz; the sides (7.1) and the back
 *     centre come from the rear pair through short allpasses;
 *   - an "intensity" (-100..99 percent) sets the rear and side level; it slews 0.1 per block, starting from
 *     the quietest end, 0.99.
 * The output lags the input by one block (bs frames) on top of the STFT's half-frame.
 * Platform independent. */
#ifndef REAL_CMSS3_H
#define REAL_CMSS3_H

#include <stdint.h>

typedef struct rl_cmss3 rl_cmss3;

/* rate: the output rate (up to 192000); mask: the output's speaker bits.  For a stereo mask (0x3) the
 * upmixer runs ALchemy's quad path and the caller folds the rear pair (virt != 0 adds ALchemy's rear
 * allpasses, used for stereo speakers but not headphones).  NULL on failure. */
rl_cmss3 *rl_cmss3_create(uint32_t rate, uint32_t mask, int virt);
void      rl_cmss3_destroy(rl_cmss3 *c);
void      rl_cmss3_reset(rl_cmss3 *c);
/* the target intensity in percent (ALchemy's setting; 0 = neutral, -100 = most surround, 99 = least) */
void      rl_cmss3_set_intensity(rl_cmss3 *c, int percent);
/* the speaker bits the upmixer writes (the output mask without LFE; 0x33 for a stereo output) */
uint32_t  rl_cmss3_mask(const rl_cmss3 *c);
/* frames of delay between input and output */
uint32_t  rl_cmss3_latency(const rl_cmss3 *c);
/* run n frames: in L/R, out[b] for each speaker bit b (index = bit number, NULL to skip; written, not
 * added).  Bits the upmixer does not produce are zero-filled. */
void      rl_cmss3_process(rl_cmss3 *c, const float *l, const float *r, float *const out[12], uint32_t n);

#endif
