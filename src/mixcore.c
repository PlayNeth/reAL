#include "mixcore.h"
#include "cmss3.h"
#include "virt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    if (v->sfmt == RL_S16 && v->channels <= 2 && nxt < nframes) {
        /* the common case, without the per-sample format switch */
        const int16_t *q = (const int16_t *)(const void *)(v->data + (size_t)idx * v->block);
        if (v->channels == 1) {
            float a = q[0] * (1.0f / 32768.0f), b = q[1] * (1.0f / 32768.0f);
            *l = *r = a + (b - a) * frac;
        } else {
            float a = q[0] * (1.0f / 32768.0f), b = q[2] * (1.0f / 32768.0f);
            float c = q[1] * (1.0f / 32768.0f), d = q[3] * (1.0f / 32768.0f);
            *l = a + (b - a) * frac;
            *r = c + (d - c) * frac;
        }
        return;
    }
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

/* ---- windowed-sinc interpolation ----
 * Polyphase: for each kernel width a table of 256 phases (+1) x T taps, each row normalised to sum 1
 * (DC passes exactly); the weights for a position are interpolated linearly between two rows.
 * Sources played faster than the output rate use a kernel widened by a scale s <= out/in from a ladder
 * of widths (ratio 0.9, down to 0.25 = 4x), so nothing above the output Nyquist frequency aliases. */
#define SINC_W      16               /* half width in source frames at s = 1 (32 taps) */
#define SINC_FC     0.93             /* cutoff, fraction of the source Nyquist frequency */
#define SINC_BETA   7.5              /* Kaiser window */
#define SINC_PHASES 256
#define SINC_LEVELS 14               /* s = 0.9^j, last one clamped to 0.25 */
#define SINC_MAXT   128

typedef struct sinc_tab { int half, taps; float s; float w[]; } sinc_tab;
static sinc_tab *s_tabs[SINC_LEVELS];

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64 && term > 1e-12 * sum; k++) { term *= q / ((double)k * k); sum += term; }
    return sum;
}

static float level_scale(int j)
{
    float s = 1.0f;
    for (int i = 0; i < j; i++) s *= 0.9f;
    return j == SINC_LEVELS - 1 || s < 0.25f ? 0.25f : s;
}

static const sinc_tab *sinc_get(int j)
{
    sinc_tab *t = __atomic_load_n(&s_tabs[j], __ATOMIC_ACQUIRE);
    if (t) return t;
    const double s = level_scale(j), wid = SINC_W / s, i0b = bessel_i0(SINC_BETA);
    int half = (int)ceil(wid);
    half += half & 1;                                    /* taps a multiple of 4 */
    const int taps = 2 * half;
    t = malloc(sizeof(*t) + sizeof(float) * (size_t)(SINC_PHASES + 1) * taps);
    if (!t) return NULL;
    t->half = half; t->taps = taps; t->s = (float)s;
    for (int p = 0; p <= SINC_PHASES; p++) {
        double frac = (double)p / SINC_PHASES, sum = 0;
        float *row = t->w + (size_t)p * taps;
        for (int k = 1 - half; k <= half; k++) {
            double x = (k - frac) * s, r = fabs(x) / SINC_W, h = 0;
            if (r < 1.0) {
                double px = M_PI * SINC_FC * x;
                h = (fabs(px) < 1e-12 ? 1.0 : sin(px) / px) * bessel_i0(SINC_BETA * sqrt(1.0 - r * r)) / i0b;
            }
            row[k + half - 1] = (float)h; sum += h;
        }
        for (int k = 0; k < taps; k++) row[k] = (float)(row[k] / sum);
    }
    sinc_tab *expect = NULL;
    if (!__atomic_compare_exchange_n(&s_tabs[j], &expect, t, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(t);                                         /* another thread built it first */
        t = expect;
    }
    return t;
}

void rl_mix_prepare(void)
{
    for (int j = 0; j < SINC_LEVELS; j++) sinc_get(j);
}

/* the table for a step (source frames per output frame, 32.32) */
static const sinc_tab *sinc_for_step(uint64_t step)
{
    int j = 0;
    if (step > (1ull << 32)) {
        float need = (float)(4294967296.0 / (double)step);
        while (j < SINC_LEVELS - 1 && level_scale(j) > need) j++;
    }
    return sinc_get(j);
}

typedef int16_t __attribute__((may_alias)) s16a;
typedef float   __attribute__((may_alias)) f32a;

#ifdef __SSE2__
#include <emmintrin.h>
/* the S16 kernels four taps at a time: weights interpolated between the two phase rows, samples widened */
static inline float sinc_s16_mono(const float *a, const float *b, float u, const uint8_t *p, int T)
{
    const __m128 vu = _mm_set1_ps(u);
    __m128 acc = _mm_setzero_ps();
    for (int k = 0; k < T; k += 4) {
        __m128 wa = _mm_loadu_ps(a + k), wb = _mm_loadu_ps(b + k);
        __m128 w = _mm_add_ps(wa, _mm_mul_ps(vu, _mm_sub_ps(wb, wa)));
        __m128i s = _mm_loadl_epi64((const __m128i *)(p + 2 * k));
        __m128 x = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(s, s), 16));
        acc = _mm_add_ps(acc, _mm_mul_ps(w, x));
    }
    acc = _mm_add_ps(acc, _mm_movehl_ps(acc, acc));
    acc = _mm_add_ss(acc, _mm_shuffle_ps(acc, acc, 1));
    return _mm_cvtss_f32(acc);
}

