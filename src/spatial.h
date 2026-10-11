/* reAL - placeholder DirectSound3D panning model (milestone 1).
 * Will be replaced by the HRTF path recovered from C3DStream / CHspHrtfFilterState. */
#ifndef REAL_SPATIAL_H
#define REAL_SPATIAL_H

#include <stdint.h>

typedef struct rl_vec3 { float x, y, z; } rl_vec3;

typedef struct rl_listener3d {
    rl_vec3 pos, vel, front, top;
    float distance_factor, rolloff_factor, doppler_factor;
} rl_listener3d;

typedef struct rl_source3d {
    rl_vec3 pos, vel, cone_orient;
    uint32_t cone_inside, cone_outside;   /* degrees */
    int32_t  cone_outside_volume;         /* hundredths of dB */
    float    min_dist, max_dist;
    uint32_t mode;                        /* DS3DMODE_* */
    int      mute_at_max;                 /* DSBCAPS_MUTE3DATMAXDISTANCE */
} rl_source3d;

#define RL_DS3DMODE_NORMAL       0
#define RL_DS3DMODE_HEADRELATIVE 1
#define RL_DS3DMODE_DISABLE      2

void rl_listener_defaults(rl_listener3d *l);
void rl_source_defaults(rl_source3d *s);

/* Everything the mixer and EAX need about one source as heard by the listener. */
typedef struct rl_geom {
    float pan[2];         /* stereo panning gains at unit level */
    float dist_gain;      /* DS3D distance attenuation (0 when muted at max distance) */
    float cone_gain;      /* sound cone attenuation */
    float x;              /* (min(dist, max) - min) / min: ALchemy's distance measure, 0 inside min */
    float cone_factor;    /* 0 inside the inner cone .. 1 outside the outer cone (1 in DISABLE mode) */
    int   muted;          /* beyond max distance with DSBCAPS_MUTE3DATMAXDISTANCE */
} rl_geom;

void rl_spatial_geometry(const rl_listener3d *l, const rl_source3d *s, rl_geom *g);

/* DirectSound3D Doppler as ALchemy computes it (fn 0x10043420): only the velocity components along
 * the listener-source line count, c = 342.38 m/s scaled by the distance factor, result 0..10.
 * factor: the listener's Doppler factor (EAX 3 adds the buffer's own). */
float rl_spatial_doppler(const rl_listener3d *l, const rl_source3d *s, float factor);

/* Microsoft DirectSound's software Doppler (Windows 7 dsound.dll 6efcbb49 with 6efcb2ff / 6efcb36d), which is what
 * DirectMusic's sink buffers got under ALchemy (DirectMusic played through the real DirectSound): only the relative
 * velocity along the listener-source line counts, speed of sound 359.66 m/s, the frequency truncated to whole Hz. */
uint32_t rl_spatial_doppler_ds(const rl_listener3d *l, const rl_source3d *s, uint32_t freq);
/* a sink buffer's frequency as the synth's pitch offset, as dsound passes it (6efd6bbe): trunc(1200 log2(f / rate)) */
int32_t rl_sink_cents(uint32_t freq, uint32_t rate);

/* Compute stereo gains (multiplied with volume elsewhere). */
/* dist_gain (may be NULL) receives the distance+cone attenuation without panning */
void rl_spatialize(const rl_listener3d *l, const rl_source3d *s, float gains[2], float *dist_gain);
/* direction of the source as heard by the listener: azimuth clockwise from front, elevation up (deg) */
void rl_spatial_direction(const rl_listener3d *l, const rl_source3d *s, float *az, float *el);

#endif
