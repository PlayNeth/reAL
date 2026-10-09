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

/* linear-interpolated stereo sample at fixed point position `pos` (mono sources duplicate L) */
static inline void read_at(const rl_voice *v, uint64_t pos, uint32_t nframes, float *l, float *r)
{
    uint32_t idx  = (uint32_t)(pos >> 32);
    float    frac = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
    uint32_t nxt  = idx + 1;
    int have_next = 1;
    if (nxt >= nframes) { if (v->looping) nxt = 0; else have_next = 0; }
    float l0 = fetch(v, idx, 0), l1 = have_next ? fetch(v, nxt, 0) : 0.0f;
    *l = l0 + (l1 - l0) * frac;
    if (v->channels >= 2) {
        float r0 = fetch(v, idx, 1), r1 = have_next ? fetch(v, nxt, 1) : 0.0f;
        *r = r0 + (r1 - r0) * frac;
    } else {
        *r = *l;
    }
}

void rl_voice_jump(rl_voice *v, uint64_t newpos)
{
    if (v->playing && v->xfade_left == 0) {
        v->xfade_pos = v->pos;
        v->xfade_left = RL_XFADE_PENDING;
    }
    v->pos = newpos;
}

void rl_voice_start(rl_voice *v)
{
    /* a voice starting from silence takes its gains immediately (no fade-in on attacks) */
    if (!v->playing) {
        v->cur_gain[0] = v->gain[0];
        v->cur_gain[1] = v->gain[1];
        v->cur_send = v->send_gain;
        v->xfade_left = 0;
        v->lp_z[0] = v->lp_z[1] = v->send_z = 0.0f;
    }
    v->playing = 1;
}

static int mix_inner(rl_voice *v, float *out, float *mono, float *send, uint32_t frames, uint32_t out_rate, rl_signal_fn signal)
{
    uint32_t nframes = rl_voice_frames(v);
    if (!v->playing || !nframes || !out_rate) return 0;

    const uint64_t end  = (uint64_t)nframes << 32;
    const uint64_t step = ((uint64_t)v->rate << 32) / out_rate;
    const uint32_t p0 = rl_voice_play_bytes(v);
    uint32_t wraps = 0;
    int stopped = 0;

    /* gain smoothing: ramp from the current to the target gains over ~5 ms */
    uint32_t ramp = out_rate / 200;
    if (ramp > frames) ramp = frames;
    if (ramp < 1) ramp = 1;
    float gl = v->cur_gain[0], gr = v->cur_gain[1], gs = v->cur_send;
    const float dl = (v->gain[0] - gl) / (float)ramp;
    const float dr = (v->gain[1] - gr) / (float)ramp;
    const float ds = (v->send_gain - gs) / (float)ramp;
    if (!send || (gs <= 0.0f && v->send_gain <= 0.0f)) send = NULL;
    const float la = v->lp_a > 0.0f && v->lp_a < 1.0f ? v->lp_a : 1.0f, sa = v->send_lp_a > 0.0f && v->send_lp_a < 1.0f ? v->send_lp_a : 1.0f;
    float z0 = v->lp_z[0], z1 = v->lp_z[1], zs = v->send_z;

    /* declick a position jump: crossfade from the old position over ~5 ms */
    if (v->xfade_left == RL_XFADE_PENDING) v->xfade_left = out_rate / 200 ? out_rate / 200 : 1;
    const uint32_t xf_total = out_rate / 200 ? out_rate / 200 : 1;

    for (uint32_t i = 0; i < frames; i++) {
        if (i < ramp) { gl += dl; gr += dr; gs += ds; }
        else { gl = v->gain[0]; gr = v->gain[1]; gs = v->send_gain; }

        float l, r;
        read_at(v, v->pos, nframes, &l, &r);

        if (v->xfade_left) {
            float t = (float)v->xfade_left / (float)xf_total;   /* 1 -> 0 */
            float ol = 0.0f, orr = 0.0f;
            if (v->xfade_pos < end) read_at(v, v->xfade_pos, nframes, &ol, &orr);
            l = l * (1.0f - t) + ol * t;
            r = r * (1.0f - t) + orr * t;
            v->xfade_pos += step;
            if (v->xfade_pos >= end && v->looping) v->xfade_pos -= end;
            v->xfade_left--;
        }

        if (send) {
            float s = 0.5f * (l + r);
            if (sa < 1.0f) { zs += sa * (s - zs); s = zs; }
            send[i] += s * gs;
        }
        if (la < 1.0f) { z0 += la * (l - z0); z1 += la * (r - z1); l = z0; r = z1; }
        if (mono) {
            mono[i] = 0.5f * (l + r) * gl;
        } else {
            out[i * 2]     += l * gl;
            out[i * 2 + 1] += r * gr;
        }

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
    v->cur_gain[0] = gl; v->cur_gain[1] = gr; v->cur_send = gs;
    v->lp_z[0] = z0; v->lp_z[1] = z1; v->send_z = zs;
    if (ramp == frames) { /* ramp finished exactly at the block end */ }

    if (stopped) {
        v->xfade_left = 0;
        fire_range(v, p0, v->size, 0, signal);
        rl_fire_stop(v, signal);
    } else {
        rl_fire_notifies(v, p0, rl_voice_play_bytes(v), wraps, signal);
    }
    return stopped;
}

int rl_voice_mix(rl_voice *v, float *out, float *send, uint32_t frames, uint32_t out_rate, rl_signal_fn signal)
{
    if (!v->hrtf || !v->hrtf_set || !v->hrtf_set->dirs)
        return mix_inner(v, out, NULL, send, frames, out_rate, signal);

    /* HRTF path: render the voice to mono in chunks, then convolve into the stereo output */
    float mono[512];
    int stopped = 0;
    for (uint32_t done = 0; done < frames; ) {
        uint32_t n = frames - done > 512 ? 512 : frames - done;
        memset(mono, 0, n * sizeof(float));
        if (!stopped && v->playing)
            stopped = mix_inner(v, NULL, mono, send ? send + done : NULL, n, out_rate, signal);
        rl_hrtf_render(v->hrtf, v->hrtf_set, mono, out + done * 2, n);
        done += n;
    }
    return stopped;
}
