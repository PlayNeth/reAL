/* reAL - port of ALchemy's software reverb (CHRvbReverb, "Creative Software 3D Library").
 *
 * Three layers, mirroring ALchemy:
 *   alrv_props  - the reverb property set ALchemy keeps per listener (linear units, 23 floats).
 *                 Filled from EAX 1.0 by alrv_props_from_eax1() (ALchemy fn 0x1004a050).
 *   alrv_params - the engine's internal parameter block (348 bytes, verbatim layout), computed from
 *                 the properties by alrv_compute_params() (CHRvbParamUpdate, fn 0x10060c50).
 *   alrv        - the DSP: input filters, early-reflection network, 4-line FDN late reverb with
 *                 nested allpasses, rotation matrices and a modulated output delay (fn 0x1005de10).
 * Output is ALchemy's 8-slot layout, already scaled by the engine's x40.
 * Platform independent (no Windows headers). */
#ifndef REAL_ALRV_H
#define REAL_ALRV_H

#include <stdint.h>

enum { ALRV_FL, ALRV_FR, ALRV_FC, ALRV_BL, ALRV_BR, ALRV_CX, ALRV_SL, ALRV_SR, ALRV_NOUT };

typedef struct alrv_props {           /* ALchemy's per-listener reverb properties (offsets in the struct) */
    float environment;                /* 0x00 (unused by the engine) */
    float size;                       /* 0x04 min(env_size^3 / 16, 1) */
    float diffusion;                  /* 0x08 0..1 */
    float room;                       /* 0x0c linear */
    float room_hf;                    /* 0x10 linear */
    float room_lf;                    /* 0x14 linear */
    float decay;                      /* 0x18 s */
    float decay_hf;                   /* 0x1c s (decay * hf ratio) */
    float decay_lf;                   /* 0x20 s */
    float reflections;                /* 0x24 linear */
    float reflections_delay;          /* 0x28 s */
    float reflections_pan[2];         /* 0x2c */
    float reverb;                     /* 0x34 linear */
    float reverb_delay;               /* 0x38 s */
    float reverb_pan[2];              /* 0x3c */
    float echo_time, echo_depth;      /* 0x44 0x48 */
    float mod_time, mod_depth;        /* 0x4c 0x50 */
    float hf_ref, lf_ref;             /* 0x54 0x58 Hz */
} alrv_props;

typedef struct alrv_params {          /* CHRvbReverb parameter block, 0x15c bytes */
    float   room;                     /* 0x000 */
    float   in_lp_gain, in_lp_f;      /* 0x004 room HF: gain, f/fs */
    float   in_hp_gain, in_hp_f;      /* 0x00c room LF */
    float   line_g[4];                /* 0x014 per-pass gain of each FDN line */
    float   line_hf_f[4];             /* 0x024 */
    float   line_hf_ratio[4];         /* 0x034 */
    float   line_shelf_g[4];          /* 0x044 */
    float   line_shelf_f[4];          /* 0x054 */
    int32_t line_shelf_high[4];       /* 0x064 */
    int32_t refl_delay;               /* 0x074 samples */
    int32_t late_tap_delay[4];        /* 0x078 */
    float   refl_gain;                /* 0x088 */
    float   late_tap_gain[4];         /* 0x08c */
    int32_t eap_delay[4];             /* 0x09c early allpasses */
    float   eap_gain[4];              /* 0x0ac */
    float   eap_coef;                 /* 0x0bc */
    int32_t lap_delay[4];             /* 0x0c0 late allpasses */
    int32_t fdn_delay[4];             /* 0x0d0 */
    float   lap_gain[4];              /* 0x0e0 */
    float   fdn_gain[4];              /* 0x0f0 */
    float   lap_coef;                 /* 0x100 */
    float   fdn_rot, lap_rot;         /* 0x104 0x108 rotation (tan theta) */
    int32_t mod_depth;                /* 0x10c samples */
    float   mod_freq;                 /* 0x110 */
    float   early_out[9];             /* 0x114 */
    float   late_out[9];              /* 0x138 */
} alrv_params;

#define ALRV_FADE 128   /* ALchemy ramps the input in over its first block (kernel 0xb8) */

typedef struct alrv_ring { float *buf; uint32_t len, pos; } alrv_ring;

typedef struct alrv {
    uint32_t rate;
    float   *mem;
    /* fixed (rate dependent) structure */
    uint32_t early_off[4];            /* early tap offsets added to the reflections delay */
    uint32_t eout_delay[3];           /* early output delays (BR, FL, BL) */
    uint32_t lout_delay[4];           /* late output delays (FL, SR, FR, BL) */
    uint32_t mod_len, mod_tap[2];     /* modulated delay ring and its fixed taps */
    alrv_ring in, eap[4], eout[3], fdn[4], lap[4], lout[2];
    float   *modbuf; uint32_t mod_wp;
    /* current parameters */
    alrv_params p;
    uint32_t etap[4];                 /* early tap delays */
    float    hp_a, lp_a;              /* input filters */
    float    shelf_c0[4], shelf_c1[4], shelf_a[4];
    float    damp_a[4];
    /* filter state */
    float    hp_s, lp_v, lp_w;
    float    shelf_t[4], damp_y[4];
    float    osc_x, osc_y, mod_amp;
    int      have_params;
    uint32_t fade_pos;                /* < ALRV_FADE: input is still being faded in after a (re)start */
} alrv;

/* properties */
void alrv_props_default(alrv_props *e);
/* EAX 1.0 listener: environment (0..25), volume 0..1, decay s, damping (ALchemy fn 0x1004a050) */
void alrv_props_from_eax1(alrv_props *e, uint32_t env, float volume, float decay, float damping);

void alrv_compute_params(alrv_params *p, const alrv_props *e, uint32_t rate);

int  alrv_init(alrv *r, uint32_t rate);
void alrv_free(alrv *r);
void alrv_set_params(alrv *r, const alrv_params *p);
void alrv_reset(alrv *r);
/* in: mono, out: ALRV_NOUT separate channel buffers, overwritten */
void alrv_process(alrv *r, const float *in, float *const out[ALRV_NOUT], uint32_t frames);

/* coefficient helpers (exposed for tests) */
float alrv_coef_hp(float g, float f);
float alrv_coef_lp(float g, float f);
float alrv_coef_damp(float g, float f, float r);
void  alrv_coef_shelf(float g, float f, int high, float *c0, float *c1, float *a);

#endif
