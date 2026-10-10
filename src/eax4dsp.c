/* reAL - EAX 4 effect DSP ported from ALchemy (see eax4dsp.h).  Rounded to float exactly where
 * ALchemy stores; the rest in double (ALchemy runs the FPU at 53-bit precision). */
#include "eax4dsp.h"
#include "eax.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float mb_lin(int32_t mb)
{
    if (mb < -9999) return 0.0f;
    float x = (float)((double)mb * 0.0005000000237487257);
    return (float)exp2((double)3.321928024291992f * (double)x);
}

/* per-sample parameter ramp: cur moves toward target by step, flag cleared on arrival */
static inline void ramp(float *cur, float target, float step, uint32_t *flags, uint32_t bit)
{
    if (!(*flags & bit)) return;
    float c = *cur;
    if (target <= c) {
        float n = (float)((double)c - step);
        *cur = n;
        if ((double)c - step <= target) { *cur = target; *flags &= ~bit; }
    } else {
        float n = (float)((double)c + step);
        *cur = n;
        if (!((double)c + step < target)) { *cur = target; *flags &= ~bit; }
    }
}

/* ================================================================== distortion (CPlugDistortion)
 * per sample: one-pole low-pass, log waveshaper y = sign(t) log10(1 + k|t|) g, a 2-pole lattice EQ band,
 * then a dry/wet crossfade (fns 0x10054790, 0x100543e0; parameters 0x10054900/970/a20, update 0x10051100) */

void fxd_edge(fx_distortion *d, float e)                     /* fn 0x10054900 */
{
    if (!((double)e <= 0.949999988079071)) e = 0.95f;
    d->flags |= 0xc;
    float a = (float)((1.0 - (double)e) * 0.28999999165534973 + 0.5);
    d->shape_t = (float)(3000.0 * (double)a);
    d->edge_norm = (float)(1.0 / (double)a);
    d->out_t = (float)((double)d->edge_norm * d->eq_norm * d->gain);
}

void fxd_gain(fx_distortion *d, int32_t g)                    /* update, dirty bit 2 */
{
    d->gain = mb_lin(g);
    d->flags |= 8;
    d->out_t = (float)((double)d->eq_norm * d->edge_norm * d->gain);
}

void fxd_cutoff(fx_distortion *d, float c)                   /* fn 0x10054970 */
{
    d->cutoff = c;
    if (!((double)d->rate * 0.5 > (double)c)) {                /* at or above Nyquist */
        d->flags |= 0x30;
        d->lpp_t = 0.4472135901451111f;
        d->lpg_t = 0.8944271802902222f;
        return;
    }
    float w = (float)((double)c * 6.2831854820251465 / d->rate);
    float cs = (float)cos(w);
    float v = (float)(((double)cs + cs + 3.0) / 5.0);
    d->lpp_t = (float)sqrt(v);
    d->lpg_t = (float)sqrt((float)(1.0 - v));
    d->flags |= 0x30;
}

static void eq_coefs(fx_distortion *d)                       /* fn 0x10054a20 */
{
    double half = (double)d->rate * 0.5;
    if ((double)d->center < half) d->eqc_t = -(float)cos((float)((double)d->center * 6.2831854820251465 / d->rate));
    else d->eqc_t = 1.0f;
    if ((double)d->bandwidth < half) {
        float t = (float)tan((float)((double)d->bandwidth * 3.1415927410125732 / d->rate));
        d->eqb_t = (float)((1.0 - t) / (1.0 + t));
    } else {
        d->eqb_t = -1.0f;
    }
    float g1 = (float)sqrt((float)(1.0 - (double)d->eqc_t * d->eqc_t));
    float g2 = (float)sqrt((float)(1.0 - (double)d->eqb_t * d->eqb_t));
    d->flags |= 0xb;
    d->eq_norm = (float)((double)g2 * g1);
    d->out_t = (float)((double)d->eq_norm * d->edge_norm * d->gain);
}

void fxd_eq(fx_distortion *d, float center, float bandwidth)
{
    d->center = center; d->bandwidth = bandwidth;
    eq_coefs(d);
}

void fxd_init(fx_distortion *d, uint32_t rate)                /* fn 0x10054850 */
{
    memset(d, 0, sizeof(*d));
    d->rate = (float)rate;
    d->ramp_mix = (float)(1.0 / ((double)d->rate * 0.019999999552965164));
    d->gain = 0.05011872202157974f;
    d->center = d->bandwidth = 3600.0f;
    d->mix = d->eq_norm = 1.0f;
    fxd_edge(d, 0.2f);
    fxd_cutoff(d, 8000.0f);
    eq_coefs(d);
    d->shape = d->shape_t; d->out = d->out_t; d->lpg = d->lpg_t; d->lpp = d->lpp_t;
    d->eqc = d->eqc_t; d->eqb = d->eqb_t;
}

static inline float dist_sample(const fx_distortion *d, float *s, float x)   /* fn 0x10054790 */
{
    float t = (float)((double)d->lpp * s[0] + (double)d->lpg * x + 9.99999993922529e-09);
    s[0] = t;
    float a = fabsf(t);
    float u = (float)((double)d->shape * a + 1.0);
    float y = (float)((double)(float)log10(u) * d->out);
    if (0.0f > t) y = -y;
    float old2 = s[2];
    float v = (float)(((double)y - (double)d->eqb * s[1]) - (double)d->eqc * s[2]);
    s[2] = v;
    s[1] = (float)((double)v * d->eqc + old2);
    return v;
}

void fxd_process(fx_distortion *d, float *const ch[2], uint32_t n)           /* fn 0x100543e0 */
{
    for (uint32_t i = 0; i < n; i++) {
        for (int c = 0; c < 2; c++) {
            if (!ch[c]) continue;
            double mix = d->mix;
            float x = ch[c][i];
            float v = dist_sample(d, d->st[c], x);
            ch[c][i] = (float)((1.0 - mix) * x + (double)v * mix);
        }
        ramp(&d->shape, d->shape_t, 0.01f, &d->flags, 4);
        ramp(&d->out, d->out_t, 0.001f, &d->flags, 8);
        ramp(&d->lpg, d->lpg_t, 0.01f, &d->flags, 0x10);
        ramp(&d->lpp, d->lpp_t, 0.01f, &d->flags, 0x20);
        ramp(&d->eqc, d->eqc_t, 0.01f, &d->flags, 1);
        ramp(&d->eqb, d->eqb_t, 0.01f, &d->flags, 2);
        ramp(&d->mix, d->mix_t, d->ramp_mix, &d->flags, 0x40);
    }
}

/* ================================================================== shared kernels */

void hsp_hpf_set(hsp_hpf *f, float fr)                       /* fn 0x1006f800 */
{
    if (fr == 0.0f) { f->m = f->k = 1.0f; return; }           /* bypass */
    if (!((double)fr < 0.4999499917030334)) { f->m = -1.0f; f->k = 0.0f; f->z = 0.0f; return; }
    float t = (float)tan((float)((double)fr * 3.1415927410125732));
    float k = (float)(1.0 / ((double)t + 1.0));
    f->k = k;
    f->m = (float)((double)k * (1.0 - (double)t));
}

/* ALchemy's kernels skip a block whose samples are all +0 or denormal: output zero, state cleared */
static int all_silent(const float *x, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u; memcpy(&u, &x[i], 4);
        if (u && (u & 0x7f800000u || !(u & 0x7fffffu))) return 0;
    }
    return 1;
}

void hsp_hpf_run(hsp_hpf *f, float *x, uint32_t n)           /* kernel #97: w = x + m w1, y = k (w - w1) */
{
    if (!n) return;
    if (f->k == 1.0f) { f->z = x[n - 1]; return; }
    if (f->k == 0.0f) { memset(x, 0, n * sizeof(float)); return; }
    if (all_silent(x, n)) { memset(x, 0, n * sizeof(float)); f->z = 0.0f; return; }
    float z = f->z;
    for (uint32_t i = 0; i < n; i++) {
        float w = (float)((double)z * f->m + x[i]);
        x[i] = (float)(((double)w - z) * f->k);
        z = w;
    }
    f->z = z;
}

/* ALchemy's LFO waveforms: phase in [-1, 1), wrapped after adding the offset (fns 0x1005bb20 / bc90 / bda0) */
static float osc_wrap(float p)
{
    if (p > 1.0f) p = (float)((double)p - 2.0);
    if (-1.0f > p) p = (float)((double)p + 2.0);
    return p;
}
static float osc_value(uint32_t wf, float phase, float off)
{
    float p = osc_wrap((float)((double)phase + off));
    if (wf == 1) return p;                                       /* saw */
    if (wf == 2) return 0.0f > p ? -1.0f : 1.0f;                 /* square */
    return (float)sin((float)((double)p * 3.1415927410125732));  /* sine */
}

/* ================================================================== ring modulator (CPlugRingModulator)
 * high-pass (cutoff glides x1.01 / /1.01 per 8 samples), then multiplication by a sine / saw / square
 * oscillator (process 0x10054ea0, update 0x10051480) */

static void rm_set_hp(fx_ringmod *r) { for (int c = 0; c < 2; c++) hsp_hpf_set(&r->hp[c], r->hp_cur); }

static void rm_rate(fx_ringmod *r, uint32_t rate)            /* fn 0x10054d90 */
{
    r->irate = rate;
    float fr = (float)rate;
    r->two_over_fs = (float)(2.0 / fr);
    float t = (float)((double)r->hpf_hz / fr);
    r->hp_t = 0.5f > t ? t : 0.5f;
    if (r->hp_cur != r->hp_t) { r->hp_cur = r->hp_t; rm_set_hp(r); }
}

void fxr_init(fx_ringmod *r, uint32_t rate)                  /* fn 0x10054c20 */
{
    memset(r, 0, sizeof(*r));
    r->inc = 0.018333333f; r->two_over_fs = 4.1667e-05f;
    for (int c = 0; c < 2; c++) { r->hp[c].m = 1.0f; r->hp[c].k = 1.0f; }
    rm_rate(r, rate);
}

void fxr_freq(fx_ringmod *r, float hz)
{
    r->freq = hz;
    float inc = (float)((double)hz * r->two_over_fs);
    if (inc <= 1.0f) r->inc = inc;
}

void fxr_highpass(fx_ringmod *r, float hz)                    /* fn 0x10054e20 */
{
    r->hpf_hz = hz;
    float t = (float)((double)hz / (double)(float)r->irate);
    r->hp_t = t;
    if (0.5f < t || 0.5f == t) r->hp_t = 0.5f;
}

void fxr_waveform(fx_ringmod *r, uint32_t w) { r->waveform = w; }

