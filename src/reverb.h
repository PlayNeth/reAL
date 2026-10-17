/* reAL - environmental reverb.
 *
 * The engine is a port of ALchemy's own software reverb (alrv.c / alrv_params.c, verified sample
 * for sample against ALchemy running in an emulator).  This layer adds what ALchemy does around it
 * on a stereo device: the centre-copy fold into the rear pair and the "mode 3" stereo virtualizer
 * (fn 0x10036860) that turns the 7 reverb channels into two, plus click-free parameter changes.
 * Platform independent. */
#ifndef REAL_REVERB_H
#define REAL_REVERB_H

#include <stdint.h>
#include "alrv.h"
#include "speakers.h"

/* EAX 1.0 environment presets (environment, volume, decay, damping) — ALchemy .rdata 0x100a7ef0 */
typedef struct rl_eax1_preset { float volume, decay, damping; } rl_eax1_preset;
extern const rl_eax1_preset rl_eax1_presets[26];

typedef struct rl_virt {                 /* stereo virtualizer state */
    const void *tab;                     /* alrv_virt_tables*, NULL = plain fold-down */
    float  *hist;                        /* 4 FIR inputs (side sum/diff, back sum/diff) with history */
    uint32_t hist_cap, hist_len;
    float   bq[10][2];                   /* biquad states w1 w2 */
    float   dly[2][16]; uint32_t dpos;   /* crossfeed delay (<= 12 samples) */
} rl_virt;

typedef struct rl_reverb {
    uint32_t rate;
    alrv     eng[2];                     /* [cur] is fed; the other rings out after a change */
    int      cur;
    uint32_t ring_left;                  /* samples the old engine keeps ringing */
    uint32_t ring_total;
    alrv_params target;                  /* parameters of eng[cur] */
    float    decay;                      /* its decay time */
    float    gain_from[5], gain_cur[5];  /* input gains being ramped (room-type changes) */
    uint32_t ramp_left;
    rl_virt  virt;
    float   *scratch;                    /* 2 x 8 channel buffers */
    uint32_t scratch_frames;
    int      active;
} rl_reverb;

int  rl_reverb_init(rl_reverb *r, uint32_t rate);
void rl_reverb_free(rl_reverb *r);
/* EAX 1.0 listener settings (ALchemy's conversion; volume/decay are clamped by the caller) */
void rl_reverb_set_eax1(rl_reverb *r, uint32_t env, float volume, float decay, float damping);
/* any property set (for future EAX 2+ support) */
void rl_reverb_set_props(rl_reverb *r, const alrv_props *props);
/* in: mono send bus (frames), out: interleaved stereo, accumulated */
void rl_reverb_process(rl_reverb *r, const float *in, float *out, uint32_t frames);
/* the same into a speaker layout (out interleaved with lay->n channels, accumulated): the stereo virtualizer on
 * two channels, ALchemy's channel converter on more */
void rl_reverb_process_n(rl_reverb *r, const float *in, float *out, const rl_layout *lay, uint32_t frames);

/* ALchemy's EAX 1.0 damping -> decay HF ratio, and the derived room HF (mB); exposed for tests */
float rl_eax1_damping_to_hf_ratio(uint32_t env, float damping);
int   rl_eax1_room_hf(uint32_t env, float hf_ratio);

#endif
