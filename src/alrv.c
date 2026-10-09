/* reAL - port of ALchemy's software reverb (see alrv.h).  DSP part.
 *
 * ALchemy runs the network block-wise (chunks no longer than the shortest delay in each feedback
 * path); that is equivalent to the per-sample loop below.  Addresses refer to ALchemy's dsound.dll. */
#include "alrv.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI_F 6.2831854820251465   /* ALchemy's 2*pi constant (a float widened to double) */

/* ------------------------------------------------------------------ coefficient helpers */

/* one-pole high-pass for room LF (fn 0x1005c210); 1 = bypass */
float alrv_coef_hp(float g, float f)
{
    if (g == 1.0f || f == 0.0f) return 1.0f;
    float c = (float)cos((float)(f * TWO_PI_F));
    float G = g * g;
    float A = 2.0f * G + c - 1.0f;
    float B = c - 2.0f * c * G - 1.0f;
    float S = sqrtf(B * B - A * A);
    float r = (-B - S) / A;
    return 0.5f * r + 0.5f;
}

/* one-pole low-pass (applied twice) for room HF (fn 0x1002d860); 1 = bypass */
float alrv_coef_lp(float g, float f)
{
    if (g == 1.0f) return 1.0f;
    if (f == 0.0f) return 0.0f;
    double c = (float)cos((float)(f * TWO_PI_F));
    double t2 = 1.0 - c * g;
    double t4 = sqrtf((float)(g * ((1.0 - c) + (1.0 - c) - (1.0 - c * c) * g)));
    return (float)(1.0 - (t2 - t4) / (1.0 - g));
}

/* per-line HF damping low-pass (fn 0x10061cf0): g = per-pass gain, f = hf_ref/fs, r = hf ratio */
float alrv_coef_damp(float g, float f, float r)
{
    float w = (float)(f * TWO_PI_F);
    if (r < 0.1f) r = 0.1f;
    else if (r > 1.0f) w = 3.1415927410125732f;
    if (r <= 0.0f) return 0.0f;
    float p = (float)pow(g, 2.0 - 2.0 / r);
    if (isinf(p) || isnan(p)) return 0.0f;
    double c = (float)cos(w);
    double B = (p - 1.0) / (1.0 - c);
    if (B == 0.0) return 1.0f;
    double D = -2.0 * B - 2.0;
    double S = sqrtf((float)(D * D - 4.0 * B * B));
    double x = (S - D) / (2.0 * B);
    if (fabs(x) > 1.0) x = (-D - S) / (2.0 * B);
    float a = (float)(1.0 - x);
    return a < 0.985f ? a : 0.985f;
}

/* per-line LF shelf built from a first-order allpass (fns 0x10061eb0 / 0x10061fe0):
 * y = c1*AP(x) + c0*x.  Low shelf: DC gain g; high shelf: Nyquist gain g. */
void alrv_coef_shelf(float g, float f, int high, float *c0, float *c1, float *a)
{
    if (g == 1.0f || f == 0.0f) { *c0 = 1.0f; *c1 = 0.0f; *a = 0.0f; return; }
    float h = (g + 1.0f) * 0.5f;
    *c0 = h;
    *c1 = high ? 1.0f - h : h - 1.0f;
    float w = (float)(f * TWO_PI_F);
    float sn = (float)sin(w);
    float k = (float)(sqrtf(2.0f) * ((float)log(2.0) * 0.5) * w / sn);
    float al = (float)sinh(k) * sn;
    if (high) al *= g;
    float q = al / sqrtf(g);
    *a = (1.0f - q) / (1.0f + q);
}

/* ------------------------------------------------------------------ rings */

static uint32_t ceil8_ms(uint32_t rate, double ms)   /* (7 - round(-rate*ms/1000)) & ~7 */
{
    long v = lrint(-(double)(float)((double)rate * ms) / 1000.0);
    return (uint32_t)((7 - v) & ~7L);
}

static inline float ring_read(const alrv_ring *r, uint32_t d)
{
    uint32_t i = r->pos >= d ? r->pos - d : r->pos + r->len - d;
    return r->buf[i];
}

static inline void ring_write(alrv_ring *r, float v)
{
    r->buf[r->pos] = v;
    if (++r->pos == r->len) r->pos = 0;
}

static uint32_t clamp_delay(const alrv_ring *r, int32_t d)
{
    uint32_t u = (uint32_t)d;
    if (u >= r->len) u = r->len - 8;
    if (u < 8) u = 8;
    return u;
}