void fxr_process(fx_ringmod *r, float *const ch[2], uint32_t n)   /* fn 0x10054ea0 */
{
    /* ALchemy bug: every channel loop in 0x10054ea0 re-tests the channel it just found instead of moving
     * on, so the left channel is filtered and modulated twice and the right one is left dry.
     * reAL processes each channel once; r->alchemy_bug reproduces ALchemy for the tests. */
    static const int fixed[2] = { 0, 1 }, buggy[2] = { 0, 0 };
    const int *cm = r->alchemy_bug ? buggy : fixed;
    if (r->hp[0].k == 0.0f && r->hp_cur == r->hp_t) {
        for (int c = 0; c < 2; c++) if (ch[cm[c]]) memset(ch[cm[c]], 0, n * sizeof(float));
        return;
    }
    uint32_t pos = 0, left = n;
    while (left && r->hp_cur != r->hp_t) {
        float cur = r->hp_cur, tgt = r->hp_t;
        if (cur < tgt) {
            cur = cur == 0.0f ? 0.0001f : (float)((double)cur * 1.0099999904632568);
            r->hp_cur = cur;
            if (!(cur < tgt || cur == tgt)) r->hp_cur = tgt;
        } else {
            float nc = (float)((double)cur * 0.9900990128517151);
            r->hp_cur = nc;
            if (nc < tgt) r->hp_cur = tgt;
            else if (tgt == 0.0f && !(0.0001 < (double)nc)) r->hp_cur = 0.0f;
        }
        rm_set_hp(r);
        for (int c = 0; c < 2; c++) if (ch[cm[c]]) hsp_hpf_run(&r->hp[cm[c]], ch[cm[c]] + pos, 8);
        pos += 8; left -= 8;
    }
    if (r->hp[0].k != 1.0f && left)
        for (int c = 0; c < 2; c++) if (ch[cm[c]]) hsp_hpf_run(&r->hp[cm[c]], ch[cm[c]] + pos, left);
    if (r->freq == 0.0f) {
        float v = osc_value(r->waveform, r->phase, 0.0f);
        for (int c = 0; c < 2; c++) if (ch[cm[c]]) for (uint32_t i = 0; i < n; i++) ch[cm[c]][i] = (float)((double)ch[cm[c]][i] * v);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        float v = osc_value(r->waveform, r->phase, 0.0f);
        float p = (float)((double)r->phase + r->inc);
        r->phase = p;
        if (!(1.0f > p)) { p = (float)((double)p - 2.0); r->phase = p; }
        if (-1.0f > p) r->phase = (float)((double)p + 2.0);
        for (int c = 0; c < 2; c++) if (ch[cm[c]]) ch[cm[c]][i] = (float)((double)ch[cm[c]][i] * v);
    }
}

/* ---- frequency shifter (CPlugFrequencyShifter) ---- */
void fxf_init(fx_fshift *f, uint32_t rate)                    /* ctor 0x10058ea0, rate 0x10058fc0 */
{
    static const float k[6] = { -0.153796971f, 0.842622697f, -0.996813357f, -0.503923237f, 0.973453581f,
                                -0.999708116f };
    memset(f, 0, sizeof(*f));
    memcpy(f->c, k, sizeof(k));
    f->rate = (float)rate;
}

/* ALchemy bug (update 0x10052460): the increment is 4 pi f / fs, so it shifts by twice the frequency asked
 * for.  reAL uses 2 pi f / fs; alchemy_bug restores ALchemy's for the tests. */
void fxf_freq(fx_fshift *f, float hz)
{
    f->inc = (float)((double)hz * (f->alchemy_bug ? 12.566370964050293 : 6.2831854820251465) / f->rate);
}
void fxf_dir(fx_fshift *f, uint32_t l, uint32_t r) { f->dir[0] = l; f->dir[1] = r; }

static inline float ap_chain(const float *c, float *st, float x)   /* returns the first stage output */
{
    float out = (float)((1.0 - c[0]) * st[0] + (double)c[0] * x);
    float u1 = (float)((double)x * (c[0] + 1.0) - (double)c[0] * st[0]);
    st[0] = (float)((1.0 - c[1]) * st[1] + (double)u1 * c[1]);
    float u2 = (float)((double)u1 * (c[1] + 1.0) - (double)c[1] * st[1]);
    st[1] = (float)((1.0 - c[2]) * st[2] + (double)u2 * c[2]);
    st[2] = (float)((double)u2 * (c[2] + 1.0) - (double)c[2] * st[2]);
    return out;
}

void fxf_process(fx_fshift *f, float *const ch[2], uint32_t n)  /* fn 0x10059010 */
{
    for (uint32_t i = 0; i < n; i++) {
        float sum = 0.0f;
        for (int c = 0; c < 2; c++) sum = (float)((double)(ch[c] ? ch[c][i] : 0.0f) + sum);
        float inv = (float)(1.0 / 2.0);
        float x = (float)((double)inv * 0.5 * sum);
        float a = ap_chain(f->c, f->st, x);
        float b = ap_chain(f->c + 3, f->st + 3, f->st[6]);
        f->st[6] = x;
        float p = (float)(1.0 + f->phase);
        f->phase = p;
        float w = (float)((double)p * f->inc);
        if (!(w < 6.2831854820251465)) {
            w = (float)((double)w - 6.2831854820251465);
            f->phase = (float)((double)w / f->inc);
        }
        float re = (float)((double)(float)sin(w) * a);
        float im = (float)((double)(float)cos(w) * b);
        float dn = (float)((double)re - im), up = (float)((double)im + re);
        for (int c = 0; c < 2; c++) {
            if (!ch[c]) continue;
            if (f->dir[c] == 0) ch[c][i] = dn;
            else if (f->dir[c] == 1) ch[c][i] = up;
        }
    }
}

/* ---- AGC compressor (CPlugAGCCompressor): look-ahead compressor, detector on the loudest channel ---- */
static double rnd_magic(double v) { return nearbyint(v); }  /* x87 "+ 1.5*2^52" rounding, nearest even */

static void agc_reset(fx_agc *a)                              /* fn 0x10059a30 */
{
    if (!a->on) return;
    a->save_on = a->onv; a->save_mk = a->makeup;
    float d = (float)(a->rate * (double)0.02f);
    uint32_t n = (uint32_t)(int64_t)rnd_magic((double)d + 1.4999999464748726e-08);
    a->save_d = a->d1 = a->d2 = n;
    memset(a->hp, 0, sizeof(a->hp)); memset(a->P, 0, sizeof(a->P));
    memset(a->w, 0, sizeof(a->w)); memset(a->buf, 0, sizeof(a->buf));
    a->fade_dn = 0; a->cf = 1.0f; a->fade_up = 0;
    a->E = a->H = a->Gs = 1.0f;
}

static void agc_attack(fx_agc *a, float t)                     /* fn 0x10059c20 */
{
    a->att_t = t;
    float x = (float)((double)t * (double)0.46593979001045227);
    float e = (float)(-1.0 / ((double)x * a->rate));
    a->c_att = (float)(1.0 - (float)pow(10.0, e));
}

static void agc_makeup(fx_agc *a, float db)                    /* fn 0x10059c80 */
{
    float v = a->on ? db : 0.0f;
    a->makeup = (float)pow(10.0, (float)(v / 20.0));
}

static void agc_lookahead(fx_agc *a, float s)                  /* fn 0x10059cd0 */
{
    a->look = s;
    float v = a->on ? (float)((double)s * a->rate) : 0.0f;
    if (!(v > 30.0)) v = 30.0f;
    float cap = (float)(a->rate * (double)0.05f);
    if (!(cap > v)) v = cap;
    double t = (double)v - 0.5 + 1.4999999464748726e-08;   /* v >= 0 here */
    uint32_t n = (uint32_t)(int64_t)rnd_magic(t);
    if (0.5f < a->cf) { a->fade_dn = 1; a->fade_up = 0; a->d2 = n; }
    else { a->fade_up = 1; a->fade_dn = 0; a->d1 = n; }
}

static void agc_dcblock(float fs, float *b, float *a)          /* fn 0x10059e10 */
{
    static const float t[][3] = {
        { 192000, 0.996699989f, -0.993499994f }, { 176400, 0.996500015f, -0.992900014f },
        { 96000, 0.993499994f, -0.986999989f }, { 88200, 0.992900014f, -0.985899985f },
        { 48000, 0.987100005f, -0.97420001f }, { 44100, 0.986000001f, -0.971899986f },
        { 32000, 0.980700016f, -0.961499989f }, { 24000, 0.9745f, -0.949000001f },
        { 22050, 0.972299993f, -0.944599986f }, { 16000, 0.962199986f, -0.924399972f },
        { 12000, 0.950200021f, -0.900399983f }, { 11025, 0.94599998f, -0.892099977f } };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (fs >= t[i][0]) { *b = t[i][1]; *a = t[i][2]; return; }
    *b = 0.926999986f; *a = -0.854099989f;
}

static void agc_rate(fx_agc *a, uint32_t rate)                 /* fn 0x10059520 */
{
    float fs = (float)rate;
    if (fs == a->rate) { agc_reset(a); return; }
    a->rate = fs;
    agc_dcblock(fs, &a->hb, &a->ha);
    float l = (float)log10((double)0.1f);
    float e = (float)(l / ((double)fs * (double)0.04f));
    a->c_pow = (float)(1.0 - (float)pow(10.0, e));
    e = (float)(-1.0 / ((double)fs * (double)0.01f));
    a->c_gs = (float)(1.0 - (float)pow(10.0, e));
    a->cf_step = (float)(1.0 / ((double)fs * (double)0.08f));
    agc_attack(a, a->att_t);
    e = (float)(-1.0 / ((double)fs * a->rel_t));
    a->c_rel = (float)(1.0 - (float)pow(10.0, e));
    agc_lookahead(a, a->look);
    agc_reset(a);
}

void fxa_init(fx_agc *a, uint32_t rate)                       /* ctor 0x10059360 */
{
    memset(a, 0, sizeof(*a));
    a->on = 1; a->thr = -6.2f; a->ratio = 0.4f;
    a->makeup = (float)pow(10.0, (double)1.1f);
    a->onv = 1.0f; a->att_t = 0.004f; a->rel_t = 8.0f; a->look = 0.02f;
    agc_reset(a);
    agc_rate(a, rate);
}

void fxa_onoff(fx_agc *a, int on)                             /* fn 0x10059b20 */
{
    if (a->on) {
        if (on) return;
        a->save_on = a->onv; a->on = 0; a->onv = 0.0f; a->save_mk = a->makeup;
        agc_makeup(a, 0.0f);
        a->save_d = 0.5f < a->cf ? a->d1 : a->d2;
        agc_lookahead(a, 0.0f);
    } else {
        if (!on) return;
        a->onv = a->save_on; a->on = 1; a->makeup = a->save_mk;
        /* ALchemy bug: it loads the saved look-ahead into the idle tap but fades towards the active one, so
         * after off -> on the look-ahead stays at 30 samples.  reAL fades to the restored tap. */
        int up = 0.5f < a->cf ? a->alchemy_bug : !a->alchemy_bug;
        if (0.5f < a->cf) a->d2 = a->save_d; else a->d1 = a->save_d;
        a->fade_up = up; a->fade_dn = !up;
    }
}