static inline void sinc_s16_stereo(const float *a, const float *b, float u, const uint8_t *p, int T,
                                   float *l, float *r)
{
    const __m128 vu = _mm_set1_ps(u);
    __m128 acc = _mm_setzero_ps();
    for (int k = 0; k < T; k += 4) {
        __m128 wa = _mm_loadu_ps(a + k), wb = _mm_loadu_ps(b + k);
        __m128 w = _mm_add_ps(wa, _mm_mul_ps(vu, _mm_sub_ps(wb, wa)));
        __m128i s = _mm_loadu_si128((const __m128i *)(p + 4 * k));           /* L0 R0 L1 R1 L2 R2 L3 R3 */
        __m128 x01 = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(s, s), 16));
        __m128 x23 = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpackhi_epi16(s, s), 16));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_unpacklo_ps(w, w), x01));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_unpackhi_ps(w, w), x23));
    }
    acc = _mm_add_ps(acc, _mm_movehl_ps(acc, acc));                          /* L R */
    *l = _mm_cvtss_f32(acc);
    *r = _mm_cvtss_f32(_mm_shuffle_ps(acc, acc, 1));
}
#endif

static inline void read_sinc(const rl_voice *v, const sinc_tab *t, uint64_t pos, uint32_t nframes,
                             float *l, float *r)
{
    const int T = t->taps;
    const uint32_t f32 = (uint32_t)pos;
    const float *a = t->w + (size_t)(f32 >> 24) * T, *b = a + T;
    const float u = (float)(f32 & 0xFFFFFF) * (1.0f / 16777216.0f);
    const int64_t base = (int64_t)(pos >> 32) - t->half + 1;
    const int stereo = v->channels >= 2;
    const int inside = base >= 0 && base + T <= (int64_t)nframes;
#ifdef __SSE2__
    if (inside && v->sfmt == RL_S16 && v->channels <= 2) {
        const uint8_t *p = v->data + (size_t)base * v->block;
        if (v->channels == 1) {
            *l = *r = sinc_s16_mono(a, b, u, p, T) * (1.0f / 32768.0f);
        } else {
            sinc_s16_stereo(a, b, u, p, T, l, r);
            *l *= 1.0f / 32768.0f; *r *= 1.0f / 32768.0f;
        }
        return;
    }
#endif
    float w[SINC_MAXT];
    for (int k = 0; k < T; k++) w[k] = a[k] + u * (b[k] - a[k]);

    float al = 0.0f, ar = 0.0f;
    if (inside) {
        const uint8_t *p = v->data + (size_t)base * v->block;
        if (v->sfmt == RL_S16 && v->channels == 1) {
            const s16a *q = (const s16a *)p;
            for (int k = 0; k < T; k++) al += w[k] * (float)q[k];
            *l = *r = al * (1.0f / 32768.0f);
            return;
        }
        if (v->sfmt == RL_S16 && v->channels == 2) {
            const s16a *q = (const s16a *)p;
            for (int k = 0; k < T; k++) { al += w[k] * (float)q[2 * k]; ar += w[k] * (float)q[2 * k + 1]; }
            *l = al * (1.0f / 32768.0f); *r = ar * (1.0f / 32768.0f);
            return;
        }
        if (v->sfmt == RL_U8 && v->channels <= 2) {
            const int c = v->channels;
            for (int k = 0; k < T; k++) al += w[k] * (float)p[c * k];
            if (c == 2) for (int k = 0; k < T; k++) ar += w[k] * (float)p[2 * k + 1];
            /* weights sum to 1: subtract the 128 offset once */
            *l = (al - 128.0f) * (1.0f / 128.0f);
            *r = c == 2 ? (ar - 128.0f) * (1.0f / 128.0f) : *l;
            return;
        }
        if (v->sfmt == RL_F32 && v->channels == 1) {
            const f32a *q = (const f32a *)p;
            for (int k = 0; k < T; k++) al += w[k] * q[k];
            *l = *r = al;
            return;
        }
        for (int k = 0; k < T; k++) {
            al += w[k] * fetch(v, (uint32_t)(base + k), 0);
            if (stereo) ar += w[k] * fetch(v, (uint32_t)(base + k), 1);
        }
    } else {
        /* near the ends: wrap a looping buffer, silence outside a one-shot */
        for (int k = 0; k < T; k++) {
            int64_t n = base + k;
            if (n < 0 || n >= (int64_t)nframes) {
                if (!v->looping) continue;
                n %= (int64_t)nframes;
                if (n < 0) n += nframes;
            }
            al += w[k] * fetch(v, (uint32_t)n, 0);
            if (stereo) ar += w[k] * fetch(v, (uint32_t)n, 1);
        }
    }
    *l = al;
    *r = stereo ? ar : al;
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
        for (int k = 0; k < RL_SENDS; k++) v->cur_send[k] = v->send_gain[k];
        v->xfade_left = 0;
        v->lp_z[0] = v->lp_z[1] = 0.0f;
        for (int k = 0; k < RL_SENDS; k++) v->send_z[k] = 0.0f;
        memcpy(v->rc, v->rt, sizeof(v->rc));
        v->lfe_lim.env = 0.0f;
        if (v->cmss) rl_cmss3_reset(v->cmss);
        if (v->cmss_virt) rl_virt_reset(v->cmss_virt);
        if (v->hrtf) rl_hrtf_voice_restart(v->hrtf);
    }
    v->playing = 1;
}