/* ------------------------------------------------------------------ setup */

static const double k_early_ms[4] = { 0.0, 1.61, 2.3, 3.7 };   /* 0x100b4ec0, 0x100b55e0/d8/d0 */

int alrv_init(alrv *r, uint32_t rate)
{
    memset(r, 0, sizeof(*r));
    r->rate = rate;
    /* ring sizes as in fn 0x1005bdf0 (+8 like ALchemy's buffers) */
    uint32_t in_len   = ceil8_ms(rate, 480.0) + 8;
    uint32_t eap_len  = ceil8_ms(rate, 28.826) + 8;
    uint32_t fdn_len  = 0x4000 * (rate > 48000 ? (rate + 47999) / 48000 : 1);
    uint32_t lap_len  = ceil8_ms(rate, 21.93) + 8;
    r->eout_delay[0] = ceil8_ms(rate, 0.5);  r->eout_delay[1] = ceil8_ms(rate, 1.03);
    r->eout_delay[2] = ceil8_ms(rate, 1.47);
    r->lout_delay[0] = ceil8_ms(rate, 0.58); r->lout_delay[1] = ceil8_ms(rate, 0.91);
    r->lout_delay[2] = ceil8_ms(rate, 0.75); r->lout_delay[3] = ceil8_ms(rate, 1.33);
    r->mod_len    = ceil8_ms(rate, 12.0);
    r->mod_tap[0] = ceil8_ms(rate, 11.316);
    r->mod_tap[1] = ceil8_ms(rate, 6.0);
    for (int j = 0; j < 4; j++) r->early_off[j] = ceil8_ms(rate, k_early_ms[j]);
    uint32_t eout_len = r->eout_delay[2] + 8, lout_len = r->lout_delay[3] + 8;

    size_t total = in_len + 4 * (size_t)eap_len + 3 * (size_t)eout_len + 4 * (size_t)fdn_len +
                   4 * (size_t)lap_len + 2 * (size_t)lout_len + r->mod_len;
    r->mem = calloc(total, sizeof(float));
    if (!r->mem) return 0;
    float *m = r->mem;
#define TAKE(ring, n) do { (ring).buf = m; (ring).len = (n); m += (n); } while (0)
    TAKE(r->in, in_len);
    for (int i = 0; i < 4; i++) TAKE(r->eap[i], eap_len);
    for (int i = 0; i < 3; i++) TAKE(r->eout[i], eout_len);
    for (int i = 0; i < 4; i++) TAKE(r->fdn[i], fdn_len);
    for (int i = 0; i < 4; i++) TAKE(r->lap[i], lap_len);
    for (int i = 0; i < 2; i++) TAKE(r->lout[i], lout_len);
#undef TAKE
    r->modbuf = m;
    r->osc_x = 1.0f;
    return 1;
}

void alrv_free(alrv *r)
{
    free(r->mem);
    memset(r, 0, sizeof(*r));
}

void alrv_reset(alrv *r)
{
    alrv_ring *rings[] = { &r->in, &r->eap[0], &r->eap[1], &r->eap[2], &r->eap[3], &r->eout[0], &r->eout[1],
                           &r->eout[2], &r->fdn[0], &r->fdn[1], &r->fdn[2], &r->fdn[3], &r->lap[0], &r->lap[1],
                           &r->lap[2], &r->lap[3], &r->lout[0], &r->lout[1] };
    for (size_t i = 0; i < sizeof(rings) / sizeof(rings[0]); i++) {
        memset(rings[i]->buf, 0, rings[i]->len * sizeof(float));
        rings[i]->pos = 0;
    }
    memset(r->modbuf, 0, r->mod_len * sizeof(float));
    r->mod_wp = 0;
    r->hp_s = r->lp_v = r->lp_w = 0.0f;
    memset(r->shelf_t, 0, sizeof(r->shelf_t));
    memset(r->damp_y, 0, sizeof(r->damp_y));
    r->osc_x = 1.0f; r->osc_y = 0.0f;
    r->fade_pos = 0;
}