static void agc_fade(fx_agc *a)                               /* fn 0x10059fc0 */
{
    if (a->fade_dn) {
        float v = (float)((double)a->cf - a->cf_step); a->cf = v;
        if (!(0.0f < v || 0.0f == v)) { a->cf = 0.0f; a->fade_dn = 0; }
    } else if (a->fade_up) {
        float v = (float)((double)a->cf_step + a->cf); a->cf = v;
        if (1.0f < v) { a->cf = 1.0f; a->fade_up = 0; }
    }
}

void fxa_process(fx_agc *a, float *const ch[2], uint32_t n)   /* fn 0x10059690 */
{
    for (uint32_t i = 0; i < n; i++) {
        float m = 0.0f;
        for (int c = 0; c < 2; c++) {
            float x = ch[c] ? ch[c][i] : 0.0f;
            a->buf[c][a->w[c]] = x;
            if (++a->w[c] == FXA_DLEN) a->w[c] = 0;
            float t = (float)(((double)x - a->hp[c][0]) * a->hb);
            a->hp[c][0] = x;
            float y = (float)((double)t - (double)a->hp[c][1] * a->ha);
            a->hp[c][1] = y;
            float q = (float)((double)y * y);
            float p = (float)((double)q * a->c_pow + (1.0 - a->c_pow) * a->P[c]);
            a->P[c] = p;
            if (c == 0 || m < p) m = p;
        }
        float L;
        if (m == 0.0f) L = a->thr;
        else { L = (float)log10(m); if (a->thr > L) L = a->thr; }
        float T = (float)((double)a->thr + ((double)L - a->thr) * a->ratio);
        float G = (float)((double)T - L);
        float g = (float)pow(10.0, (float)(G * 0.5));
        float k = (a->E < g || a->E == g || a->E != a->E || g != g) ? a->c_rel : a->c_att;
        float Eo = a->E;
        a->E = (float)((double)g * k + (double)a->E * (1.0 - k));
        float K = (float)(((double)a->onv * a->H + 1.0 - a->onv) * a->makeup);
        a->H = (float)((double)a->H * (1.0 - a->c_att) + (double)a->c_att * Eo);
        float gs = a->Gs;
        a->Gs = (float)((double)a->Gs + ((double)K - a->Gs) * a->c_gs);
        for (int c = 0; c < 2; c++) {
            uint32_t w = a->w[c];
            uint32_t i1 = w >= a->d1 ? w - a->d1 : w - a->d1 + FXA_DLEN;
            uint32_t i2 = w >= a->d2 ? w - a->d2 : w - a->d2 + FXA_DLEN;
            float A = a->buf[c][i1], B = a->buf[c][i2];
            if (ch[c]) ch[c][i] = (float)(((1.0 - a->cf) * B + (double)a->cf * A) * gs);
        }
        if (a->fade_dn || a->fade_up) agc_fade(a);
    }
}

/* ---- equalizer (CPlugEqualiser) ---- */
#define F_PI 3.1415927410125732          /* (double)(float)pi, as ALchemy multiplies */
#define F_SQRT2 1.4142135381698608

static void eq_pass(eq_filter *f, float g)
{
    if (g == 0.0f) { memset(f->c, 0, sizeof(f->c)); f->s1 = f->s2 = 0.0f; return; }
    f->c[0] = f->c[1] = f->c[3] = f->c[4] = 0.0f; f->c[2] = 1.0f;
}

static void eq_peak(eq_filter *f)                              /* fn 0x1006f8a0 */
{
    float G = f->g;
    if (G == 0.0f || G == 1.0f) { eq_pass(f, G); return; }
    float t = (float)tan((float)(f->F * F_PI));
    float A = (float)((double)t * t + 1.0);
    float B = (float)((double)t / f->q);
    if (1.0f < G) {
        float D = (float)((double)B + A);
        float c4 = (float)((2.0 - A) * 2.0 / D);
        f->c[0] = c4;
        f->c[1] = (float)(((double)B - A) / D);
        f->c[2] = (float)(((double)A + (double)B * G) / D);
        f->c[3] = -c4;
        f->c[4] = (float)(((double)A - (double)B * G) / D);
    } else {
        float Bg = (float)((double)B / G);
        float D = (float)((double)Bg + A);
        float c4 = (float)((2.0 - A) * 2.0 / D);
        f->c[0] = c4;
        f->c[1] = (float)(((double)Bg - A) / D);
        f->c[2] = (float)(((double)B + A) / D);
        f->c[3] = -c4;
        f->c[4] = (float)(((double)A - B) / D);
    }
}

static void eq_low(eq_filter *f)                               /* fn 0x1006fce0 */
{
    float G = f->g;
    if (G == 0.0f || G == 1.0f) { eq_pass(f, G); return; }
    float t = (float)tan((float)(f->F * F_PI));
    if (1.0f < G) {
        float A = (float)((double)t * t + 1.0);
        float R = (float)(F_SQRT2 * t);
        float Q = (float)((double)t * ((double)t * G));
        float D = (float)((double)R + A);
        f->c[0] = (float)((2.0 - A) * 2.0 / D);
        f->c[1] = (float)(((double)R - A) / D);
        float sG = (float)sqrt(G);
        f->c[2] = (float)((((double)sG * R + 1.0) + Q) / D);
        f->c[3] = (float)(((double)Q * 2.0 - 2.0) / D);
        f->c[4] = (float)(((1.0 - (double)sG * R) + Q) / D);
    } else {
        float iG = (float)(1.0 / G);
        float T2 = (float)((double)t * t);
        float A = (float)(1.0 + T2);
        float R = (float)((double)t * F_SQRT2);
        float P = (float)((double)T2 * iG);
        float sI = (float)sqrt(iG);
        float SR = (float)((double)sI * R);
        float D = (float)(((double)SR + 1.0) + P);
        f->c[0] = (float)((2.0 - (double)P * 2.0) / D);
        f->c[1] = (float)((((double)SR - P) - 1.0) / D);
        f->c[2] = (float)(((double)R + A) / D);
        f->c[3] = (float)(((double)T2 * 2.0 - 2.0) / D);
        f->c[4] = (float)(((double)A - R) / D);
    }
}

static void eq_high(eq_filter *f)                              /* fn 0x10070120 */
{
    float G = f->g;
    if (G == 0.0f) { eq_pass(f, G); return; }
    if (G == 1.0f || !(f->F < 0.5f)) { eq_pass(f, 1.0f); return; }
    float t = (float)tan((float)(f->F * F_PI));
    float T2 = (float)((double)t * t);
    if (1.0f < G) {
        float A = (float)((double)T2 + 1.0);
        float R = (float)((double)t * F_SQRT2);
        float D = (float)((double)R + A);
        f->c[0] = (float)(((1.0 - T2) + (1.0 - T2)) / D);
        f->c[1] = (float)(((double)R - A) / D);
        float sG = (float)sqrt(G);
        f->c[2] = (float)((((double)G + (double)sG * R) + T2) / D);
        f->c[3] = (float)((((double)T2 - G) + ((double)T2 - G)) / D);
        f->c[4] = (float)(((double)T2 + ((double)G - (double)sG * R)) / D);
    } else {
        float iG = (float)(1.0 / G);
        float A = (float)((double)T2 + 1.0);
        float R = (float)((double)t * F_SQRT2);
        float sG = (float)sqrt(G);
        float SR = (float)((double)sG * R);
        float TG = (float)((double)T2 * G);
        float D = (float)(((double)SR + 1.0) + TG);
        float sI = (float)sqrt(iG);
        float N = (float)((((double)sI * R) + iG) + T2);
        f->c[0] = (float)((1.0 - TG) * 2.0 / D);
        f->c[1] = (float)((((double)SR - TG) - 1.0) / D);
        f->c[2] = (float)(((double)R + A) / N);
        f->c[3] = (float)(2.0 * ((double)T2 - 1.0) / N);
        f->c[4] = (float)(((double)A - R) / N);
    }
}

static void peq_coefs(eq_filter *f)
{
    if (f->type == 0) eq_low(f); else if (f->type == 1) eq_peak(f); else eq_high(f);
}

static void eq_init_filter(eq_filter *f, int type, float hz, float fs)   /* fn 0x1005ae20 / 0x1005aff0 */
{
    f->type = type;
    f->g = f->gt = 1.0f;
    f->hz = hz; f->fs = fs;
    f->F = f->Ft = (float)((double)hz / fs);
    peq_coefs(f);
}

static int eq_gliding(const eq_filter *f)                      /* vtable +0xc */
{
    return f->g != f->gt || f->F != f->Ft || (f->type == 1 && f->q != f->qt);
}

static void eq_glide(eq_filter *f)                             /* vtable +0x10: 0x1005af20 / 0x1005b090 */
{
    if (f->gt != f->g) {
        if (f->gt <= f->g) { f->g = (float)((double)f->g * 0.9803921580314636); if (f->g < f->gt) f->g = f->gt; }
        else { f->g = (float)((double)f->g * 1.0199999809265137); if (f->gt < f->g) f->g = f->gt; }
    }
    if (f->type == 1 && f->qt != f->q) {
        if (f->qt <= f->q) { f->q = (float)((double)f->q * 0.9990009665489197); if (f->q < f->qt) f->q = f->qt; }
        else { f->q = (float)((double)f->q * 1.0010000467300415); if (f->qt < f->q) f->q = f->qt; }
    }
    if (f->Ft != f->F) {
        if (f->Ft <= f->F) { f->F = (float)((double)f->F * 0.9990009665489197); if (f->F < f->Ft) f->F = f->Ft; }
        else { f->F = (float)((double)f->F * 1.0010000467300415); if (f->Ft < f->F) f->F = f->Ft; }
    }
}

static void eq_kernel(eq_filter *f, float *x, uint32_t n)       /* kernel #88 0x10071e70: biquad, DF II */
{
    if (all_silent(x, n)) { memset(x, 0, n * sizeof(float)); f->s1 = f->s2 = 0.0f; return; }
    float s1 = f->s1, s2 = f->s2;
    const float *c = f->c;
    for (uint32_t i = 0; i < n; i++) {
        float w = (float)(((double)s1 * c[0] + x[i]) + (double)c[1] * s2);
        x[i] = (float)(((double)w * c[2] + (double)s1 * c[3]) + (double)c[4] * s2);
        s2 = s1; s1 = w;
    }
    f->s1 = s1; f->s2 = s2;
}

