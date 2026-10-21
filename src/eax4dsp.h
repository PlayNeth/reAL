/* reAL - DSP of the EAX 4 effects other than reverb, ported from ALchemy's CPlug* classes.
 * Each effect runs on ALchemy's 2-channel effect bus; parameters ramp per sample like ALchemy's.
 * Platform independent; build without -ffast-math (the arithmetic follows ALchemy's x87 code). */
#ifndef REAL_EAX4DSP_H
#define REAL_EAX4DSP_H
#include <stdint.h>

typedef struct fx_distortion {        /* CPlugDistortion state (ALchemy offsets from plug + 8) */
    float rate, ramp_mix;             /* 0x4c, 0xb0 */
    float edge_norm, eq_norm, gain;   /* 0x44, 0x48, 0x50 */
    float out_t, out;                 /* 0x54 -> 0x58: overall gain */
    float center, bandwidth;          /* 0x5c, 0x60 */
    float lpg_t, lpg, lpp_t, lpp;     /* 0x64 -> 0x68 input weight, 0x6c -> 0x70 pole */
    float shape_t, shape;             /* 0x74 -> 0x78 */
    float eqc_t, eqc, eqb_t, eqb;     /* 0x7c -> 0x80, 0x84 -> 0x88 */
    float mix_t, mix;                 /* 0x8c -> 0x90 */
    float cutoff;                     /* 0x94 */
    uint32_t flags;                   /* 0xb4: parameters still ramping */
    float st[2][3];                   /* 0x98: per channel low-pass, EQ states */
} fx_distortion;

void fxd_init(fx_distortion *d, uint32_t rate);
void fxd_edge(fx_distortion *d, float edge);
void fxd_gain(fx_distortion *d, int32_t gain_mb);
void fxd_cutoff(fx_distortion *d, float hz);
void fxd_eq(fx_distortion *d, float center, float bandwidth);
void fxd_process(fx_distortion *d, float *const ch[2], uint32_t frames);

/* first-order high-pass of ALchemy's DSP kernels (coefficients fn 0x1006f800, kernel #97 0x10071640) */
typedef struct hsp_hpf { float m, k, z; } hsp_hpf;
void hsp_hpf_set(hsp_hpf *f, float f_over_fs);
void hsp_hpf_run(hsp_hpf *f, float *buf, uint32_t n);

typedef struct fx_ringmod {           /* CPlugRingModulator (offsets from the plug) */
    uint32_t irate;                   /* 0x44 */
    float freq, hpf_hz;               /* 0x48, 0x4c */
    float hp_cur, hp_t;               /* 0x50 -> 0x54: f/fs, glides x1.01 per 8 samples */
    uint32_t waveform;                /* 0x58 */
    float phase, inc, two_over_fs;    /* 0x5c oscillator */
    hsp_hpf hp[2];
    int alchemy_bug;                  /* tests: reproduce ALchemy's left-only processing */
} fx_ringmod;

void fxr_init(fx_ringmod *r, uint32_t rate);
void fxr_freq(fx_ringmod *r, float hz);
void fxr_highpass(fx_ringmod *r, float hz);
void fxr_waveform(fx_ringmod *r, uint32_t w);
void fxr_process(fx_ringmod *r, float *const ch[2], uint32_t frames);

typedef struct fx_fshift {             /* CPlugFrequencyShifter (offsets from the plug) */
    uint32_t dir[2];                  /* 0x44: per channel 0 down, 1 up, 2 off (channel left dry) */
    float rate;                       /* 0x4c */
    float c[6];                       /* 0x50: two 3-stage allpass chains (90 degree phase pair) */
    float st[7];                      /* 0x68: chain states, 0x80 one-sample delay of chain B input */
    float inc, phase;                 /* 0x84 rad/sample, 0x88 samples since wrap */
    int alchemy_bug;                  /* tests: ALchemy's doubled shift */
} fx_fshift;

void fxf_init(fx_fshift *f, uint32_t rate);
void fxf_freq(fx_fshift *f, float hz);
void fxf_dir(fx_fshift *f, uint32_t left, uint32_t right);
void fxf_process(fx_fshift *f, float *const ch[2], uint32_t frames);

