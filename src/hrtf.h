/* reAL - HRTF rendering for 3D voices (headphones).
 * Data comes from reAL_hrtf.bin (built by tools/hrtf_build.py from an hrtfprobe measurement).
 * Platform independent. */
#ifndef REAL_HRTF_H
#define REAL_HRTF_H

#include <stdint.h>

#define RL_HRTF_MAX_TAPS 256          /* 64 measured taps at 48 kHz resampled for 192 kHz */
#define RL_HRTF_HIST     1024         /* power of two, > taps + max delay */
#define RL_HRTF_MAX_RINGS 16

typedef struct rl_hrtf_filter {
    float delay[2];                   /* samples */
    float coef[2][RL_HRTF_MAX_TAPS];
} rl_hrtf_filter;

typedef struct rl_hrtf_set {
    uint32_t rate, taps, n_rings;
    float    ring_el[RL_HRTF_MAX_RINGS];
    uint32_t ring_naz[RL_HRTF_MAX_RINGS];
    uint32_t ring_first[RL_HRTF_MAX_RINGS];   /* index of the ring's first direction */
    uint32_t n_dirs;
    rl_hrtf_filter *dirs;
} rl_hrtf_set;

typedef struct rl_hrtf_voice {
    float    hist[RL_HRTF_HIST * 2];   /* doubled so every window is contiguous */
    uint32_t w;
    rl_hrtf_filter cur, target;        /* coefficients stored reversed, for a forward dot product */
    int      have_cur, dirty;
    uint32_t quiet;                    /* zero inputs in a row (saturating): at RL_HRTF_HIST the history is all zero */
} rl_hrtf_voice;

/* load from memory (file contents); resamples to out_rate if needed. returns 0 on error */
int  rl_hrtf_load(rl_hrtf_set *set, const void *data, uint32_t size, uint32_t out_rate);
void rl_hrtf_free(rl_hrtf_set *set);

/* interpolated filter for a direction: az degrees clockwise from front (0..360), el degrees up */
void rl_hrtf_lookup(const rl_hrtf_set *set, float az, float el, rl_hrtf_filter *out);

void rl_hrtf_voice_reset(rl_hrtf_voice *v);
void rl_hrtf_voice_set(rl_hrtf_voice *v, const rl_hrtf_filter *f);
/* a (re)started voice: no history from its previous play, the current direction without a crossfade */
void rl_hrtf_voice_restart(rl_hrtf_voice *v);
/* convolve mono `in` into interleaved stereo `out` (accumulate); crossfades on filter change.  `in` may be NULL for
 * silence.  Silence after a whole history of silence adds exactly nothing, so it is skipped: returns 1 then. */
int  rl_hrtf_render(rl_hrtf_voice *v, const rl_hrtf_set *set, const float *in, float *out, uint32_t frames);

#endif