static void eq_run(eq_filter *f, float *x, uint32_t n)         /* fn 0x1005ae70 */
{
    while (n && eq_gliding(f)) {
        eq_glide(f); peq_coefs(f);
        uint32_t k = n < 8 ? n : 8;
        eq_kernel(f, x, k);
        x += k; n -= k;
    }
    if (n && f->g != 1.0f) eq_kernel(f, x, n);
}

void fxe_init(fx_eq *e, uint32_t rate)                        /* ctor 0x1005b4f0, rate 0x1005b670 */
{
    static const float mid_hz[2] = { 500.0f, 3000.0f }, mid_x[2] = { 200.0f, 500.0f };
    memset(e, 0, sizeof(*e));
    float fs = (float)rate;
    e->rate = fs;
    for (int c = 0; c < 2; c++) {
        eq_filter *f = e->f[c];
        for (int b = 0; b < 4; b++) { f[b].g = f[b].gt = 1.0f; f[b].fs = 1.0f; f[b].q = f[b].qt = 1.0f; }
        eq_init_filter(&f[0], 0, 200.0f, fs);
        for (int b = 1; b <= 2; b++) {
            f[b].type = 1;
            f[b].q = (float)((double)mid_hz[b - 1] / mid_x[b - 1]);
            f[b].g = f[b].gt = 1.0f;
            f[b].hz = mid_hz[b - 1]; f[b].fs = fs;
            f[b].F = f[b].Ft = (float)((double)mid_hz[b - 1] / fs);
            peq_coefs(&f[b]);
        }
        eq_init_filter(&f[3], 2, 6000.0f, fs);
    }
}

void fxe_gain(fx_eq *e, int band, int32_t mb)
{
    float g = mb_lin(mb);
    for (int c = 0; c < 2; c++) e->f[c][band].gt = g;
}

void fxe_freq(fx_eq *e, int band, float hz)                   /* fn 0x1005ae60 */
{
    for (int c = 0; c < 2; c++) {
        eq_filter *f = &e->f[c][band];
        f->hz = hz; f->Ft = (float)((double)hz / f->fs);
    }
}

void fxe_width(fx_eq *e, int band, float w)                   /* fn 0x1005b9c0: Q from octaves */
{
    float a = (float)pow(2.0, (float)(w * 0.5));
    float b = (float)pow(2.0, (float)(-w * 0.5));
    float q = (float)(1.0 / ((double)a - b));
    for (int c = 0; c < 2; c++) e->f[c][band].qt = q;
}

void fxe_process(fx_eq *e, float *const ch[2], uint32_t n)    /* fn 0x1005b710 */
{
    for (int c = 0; c < 2; c++) {
        if (!ch[c]) continue;
        for (int b = 0; b < 4; b++) eq_run(&e->f[c][b], ch[c], n);
    }
}

/* ---- autowah (CPlugAutowah): envelope follower (log10 level) sweeping an all-pass based resonance ---- */
enum { AW_ATT, AW_REL, AW_RES, AW_PEAK, AW_MIX };
static const uint32_t aw_bit[5] = { 1, 2, 0x20, 0x80, 0x10 };

void fxw_init(fx_autowah *w, uint32_t rate)                   /* ctor 0x1005a060 */
{
    memset(w, 0, sizeof(*w));
    float fs = (float)rate;
    w->fs = fs; w->b = 0.985000014f; w->env = 0.5f;
    float c06 = (float)pow(10.0, -1.0 / ((double)fs * 0.05999999865889549));
    float c018 = (float)pow(10.0, -1.0 / ((double)fs * 0.018000001087784767));
    float r = (float)((double)(float)pow(2.0, 3.0) * 0.5);
    w->cur[AW_ATT] = w->cur[AW_REL] = c06;
    w->tgt[AW_ATT] = w->tgt[AW_REL] = c018;
    w->cur[AW_RES] = w->tgt[AW_RES] = r;
    w->cur[AW_PEAK] = w->tgt[AW_PEAK] = -0.180000007f;
    w->cur[AW_MIX] = w->tgt[AW_MIX] = 1.0f;
    double t = (double)0.02f * fs;
    w->step[AW_ATT] = w->step[AW_REL] = (float)(0.3809860348701477 / t);
    w->step[AW_RES] = (float)(3.384428024291992 / t);
    w->step[AW_PEAK] = (float)(3.5999999046325684 / t);
    w->step[AW_MIX] = (float)(1.0 / t);
    w->att_time = w->rel_time = 0.0054000006f;
    w->flags = 3;
}

/* attack/release: per-sample smoothing coefficient 10^(-1 / (t fs)) (update 0x10052800).  ALchemy also keeps
 * 0.3 t and its rate change (0x1005a280) rebuilds the coefficient from that, multiplying by 0.3 again each time;
 * reAL rebuilds effects on a rate change instead, so only the update's formula matters. */
void fxw_attack(fx_autowah *w, float s)
{
    w->att_time = (float)(0.30000001192092896 * s);
    w->tgt[AW_ATT] = (float)pow(10.0, -1.0 / ((double)s * w->fs));
    w->flags |= 1;
}

void fxw_release(fx_autowah *w, float s)
{
    w->rel_time = (float)(0.30000001192092896 * s);
    w->tgt[AW_REL] = (float)pow(10.0, -1.0 / ((double)s * w->fs));
    w->flags |= 2;
}

void fxw_resonance(fx_autowah *w, int32_t mb)
{
    float e = (float)((double)mb / 2000.0);
    w->tgt[AW_RES] = (float)((double)(float)pow(2.0, e) * 0.5);
    w->flags |= 0x20;
}

void fxw_peak(fx_autowah *w, int32_t mb)
{
    float p = (float)(-((double)mb / 100.0 - 12.0));
    w->tgt[AW_PEAK] = (float)((double)p / 50.0);
    w->flags |= 0x80;
}

void fxw_process(fx_autowah *w, float *const ch[2], uint32_t n)   /* fn 0x1005a3b0 */
{
    static const int order[5] = { AW_ATT, AW_REL, AW_RES, AW_PEAK, AW_MIX };
    for (uint32_t i = 0; i < n; i++) {
        float sum = 0.0f;
        for (int c = 0; c < 2; c++) sum = (float)((double)(ch[c] ? ch[c][i] : 0.0f) + sum);
        float s = (float)((double)sum * 0.7070000171661377);
        s = fabsf(s);
        s = (float)((double)s + 9.999999974752427e-07);
        float L = (float)log10(s);
        float K = (w->env < L || w->env == L) ? w->cur[AW_ATT] : w->cur[AW_REL];
        w->env = (float)((double)L * (1.0 - K) + (double)K * w->env);
        float v = (float)((double)w->env + w->cur[AW_PEAK]);
        if (0.0f < v) v = 0.0f;
        float p = (float)pow(10.0, v);
        float a = (float)((double)p - 1.0);
        if (0.0f < a) a = 0.0f;
        else if (a < -1.0f) a = -1.0f;
        w->a = a;
        double b = w->b, r = w->cur[AW_RES], m = w->cur[AW_MIX];
        for (int c = 0; c < 2; c++) {
            if (!ch[c]) continue;
            float x = ch[c][i], *P = w->P[c];
            float h = (float)(((b - 1.0) * x + (1.0 - b) * P[0]) * r);
            float y = (float)((double)h + x);
            float u = (float)((double)x * (b + 1.0) - (double)P[0] * b);
            P[0] = (float)((double)a * u + (1.0 - a) * P[1]);
            P[1] = (float)((double)u * (a + 1.0) - (double)P[1] * a);
            ch[c][i] = (float)(m * y + (1.0 - m) * x);
        }
        for (int k = 0; k < 5; k++) {
            int j = order[k];
            if (!(w->flags & aw_bit[j])) continue;
            float cur = w->cur[j], tg = w->tgt[j];
            if (tg < cur || tg == cur) {
                cur = (float)((double)cur - w->step[j]); w->cur[j] = cur;
                if (!(tg < cur)) { w->cur[j] = tg; w->flags &= ~aw_bit[j]; }
            } else {
                cur = (float)((double)cur + w->step[j]); w->cur[j] = cur;
                if (tg < cur || tg == cur) { w->cur[j] = tg; w->flags &= ~aw_bit[j]; }
            }
        }
    }
}

/* ---- echo (CPlugEcho) ---- */
static uint32_t rnd_samples(float v)                          /* x87 "-0.5 + 1.5e-8, + 1.5*2^52" rounding */
{
    double t = v < 0.0f ? ((double)v + 0.5) - 1.4999999464748726e-08 : ((double)v - 0.5) + 1.4999999464748726e-08;
    return (uint32_t)(int64_t)nearbyint(t);
}

static void echo_tap2(fx_echo *o)                              /* fn 0x10055db0 */
{
    o->n2 = rnd_samples((float)((double)o->rate * ((double)o->lrdelay + o->delay)));
    o->xf2 = 1.0f;
}

void fxo_delay(fx_echo *o, float d)                           /* fn 0x10055c20 */
{
    o->delay = d;
    if (o->c1 == o->n1) { o->n1 = rnd_samples((float)((double)o->rate * d)); o->xf1 = 1.0f; }
    if (o->c2 == o->n2) echo_tap2(o);
}

void fxo_lrdelay(fx_echo *o, float s) { o->lrdelay = s; if (o->c2 == o->n2) echo_tap2(o); }   /* update bit 2 */
void fxo_damping(fx_echo *o, float v) { o->damp_t = v; }      /* fn 0x10055e10 */
void fxo_feedback(fx_echo *o, float v) { o->fb_t = v; }       /* fn 0x10055ed0 */
void fxo_spread(fx_echo *o, float v)
{
    o->spread = v;
    o->sp_t = (float)(((double)v + 1.0) * 0.5);
}

static float echo_coef(float damp)                             /* fn 0x100553f0 */
{
    float c = (float)pow(damp, 0.5);
    return c < 0.99f || c == 0.99f ? c : 0.99f;
}

int fxo_init(fx_echo *o, uint32_t rate)                       /* ctor 0x10055430 + alloc 0x10055ab0 */
{
    memset(o, 0, sizeof(*o));
    o->rate = rate;
    o->len = rnd_samples((float)((double)rate * 0.6110000014305115)) + 1;
    o->buf = calloc(o->len, sizeof(float));
    if (!o->buf) return 0;
    o->delay = 0.1f; o->lrdelay = 0.1f;
    o->c1 = o->n1 = rnd_samples((float)((double)rate * o->delay));
    o->c2 = o->n2 = rnd_samples((float)((double)rate * ((double)o->lrdelay + o->delay)));
    o->xf1 = o->xf2 = 1.0f;
    o->xstep = (float)(1.0 / ((double)(float)rate * 0.20000000298023224));
    o->damp = o->damp_t = 0.5f; o->fb = o->fb_t = 0.5f;
    o->spread = -1.0f; o->sp = o->sp_t = 0.0f;
    fxo_activate(o);
    return 1;
}

