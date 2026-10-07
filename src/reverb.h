/* reAL - reverb (milestone 2).
 *
 * Parameters and the EAX 1.0 -> parameter conversion are taken from ALchemy
 * (fn 0x1004a050, preset table .rdata 0x100a8280, damping constants 0x100a7e20 / 0x100a7e88).
 * The DSP itself is still our own: pre-delay, early reflection taps, input diffusion and a
 * 4-line feedback delay network. It will be replaced by a port of CHRvbReverb. Platform independent. */
#ifndef REAL_REVERB_H
#define REAL_REVERB_H

#include <stdint.h>

#define RL_FDN_LINES 4
#define RL_AP_STAGES 2
#define RL_ER_TAPS   6

/* EAX 2/3-style listener reverb parameters (subset the DSP uses) */
typedef struct rl_reverb_params {
    float room;            /* linear gain on everything (EAX1: volume) */
    int   room_hf;         /* mB attenuation at hf_ref */
    float decay;           /* s */
    float decay_hf_ratio;
    int   reflections;     /* mB */
    float reflections_delay;  /* s */
    int   reverb;          /* mB, late reverb level */
    float reverb_delay;    /* s, after the reflections */
    float env_size;        /* m */
    float diffusion;       /* 0..1 */
    float hf_ref;          /* Hz */
} rl_reverb_params;

typedef struct rl_reverb {
    uint32_t rate;
    float   *mem;
    float   *line[RL_FDN_LINES];
    uint32_t cap[RL_FDN_LINES], len[RL_FDN_LINES], pos[RL_FDN_LINES];
    float    fb[RL_FDN_LINES], hf_g[RL_FDN_LINES], lp_z[RL_FDN_LINES];
    float    xover_a;           /* one-pole crossover at hf_ref for the HF shelves */
    float   *ap[RL_AP_STAGES];
    uint32_t ap_cap[RL_AP_STAGES], ap_len[RL_AP_STAGES], ap_pos[RL_AP_STAGES];
    float    ap_g;
    float   *pre;               /* input delay line (feeds reflections and late reverb) */
    uint32_t pre_cap, pre_pos;
    uint32_t er_off[RL_ER_TAPS];
    float    er_gain[RL_ER_TAPS][2];
    uint32_t late_off;
    float    in_hf_g, in_z;     /* room HF shelf on the input */
    float    room, late_gain;
    int      active;
} rl_reverb;

/* EAX 1.0 environment presets (environment, volume, decay, damping) — ALchemy .rdata 0x100a7ef0 */
typedef struct rl_eax1_preset { float volume, decay, damping; } rl_eax1_preset;
extern const rl_eax1_preset rl_eax1_presets[26];

int  rl_reverb_init(rl_reverb *r, uint32_t rate);
void rl_reverb_free(rl_reverb *r);
void rl_reverb_set_params(rl_reverb *r, const rl_reverb_params *p);
/* in: mono send bus (frames), out: interleaved stereo, accumulated */
void rl_reverb_process(rl_reverb *r, const float *in, float *out, uint32_t frames);

/* ALchemy's EAX 1.0 -> reverb parameter conversion */
void rl_eax1_to_params(uint32_t env, float volume, float decay, float damping, rl_reverb_params *p);
/* exposed for tests: damping -> decay HF ratio, and the derived room HF (mB) */
float rl_eax1_damping_to_hf_ratio(uint32_t env, float damping);
int   rl_eax1_room_hf(uint32_t env, float hf_ratio);

#endif
