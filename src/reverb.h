/* reAL - placeholder reverb (milestone 2).
 * A small feedback delay network driven by EAX 1.0 parameters, so EAX games get audible reverb
 * now. To be replaced by a port of ALchemy's own CHRvbReverb. Platform independent. */
#ifndef REAL_REVERB_H
#define REAL_REVERB_H

#include <stdint.h>

#define RL_FDN_LINES 4
#define RL_AP_STAGES 2

typedef struct rl_reverb {
    uint32_t rate;
    float   *mem;               /* one allocation for all delay lines */
    float   *line[RL_FDN_LINES];
    uint32_t len[RL_FDN_LINES], pos[RL_FDN_LINES];
    float    fb[RL_FDN_LINES];  /* broadband feedback gain per line */
    float    lp_a[RL_FDN_LINES], lp_z[RL_FDN_LINES];
    float   *ap[RL_AP_STAGES];
    uint32_t ap_len[RL_AP_STAGES], ap_pos[RL_AP_STAGES];
    float   *pre;
    uint32_t pre_len, pre_pos;
    float    out_gain;
    int      active;
} rl_reverb;

/* EAX 1.0 environment presets (verified against ALchemy's table @ .rdata 0x100a7ef0) */
typedef struct rl_eax1_preset { float volume, decay, damping; } rl_eax1_preset;
extern const rl_eax1_preset rl_eax1_presets[26];

int  rl_reverb_init(rl_reverb *r, uint32_t rate);
void rl_reverb_free(rl_reverb *r);
/* decay: seconds (0.1-20), hf_ratio: high-frequency decay ratio (0.1-2), volume: linear 0-1 */
void rl_reverb_set(rl_reverb *r, float decay, float hf_ratio, float volume);
/* in: mono send bus (frames), out: interleaved stereo, accumulated */
void rl_reverb_process(rl_reverb *r, const float *in, float *out, uint32_t frames);

#endif
