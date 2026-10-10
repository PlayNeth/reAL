/* reAL - platform independent voice mixing core.
 * No Windows headers here so it can be unit tested natively on Linux. */
#ifndef REAL_MIXCORE_H
#define REAL_MIXCORE_H

#include <stdint.h>
#include "hrtf.h"

#define RL_NOTIFY_STOP 0xFFFFFFFFu
#define RL_SENDS 4                  /* effect sends per voice (EAX 4 FX slots) */
#define RL_XFADE_PENDING 0xFFFFFFFFu

typedef struct rl_notify {
    uint32_t offset;       /* byte offset in buffer, or RL_NOTIFY_STOP */
    void    *event;        /* HANDLE on Windows */
} rl_notify;

typedef enum rl_sfmt { RL_U8, RL_S16, RL_S24, RL_S32, RL_F32 } rl_sfmt;

/* resampling: linear (ALchemy's) or a 32-tap Kaiser-windowed sinc (flat to 0.8 of the source's
 * Nyquist frequency, images and aliases >= 76 dB down; widened when the source plays faster than the
 * output rate so it does not alias, up to 4x).  Same-rate playback is copied through either way. */
enum { RL_INTERP_LINEAR = 0, RL_INTERP_SINC = 1 };

typedef struct rl_voice {
    /* source data (owned elsewhere) */
    const uint8_t *data;
    uint32_t size;          /* bytes, multiple of block */
    uint16_t channels;
    uint16_t block;         /* bytes per frame */
    rl_sfmt  sfmt;

    /* playback state */
    uint64_t pos;           /* 32.32 fixed point frame position */
    uint32_t rate;          /* current playback frequency in Hz */
    float    pitch;         /* Doppler shift, multiplies rate (0 is taken as 1) */
    int      interp;        /* RL_INTERP_* */
    int      playing;
    int      looping;

    /* final per-output-channel gains (stereo out) and the mono effect sends (one per FX slot) */
    float    gain[2];
    float    send_gain[RL_SENDS];
    /* EAX high-frequency damping: one-pole low-passes y += a (x - y) on the direct path and on the
     * reverb send (a = 1: off), as ALchemy applies the direct HF / room HF gains */
    float    lp_a, send_lp_a[RL_SENDS];
    float    lp_z[2], send_z[RL_SENDS];
    /* smoothing state (owned by the mixer) */
    float    cur_gain[2];
    float    cur_send[RL_SENDS];
    uint64_t xfade_pos;     /* old position being faded out after a jump */
    uint32_t xfade_left;    /* samples of crossfade left, or RL_XFADE_PENDING */

    /* HRTF (3D voices on headphones): gain[0] is the mono gain, filter chosen per direction */
    rl_hrtf_voice     *hrtf;
    const rl_hrtf_set *hrtf_set;

    /* notifications (sorted not required) */
    const rl_notify *notifies;
    uint32_t n_notifies;
} rl_voice;

typedef void (*rl_signal_fn)(void *event);

static inline uint32_t rl_voice_frames(const rl_voice *v) { return v->block ? v->size / v->block : 0; }
static inline uint32_t rl_voice_play_bytes(const rl_voice *v) { return (uint32_t)(v->pos >> 32) * v->block; }

/* Accumulate `frames` stereo float frames into `out` (interleaved L,R) at out_rate.
 * Advances the voice, fires notifications through `signal`.
 * Returns 1 if the voice stopped (reached end of a non-looping buffer). */
/* `sends` (may be NULL, as may each entry): RL_SENDS mono effect buses; bus k receives the voice scaled by
 * send_gain[k] through its own HF filter. */
int rl_voice_mix(rl_voice *v, float *out, float *const *sends, uint32_t frames, uint32_t out_rate, rl_signal_fn signal);

/* Start playing (snaps smoothed gains to target if the voice was stopped). */
void rl_voice_start(rl_voice *v);
/* Move the play position; a playing voice crossfades from the old position (declick). */
void rl_voice_jump(rl_voice *v, uint64_t newpos);

/* Fire notifications for a cursor movement from byte p0 to p1 having wrapped `wraps` times.
 * Exposed for tests. */
void rl_fire_notifies(const rl_voice *v, uint32_t p0, uint32_t p1, uint32_t wraps, rl_signal_fn signal);
void rl_fire_stop(const rl_voice *v, rl_signal_fn signal);

/* DirectSound volume/pan helpers (hundredths of dB). */
float rl_db100_to_gain(int32_t mb);
void  rl_pan_gains(int32_t pan, float out[2]);

#endif