void alrv_set_params(alrv *r, const alrv_params *p)
{
    r->p = *p;
    r->hp_a = alrv_coef_hp(p->in_hp_gain, p->in_hp_f);
    r->lp_a = alrv_coef_lp(p->in_lp_gain, p->in_lp_f);
    for (int i = 0; i < 4; i++) {
        r->damp_a[i] = alrv_coef_damp(p->line_g[i], p->line_hf_f[i], p->line_hf_ratio[i]);
        alrv_coef_shelf(p->line_shelf_g[i], p->line_shelf_f[i], p->line_shelf_high[i] != 0,
                        &r->shelf_c0[i], &r->shelf_c1[i], &r->shelf_a[i]);
        r->etap[i] = clamp_delay(&r->in, p->refl_delay + (int32_t)r->early_off[i]);
        r->p.late_tap_delay[i] = (int32_t)clamp_delay(&r->in, p->late_tap_delay[i]);
        r->p.eap_delay[i] = (int32_t)clamp_delay(&r->eap[i], p->eap_delay[i]);
        r->p.lap_delay[i] = (int32_t)clamp_delay(&r->lap[i], p->lap_delay[i]);
        r->p.fdn_delay[i] = (int32_t)clamp_delay(&r->fdn[i], p->fdn_delay[i]);
    }
    r->mod_amp = (float)p->mod_depth;
    r->have_params = 1;
}

/* ------------------------------------------------------------------ processing */