void fxo_free(fx_echo *o) { free(o->buf); o->buf = NULL; }

void fxo_activate(fx_echo *o)                                 /* state machine 0x10055580, states 1 and 3 */
{
    o->damp = o->damp_t; o->c1 = o->n1; o->c2 = o->n2; o->fb = o->fb_t;
    o->coef = echo_coef(o->damp);
}

static inline uint32_t echo_idx(const fx_echo *o, uint32_t k) { return o->wp >= k ? o->wp - k : o->len - k + o->wp; }

static inline void ramp_lin(float *cur, float tgt, double step)
{
    if (*cur < tgt) { *cur = (float)((double)*cur + step); if (tgt < *cur) *cur = tgt; }
    else { *cur = (float)((double)*cur - step); if (!(tgt < *cur || tgt == *cur)) *cur = tgt; }
}

void fxo_process(fx_echo *o, float *const ch[2], uint32_t n)   /* fn 0x10055f80 */
{
    for (uint32_t i = 0; i < n; i++) {
        float v1, v2;
        if (o->c1 == o->n1) v1 = o->buf[echo_idx(o, o->c1)];
        else {
            float A = o->buf[echo_idx(o, o->c1)], B = o->buf[echo_idx(o, o->n1)];
            v1 = (float)((1.0 - o->xf1) * B + (double)A * o->xf1);
            o->xf1 = (float)((double)o->xf1 - o->xstep);
            if (!(0.0f < o->xf1)) { o->c1 = o->n1; fxo_delay(o, o->delay); }
        }
        if (o->c2 == o->n2) v2 = o->buf[echo_idx(o, o->c2)];
        else {
            float A = o->buf[echo_idx(o, o->c2)], B = o->buf[echo_idx(o, o->n2)];
            v2 = (float)((double)B * (1.0 - o->xf2) + (double)o->xf2 * A);
            o->xf2 = (float)((double)o->xf2 - o->xstep);
            if (!(0.0f < o->xf2)) { o->c2 = o->n2; echo_tap2(o); }
        }
        if (o->damp != o->damp_t) { ramp_lin(&o->damp, o->damp_t, 9.999999747378752e-05); o->coef = echo_coef(o->damp); }
        if (o->fb != o->fb_t) ramp_lin(&o->fb, o->fb_t, 0.0010000000474974513);
        if (o->sp != o->sp_t) ramp_lin(&o->sp, o->sp_t, 0.0010000000474974513);
        double q0 = (double)o->fb * v2;
        float q = (float)(ch[0] ? q0 + ch[0][i] : q0);
        float lp = (float)((1.0 - o->coef) * q + (double)o->lp * o->coef);
        o->lp = lp;
        o->buf[o->wp] = lp;
        if (++o->wp == o->len) o->wp = 0;
        float om = (float)(1.0 - o->sp);
        float L = (float)((double)o->sp * v1 + (double)om * v2);
        float R = (float)((double)v2 * o->sp + (double)om * v1);
        if (ch[0]) ch[0][i] = L;
        if (ch[1]) ch[1][i] = R;
    }
}

/* ---- chorus (CPlugChorus): LFO-modulated delay with feedback; changes of the centre delay, depth or
 * phase cross-fade between the old and new taps over 20 ms ---- */
static float lfo_value(const fx_osc *o, float off)            /* fns 0x1005bb20 (sine), 0x1005bc20 (triangle) */
{
    float q = osc_wrap((float)((double)o->phase + off));
    if (o->tri) return (float)(2.0 * fabsf(q) - 1.0);
    return (float)sin((float)((double)q * 3.1415927410125732));
}

static float chorus_read(const fx_chorus *c, float D)           /* fn 0x10057d50: linear interpolation */
{
    uint32_t n = rnd_samples(D);
    float fr = (float)((double)D - (double)n);
    uint32_t i0 = c->wp >= n ? c->wp - n : c->wp - n + FXC_DLEN;
    uint32_t i1 = c->wp >= n + 1 ? c->wp - n - 1 : c->wp - n + FXC_DLEN - 1;
    return (float)((double)c->buf[i0] * (1.0 - fr) + (double)fr * c->buf[i1]);
}

static float chorus_amount(const fx_chorus *c, float depth)    /* fn 0x10057890 */
{
    float m = (float)((double)c->delay * c->fs);
    if (!(25.0 < m || 25.0 == m)) m = 25.0f;
    float a = (float)((double)m * depth);
    double lim = (double)m - 25.0;
    return (double)a < lim || (double)a == lim ? a : (float)lim;
}

static int right_side(int k) { return k == 1; }               /* ALchemy: speaker 1, 5, 7, 10 */

void fxc_delay(fx_chorus *c, float d)                         /* fn 0x10057b30 */
{
    c->delay = d;
    float m = (float)((double)d * c->fs);
    if (m < 25.0f) m = 25.0f;
    float D0 = (float)floor((double)(float)(0.5 + m));
    if (!c->running) {
        c->d_cur = c->d_new = c->fd_cur = c->fd_new = D0;
        c->amt_cur = c->amt_new = chorus_amount(c, c->depth);
        for (int k = 0; k < 2; k++) c->flags[k] &= ~0x30u;
        return;
    }
    int idle = 1;
    for (int k = 0; k < 2; k++) if (c->flags[k] & 0x10) { c->flags[k] |= 0x40; idle = 0; }
    if (c->flags[0] & 0x20) { c->flags[0] |= 0x80; return; }
    if (!idle) return;
    c->d_new = c->fd_new = D0;
    c->amt_new = chorus_amount(c, c->depth);
    c->ph_new = (float)((double)c->phase / 180.0);
    for (int k = 0; k < 2; k++) { c->flags[k] |= 0x10; c->xf[k] = 1.0f; }
    c->flags[0] |= 0x20;
    c->fxf = 1.0f;
}

void fxc_depth(fx_chorus *c, float d)                         /* fn 0x100578f0 */
{
    c->depth = d;
    float a = chorus_amount(c, d);
    if (!c->running) {
        for (int k = 0; k < 2; k++) c->flags[k] &= ~0x10u;
        c->amt_cur = c->amt_new = a;
        return;
    }
    int idle = 1;
    for (int k = 0; k < 2; k++) if (c->flags[k] & 0x10) { c->flags[k] |= 0x40; idle = 0; }
    if (!idle) return;
    c->amt_new = a;
    for (int k = 0; k < 2; k++) { c->flags[k] |= 0x10; c->xf[k] = 1.0f; }
}

void fxc_phase(fx_chorus *c, int32_t deg)                     /* update bit 2 / fn 0x100579b0 */
{
    float p = (float)deg;
    c->phase = p;
    if (!c->running) { c->ph_cur = c->ph_new = (float)((double)p / 180.0); return; }
    int idle = 1;
    for (int k = 0; k < 2; k++)
        if (c->flags[k] & 0x10) { idle = 0; if (right_side(k)) c->flags[k] |= 0x40; }
    if (!idle) return;
    for (int k = 0; k < 2; k++) if (right_side(k)) { c->flags[k] |= 0x10; c->xf[k] = 1.0f; }
    c->ph_new = (float)((double)p / 180.0);
}

void fxc_rate(fx_chorus *c, float hz)                         /* fn 0x10058880 */
{
    for (int k = 0; k < 2; k++) {
        float v = (float)((double)c->osc[k].two_over_fs * hz);
        if (!(v > 1.0f)) c->osc[k].inc = v;
    }
}

void fxc_waveform(fx_chorus *c, uint32_t tri) { for (int k = 0; k < 2; k++) c->osc[k].tri = tri != 0; }

void fxc_feedback(fx_chorus *c, float fb)                     /* fn 0x10057a70 */
{
    c->fb_t = fb;
    if (!c->running) { c->flags[0] &= ~8u; c->fb = fb; }
    else c->flags[0] |= 8;
}

void fxc_init(fx_chorus *c, uint32_t rate)                    /* ctor 0x10056b90, defaults 0x100576e0 */
{
    memset(c, 0, sizeof(*c));
    c->fs = (float)rate;
    c->step = (float)(1.0 / ((double)c->fs * 0.019999999552965164));
    for (int k = 0; k < 2; k++) {
        c->osc[k].two_over_fs = (float)(2.0 / c->fs);
        float v = (float)((double)c->osc[k].two_over_fs * 1.100000023841858);
        if (!(v > 1.0f)) c->osc[k].inc = v;
        c->osc[k].tri = 1;
    }
    c->phase = 90.0f; c->ph_cur = c->ph_new = 0.5f;
    fxc_feedback(c, 0.25f);
    fxc_delay(c, 0.016f);
    fxc_depth(c, 0.1f);
    for (int k = 0; k < 2; k++) c->mix[k] = 1.0f;
}

static inline void ramp_step(float *cur, float tgt, float step, uint32_t *flags, uint32_t bit)
{
    if (tgt < *cur || tgt == *cur) {
        *cur = (float)((double)*cur - step);
        if (!(tgt < *cur)) { *cur = tgt; *flags &= ~bit; }
    } else {
        *cur = (float)((double)*cur + step);
        if (tgt < *cur || tgt == *cur) { *cur = tgt; *flags &= ~bit; }
    }
}