#define FXA_DLEN 9600
typedef struct fx_agc {               /* CPlugAGCCompressor (offsets from the plug, + 0x4b000) */
    float rate;                       /* 0c0 */
    int on;                           /* 08c */
    float thr, ratio, makeup, onv;    /* 0a4 threshold (log10 power), 098, 09c, 0a8 */
    float save_on, save_mk;           /* 0b4, 0b8 */
    uint32_t save_d, d1, d2;          /* 0bc, 084, 088: look-ahead delays (samples) */
    float c_att, c_rel, c_pow, c_gs;  /* 090, 094, 0ac, 0b0 */
    float hb, ha;                     /* 0c8, 0cc: detector DC blocker */
    float cf, cf_step;                /* 0d0, 0d4: delay cross-fade */
    int fade_up, fade_dn;             /* 0c4, 0c5 */
    float look, rel_t, att_t;         /* 144, 148, 14c */
    float E, H, Gs;                   /* 138, 13c, 140 */
    float hp[2][2], P[2];             /* 0d8, 118 */
    uint32_t w[2];
    float buf[2][FXA_DLEN];
    int alchemy_bug;                  /* tests: ALchemy's off -> on look-ahead restore */
} fx_agc;

void fxa_init(fx_agc *a, uint32_t rate);
void fxa_onoff(fx_agc *a, int on);
void fxa_process(fx_agc *a, float *const ch[2], uint32_t frames);

typedef struct eq_filter {            /* CParamEqFilter{LowFreq,Peak,HighFreq} */
    int type;                         /* 0 low shelf, 1 peak, 2 high shelf */
    float c[5];                       /* +4: -a1, -a2, b0, b1, b2 (direct form II) */
    float s1, s2;                     /* +0x18 */
    float g, gt, F, Ft, hz, fs, q, qt;/* +0x20: gain, f/fs and Q glide towards their targets every 8 samples */
} eq_filter;

typedef struct fx_eq {                /* CPlugEqualiser: 2 channels x {low, mid1, mid2, high} */
    float rate;
    eq_filter f[2][4];
} fx_eq;

void fxe_init(fx_eq *e, uint32_t rate);
void fxe_gain(fx_eq *e, int band, int32_t mb);
void fxe_freq(fx_eq *e, int band, float hz);
void fxe_width(fx_eq *e, int band, float octaves);   /* bands 1, 2 */
void fxe_process(fx_eq *e, float *const ch[2], uint32_t frames);

typedef struct fx_autowah {            /* CPlugAutowah (offsets from the plug) */
    float fs, b, env, a;              /* 0x44, 0x7c, 0x80 (envelope, log10), 0x78 (all-pass coefficient) */
    float cur[5], tgt[5], step[5];    /* attack, release, resonance, peak, mix: 0x48.., 0x60.., 0x98.. */
    uint32_t flags;                   /* 0x94: bits 1, 2, 0x20, 0x80, 0x10 */
    float att_time, rel_time;         /* 0xac, 0xb0 */
    float P[2][2];                    /* 0x84 */
} fx_autowah;

void fxw_init(fx_autowah *w, uint32_t rate);
void fxw_attack(fx_autowah *w, float s);
void fxw_release(fx_autowah *w, float s);
void fxw_resonance(fx_autowah *w, int32_t mb);
void fxw_peak(fx_autowah *w, int32_t mb);
void fxw_process(fx_autowah *w, float *const ch[2], uint32_t frames);

typedef struct fx_echo {               /* CPlugEcho (offsets from the plug): mono in (left), stereo out */
    uint32_t rate;                    /* 0x44 */
    float delay, lrdelay;             /* 0x48, 0x4c */
    float damp, fb, damp_t, fb_t;     /* 0x50, 0x54 -> 0x5c, 0x60 */
    float spread;                     /* 0x58 */
    uint32_t c1, n1; float xf1;       /* 0x64, 0x68, 0x6c: tap 1 (delay), cross-faded on change */
    uint32_t c2, n2; float xf2;       /* 0x70, 0x74, 0x78: tap 2 (delay + lrdelay), feeds back */
    float sp, sp_t, xstep;            /* 0x7c -> 0x80, 0x84 */
    float coef, lp;                   /* 0x9c damping low-pass, 0xa0 */
    float *buf; uint32_t len, wp;     /* 0x90, 0x94, 0x98 */
} fx_echo;