void alrv_process(alrv *r, const float *in, float *const out[ALRV_NOUT], uint32_t frames)
{
    const alrv_params *P = &r->p;
    const float hp_a = r->hp_a, lp_a = r->lp_a;
    const float ke = P->eap_coef, kl = P->lap_coef, rl = P->lap_rot, rf = P->fdn_rot;
    const float *G = P->early_out, *H = P->late_out;
    float *Q = r->modbuf;
    const uint32_t mlen = r->mod_len;

    for (uint32_t n = 0; n < frames; n++) {
        float x = in[n];

        /* ---- input filters: room LF high-pass, room HF double low-pass */
        if (hp_a == 0.0f) x = 0.0f;
        else if (hp_a != 1.0f) {
            float s = r->hp_s * ((hp_a + hp_a) - 1.0f) + x;
            x = (s - r->hp_s) * hp_a;
            r->hp_s = s;
        }
        if (lp_a == 0.0f) x = 0.0f;
        else if (lp_a != 1.0f) {
            r->lp_v = r->lp_v * (1.0f - lp_a) + x * lp_a;
            r->lp_w = r->lp_v * lp_a + r->lp_w * (1.0f - lp_a);
            x = r->lp_w;
        }
        if (r->fade_pos < ALRV_FADE) x *= (float)r->fade_pos++ * (1.0f / ALRV_FADE);

        /* ---- input delay: early taps and late-reverb feed taps */
        float e[4], t[4];
        for (int j = 0; j < 4; j++) {
            e[j] = ring_read(&r->in, r->etap[j]) * P->refl_gain;
            t[j] = ring_read(&r->in, (uint32_t)P->late_tap_delay[j]) * P->late_tap_gain[j];
        }
        ring_write(&r->in, x);

        /* ---- early reflections: 4 allpasses */
        float ye[4];
        for (int i = 0; i < 4; i++) {
            float d = ring_read(&r->eap[i], (uint32_t)P->eap_delay[i]) * P->eap_gain[i];
            ye[i] = d + e[i] * -ke;
            ring_write(&r->eap[i], e[i] + ye[i] * ke);
        }
        float o[ALRV_NOUT];
        o[ALRV_FC] = ye[0] * G[1];
        o[ALRV_CX] = ye[0] * G[5];
        o[ALRV_SL] = ye[1] * G[7];
        o[ALRV_SR] = ye[2] * G[3];
        o[ALRV_FR] = ye[3] * G[2];
        o[ALRV_BR] = ring_read(&r->eout[0], r->eout_delay[0]) * G[4];
        o[ALRV_FL] = ring_read(&r->eout[1], r->eout_delay[1]) * G[8];
        o[ALRV_BL] = ring_read(&r->eout[2], r->eout_delay[2]) * G[6];
        ring_write(&r->eout[0], ye[1]);
        ring_write(&r->eout[1], ye[2]);
        ring_write(&r->eout[2], ye[3]);

        /* ---- late reverb: FDN lines -> LF shelf -> + feed -> HF damping -> nested allpasses */
        float L[4], A[4];
        for (int i = 0; i < 4; i++) {
            float v = ring_read(&r->fdn[i], (uint32_t)P->fdn_delay[i]) * P->fdn_gain[i];
            float a = r->shelf_a[i];
            float tt = v + r->shelf_t[i] * a;
            float ap = r->shelf_t[i] - tt * a;
            r->shelf_t[i] = tt;
            v = ap * r->shelf_c1[i] + v * r->shelf_c0[i];
            v += t[i];
            float da = r->damp_a[i];
            if (da == 0.0f) v = 0.0f;
            else if (da != 1.0f) { r->damp_y[i] = r->damp_y[i] * (1.0f - da) + v * da; v = r->damp_y[i]; }
            L[i] = v;
        }
        for (int i = 0; i < 4; i++) {
            A[i] = ring_read(&r->lap[i], (uint32_t)P->lap_delay[i]) * P->lap_gain[i];
            A[i] = A[i] + L[i] * -kl;
            L[i] = L[i] + A[i] * kl;
        }
        /* rotation of the allpass states (fn 0x10064160) */
        { float a0 = L[0], a1 = L[1]; L[0] = rl * a1 + a0; L[1] = a1 - rl * a0; }
        { float a0 = L[2], a1 = L[3]; L[2] = rl * a1 + a0; L[3] = a1 - rl * a0; }
        { float a0 = L[0], a1 = L[2]; L[0] = rl * a1 + a0; L[2] = a1 - rl * a0; }
        { float a0 = L[1], a1 = L[3]; L[1] = rl * a1 + a0; L[3] = a1 - rl * a0; }
        for (int i = 0; i < 4; i++) ring_write(&r->lap[i], L[i]);
        /* output rotation (fn 0x10064220) */
        { float a0 = A[0], a1 = A[1]; A[0] = rf * a1 + a0; A[1] = a1 - rf * a0; }
        { float a0 = A[2], a1 = A[3]; A[2] = rf * a1 + a0; A[3] = a1 - rf * a0; }
        { float a0 = A[0], a1 = A[3]; A[0] = rf * a1 + a0; A[3] = a1 - rf * a0; }
        { float a0 = A[1], a1 = A[2]; A[1] = a0 - a1 * rf; A[2] = rf * a0 + a1; }

        /* modulated delay on line 0 (inline loop in fn 0x10062580) */
        float fv = (float)r->mod_tap[1] + r->osc_y * r->mod_amp;
        long ic = (long)fv;                    /* FPU set to truncate here */
        float frac = fv - (float)ic;
        if (r->mod_amp != 0.0f) {
            float xo = r->osc_x - P->mod_freq * r->osc_y;
            r->osc_x = xo;
            r->osc_y = xo * P->mod_freq + r->osc_y;
        }
        uint32_t idx = (uint32_t)((mlen - (uint32_t)ic) + r->mod_wp) % mlen;
        float M = idx == 0 ? (1.0f - frac) * Q[0] + frac * Q[mlen - 1]
                           : (1.0f - frac) * Q[idx] + Q[idx - 1] * frac;
        float T1 = Q[((mlen - r->mod_tap[1]) + r->mod_wp) % mlen];   /* 6 ms */
        float T0 = Q[((mlen - r->mod_tap[0]) + r->mod_wp) % mlen];   /* 11.3 ms */
        if (r->mod_wp == mlen) { Q[0] = A[0]; r->mod_wp = 1; }
        else Q[r->mod_wp++] = A[0];
        A[0] = M;

        /* feedback, permuted (table at object +0x274) */
        ring_write(&r->fdn[0], A[2]);
        ring_write(&r->fdn[1], A[0]);
        ring_write(&r->fdn[2], A[3]);
        ring_write(&r->fdn[3], A[1]);

        o[ALRV_FC] += A[3] * H[1];
        o[ALRV_CX] += A[3] * H[5];
        o[ALRV_BR] += T0 * H[4];
        o[ALRV_SL] += T1 * H[7];
        o[ALRV_FL] += ring_read(&r->lout[0], r->lout_delay[0]) * H[8];
        /* ALchemy multiplies these two taps by H[2] (SR) and H[3] (FR), i.e. it routes the 30 degree
         * pan slot to the side and the 90 degree slot to the front, mirrored against the early
         * reflections and the left side.  reAL fixes that; with an unpanned reverb H[2] == H[3] and
         * the output is unchanged (tests/test_alrv swaps them back to compare with ALchemy). */
        o[ALRV_SR] += ring_read(&r->lout[0], r->lout_delay[1]) * H[3];
        o[ALRV_FR] += ring_read(&r->lout[1], r->lout_delay[2]) * H[2];
        o[ALRV_BL] += ring_read(&r->lout[1], r->lout_delay[3]) * H[6];
        ring_write(&r->lout[0], A[1]);
        ring_write(&r->lout[1], A[2]);

        for (int c = 0; c < ALRV_NOUT; c++) out[c][n] = o[c] * 40.0f;
    }
}

/* parameter computation lives in alrv_params.c */