void fxc_process(fx_chorus *c, float *const ch[2], uint32_t n)  /* fn 0x100571c0 */
{
    for (uint32_t i = 0; i < n; i++) {
        float x = ch[0] ? ch[0][i] : 0.0f;
        float w;
        if (!(c->flags[0] & 0x20)) w = (float)((double)chorus_read(c, c->fd_cur) * c->fb + x);
        else {
            c->fxf = (float)((double)c->fxf - c->step);
            if (!(c->fxf > 0.0f)) { c->fxf = 0.0f; c->flags[0] &= ~0x20u; }
            double xd = c->fxf;
            double t = (1.0 - xd) * chorus_read(c, c->fd_new);
            w = (float)(((double)chorus_read(c, c->fd_cur) * xd + t) * c->fb + x);
            if (!(c->flags[0] & 0x20)) {
                c->fd_cur = c->fd_new;
                if (c->flags[0] & 0x80) { c->flags[0] &= ~0x80u; fxc_delay(c, c->delay); }
            }
        }
        c->buf[c->wp] = w;
        if (++c->wp == FXC_DLEN) c->wp = 0;
        if (c->flags[0] & 8) ramp_step(&c->fb, c->fb_t, c->step, &c->flags[0], 8);
        for (int k = 0; k < 2; k++) {
            fx_osc *o = &c->osc[k];
            float lfo = lfo_value(o, right_side(k) ? c->ph_cur : 0.0f);
            float y;
            if (!(c->flags[k] & 0x10)) y = chorus_read(c, (float)((double)c->amt_cur * lfo + c->d_cur));
            else {
                float lfo2 = right_side(k) ? lfo_value(o, c->ph_new) : lfo;
                c->xf[k] = (float)((double)c->xf[k] - c->step);
                if (!(0.0f < c->xf[k])) { c->xf[k] = 0.0f; c->flags[k] &= ~0x10u; }
                double xd = c->xf[k];
                double t = (1.0 - xd) * chorus_read(c, (float)((double)c->amt_new * lfo2 + c->d_new));
                y = (float)((double)chorus_read(c, (float)((double)c->amt_cur * lfo + c->d_cur)) * xd + t);
                if (!(c->flags[k] & 0x10)) {
                    c->d_cur = c->d_new; c->amt_cur = c->amt_new; c->ph_cur = c->ph_new;
                    if (c->flags[k] & 0x40) { c->flags[k] &= ~0x40u; fxc_delay(c, c->delay); }
                }
            }
            float xin = ch[0] ? ch[0][i] : 0.0f;    /* ALchemy re-reads the left buffer (already the left output for k = 1) */
            float out = (float)((1.0 - c->mix[k]) * xin + (double)y * c->mix[k]);
            if (ch[k]) ch[k][i] = out;
            float p = (float)((double)o->inc + o->phase);
            o->phase = p;
            if (!(p < 1.0f)) o->phase = (float)((double)p - 2.0);
            if (!(-1.0f < o->phase)) o->phase = (float)((double)o->phase + 2.0);
            if (c->flags[k] & 1) ramp_step(&c->mix[k], c->mix_t[k], c->step, &c->flags[k], 1);
        }
    }
    c->running = 1;
}

/* ---- flanger (CPlugFlanger) ---- */
static float flanger_read(const fx_flanger *f, int k, float D)  /* fn 0x10058dd0 */
{
    uint32_t n = rnd_samples(D), wp = f->wp[k];
    float fr = (float)((double)D - (double)n);
    uint32_t i0 = wp >= n ? wp - n : wp - n + FXL_DLEN;
    uint32_t i1 = wp >= n + 1 ? wp - n - 1 : wp - n + FXL_DLEN - 1;
    return (float)((double)f->buf[k][i0] * (1.0 - fr) + (double)fr * f->buf[k][i1]);
}

static float flanger_amount(const fx_flanger *f, float depth) /* fn 0x100588c0 */
{
    float m = (float)((double)f->delay * f->fs);
    if (m < 10.0f) m = 10.0f;
    float a = (float)((double)m * depth);
    double lim = ((double)m - 10.0) + 2.0;
    return (double)a < lim || (double)a == lim ? a : (float)lim;
}

void fxl_delay(fx_flanger *f, float d)                        /* fn 0x10058bd0 */
{
    f->delay = d;
    float m = (float)((double)d * f->fs);
    if (m < 10.0f) m = 10.0f;
    float D0 = (float)floor((double)(float)(0.5 + m));
    if (!f->running) {
        f->d_cur = f->d_new = f->fd_cur = f->fd_new = D0;
        f->amt_cur = f->amt_new = flanger_amount(f, f->depth);
        for (int k = 0; k < 2; k++) f->flags[k] &= ~0x30u;
        return;
    }
    int idle = 1;
    for (int k = 0; k < 2; k++) {
        if (f->flags[k] & 0x10) { f->flags[k] |= 0x40; idle = 0; }
        if (f->flags[k] & 0x20) { f->flags[k] |= 0x80; idle = 0; }
    }
    if (!idle) return;
    f->d_new = f->fd_new = D0;
    f->amt_new = flanger_amount(f, f->depth);
    f->ph_new = (float)((double)f->phase / 180.0);
    for (int k = 0; k < 2; k++) { f->flags[k] |= 0x30; f->xf[k] = 1.0f; f->fxf[k] = 1.0f; }
}

void fxl_depth(fx_flanger *f, float d)                        /* fn 0x10058930 */
{
    f->depth = d;
    float a = flanger_amount(f, d);
    if (!f->running) {
        for (int k = 0; k < 2; k++) f->flags[k] &= ~0x10u;
        f->amt_cur = f->amt_new = a;
        return;
    }
    int idle = 1;
    for (int k = 0; k < 2; k++) if (f->flags[k] & 0x10) { f->flags[k] |= 0x40; idle = 0; }
    if (!idle) return;
    f->amt_new = a;
    for (int k = 0; k < 2; k++) { f->flags[k] |= 0x10; f->xf[k] = 1.0f; }
}

void fxl_phase(fx_flanger *f, int32_t deg)                    /* update bit 2 / fn 0x100589f0 */
{
    float p = (float)deg;
    f->phase = p;
    if (!f->running) { f->ph_cur = f->ph_new = (float)((double)p / 180.0); return; }
    int idle = 1;
    for (int k = 0; k < 2; k++)
        if (f->flags[k] & 0x10) { idle = 0; if (right_side(k)) f->flags[k] |= 0x40; }
    if (!idle) return;
    for (int k = 0; k < 2; k++) if (right_side(k)) { f->flags[k] |= 0x10; f->xf[k] = 1.0f; }
    f->ph_new = (float)((double)p / 180.0);
}

void fxl_rate(fx_flanger *f, float hz)                        /* fn 0x10058880 */
{
    for (int k = 0; k < 2; k++) {
        float v = (float)((double)f->osc[k].two_over_fs * hz);
        if (!(v > 1.0f)) f->osc[k].inc = v;
    }
}

void fxl_waveform(fx_flanger *f, uint32_t tri) { for (int k = 0; k < 2; k++) f->osc[k].tri = tri != 0; }

void fxl_feedback(fx_flanger *f, float fb)                    /* fn 0x10058ab0 */
{
    for (int k = 0; k < 2; k++) {
        f->fb_t[k] = fb;
        if (!f->running) { f->flags[k] &= ~8u; f->fb[k] = fb; }
        else f->flags[k] |= 8;
    }
}

void fxl_init(fx_flanger *f, uint32_t rate)                   /* ctor 0x10057e20, defaults 0x10058710 */
{
    memset(f, 0, sizeof(*f));
    f->fs = (float)rate;
    f->step = (float)(1.0 / ((double)f->fs * 0.019999999552965164));
    for (int k = 0; k < 2; k++) {
        f->osc[k].two_over_fs = (float)(2.0 / f->fs);
        float v = (float)((double)f->osc[k].two_over_fs * 0.27000001072883606);
        if (!(v > 1.0f)) f->osc[k].inc = v;
        f->osc[k].tri = 1;
    }
    fxl_feedback(f, -0.5f);
    fxl_delay(f, 0.002f);
    fxl_depth(f, 1.0f);
    for (int k = 0; k < 2; k++) f->mix[k] = 1.0f;
}

void fxl_process(fx_flanger *f, float *const ch[2], uint32_t n)  /* fn 0x100581e0 */
{
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k < 2; k++) {
            float x = ch[k] ? ch[k][i] : 0.0f;
            float w;
            if (!(f->flags[k] & 0x20)) w = (float)((double)flanger_read(f, k, f->fd_cur) * f->fb[k] + x);
            else {
                f->fxf[k] = (float)((double)f->fxf[k] - f->step);
                if (!(f->fxf[k] > 0.0f)) { f->fxf[k] = 0.0f; f->flags[k] &= ~0x20u; }
                double xd = f->fxf[k];
                double t = (1.0 - xd) * flanger_read(f, k, f->fd_new);
                w = (float)(((double)flanger_read(f, k, f->fd_cur) * xd + t) * f->fb[k] + x);
                if (!(f->flags[k] & 0x20)) {
                    f->fd_cur = f->fd_new;
                    if (f->flags[k] & 0x80) { f->flags[k] &= ~0x80u; fxl_delay(f, f->delay); }
                }
            }
            f->buf[k][f->wp[k]] = w;
            if (++f->wp[k] == FXL_DLEN) f->wp[k] = 0;
            fx_osc *o = &f->osc[k];
            float lfo = lfo_value(o, right_side(k) ? f->ph_cur : 0.0f);
            float y;
            if (!(f->flags[k] & 0x10)) y = flanger_read(f, k, (float)((double)lfo * f->amt_cur + f->d_cur));
            else {
                float lfo2 = right_side(k) ? lfo_value(o, f->ph_new) : lfo;
                f->xf[k] = (float)((double)f->xf[k] - f->step);
                if (!(0.0f < f->xf[k])) { f->xf[k] = 0.0f; f->flags[k] &= ~0x10u; }
                double xd = f->xf[k];
                double t = (1.0 - xd) * flanger_read(f, k, (float)((double)f->amt_new * lfo2 + f->d_new));
                y = (float)((double)flanger_read(f, k, (float)((double)lfo * f->amt_cur + f->d_cur)) * xd + t);
                if (!(f->flags[k] & 0x10)) {
                    f->d_cur = f->d_new; f->amt_cur = f->amt_new; f->ph_cur = f->ph_new;
                    if (f->flags[k] & 0x40) { f->flags[k] &= ~0x40u; fxl_delay(f, f->delay); }
                }
            }
            if (ch[k]) ch[k][i] = (float)((1.0 - f->mix[k]) * x + (double)y * f->mix[k]);
            float p = (float)((double)o->inc + o->phase);
            o->phase = p;
            if (!(p < 1.0f)) o->phase = (float)((double)p - 2.0);
            if (!(-1.0f < o->phase)) o->phase = (float)((double)o->phase + 2.0);
            if (f->flags[k] & 1) ramp_step(&f->mix[k], f->mix_t[k], f->step, &f->flags[k], 1);
            if (f->flags[k] & 8) ramp_step(&f->fb[k], f->fb_t[k], f->step, &f->flags[k], 8);
        }
    }
    f->running = 1;
}

/* ---- pitch shifter (CPlugPitchShifter): two taps sweeping through a 2048 sample window, cross-faded with
 * sin / cos of the window angle ---- */
static float pshift_read(const fx_pshift *p, float D)          /* fn 0x1005ad50 */
{
    uint32_t n = rnd_samples(D);
    float fr = (float)((double)D - (double)(int32_t)n);
    uint32_t i0 = p->wp >= n ? p->wp - n : p->wp - n + FXP_DLEN;
    uint32_t i1 = p->wp >= n + 1 ? p->wp - n - 1 : p->wp - n + FXP_DLEN - 1;
    return (float)((double)p->buf[i0] * (1.0 - fr) + (double)fr * p->buf[i1]);
}