#define MIXCHUNK 256

/* source frames at pos, pos + step, ... into l[] and r[] (mono sources: r = l); a one-shot that reaches its
 * end stops after that frame (*ended set) */
static uint32_t render_src(const rl_voice *v, const sinc_tab *kt, uint64_t *ppos, uint64_t step, uint64_t end,
                           uint32_t nframes, uint32_t n, float *l, float *r, uint32_t *wraps, int *ended)
{
    uint64_t pos = *ppos;
    uint32_t i = 0;
    while (i < n) {
        if (kt) read_sinc(v, kt, pos, nframes, &l[i], &r[i]);
        else read_at(v, pos, nframes, &l[i], &r[i]);
        i++;
        pos += step;
        if (pos >= end) {
            if (!v->looping) { *ended = 1; break; }
            while (pos >= end) { pos -= end; (*wraps)++; }
        }
    }
    *ppos = pos;
    return i;
}

/* a gain ramped from g by d per frame for the first `ramp` frames of the voice's block (i0 = this chunk's
 * start), then held at t: the same sequence the per-frame loop produced */
static inline void gain_run(float *gv, float g, float d, float t, uint32_t ramp, uint32_t i0, uint32_t n, float *out_g)
{
    for (uint32_t i = 0; i < n; i++) {
        if (i0 + i < ramp) g += d; else g = t;
        gv[i] = g;
    }
    *out_g = g;
}

