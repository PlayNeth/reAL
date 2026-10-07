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

/* Compute stereo gains (multiplied with volume elsewhere). */
/* dist_gain (may be NULL) receives the distance+cone attenuation without panning */
void rl_spatialize(const rl_listener3d *l, const rl_source3d *s, float gains[2], float *dist_gain);

#endif
