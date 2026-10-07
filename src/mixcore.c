#include "mixcore.h"
#include <math.h>
#include <string.h>

float rl_db100_to_gain(int32_t mb)
{
    if (mb <= -10000) return 0.0f;
    if (mb >= 0) return 1.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

/* DirectSound pan: positive pan attenuates the left channel, negative the right. */
void rl_pan_gains(int32_t pan, float out[2])
{
    out[0] = pan > 0 ? rl_db100_to_gain(-pan) : 1.0f;
    out[1] = pan < 0 ? rl_db100_to_gain(pan) : 1.0f;
}

static inline float fetch(const rl_voice *v, uint32_t frame, int ch)
{
    if (ch >= v->channels) ch = v->channels - 1;
    const uint8_t *p = v->data + (size_t)frame * v->block;
    switch (v->sfmt) {
    case RL_U8:  return ((int)p[ch] - 128) * (1.0f / 128.0f);
    case RL_S16: { int16_t s; memcpy(&s, p + ch * 2, 2); return s * (1.0f / 32768.0f); }
    case RL_S24: { const uint8_t *q = p + ch * 3;
                   int32_t s = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24);
                   return (s >> 8) * (1.0f / 8388608.0f); }
    case RL_S32: { int32_t s; memcpy(&s, p + ch * 4, 4); return s * (1.0f / 2147483648.0f); }
    case RL_F32: { float f; memcpy(&f, p + ch * 4, 4); return f; }
    }
    return 0.0f;
}

static void fire_range(const rl_voice *v, uint32_t lo, uint32_t hi, int incl_lo, rl_signal_fn signal)
{
    /* fire offsets in (lo, hi]  (or [lo, hi] if incl_lo) */
    for (uint32_t i = 0; i < v->n_notifies; i++) {
        uint32_t o = v->notifies[i].offset;
        if (o == RL_NOTIFY_STOP) continue;
        if ((incl_lo ? o >= lo : o > lo) && o <= hi) signal(v->notifies[i].event);
    }
}

void rl_fire_notifies(const rl_voice *v, uint32_t p0, uint32_t p1, uint32_t wraps, rl_signal_fn signal)
{
    if (!v->n_notifies || !signal) return;
    if (wraps == 0) {
        if (p1 > p0) fire_range(v, p0, p1, 0, signal);
    } else if (wraps == 1) {
        fire_range(v, p0, v->size, 0, signal);
        fire_range(v, 0, p1, 1, signal);
    } else {
        /* cursor lapped the whole buffer: every position was passed */
        fire_range(v, 0, v->size, 1, signal);
    }
}

void rl_fire_stop(const rl_voice *v, rl_signal_fn signal)
{
    if (!signal) return;
    for (uint32_t i = 0; i < v->n_notifies; i++)
        if (v->notifies[i].offset == RL_NOTIFY_STOP) signal(v->notifies[i].event);
}

int rl_voice_mix(rl_voice *v, float *out, uint32_t frames, uint32_t out_rate, rl_signal_fn signal)
{
    uint32_t nframes = rl_voice_frames(v);
    if (!v->playing || !nframes || !out_rate) return 0;

    const uint64_t end  = (uint64_t)nframes << 32;
    const uint64_t step = ((uint64_t)v->rate << 32) / out_rate;
    const uint32_t p0 = rl_voice_play_bytes(v);
    uint32_t wraps = 0;
    int stopped = 0;
    const int stereo = v->channels >= 2;
    const float gl = v->gain[0], gr = v->gain[1];

    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx  = (uint32_t)(v->pos >> 32);
        float    frac = (float)(uint32_t)v->pos * (1.0f / 4294967296.0f);
        uint32_t nxt  = idx + 1;
        int have_next = 1;
        if (nxt >= nframes) { if (v->looping) nxt = 0; else have_next = 0; }

        float l0 = fetch(v, idx, 0);
        float l1 = have_next ? fetch(v, nxt, 0) : 0.0f;
        float l  = l0 + (l1 - l0) * frac;
        float r  = l;
        if (stereo) {
            float r0 = fetch(v, idx, 1);
            float r1 = have_next ? fetch(v, nxt, 1) : 0.0f;
            r = r0 + (r1 - r0) * frac;
        }
        out[i * 2]     += l * gl;
        out[i * 2 + 1] += r * gr;

        v->pos += step;
        if (v->pos >= end) {
            if (v->looping) {
                while (v->pos >= end) { v->pos -= end; wraps++; }
            } else {
                v->pos = 0;            /* DirectSound rewinds a finished buffer to 0 */
                v->playing = 0;
                stopped = 1;
                break;
            }
        }
    }

    if (stopped) {
        fire_range(v, p0, v->size, 0, signal);
        rl_fire_stop(v, signal);
    } else {
        rl_fire_notifies(v, p0, rl_voice_play_bytes(v), wraps, signal);
    }
    return stopped;
}