static int mix_inner(rl_voice *v, float *out, uint32_t ostride, float *mono, float *const *sends, uint32_t frames, uint32_t out_rate, rl_signal_fn signal)
{
    uint32_t nframes = rl_voice_frames(v);
    if (!v->playing || !nframes || !out_rate) return 0;

    const uint64_t end  = (uint64_t)nframes << 32;
    const uint64_t step = (v->pitch > 0.0f && v->pitch != 1.0f)
        ? (uint64_t)((double)v->rate * v->pitch * 4294967296.0 / out_rate)
        : ((uint64_t)v->rate << 32) / out_rate;
    const uint32_t p0 = rl_voice_play_bytes(v);

    /* interpolator: exact copy when the rates match (the phase never changes), else linear or sinc */
    int use_sinc = v->interp == RL_INTERP_SINC && !(step == (1ull << 32) && (uint32_t)v->pos == 0 &&
                                                   (!v->xfade_left || (uint32_t)v->xfade_pos == 0));
    const sinc_tab *kt = use_sinc ? sinc_for_step(step) : NULL;
    uint32_t wraps = 0;
    int stopped = 0;

    /* gain smoothing: ramp from the current to the target gains over ~5 ms */
    uint32_t ramp = out_rate / 200;
    if (ramp > frames) ramp = frames;
    if (ramp < 1) ramp = 1;
    float gl = v->cur_gain[0], gr = v->cur_gain[1];
    const float dl = (v->gain[0] - gl) / (float)ramp;
    const float dr = (v->gain[1] - gr) / (float)ramp;
    /* active sends: bus present and some gain */
    int ns = 0, sk[RL_SENDS];
    float *sb[RL_SENDS], gs[RL_SENDS], ds[RL_SENDS], sa[RL_SENDS], zs[RL_SENDS];
    for (int k = 0; k < RL_SENDS; k++) {
        if (!sends || !sends[k] || (v->cur_send[k] <= 0.0f && v->send_gain[k] <= 0.0f)) continue;
        sk[ns] = k; sb[ns] = sends[k]; gs[ns] = v->cur_send[k];
        ds[ns] = (v->send_gain[k] - gs[ns]) / (float)ramp;
        sa[ns] = v->send_lp_a[k] > 0.0f && v->send_lp_a[k] < 1.0f ? v->send_lp_a[k] : 1.0f;
        zs[ns] = v->send_z[k];
        ns++;
    }
    const float la = v->lp_a > 0.0f && v->lp_a < 1.0f ? v->lp_a : 1.0f;
    float z0 = v->lp_z[0], z1 = v->lp_z[1];

    /* declick a position jump: crossfade from the old position over ~5 ms */
    if (v->xfade_left == RL_XFADE_PENDING) v->xfade_left = out_rate / 200 ? out_rate / 200 : 1;
    const uint32_t xf_total = out_rate / 200 ? out_rate / 200 : 1;

    float L[MIXCHUNK], R[MIXCHUNK], G[MIXCHUNK], G2[MIXCHUNK];
    for (uint32_t i0 = 0; i0 < frames && !stopped; ) {
        uint32_t want = frames - i0 > MIXCHUNK ? MIXCHUNK : frames - i0;
        uint64_t pos = v->pos;
        uint32_t n = render_src(v, kt, &pos, step, end, nframes, want, L, R, &wraps, &stopped);
        if (stopped) {
            v->pos = 0;                /* DirectSound rewinds a finished buffer to 0 */
            v->playing = 0;
        } else {
            v->pos = pos;
        }

        /* the old position fading out after a jump */
        for (uint32_t i = 0; i < n && v->xfade_left; i++) {
            float t = (float)v->xfade_left / (float)xf_total;   /* 1 -> 0 */
            float ol = 0.0f, orr = 0.0f;
            if (v->xfade_pos < end) {
                if (kt) read_sinc(v, kt, v->xfade_pos, nframes, &ol, &orr);
                else read_at(v, v->xfade_pos, nframes, &ol, &orr);
            }
            L[i] = L[i] * (1.0f - t) + ol * t;
            R[i] = R[i] * (1.0f - t) + orr * t;
            v->xfade_pos += step;
            if (v->xfade_pos >= end && v->looping) v->xfade_pos -= end;
            v->xfade_left--;
        }

        /* effect sends: the mono sum, through each send's HF filter */
        for (int j = 0; j < ns; j++) {
            gain_run(G, gs[j], ds[j], v->send_gain[sk[j]], ramp, i0, n, &gs[j]);
            float *dst = sb[j] + i0;
            if (sa[j] < 1.0f) {
                float z = zs[j];
                for (uint32_t i = 0; i < n; i++) { z += sa[j] * (0.5f * (L[i] + R[i]) - z); dst[i] += z * G[i]; }
                zs[j] = z;
            } else {
                for (uint32_t i = 0; i < n; i++) dst[i] += 0.5f * (L[i] + R[i]) * G[i];
            }
        }
        /* direct path */
        if (la < 1.0f) {
            for (uint32_t i = 0; i < n; i++) { z0 += la * (L[i] - z0); z1 += la * (R[i] - z1); L[i] = z0; R[i] = z1; }
        }
        gain_run(G, gl, dl, v->gain[0], ramp, i0, n, &gl);
        if (mono) {
            float *dst = mono + i0;
            for (uint32_t i = 0; i < n; i++) dst[i] = 0.5f * (L[i] + R[i]) * G[i];
            gain_run(G2, gr, dr, v->gain[1], ramp, i0, n, &gr);
        } else {
            gain_run(G2, gr, dr, v->gain[1], ramp, i0, n, &gr);
            float *dst = out + (size_t)i0 * ostride;
            if (ostride == 2) {
                for (uint32_t i = 0; i < n; i++) { dst[2 * i] += L[i] * G[i]; dst[2 * i + 1] += R[i] * G2[i]; }
            } else {
                for (uint32_t i = 0; i < n; i++) { dst[i * ostride] += L[i] * G[i]; dst[i * ostride + 1] += R[i] * G2[i]; }
            }
        }
        i0 += n;
    }
    v->cur_gain[0] = gl; v->cur_gain[1] = gr;
    for (int j = 0; j < ns; j++) { v->cur_send[sk[j]] = gs[j]; v->send_z[sk[j]] = zs[j]; }
    v->lp_z[0] = z0; v->lp_z[1] = z1;

    if (stopped) {
        v->xfade_left = 0;
        fire_range(v, p0, v->size, 0, signal);
        rl_fire_stop(v, signal);
    } else {
        rl_fire_notifies(v, p0, rl_voice_play_bytes(v), wraps, signal);
    }
    return stopped;
}

