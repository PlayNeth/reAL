/* reAL - the mix converted to the device's integer formats.  See outconv.h.
 *
 * Built without -ffast-math (build.sh): the rounding is ALchemy's, to the bit. */
#include <math.h>
#include "outconv.h"

/* ALchemy rounds with x + 1.5e-8f + 1.5 * 2^52 in a double, whose low half is then the whole number: the nearest,
 * and the 1.5e-8 takes an exact half upwards (a float of this size is never within 1.5e-8 of a half without being
 * one).  That is floor(x + 0.5): the same for every float there is, within the range (checked on all of them). */
static inline double nearest(float v) { return floor((double)v + 0.5); }

int16_t rl_out_s16_one(float x, uint32_t *dither)
{
    float v = x * 32768.0f;
    if (dither && x != 0.0f) {
        /* two even spreads of one step each, summed: -1 to 1 step, most often near 0 */
        uint32_t s = *dither ? *dither : 0x9e3779b9u;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        *dither = s;
        v += (float)((int32_t)(s & 0xffff) - (int32_t)(s >> 16)) * (1.0f / 65536.0f);
    }
    if (!(v < 32767.5f)) return v != v ? 0 : 32767;        /* (a NaN: nothing) */
    if (v < -32768.0f) return -32768;
    return (int16_t)nearest(v);
}

int32_t rl_out_s32_one(float x, int fixed)
{
    const float v = x * 2147483648.0f;
    /* (ALchemy's test lets full scale exactly through to the rounding, where it wraps to the lowest value) */
    if (!fixed && v == 2147483648.0f) return -2147483647 - 1;
    if (!(v < 2147483648.0f)) return v != v ? 0 : 2147483647;
    if (v < -2147483648.0f) return -2147483647 - 1;
    return (int32_t)nearest(v);
}

void rl_out_s16(int16_t *dst, const float *src, uint32_t n, uint32_t *dither)
{
    for (uint32_t i = 0; i < n; i++) dst[i] = rl_out_s16_one(src[i], dither);
}

void rl_out_s32(int32_t *dst, const float *src, uint32_t n, int fixed)
{
    for (uint32_t i = 0; i < n; i++) dst[i] = rl_out_s32_one(src[i], fixed);
}