int fxo_init(fx_echo *o, uint32_t rate);   /* allocates; 0 on failure */
void fxo_free(fx_echo *o);
void fxo_activate(fx_echo *o);              /* slot activation: parameters jump to their targets */
void fxo_delay(fx_echo *o, float s);
void fxo_lrdelay(fx_echo *o, float s);
void fxo_damping(fx_echo *o, float v);
void fxo_feedback(fx_echo *o, float v);
void fxo_spread(fx_echo *o, float v);
void fxo_process(fx_echo *o, float *const ch[2], uint32_t frames);

#define FXC_DLEN 6160              /* ALchemy: 3097, too short for 0.016 s at 192 kHz; only relative indices matter */
typedef struct fx_osc { float phase, inc, two_over_fs; int tri; } fx_osc;
typedef struct fx_chorus {             /* CPlugChorus (offsets from the plug): mono in (left), stereo out */
    float fs, step;                   /* 0x134, 0x3270: cross-fade / ramp step = 1 / (0.02 fs) */
    fx_osc osc[2];                    /* 0x44 + 0x14 k */
    float *buf; uint32_t len, wp;     /* 0x140, 0x31a4: inl, or allocated by fxc_init above 192 kHz (fxc_free) */
    float inl[FXC_DLEN];
    float ph_cur, ph_new, phase;      /* 0x31a8, 0x31ac (offset of the right LFO, units of pi), 0x31b0 degrees */
    float depth, amt_cur, amt_new;    /* 0x31b4, 0x31b8, 0x31bc: modulation depth in samples */
    float delay;                      /* 0x31c0 */
    float d_cur, d_new, fd_cur, fd_new, fxf;   /* 0x31c4.. centre delay (taps / feedback tap), 0x31d4 */
    float xf[2];                      /* 0x31d8 */
    float fb, fb_t;                   /* 0x3208, 0x320c */
    float mix[2], mix_t[2];           /* 0x3210, 0x3240 */
    uint32_t flags[2];                /* 0x3278 */
    int running;                      /* 0x32a8 */
} fx_chorus;

void fxc_init(fx_chorus *c, uint32_t rate);
void fxc_free(fx_chorus *c);
void fxc_waveform(fx_chorus *c, uint32_t tri);
void fxc_phase(fx_chorus *c, int32_t deg);
void fxc_rate(fx_chorus *c, float hz);
void fxc_depth(fx_chorus *c, float d);
void fxc_feedback(fx_chorus *c, float fb);
void fxc_delay(fx_chorus *c, float s);
void fxc_process(fx_chorus *c, float *const ch[2], uint32_t frames);

#define FXL_DLEN 2048              /* ALchemy: 778 per channel, too short for 0.004 s at 192 kHz */
typedef struct fx_flanger {            /* CPlugFlanger: like the chorus, but stereo with a delay line per channel */
    float fs, step;                   /* 0x134, 0x1ae8 */
    fx_osc osc[2];                    /* 0x44 */
    float *buf[2]; uint32_t len, wp[2];       /* 0x140 + 0xc30 k: inl, or allocated by fxl_init above 192 kHz (fxl_free) */
    float inl[2][FXL_DLEN];
    float ph_cur, ph_new, phase;      /* 0x199c, 0x19a0, 0x19a4 */
    float depth, amt_cur, amt_new;    /* 0x19a8, 0x19ac, 0x19b0 */
    float delay;                      /* 0x19b4 */
    float d_cur, d_new, fd_cur, fd_new;   /* 0x19b8.. */
    float fxf[2], xf[2];              /* 0x19c8, 0x19f8 */
    float fb[2], fb_t[2];             /* 0x1a28, 0x1a58 */
    float mix[2], mix_t[2];           /* 0x1a88, 0x1ab8 */
    uint32_t flags[2];                /* 0x1af0 */
    int running;                      /* 0x1b20 */
} fx_flanger;