int rl_voice_mix(rl_voice *v, float *out, float *const *sends, uint32_t frames, uint32_t out_rate, rl_signal_fn signal)
{
    if (!v->hrtf || !v->hrtf_set || !v->hrtf_set->dirs)
        return mix_inner(v, out, 2, NULL, sends, frames, out_rate, signal);

    /* HRTF path: render the voice to mono in chunks, then convolve into the stereo output */
    float mono[512];
    int stopped = 0;
    for (uint32_t done = 0; done < frames; ) {
        uint32_t n = frames - done > 512 ? 512 : frames - done;
        memset(mono, 0, n * sizeof(float));
        if (!stopped && v->playing) {
            float *sub[RL_SENDS];
            for (int k = 0; k < RL_SENDS; k++) sub[k] = sends && sends[k] ? sends[k] + done : NULL;
            stopped = mix_inner(v, NULL, 2, mono, sends ? sub : NULL, n, out_rate, signal);
        }
        rl_hrtf_render(v->hrtf, v->hrtf_set, mono, out + done * 2, n);
        done += n;
    }
    return stopped;
}

/* ------------------------------------------------------------------ more than two output channels */

/* every channel of a frame (sources of more than two channels) */
static inline void read_at_n(const rl_voice *v, uint64_t pos, uint32_t nframes, float *x)
{
    uint32_t idx = (uint32_t)(pos >> 32);
    float frac = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
    uint32_t nxt = idx + 1;
    if (v->sfmt == RL_S16 && nxt < nframes) {
        const int16_t *q = (const int16_t *)(const void *)(v->data + (size_t)idx * v->block);
        const int nc = v->channels;
        for (int c = 0; c < nc; c++) {
            float a = q[c] * (1.0f / 32768.0f), b = q[nc + c] * (1.0f / 32768.0f);
            x[c] = a + (b - a) * frac;
        }
        return;
    }
    int have_next = 1;
    if (nxt >= nframes) { if (v->looping) nxt = 0; else have_next = 0; }
    for (int c = 0; c < v->channels; c++) {
        float a = fetch(v, idx, c), b = have_next ? fetch(v, nxt, c) : 0.0f;
        x[c] = a + (b - a) * frac;
    }
}