/* ALchemy bug (fn 0x1005acd0): the taps move by 2^(pitch/12) + 1 samples per sample, so the read position
 * runs backwards at the pitch ratio: every grain plays reversed (a sine still comes out at the right pitch,
 * anything else does not).  reAL moves them by 1 - ratio, which reads forwards at the ratio; the window
 * speed follows |1 - ratio| so the cross-fade stays in step with the tap wraps. */
static void pshift_coefs(fx_pshift *p)
{
    float e = (float)(((double)p->cents / 100.0 + p->semis) / 12.0);
    float ratio = (float)pow(2.0, e);
    float r = p->alchemy_bug ? (float)((double)ratio + 1.0) : (float)(1.0 - ratio);
    p->rate_new = r;
    p->win_new = 3.1415927410125732 * fabs((double)r) * 0.00048828125;
}

void fxp_set(fx_pshift *p, int32_t semis, int32_t cents) { p->semis = semis; p->cents = cents; pshift_coefs(p); }

void fxp_init(fx_pshift *p, uint32_t rate, int bug)          /* ctor 0x1005a7e0, 0x1005abc0 */
{
    (void)rate;
    memset(p, 0, sizeof(*p));
    p->ofs = 1026.0f;
    p->alchemy_bug = bug;
    fxp_set(p, 12, 0);
    p->rate = p->rate_new; p->win_inc = p->win_new;
}

void fxp_process(fx_pshift *p, float *const ch[2], uint32_t n)  /* fn 0x1005a900 */
{
    for (uint32_t i = 0; i < n; i++) {
        p->buf[p->wp] = ch[0] ? ch[0][i] : 0.0f;
        if (++p->wp == FXP_DLEN) p->wp = 0;
        float pf = (float)p->p;
        float q = (float)((double)pf - 1024.0);
        if (q < -1024.0f) q = (float)((double)q + 2048.0);
        double np = p->p + p->rate;
        p->p = np > 1024.0 ? np - 2048.0 : np < -1024.0 ? np + 2048.0 : np;
        p->n = p->n + 1.0;
        float a = (float)(p->n * p->win_inc);
        if (!(a < 6.2831854820251465)) {
            a = (float)((double)a - 6.2831854820251465);
            p->n = (double)a / p->win_inc;
            if (p->rate_new != p->rate) { p->rate = p->rate_new; p->win_inc = p->win_new; }
        }
        double ofs = p->ofs;
        float r1 = pshift_read(p, (float)(ofs + q));
        float v1 = (float)((double)(float)sin(a) * r1);
        float r2 = pshift_read(p, (float)(ofs + pf));
        float v2 = (float)((double)(float)cos(a) * r2);
        float out = (float)(((double)v2 + v1) * 0.5);
        for (int c = 0; c < 2; c++) if (ch[c]) ch[c][i] = out;
    }
}

/* ---- vocal morpher (CPlugVocalMorpher): 4 state-variable band-passes per channel whose formants move
 * between two phonemes with an LFO ---- */
static const double vm_phonemes[30][8] = {   /* .rdata 0x100a70f0: 4 formant frequencies, 4 Qs */
    { 650.0, 1080.0, 2650.0, 2900.0, 8.13, 12.0, 22.08, 22.31 },
    { 400.0, 1700.0, 2600.0, 3200.0, 5.71, 21.25, 26.0, 26.67 },
    { 290.0, 1870.0, 2800.0, 3250.0, 7.75, 20.78, 28.0, 27.08 },
    { 400.0, 800.0, 2600.0, 2800.0, 10.0, 10.0, 26.0, 23.33 },
    { 350.0, 600.0, 2700.0, 2900.0, 8.75, 10.0, 27.0, 24.17 },
    { 730.0, 1090.0, 2440.0, 3390.0, 12.0, 22.0, 24.0, 23.0 },
    { 660.0, 1720.0, 2410.0, 3350.0, 10.0, 19.0, 15.0, 22.0 },
    { 640.0, 1190.0, 2390.0, 2990.0, 11.0, 19.0, 23.0, 20.0 },
    { 570.0, 840.0, 2410.0, 3440.0, 12.0, 17.0, 25.0, 23.0 },
    { 530.0, 1840.0, 2480.0, 3260.0, 13.0, 26.0, 20.0, 22.0 },
    { 490.0, 1350.0, 1690.0, 3380.0, 11.0, 23.0, 29.0, 23.0 },
    { 390.0, 1990.0, 2550.0, 3640.0, 9.0, 28.0, 18.0, 24.0 },
    { 270.0, 2290.0, 3010.0, 3760.0, 7.0, 35.0, 18.0, 25.0 },
    { 440.0, 1020.0, 2240.0, 3250.0, 9.0, 17.0, 25.0, 22.0 },
    { 300.0, 870.0, 2240.0, 3070.0, 6.0, 15.0, 21.0, 20.0 },
    { 700.0, 1500.0, 2500.0, 3450.0, 10.0, 6.0, 13.0, 9.0 },
    { 200.0, 1500.0, 2500.0, 2950.0, 5.0, 19.0, 21.0, 20.0 },
    { 400.0, 1150.0, 2020.0, 2610.0, 6.0, 12.0, 8.0, 17.0 },
    { 280.0, 2200.0, 2200.0, 3450.0, 5.0, 28.0, 18.0, 23.0 },
    { 280.0, 1650.0, 2500.0, 3000.0, 6.0, 14.0, 21.0, 20.0 },
    { 400.0, 1700.0, 1800.0, 3020.0, 5.0, 43.0, 16.0, 20.0 },
    { 300.0, 1200.0, 2300.0, 2900.0, 12.0, 30.0, 77.0, 19.0 },
    { 250.0, 1200.0, 2200.0, 2750.0, 10.0, 5.0, 9.0, 18.0 },
    { 200.0, 1450.0, 2070.0, 2500.0, 10.0, 7.0, 21.0, 17.0 },
    { 200.0, 960.0, 1960.0, 2470.0, 4.0, 12.0, 16.0, 16.0 },
    { 400.0, 1350.0, 2000.0, 2400.0, 8.0, 17.0, 17.0, 16.0 },
    { 200.0, 1600.0, 2600.0, 3600.0, 5.0, 20.0, 22.0, 24.0 },
    { 200.0, 2300.0, 3000.0, 3800.0, 3.0, 23.0, 15.0, 25.0 },
    { 400.0, 1100.0, 2300.0, 3100.0, 3.0, 9.0, 13.0, 21.0 },
    { 350.0, 1650.0, 2500.0, 3800.0, 4.0, 7.0, 21.0, 20.0 },
};

static void vm_set(fx_vmorph *v, uint32_t ph, int32_t coarse, float *f, float *q)   /* fns 0x100569d0 / 0x10056a80 */
{
    if (ph > 29) ph = 29;
    float tune = (float)pow(2.0, (float)((double)coarse / 12.0));
    for (int j = 0; j < 4; j++) {
        float hz = (float)vm_phonemes[ph][j];
        float a = (float)((double)hz * 3.1415927410125732 * tune / v->fs);
        f[j] = (float)(2.0 * (float)sin(a));
        q[j] = (float)(1.0 / vm_phonemes[ph][4 + j]);
    }
}

void fxv_phoneme_a(fx_vmorph *v, uint32_t ph) { v->pa = ph; vm_set(v, ph, v->ca, v->fa, v->qa); }
void fxv_coarse_a(fx_vmorph *v, int32_t c) { v->ca = c; vm_set(v, v->pa, c, v->fa, v->qa); }
void fxv_phoneme_b(fx_vmorph *v, uint32_t ph) { v->pb = ph; vm_set(v, ph, v->cb, v->fb, v->qb); }
void fxv_coarse_b(fx_vmorph *v, int32_t c) { v->cb = c; vm_set(v, v->pb, c, v->fb, v->qb); }

static void vm_coef(fx_vmorph *v)                              /* fns 0x10056410 / 0x10056b30 */
{
    if (v->wf == 0) v->coef = (float)sin((float)((double)v->rate * 6.2831854820251465 / v->ofs));
    else if (v->wf <= 2) v->coef = (float)(((double)v->rate + v->rate) / v->ofs);
}

void fxv_waveform(fx_vmorph *v, uint32_t wf) { v->wf = wf; vm_coef(v); }
void fxv_rate(fx_vmorph *v, float hz) { v->rate = hz; vm_coef(v); }

/* ALchemy bug: its state reset (0x10056690, also run by the constructor) clears only four of the sixteen
 * filter states and overwrites the first formant coefficient of phoneme A with 0, which silences that formant
 * until phoneme A is set again.  reAL clears the states only. */
void fxv_reset(fx_vmorph *v)
{
    if (v->alchemy_bug) { v->lp[1][0] = v->bp[1][0] = v->bp[0][0] = 0.0f; v->fa[0] = 0.0f; }
    else { memset(v->lp, 0, sizeof(v->lp)); memset(v->bp, 0, sizeof(v->bp)); }
}

void fxv_init(fx_vmorph *v, uint32_t rate, int bug)          /* ctor 0x100565b0, 0x10056900 */
{
    memset(v, 0, sizeof(*v));
    v->alchemy_bug = bug;
    v->x = 1.0f; v->dir = 1.0f; v->ofs = 48000.0f;
    v->fs = (float)rate;
    fxv_phoneme_a(v, 0);
    fxv_phoneme_b(v, 10);
    v->ofs = v->fs;                   /* LFO sample rate */
    v->wf = 0; vm_coef(v);
    fxv_rate(v, 1.40999997f);
    v->mix = 1.0f;
    fxv_reset(v);
}

static float vm_lfo(fx_vmorph *v)                              /* fn 0x100564d0 */
{
    float t = (float)((double)v->x - (double)v->coef * v->y);
    v->y = (float)((double)v->coef * v->x + v->y);
    if (v->wf == 0) v->x = t;
    else if (v->wf == 1) {
        float n = (float)((double)v->dir * v->coef + v->x);
        v->x = n;
        if (n < -1.0f || n == -1.0f) v->dir = 1.0f;
        if (1.0f < v->x || 1.0f == v->x) v->dir = -1.0f;
    } else if (v->wf == 2) {
        float n = (float)((double)v->coef + v->x);
        v->x = n;
        if (!(n < 1.0f)) v->x = -1.0f;
    }
    if (1.0f < v->x) v->x = 1.0f;
    if (!(v->x >= -1.0f)) v->x = -1.0f;
    if (1.0f < v->y) v->y = 1.0f;
    if (!(v->y >= -1.0f)) v->y = -1.0f;
    return v->x;
}

