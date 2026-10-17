/* reAL - speaker layouts beyond stereo, as ALchemy's software engine renders them.
 *
 * ALchemy mixes into one buffer per speaker bit (KSAUDIO_SPEAKER_* bit order) and picks the layout from
 * Windows' speaker configuration (fn 0x1001c1a0): headphones and stereo 0x3, quad 0x33, 5.1 0x3f (also for
 * "5.1 surround"), 7.1 0x63f (also for "7.1 wide").  Sources whose channels the layout lacks go through its
 * channel converter (fn 0x10036740, modes 4..7); 3D sources on four or more speakers are split between the
 * front and the rear pair and fed directly to the centre, LFE and side speakers (fn 0x10034900).
 * Platform independent. */
#ifndef REAL_SPEAKERS_H
#define REAL_SPEAKERS_H

#include <stdint.h>

#define RL_MAXOUT 8                 /* channels in the largest layout (7.1) */

/* speaker bits (dwChannelMask) */
enum {
    RL_SPK_FL = 0x1, RL_SPK_FR = 0x2, RL_SPK_FC = 0x4, RL_SPK_LFE = 0x8, RL_SPK_BL = 0x10, RL_SPK_BR = 0x20,
    RL_SPK_FLC = 0x40, RL_SPK_FRC = 0x80, RL_SPK_BC = 0x100, RL_SPK_SL = 0x200, RL_SPK_SR = 0x400, RL_SPK_TC = 0x800,
};
#define RL_LAYOUT_STEREO 0x3u
#define RL_LAYOUT_QUAD   0x33u
#define RL_LAYOUT_51     0x3fu
#define RL_LAYOUT_71     0x63fu

/* a channel layout: speaker bit -> interleaved channel index (-1 when absent) */
typedef struct rl_layout {
    uint32_t mask, n;
    int8_t   idx[12];
} rl_layout;

void     rl_layout_init(rl_layout *l, uint32_t mask);
/* the mask DirectSound assumes for a plain WAVEFORMATEX with this many channels */
uint32_t rl_default_mask(uint32_t channels);
/* the ALchemy layout for an output of this many channels, and its GetSpeakerConfig value */
uint32_t rl_layout_for_channels(uint32_t channels);
uint32_t rl_layout_speaker_config(uint32_t layout, int headphones);

/* ALchemy's channel converter as a matrix: m[i][o] is the gain from the input layout's channel i to the
 * output layout's channel o.  *lfe_redirect is set when the input's LFE must go to the front pair through the
 * bass limiter (the output has no LFE: CBassBoostMixerSmallSpeakersAndLFE, fn 0x10034700). */
void rl_convert_matrix(const rl_layout *in, const rl_layout *out, float m[RL_MAXOUT][RL_MAXOUT], int *lfe_redirect);

/* the LFE redirection's limiter (kernel 0x1006bc70, set up by 0x1006bc30 with 10 / 50 and a 0.1778 threshold) */
typedef struct rl_bass_limiter { float att, rel, thr, env; } rl_bass_limiter;
void  rl_bass_limiter_init(rl_bass_limiter *b, uint32_t rate);
float rl_bass_limit(rl_bass_limiter *b, float x);
#define RL_LFE_IN_GAIN  0.5623413324356079f   /* 0x100ae8ac: -5 dB into the limiter */
#define RL_LFE_OUT_GAIN 3.34965443611145f     /* 0x100ae994: after it */

/* a 3D source on four or more speakers (fn 0x10034900 and the Q15 step 0x10035780): gains per speaker bit
 * index (0..11) for a source of level 1.  az: degrees, 0 ahead, positive to the right, +-180 behind;
 * el: degrees up; dist: the listener-source distance in metres (distance x distance factor).  The front and
 * rear pairs carry the source's left/right (HRTF or pan) signal, the others the mono signal. */
void rl_speaker_gains3d(uint32_t layout, uint32_t nchannels, float az, float el, double dist, float g[12]);

#endif