static inline void read_sinc_n(const rl_voice *v, const sinc_tab *t, uint64_t pos, uint32_t nframes, float *x)
{
    const int T = t->taps;
    const uint32_t f32 = (uint32_t)pos;
    const float *a = t->w + (size_t)(f32 >> 24) * T, *b = a + T;
    const float u = (float)(f32 & 0xFFFFFF) * (1.0f / 16777216.0f);
    float w[SINC_MAXT];
    for (int k = 0; k < T; k++) w[k] = a[k] + u * (b[k] - a[k]);
    const int64_t base = (int64_t)(pos >> 32) - t->half + 1;
    for (int c = 0; c < v->channels; c++) x[c] = 0.0f;
    if (v->sfmt == RL_S16 && base >= 0 && base + T <= (int64_t)nframes) {
        /* inside the buffer: straight through the frames, scaled once at the end */
        const int nc = v->channels;
        const int16_t *q = (const int16_t *)(const void *)(v->data + (size_t)base * v->block);
#define SINC_N(NC) for (int k = 0; k < T; k++, q += NC) for (int c = 0; c < NC; c++) x[c] += w[k] * (float)q[c]
        switch (nc) {                                  /* constant channel counts unroll */
        case 4: SINC_N(4); break;
        case 6: SINC_N(6); break;
        case 8: SINC_N(8); break;
        default: SINC_N(nc); break;
        }
#undef SINC_N
        for (int c = 0; c < nc; c++) x[c] *= 1.0f / 32768.0f;
        return;
    }
    for (int k = 0; k < T; k++) {
        int64_t n = base + k;
        if (n < 0 || n >= (int64_t)nframes) {
            if (!v->looping) continue;
            n %= (int64_t)nframes;
            if (n < 0) n += nframes;
        }
        for (int c = 0; c < v->channels; c++) x[c] += w[k] * fetch(v, (uint32_t)n, c);
    }
}

/* a source of more than two channels through its conversion matrix (C2DStream with the channel converter) */
static int mix_inner_mc(rl_voice *v, float *out, uint32_t nout, float *const *sends, uint32_t frames, uint32_t out_rate,
                        rl_signal_fn signal)
{
    uint32_t nframes = rl_voice_frames(v);
    if (!v->playing || !nframes || !out_rate) return 0;
    const uint64_t end  = (uint64_t)nframes << 32;
    const uint64_t step = (v->pitch > 0.0f && v->pitch != 1.0f)
        ? (uint64_t)((double)v->rate * v->pitch * 4294967296.0 / out_rate)
        : ((uint64_t)v->rate << 32) / out_rate;
    const uint32_t p0 = rl_voice_play_bytes(v);
    int use_sinc = v->interp == RL_INTERP_SINC && !(step == (1ull << 32) && (uint32_t)v->pos == 0 &&
                                                   (!v->xfade_left || (uint32_t)v->xfade_pos == 0));
    const sinc_tab *kt = use_sinc ? sinc_for_step(step) : NULL;
    if (!kt) use_sinc = 0;
    uint32_t wraps = 0;
    int stopped = 0;
    const int nch = v->channels > RL_MAXOUT ? RL_MAXOUT : v->channels;
    const uint32_t no = nout > RL_MAXOUT ? RL_MAXOUT : nout;

    uint32_t ramp = out_rate / 200;
    if (ramp > frames) ramp = frames;
    if (ramp < 1) ramp = 1;
    float g = v->cur_gain[0];
    const float dg = (v->gain[0] - g) / (float)ramp;
    int ns = 0, sk[RL_SENDS];
    float *sb[RL_SENDS], gs[RL_SENDS], ds[RL_SENDS];
    for (int k = 0; k < RL_SENDS; k++) {
        if (!sends || !sends[k] || (v->cur_send[k] <= 0.0f && v->send_gain[k] <= 0.0f)) continue;
        sk[ns] = k; sb[ns] = sends[k]; gs[ns] = v->cur_send[k];
        ds[ns] = (v->send_gain[k] - gs[ns]) / (float)ramp;
        ns++;
    }
    if (v->xfade_left == RL_XFADE_PENDING) v->xfade_left = out_rate / 200 ? out_rate / 200 : 1;
    const uint32_t xf_total = out_rate / 200 ? out_rate / 200 : 1;
    const int lfe = v->mc_lfe, fl = no > 0 ? 0 : -1, fr = no > 1 ? 1 : -1;
    /* the matrix's non-zero entries (most are zero) */
    int ne = 0;
    uint8_t es[RL_MAXOUT * RL_MAXOUT], ec[RL_MAXOUT * RL_MAXOUT];
    float eg[RL_MAXOUT * RL_MAXOUT];
    for (uint32_t c = 0; c < no; c++)
        for (int s = 0; s < nch; s++)
            if (v->mc[s][c] != 0.0f) { es[ne] = (uint8_t)s; ec[ne] = (uint8_t)c; eg[ne] = v->mc[s][c]; ne++; }

    for (uint32_t i = 0; i < frames; i++) {
        if (i < ramp) { g += dg; for (int j = 0; j < ns; j++) gs[j] += ds[j]; }
        else { g = v->gain[0]; for (int j = 0; j < ns; j++) gs[j] = v->send_gain[sk[j]]; }
        float x[RL_MAXOUT] = { 0 }, ox[RL_MAXOUT] = { 0 };
        if (use_sinc) read_sinc_n(v, kt, v->pos, nframes, x); else read_at_n(v, v->pos, nframes, x);
        if (v->xfade_left) {
            float t = (float)v->xfade_left / (float)xf_total;
            if (v->xfade_pos < end) {
                if (use_sinc) read_sinc_n(v, kt, v->xfade_pos, nframes, ox); else read_at_n(v, v->xfade_pos, nframes, ox);
            }
            for (int c = 0; c < nch; c++) x[c] = x[c] * (1.0f - t) + ox[c] * t;
            v->xfade_pos += step;
            if (v->xfade_pos >= end && v->looping) v->xfade_pos -= end;
            v->xfade_left--;
        }
        if (ns) {
            float m = 0.0f;
            for (int c = 0; c < nch; c++) m += x[c];
            m /= (float)nch;
            for (int j = 0; j < ns; j++) sb[j][i] += m * gs[j];
        }
        float bass = 0.0f;
        if (lfe >= 0 && lfe < nch)
            bass = rl_bass_limit(&v->lfe_lim, x[lfe] * RL_LFE_IN_GAIN) * RL_LFE_OUT_GAIN;
        float *o = out + (size_t)i * nout;
        float acc[RL_MAXOUT] = { 0 };
        for (int e = 0; e < ne; e++) acc[ec[e]] += x[es[e]] * eg[e];
        if (fl >= 0) acc[fl] += bass;
        if (fr >= 0) acc[fr] += bass;
        for (uint32_t c = 0; c < no; c++) o[c] += acc[c] * g;
        v->pos += step;
        if (v->pos >= end) {
            if (v->looping) {
                while (v->pos >= end) { v->pos -= end; wraps++; }
            } else {
                v->pos = 0;
                v->playing = 0;
                stopped = 1;
                break;
            }
        }
    }
    v->cur_gain[0] = v->cur_gain[1] = g;
    for (int j = 0; j < ns; j++) v->cur_send[sk[j]] = gs[j];
    if (stopped) {
        v->xfade_left = 0;
        fire_range(v, p0, v->size, 0, signal);
        rl_fire_stop(v, signal);
    } else {
        rl_fire_notifies(v, p0, rl_voice_play_bytes(v), wraps, signal);
    }
    return stopped;
}