void fxl_init(fx_flanger *f, uint32_t rate);
void fxl_free(fx_flanger *f);
void fxl_waveform(fx_flanger *f, uint32_t tri);
void fxl_phase(fx_flanger *f, int32_t deg);
void fxl_rate(fx_flanger *f, float hz);
void fxl_depth(fx_flanger *f, float d);
void fxl_feedback(fx_flanger *f, float fb);
void fxl_delay(fx_flanger *f, float s);
void fxl_process(fx_flanger *f, float *const ch[2], uint32_t frames);

#define FXP_DLEN 2052
typedef struct fx_pshift {             /* CPlugPitchShifter (offsets from the plug): mono in, same out everywhere */
    float buf[FXP_DLEN]; uint32_t wp; /* 0xb4, 0x20c4 */
    int32_t semis, cents;             /* 0x20c8, 0x20cc */
    float ofs;                        /* 0x20d0: 1026 samples */
    double win_inc, rate;             /* 0x70, 0x78: window angle per sample, delay change per sample */
    double n, p;                      /* 0x80 samples into the window cycle, 0x88 tap position (-1024..1024) */
    double rate_new, win_new;         /* 0x98, 0xa0: applied when the window cycle wraps */
    int alchemy_bug;
} fx_pshift;

void fxp_init(fx_pshift *p, uint32_t rate, int alchemy_bug);
void fxp_set(fx_pshift *p, int32_t semitones, int32_t cents);
void fxp_process(fx_pshift *p, float *const ch[2], uint32_t frames);

typedef struct fx_vmorph {             /* CPlugVocalMorpher (offsets from the plug) */
    float fs;                         /* 0x44 */
    uint32_t wf; float ofs, x, y, dir, coef, rate;   /* LFO 0x4c: waveform, 0x50 fs, 0x5c value, 0x60, 0x58, 0x64, 0x54 */
    float lp[2][4], bp[2][4];         /* 0x6c, 0x8c: state-variable filters per channel and formant */
    float fa[4], fb[4], qa[4], qb[4]; /* 0xac, 0xbc: 2 sin(pi f / fs); 0xcc, 0xdc: 1 / Q */
    uint32_t pa, pb; int32_t ca, cb;  /* 0xec, 0xf4 phonemes, 0xf0, 0xf8 coarse tuning (semitones) */
    float mix;                        /* 0x100 */
    int alchemy_bug;
} fx_vmorph;

void fxv_init(fx_vmorph *v, uint32_t rate, int alchemy_bug);
void fxv_reset(fx_vmorph *v);                  /* slot activation (state 3) */
void fxv_phoneme_a(fx_vmorph *v, uint32_t ph);
void fxv_coarse_a(fx_vmorph *v, int32_t semis);
void fxv_phoneme_b(fx_vmorph *v, uint32_t ph);
void fxv_coarse_b(fx_vmorph *v, int32_t semis);
void fxv_waveform(fx_vmorph *v, uint32_t wf);
void fxv_rate(fx_vmorph *v, float hz);
void fxv_process(fx_vmorph *v, float *const ch[2], uint32_t frames);

/* ---- an EAX 4 FX slot's effect: type (EAX4FX_*), properties as stored by eax4fx.c ---- */
typedef struct eax4fx_unit eax4fx_unit;
eax4fx_unit *eax4fx_unit_new(int type, uint32_t rate);         /* NULL for reverb / none / no memory */
void eax4fx_unit_free(eax4fx_unit *u);
int eax4fx_unit_type(const eax4fx_unit *u);
uint32_t eax4fx_unit_rate(const eax4fx_unit *u);
void eax4fx_unit_props(eax4fx_unit *u, const uint32_t fx[16]); /* applies the properties that changed */
/* mono effect bus in, stereo out (adds gain * effect output to out[2 * i], out[2 * i + 1]); bus is not modified */
void eax4fx_unit_process(eax4fx_unit *u, const float *bus, float *out, float gain, uint32_t frames);

#endif
