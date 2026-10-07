/* reAL - platform independent voice mixing core.
 * No Windows headers here so it can be unit tested natively on Linux. */
#ifndef REAL_MIXCORE_H
#define REAL_MIXCORE_H

#include <stdint.h>

#define RL_NOTIFY_STOP 0xFFFFFFFFu

typedef struct rl_notify {
    uint32_t offset;       /* byte offset in buffer, or RL_NOTIFY_STOP */
    void    *event;        /* HANDLE on Windows */
} rl_notify;

typedef enum rl_sfmt { RL_U8, RL_S16, RL_S24, RL_S32, RL_F32 } rl_sfmt;

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
    int      playing;
    int      looping;

    /* final per-output-channel gains (stereo out) and mono reverb send gain */
    float    gain[2];
    float    send_gain;

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
/* `send` (mono, may be NULL) receives the voice scaled by send_gain for the reverb bus. */
int rl_voice_mix(rl_voice *v, float *out, float *send, uint32_t frames, uint32_t out_rate, rl_signal_fn signal);

/* Fire notifications for a cursor movement from byte p0 to p1 having wrapped `wraps` times.
 * Exposed for tests. */
void rl_fire_notifies(const rl_voice *v, uint32_t p0, uint32_t p1, uint32_t wraps, rl_signal_fn signal);
void rl_fire_stop(const rl_voice *v, rl_signal_fn signal);

/* DirectSound volume/pan helpers (hundredths of dB). */
float rl_db100_to_gain(int32_t mb);
void  rl_pan_gains(int32_t pan, float out[2]);

#endif