#define ROUTE_CHUNK 512

/* CMSS-3D: the voice's pair (with its volume and pan) through the upmixer onto the speakers */
static int mix_cmss(rl_voice *v, float *out, uint32_t nout, float *const *sends, uint32_t frames, uint32_t out_rate,
                    rl_signal_fn signal)
{
    float pair[ROUTE_CHUNK * 2], l[ROUTE_CHUNK], r[ROUTE_CHUNK], bit[12][ROUTE_CHUNK], tmp[ROUTE_CHUNK * 2];
    uint32_t m = rl_cmss3_mask(v->cmss), done = 0;
    int stopped = 0;
    while (done < frames) {
        uint32_t n = frames - done > ROUTE_CHUNK ? ROUTE_CHUNK : frames - done;
        memset(pair, 0, n * 2 * sizeof(float));
        if (!stopped && v->playing) {
            float *sub[RL_SENDS];
            for (int k = 0; k < RL_SENDS; k++) sub[k] = sends && sends[k] ? sends[k] + done : NULL;
            stopped = mix_inner(v, pair, 2, NULL, sends ? sub : NULL, n, out_rate, signal);
        }
        for (uint32_t i = 0; i < n; i++) { l[i] = pair[2 * i]; r[i] = pair[2 * i + 1]; }
        float *o[12];
        for (int b = 0; b < 12; b++) o[b] = (m >> b) & 1 ? bit[b] : NULL;
        rl_cmss3_process(v->cmss, l, r, o, n);
        float *dst = out + (size_t)done * nout;
        if (v->cmss_virt) {
            /* stereo device: the quad through the converter's virtualizer (mode 3, or 0 on headphones) */
            static float zero[ROUTE_CHUNK];
            float *ch[ALRV_NOUT] = { bit[0], bit[1], zero, bit[4], bit[5], zero, zero, zero };
            if (nout == 2) {
                rl_virt_process(v->cmss_virt, ch, dst, n, tmp);
            } else {
                memset(pair, 0, n * 2 * sizeof(float));
                rl_virt_process(v->cmss_virt, ch, pair, n, tmp);
                for (uint32_t i = 0; i < n; i++) { dst[i * nout] += pair[2 * i]; dst[i * nout + 1] += pair[2 * i + 1]; }
            }
        } else {
            for (int b = 0; b < 12; b++) {
                int c = v->cmss_idx[b];
                if (!o[b] || c < 0 || (uint32_t)c >= nout) continue;
                for (uint32_t i = 0; i < n; i++) dst[i * nout + c] += bit[b][i];
            }
        }
        done += n;
    }
    return stopped;
}

