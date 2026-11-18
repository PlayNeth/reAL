/* reAL - the mix converted to the device's integer formats, platform independent.
 *
 * ALchemy's (CClipper, which hands each channel to a kernel of CStaticHspSystem: 0x100789f0 for 16 bits, 0x10078dc0
 * for 32): the sample times 32768 (2^31), as a float, rounded to the nearest whole number, a half upwards, and
 * held within the format.  tools/convref.py runs these against the DLL's in an emulator.
 *
 * With `dither` (reAL's own, 16 bits only): before the rounding, noise of a triangular spread two steps wide.
 * Rounding alone leaves an error that follows the signal: a quiet sound or the tail of a fade comes out as
 * harmonics of itself, and below half a step as nothing at all.  With the noise the error is a steady hiss 96 dB
 * below full scale that does not depend on the signal, and a sound below one step is still there under it.  A
 * sample that is exactly 0 gets none: silence stays silence. */
#ifndef REAL_OUTCONV_H
#define REAL_OUTCONV_H

#include <stdint.h>

/* `n` samples; `dither`: NULL, or the noise generator's state (any value to start with) */
void rl_out_s16(int16_t *dst, const float *src, uint32_t n, uint32_t *dither);
/* (`fixed`: 0 gives ALchemy's 32 bits as they are: a sample of full scale exactly, 1.0, comes out as the lowest
 * value there is; above and below it is held as it should be) */
void rl_out_s32(int32_t *dst, const float *src, uint32_t n, int fixed);
/* one sample (for the paths that put samples one by one) */
int16_t rl_out_s16_one(float x, uint32_t *dither);
int32_t rl_out_s32_one(float x, int fixed);

#endif