void fxv_process(fx_vmorph *v, float *const ch[2], uint32_t n)  /* fn 0x10056740 */
{
    for (uint32_t i = 0; i < n; i++) {
        float in[2], acc[2] = { 0.0f, 0.0f };
        for (int c = 0; c < 2; c++) in[c] = (float)((double)(ch[c] ? ch[c][i] : 0.0f) * 0.25);
        float m = (float)(0.5 + (double)vm_lfo(v) * 0.5);
        double om = 1.0 - m;
        for (int j = 0; j < 4; j++) {
            float F = (float)((double)v->fa[j] * om + (double)v->fb[j] * m);
            float Q = (float)((double)v->qa[j] * om + (double)v->qb[j] * m);
            for (int c = 0; c < 2; c++) {
                float lp = (float)((double)v->bp[c][j] * F + v->lp[c][j]);
                v->lp[c][j] = lp;
                float hp = (float)(((double)in[c] - (double)v->bp[c][j] * Q) - lp);
                float bp = (float)((double)v->bp[c][j] + (double)hp * F);
                v->bp[c][j] = bp;
                acc[c] = (float)((double)acc[c] + bp);
            }
        }
        for (int c = 0; c < 2; c++)
            if (ch[c]) ch[c][i] = (float)((double)acc[c] * v->mix + (1.0 - v->mix) * ch[c][i]);
    }
}

/* ================================================================== slot effect units */
struct eax4fx_unit {
    int type, fresh;
    uint32_t rate;
    uint32_t applied[16];
    float *tmp[2]; uint32_t tmp_frames;
    union {
        fx_agc agc; fx_autowah aw; fx_chorus ch; fx_distortion dist; fx_echo echo; fx_eq eq; fx_flanger fl;
        fx_fshift fs; fx_pshift ps; fx_ringmod rm; fx_vmorph vm;
    } u;
};

eax4fx_unit *eax4fx_unit_new(int type, uint32_t rate)
{
    if (type <= EAX4FX_NONE || type >= EAX4FX_COUNT || type == EAX4FX_REVERB || !rate) return NULL;
    eax4fx_unit *x = calloc(1, sizeof(*x));
    if (!x) return NULL;
    x->type = type; x->rate = rate; x->fresh = 1;
    switch (type) {
    case EAX4FX_AGCCOMPRESSOR: fxa_init(&x->u.agc, rate); break;
    case EAX4FX_AUTOWAH: fxw_init(&x->u.aw, rate); break;
    case EAX4FX_CHORUS: fxc_init(&x->u.ch, rate); break;
    case EAX4FX_DISTORTION: fxd_init(&x->u.dist, rate); break;
    case EAX4FX_ECHO: if (!fxo_init(&x->u.echo, rate)) { free(x); return NULL; } break;
    case EAX4FX_EQUALIZER: fxe_init(&x->u.eq, rate); break;
    case EAX4FX_FLANGER: fxl_init(&x->u.fl, rate); break;
    case EAX4FX_FREQSHIFTER: fxf_init(&x->u.fs, rate); break;
    case EAX4FX_PITCHSHIFTER: fxp_init(&x->u.ps, rate, 0); break;
    case EAX4FX_RINGMODULATOR: fxr_init(&x->u.rm, rate); break;
    case EAX4FX_VOCALMORPHER: fxv_init(&x->u.vm, rate, 0); break;
    }
    return x;
}

void eax4fx_unit_free(eax4fx_unit *x)
{
    if (!x) return;
    if (x->type == EAX4FX_ECHO) fxo_free(&x->u.echo);
    free(x->tmp[0]);
    free(x);
}

int eax4fx_unit_type(const eax4fx_unit *x) { return x->type; }
uint32_t eax4fx_unit_rate(const eax4fx_unit *x) { return x->rate; }

static float W2F(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

void eax4fx_unit_props(eax4fx_unit *x, const uint32_t fx[16])
{
    uint32_t d = 0;
    for (int i = 0; i < 16; i++) if (x->fresh || fx[i] != x->applied[i]) d |= 1u << i;
    if (!d) return;
    switch (x->type) {
    case EAX4FX_AGCCOMPRESSOR:
        if (d & 1) fxa_onoff(&x->u.agc, fx[0] != 0);
        break;
    case EAX4FX_AUTOWAH:
        if (d & 1) fxw_attack(&x->u.aw, W2F(fx[0]));
        if (d & 2) fxw_release(&x->u.aw, W2F(fx[1]));
        if (d & 4) fxw_resonance(&x->u.aw, (int32_t)fx[2]);
        if (d & 8) fxw_peak(&x->u.aw, (int32_t)fx[3]);
        break;
    case EAX4FX_CHORUS:
        if (d & 1) fxc_waveform(&x->u.ch, fx[0]);
        if (d & 2) fxc_phase(&x->u.ch, (int32_t)fx[1]);
        if (d & 4) fxc_rate(&x->u.ch, W2F(fx[2]));
        if (d & 8) fxc_depth(&x->u.ch, W2F(fx[3]));
        if (d & 16) fxc_feedback(&x->u.ch, W2F(fx[4]));
        if (d & 32) fxc_delay(&x->u.ch, W2F(fx[5]));
        break;
    case EAX4FX_DISTORTION:
        if (d & 1) fxd_edge(&x->u.dist, W2F(fx[0]));
        if (d & 2) fxd_gain(&x->u.dist, (int32_t)fx[1]);
        if (d & 4) fxd_cutoff(&x->u.dist, W2F(fx[2]));
        if (d & 24) fxd_eq(&x->u.dist, W2F(fx[3]), W2F(fx[4]));
        break;
    case EAX4FX_ECHO:
        if (d & 1) fxo_delay(&x->u.echo, W2F(fx[0]));
        if (d & 2) fxo_lrdelay(&x->u.echo, W2F(fx[1]));
        if (d & 4) fxo_damping(&x->u.echo, W2F(fx[2]));
        if (d & 8) fxo_feedback(&x->u.echo, W2F(fx[3]));
        if (d & 16) fxo_spread(&x->u.echo, W2F(fx[4]));
        if (x->fresh) fxo_activate(&x->u.echo);     /* ALchemy's slot activation: start at the properties */
        break;
    case EAX4FX_EQUALIZER:
        if (d & 1) fxe_gain(&x->u.eq, 0, (int32_t)fx[0]);
        if (d & 2) fxe_freq(&x->u.eq, 0, W2F(fx[1]));
        if (d & 4) fxe_gain(&x->u.eq, 1, (int32_t)fx[2]);
        if (d & 8) fxe_freq(&x->u.eq, 1, W2F(fx[3]));
        if (d & 16) fxe_width(&x->u.eq, 1, W2F(fx[4]));
        if (d & 32) fxe_gain(&x->u.eq, 2, (int32_t)fx[5]);
        if (d & 64) fxe_freq(&x->u.eq, 2, W2F(fx[6]));
        if (d & 128) fxe_width(&x->u.eq, 2, W2F(fx[7]));
        if (d & 256) fxe_gain(&x->u.eq, 3, (int32_t)fx[8]);
        if (d & 512) fxe_freq(&x->u.eq, 3, W2F(fx[9]));
        break;
    case EAX4FX_FLANGER:
        if (d & 1) fxl_waveform(&x->u.fl, fx[0]);
        if (d & 2) fxl_phase(&x->u.fl, (int32_t)fx[1]);
        if (d & 4) fxl_rate(&x->u.fl, W2F(fx[2]));
        if (d & 8) fxl_depth(&x->u.fl, W2F(fx[3]));
        if (d & 16) fxl_feedback(&x->u.fl, W2F(fx[4]));
        if (d & 32) fxl_delay(&x->u.fl, W2F(fx[5]));
        break;
    case EAX4FX_FREQSHIFTER:
        if (d & 1) fxf_freq(&x->u.fs, W2F(fx[0]));
        if (d & 6) fxf_dir(&x->u.fs, fx[1], fx[2]);
        break;
    case EAX4FX_PITCHSHIFTER:
        if (d & 3) fxp_set(&x->u.ps, (int32_t)fx[0], (int32_t)fx[1]);
        if (x->fresh) { x->u.ps.rate = x->u.ps.rate_new; x->u.ps.win_inc = x->u.ps.win_new; }
        break;
    case EAX4FX_RINGMODULATOR:
        if (d & 1) fxr_freq(&x->u.rm, W2F(fx[0]));
        if (d & 2) fxr_highpass(&x->u.rm, W2F(fx[1]));
        if (d & 4) fxr_waveform(&x->u.rm, fx[2]);
        break;
    case EAX4FX_VOCALMORPHER:
        if (d & 1) fxv_phoneme_a(&x->u.vm, fx[0]);
        if (d & 2) fxv_coarse_a(&x->u.vm, (int32_t)fx[1]);
        if (d & 4) fxv_phoneme_b(&x->u.vm, fx[2]);
        if (d & 8) fxv_coarse_b(&x->u.vm, (int32_t)fx[3]);
        if (d & 16) fxv_waveform(&x->u.vm, fx[4]);
        if (d & 32) fxv_rate(&x->u.vm, W2F(fx[5]));
        break;
    }
    memcpy(x->applied, fx, sizeof(x->applied));
    x->fresh = 0;
}

void eax4fx_unit_process(eax4fx_unit *x, const float *bus, float *out, float gain, uint32_t n)
{
    if (x->tmp_frames < n) {
        free(x->tmp[0]);
        x->tmp[0] = malloc((size_t)n * 2 * sizeof(float));
        if (!x->tmp[0]) { x->tmp_frames = 0; return; }
        x->tmp[1] = x->tmp[0] + n;
        x->tmp_frames = n;
    }
    float *L = x->tmp[0], *R = x->tmp[1], *const ch[2] = { L, R };
    memcpy(L, bus, n * sizeof(float));
    memcpy(R, bus, n * sizeof(float));
    switch (x->type) {
    case EAX4FX_AGCCOMPRESSOR: fxa_process(&x->u.agc, ch, n); break;
    case EAX4FX_AUTOWAH: fxw_process(&x->u.aw, ch, n); break;
    case EAX4FX_CHORUS: fxc_process(&x->u.ch, ch, n); break;
    case EAX4FX_DISTORTION: fxd_process(&x->u.dist, ch, n); break;
    case EAX4FX_ECHO: fxo_process(&x->u.echo, ch, n); break;
    case EAX4FX_EQUALIZER: fxe_process(&x->u.eq, ch, n); break;
    case EAX4FX_FLANGER: fxl_process(&x->u.fl, ch, n); break;
    case EAX4FX_FREQSHIFTER: fxf_process(&x->u.fs, ch, n); break;
    case EAX4FX_PITCHSHIFTER: fxp_process(&x->u.ps, ch, n); break;
    case EAX4FX_RINGMODULATOR: fxr_process(&x->u.rm, ch, n); break;
    case EAX4FX_VOCALMORPHER: fxv_process(&x->u.vm, ch, n); break;
    }
    for (uint32_t i = 0; i < n; i++) { out[2 * i] += L[i] * gain; out[2 * i + 1] += R[i] * gain; }
}