int rl_voice_mix_n(rl_voice *v, float *out, uint32_t nout, float *const *sends, uint32_t frames, uint32_t out_rate,
                   rl_signal_fn signal)
{
    if (v->mc_on && v->channels > 2) return mix_inner_mc(v, out, nout, sends, frames, out_rate, signal);
    if (v->cmss && v->channels == 2) return mix_cmss(v, out, nout, sends, frames, out_rate, signal);
    int hrtf = v->hrtf && v->hrtf_set && v->hrtf_set->dirs;
    if (!v->route_on) {
        if (nout == 2) return rl_voice_mix(v, out, sends, frames, out_rate, signal);
        if (!hrtf) return mix_inner(v, out, nout, NULL, sends, frames, out_rate, signal);
    }
    /* render the pair (and the mono sum) in chunks, then spread them over the speakers */
    float pair[ROUTE_CHUNK * 2], mono[ROUTE_CHUNK];
    const uint32_t no = nout > RL_MAXOUT ? RL_MAXOUT : nout;
    uint32_t ramp = out_rate / 200 ? out_rate / 200 : 1, done = 0;
    int stopped = 0;
    float w[3][RL_MAXOUT], dw[3][RL_MAXOUT];
    for (int k = 0; k < 3; k++)
        for (uint32_t c = 0; c < no; c++) {
            w[k][c] = v->route_on ? v->rc[k][c] : (k == 2 ? 0.0f : (uint32_t)k == c ? 1.0f : 0.0f);
            float t = v->route_on ? v->rt[k][c] : w[k][c];
            dw[k][c] = (t - w[k][c]) / (float)ramp;
        }
    uint32_t left = ramp;
    while (done < frames) {
        uint32_t n = frames - done > ROUTE_CHUNK ? ROUTE_CHUNK : frames - done;
        memset(pair, 0, n * 2 * sizeof(float));
        if (!stopped && v->playing) {
            float *sub[RL_SENDS];
            for (int k = 0; k < RL_SENDS; k++) sub[k] = sends && sends[k] ? sends[k] + done : NULL;
            if (hrtf) {
                memset(mono, 0, n * sizeof(float));
                stopped = mix_inner(v, NULL, 2, mono, sends ? sub : NULL, n, out_rate, signal);
            } else {
                stopped = mix_inner(v, pair, 2, NULL, sends ? sub : NULL, n, out_rate, signal);
                for (uint32_t i = 0; i < n; i++) mono[i] = 0.5f * (pair[2 * i] + pair[2 * i + 1]);
            }
        } else {
            memset(mono, 0, n * sizeof(float));
        }
        if (hrtf) rl_hrtf_render(v->hrtf, v->hrtf_set, mono, pair, n);
        for (uint32_t i = 0; i < n; i++) {
            if (left) {
                for (int k = 0; k < 3; k++) for (uint32_t c = 0; c < no; c++) w[k][c] += dw[k][c];
                left--;
            }
            float *o = out + (size_t)(done + i) * nout;
            const float l = pair[2 * i], r = pair[2 * i + 1], m = mono[i];
            for (uint32_t c = 0; c < no; c++) o[c] += l * w[0][c] + r * w[1][c] + m * w[2][c];
        }
        done += n;
    }
    /* the weights reached: a call shorter than the ramp continues from here next time */
    if (v->route_on) {
        if (!left) memcpy(v->rc, v->rt, sizeof(v->rc));
        else for (int k = 0; k < 3; k++) for (uint32_t c = 0; c < no; c++) v->rc[k][c] = w[k][c];
    }
    return stopped;
}
