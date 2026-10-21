/* reAL - CMSS-3D: ALchemy's stereo-to-surround upmixer (CCMSS3Upmix).  See cmss3.h.
 *
 * The names of ALchemy's functions are given where each step lives in its dsound.dll; the comments on the
 * fields give the object offsets (relative to the component at upmixer+0x76104) they mirror. */
#include "cmss3.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cmss3_tables.h"

#define FFT_N   1024u         /* STFT size (+0x36218) */
#define HOP     512u          /* frames per block at the base rate (+0x3620c) */
#define NBIN    (FFT_N / 2)

/* speaker bit numbers */
enum { B_FL = 0, B_FR = 1, B_FC = 2, B_LFE = 3, B_BL = 4, B_BR = 5, B_BC = 8, B_SL = 9, B_SR = 10 };
/* the component's output buffers (the upmixer's output map: FL FR FC BL BR BC SL SR) */
enum { O_FL, O_FR, O_FC, O_BL, O_BR, O_BC, O_SL, O_SR, O_N };
static const int k_out_bit[O_N] = { B_FL, B_FR, B_FC, B_BL, B_BR, B_BC, B_SL, B_SR };

/* ---- helpers ---- */

/* ALchemy's float-to-int (x -/+ 0.5 -/+ 1.5e-8, rounded to nearest): truncation for all practical inputs */
static int rnd_int(double x)
{
    return (int)nearbyint(x < 0 ? (x + 0.5) - 1.4999999464748726e-08 : (x - 0.5) + 1.4999999464748726e-08);
}

/* half-band polyphase filter: two chains of four first-order allpasses (fns 0x1006d610/dd70/df20) */
typedef struct { float c[8], s[8], g; } hb;

static void hb_init(hb *h, const float *a, const float *b, float g)
{
    memcpy(h->c, a, 4 * sizeof(float));
    memcpy(h->c + 4, b, 4 * sizeof(float));
    memset(h->s, 0, sizeof h->s);
    h->g = g;
}

static inline float hb_chain(float *c, float *s, float x)
{
    for (int i = 0; i < 4; i++) {
        float y = c[i] * x + s[i];
        s[i] = x - c[i] * y;
        x = y;
    }
    return x;
}

/* n input frames -> n/2 low-band (to lo) and n/2 high-band (to hi) frames; lo may alias x */
static void hb_down(hb *h, float *lo, float *hi, const float *x, unsigned n)
{
    for (unsigned i = 0; i < n / 2; i++) {
        float a = hb_chain(h->c, h->s, x[2 * i]);
        float b = hb_chain(h->c + 4, h->s + 4, x[2 * i + 1]);
        lo[i] = (a + b) * h->g;
        hi[i] = (a - b) * h->g;
    }
}

/* low band + high band, n frames each -> 2n frames */
static void hb_comb(hb *h, float *dst, const float *lo, const float *hi, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        float s = hi[i] + lo[i], d = lo[i] - hi[i];
        dst[2 * i] = hb_chain(h->c, h->s, s);
        dst[2 * i + 1] = hb_chain(h->c + 4, h->s + 4, d);
    }
}

/* low band only, n frames -> 2n frames */
static void hb_up(hb *h, float *dst, const float *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        dst[2 * i] = hb_chain(h->c, h->s, x[i]);
        dst[2 * i + 1] = hb_chain(h->c + 4, h->s + 4, x[i]);
    }
}

/* allpass over a ring (fn 0x1003e330): y = c x + buf[r]; buf[w] = x - c y */
typedef struct { float *buf; float c; unsigned r, w, len; } apring;

static void ap_init(apring *a, float *buf, unsigned len, float c, float ms, float rate)
{
    memset(buf, 0, len * sizeof(float));
    a->buf = buf;
    a->len = len;
    a->c = c;
    a->r = 0;
    a->w = (unsigned)rnd_int(floor((double)(ms * rate / 1000.0f)));
    if (a->w >= len)
        a->w -= len;
}

static void ap_run(apring *a, float *out, const float *in, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        float x = in[i];
        float y = x * a->c + a->buf[a->r];
        a->buf[a->w] = x - y * a->c;
        if (++a->r >= a->len) a->r -= a->len;
        if (++a->w >= a->len) a->w -= a->len;
        out[i] = y;
    }
}

/* delay over a ring (fn 0x1003ec90) */
static void delay_run(apring *a, float *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        a->buf[a->w] = x[i];
        x[i] = a->buf[a->r];
        if (++a->r >= a->len) a->r -= a->len;
        if (++a->w >= a->len) a->w -= a->len;
    }
}

/* fourth-order IIR, direct form II (fn 0x100724c0): w = x + d1 w1 + .. + d4 w4, y = n0 w + .. + n4 w4.
 * A block of silence (zeros and denormals) clears the state and leaves zeros. */
typedef struct { float d[4], nm[5], w[4]; } iir4;

static void iir4_run(iir4 *f, float *x, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &x[i], 4);
        if (u != 0 && ((u & 0x7f800000u) || !(u & 0x7fffffu)))
            break;
    }
    if (i == n) {
        memset(f->w, 0, sizeof f->w);
        memset(x, 0, n * sizeof(float));
        return;
    }
    /* the sums run at x87 precision and are rounded to float once, as ALchemy's loop does */
    float w1 = f->w[0], w2 = f->w[1], w3 = f->w[2], w4 = f->w[3];
    for (i = 0; i < n; i++) {
        float w = (float)((double)x[i] + (double)f->d[0] * w1 + (double)f->d[1] * w2 + (double)f->d[2] * w3 +
                          (double)f->d[3] * w4);
        x[i] = (float)((double)f->nm[0] * w + (double)f->nm[1] * w1 + (double)f->nm[2] * w2 +
                       (double)f->nm[3] * w3 + (double)f->nm[4] * w4);
        w4 = w3; w3 = w2; w2 = w1; w1 = w;
    }
    f->w[0] = w1; f->w[1] = w2; f->w[2] = w3; f->w[3] = w4;
}

/* one peaking section of fn 0x100705b0: gain a, centre f, bandwidth factor q */
static void peak_section(double co[5], float a, float f, float q, double rate)
{
    if (!(1.0f < a))
        a = 1.0000001f;
    float w = (float)(6.2831854820251465 * f / rate);
    float bw = (float)(6.2831854820251465 * (float)(f * q) / rate);
    double a2 = (double)a * a;
    float s = sqrtf((float)(a2 * 0.5));
    if (!(1.0f < s))
        s = 1.0f;
    if (a2 < 2.0)
        s = sqrtf((float)((a2 + 1.0) * 0.5));
    double s2 = (double)s * s;
    float g = sqrtf((float)((s2 - 1.0) / (a2 - s2)));
    float beta = tanf((float)(bw * 0.5)) * g;
    float nrm = 1.0f / (beta + 1.0f);
    float c = cosf(w);
    co[0] = (float)((1.0 + (double)beta * a) * nrm);  /* b0 */
    co[1] = (float)(-2.0 * nrm * c);                  /* b1 = a1 */
    co[2] = (float)((1.0 - (double)beta * a) * nrm);  /* b2 */
    co[3] = co[1];                                    /* a1 */
    co[4] = (float)((1.0 - beta) * nrm);              /* a2 */
}

/* fn 0x100705b0: two peaking sections multiplied into one fourth-order filter */
static void iir4_design_peaks(iir4 *f, float a1, float f1, float q1, float a2, float f2, float q2, float rate)
{
    double p[5], q[5];
    double r = (double)rate + 1.1920928955078125e-07;
    peak_section(p, a1, f1, q1, r);
    peak_section(q, a2, f2, q2, r);
    /* numerators (b0 b1 b2) and denominators (1 a1 a2) */
    double nb[3] = { p[0], p[1], p[2] }, mb[3] = { q[0], q[1], q[2] };
    double na[3] = { 1, p[3], p[4] }, ma[3] = { 1, q[3], q[4] };
    double num[5] = { 0 }, den[5] = { 0 };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            num[i + j] += nb[i] * mb[j];
            den[i + j] += na[i] * ma[j];
        }
    for (int i = 0; i < 5; i++)
        f->nm[i] = (float)num[i];
    for (int i = 0; i < 4; i++)
        f->d[i] = (float)-den[i + 1];
    memset(f->w, 0, sizeof f->w);
}

/* ---- real FFT in ALchemy's packed layout: buf[k] = Re X[k] (k <= n/2), buf[n-k] = Im X[k], X[k] =
 * sum x[t] e^{-2 pi i k t / n}; the inverse is scaled by 1/n ---- */

typedef struct { float re, im; } cpx;

typedef struct {
    cpx tw[FFT_N / 2];        /* e^{-2 pi i k / (n/2)}, for the n/2-point complex FFT */
    cpx post[FFT_N / 2];      /* e^{-2 pi i k / n}, for the real split */
    uint16_t rev[FFT_N / 2];
} fftplan;

static void fft_plan(fftplan *p)
{
    unsigned m = FFT_N / 2, bits = 0;
    while ((1u << bits) < m) bits++;
    for (unsigned k = 0; k < m; k++) {
        p->tw[k].re = (float)cos(2 * M_PI * k / m);
        p->tw[k].im = (float)-sin(2 * M_PI * k / m);
        p->post[k].re = (float)cos(2 * M_PI * k / FFT_N);
        p->post[k].im = (float)-sin(2 * M_PI * k / FFT_N);
        unsigned r = 0;
        for (unsigned b = 0; b < bits; b++)
            if (k & (1u << b)) r |= 1u << (bits - 1 - b);
        p->rev[k] = (uint16_t)r;
    }
}

/* in-place complex FFT of m points; inv conjugates the twiddles */
static void cfft(const fftplan *p, cpx *a, int inv)
{
    unsigned m = FFT_N / 2;
    for (unsigned i = 0; i < m; i++) {
        unsigned j = p->rev[i];
        if (j > i) { cpx t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    for (unsigned len = 2; len <= m; len <<= 1) {
        unsigned step = m / len, half = len / 2;
        for (unsigned i = 0; i < m; i += len)
            for (unsigned k = 0; k < half; k++) {
                cpx w = p->tw[k * step];
                if (inv) w.im = -w.im;
                cpx u = a[i + k], v = a[i + k + half];
                cpx t = { v.re * w.re - v.im * w.im, v.re * w.im + v.im * w.re };
                a[i + k].re = u.re + t.re; a[i + k].im = u.im + t.im;
                a[i + k + half].re = u.re - t.re; a[i + k + half].im = u.im - t.im;
            }
    }
}

static void rfft_fwd(const fftplan *p, float *buf)
{
    unsigned m = FFT_N / 2;
    cpx z[FFT_N / 2];
    for (unsigned i = 0; i < m; i++) { z[i].re = buf[2 * i]; z[i].im = buf[2 * i + 1]; }
    cfft(p, z, 0);
    buf[0] = z[0].re + z[0].im;
    buf[m] = z[0].re - z[0].im;
    for (unsigned k = 1; k < m; k++) {
        cpx a = z[k], b = { z[m - k].re, -z[m - k].im };          /* conj Z[m-k] */
        cpx e = { (a.re + b.re) * 0.5f, (a.im + b.im) * 0.5f };   /* even part */
        cpx o = { (a.re - b.re) * 0.5f, (a.im - b.im) * 0.5f };   /* odd part times i */
        /* X[k] = E + w^k * (-i) O  with O' = o: X = e + w * (o.im - i o.re) */
        cpx w = p->post[k];
        cpx t = { o.im, -o.re };
        buf[k] = e.re + (w.re * t.re - w.im * t.im);
        if (k < m)
            buf[FFT_N - k] = e.im + (w.re * t.im + w.im * t.re);
    }
}

static void rfft_inv(const fftplan *p, float *buf)
{
    unsigned m = FFT_N / 2;
    cpx z[FFT_N / 2];
    for (unsigned k = 0; k < m; k++) {
        /* X[k] and conj X[m-k] from the packed spectrum */
        cpx x = { buf[k], k ? buf[FFT_N - k] : 0.0f };
        unsigned j = m - k;
        cpx y = { buf[j], (j < m) ? -buf[FFT_N - j] : 0.0f };
        cpx e = { (x.re + y.re) * 0.5f, (x.im + y.im) * 0.5f };
        cpx d = { (x.re - y.re) * 0.5f, (x.im - y.im) * 0.5f };
        cpx w = { p->post[k].re, -p->post[k].im };                 /* e^{+2 pi i k / n} */
        cpx o = { d.re * w.re - d.im * w.im, d.re * w.im + d.im * w.re };
        z[k].re = e.re - o.im;                                      /* Z = E + i O */
        z[k].im = e.im + o.re;
    }
    cfft(p, z, 1);
    float s = 2.0f / FFT_N;
    for (unsigned i = 0; i < m; i++) {
        buf[2 * i] = z[i].re * s;
        buf[2 * i + 1] = z[i].im * s;
    }
}

/* ---- the component (fns 0x1003b550 configure, 0x1003bfb0 process) ---- */

struct rl_cmss3 {
    uint32_t rate, mask, outmask;   /* output rate, component mask (+0x36220), caller's mask */
    int virt;                       /* +0x36224: rear allpasses for stereo speakers */
    float rate24, rate48;           /* +0x36200 min(rate, 24000), +0x36204 min(rate, 48000) */
    unsigned bs, n48;               /* +0x36208 frames per block at the output rate, +0x36210 at <= 48 kHz */
    int tiers;                      /* half-band splits: 0 (<= 24 kHz) .. 3 (> 96 kHz) */

    fftplan fft;

    /* intensity (+4 target, +8 current, +0x36230 setting, +0x3622c changed) */
    float it_tgt, it_cur, it_set;
    int it_flag;
    float ap_inten;                 /* the value last pushed to the side/rear stages (+0x48) */

    /* block buffers: o[] at the output rate, up to bs frames (FL/FR double as the L/R input and spectra) */
    float *o[O_N];
    float *tmp;                     /* scratch, 2 bs (+0x401c) */
    float prev[2][HOP];             /* previous half-frames of L and R (+0x2b8f8, +0x2c10c) */
    float ola[5][HOP];              /* overlap-add tails of FL FR FC BL BR (+0x2c920 ..) */

    /* half-band tiers: t = 0 for 48 -> 24 kHz, 1 for 96 -> 48, 2 for 192 -> 96 */
    hb down[3][2];
    hb up[3][O_N];
    float *hi[3][2], *hidl[3][2];   /* high bands and their one-block delays */

    /* analysis (+0x365dc ..) */
    float pl[NBIN], pr[NBIN], cr[NBIN], ci[NBIN], coh[NBIN];
    float g_fc[NBIN + 1];
    float spl[NBIN], spr[NBIN], scr[NBIN], sci[NBIN], amb[NBIN], g_amb[NBIN];
    float fc_s5, fc_s6;             /* smoothing of the centre gains (attack, release) */
    float am_a, am_p5, am_p6;       /* smoothing of the cross spectra, and of the rear gains */
    float floor_lo;                 /* rear gain below the start bin (+0x3638c) */
    int start_bin, fc_top;          /* 120 Hz and 12 kHz in bins */

    /* rear decorrelation (fn 0x1003f020) */
    float rot_c, rot_s;
    apring rdl[2];
    float sh_b0, sh_b1, sh_r, sh_z[2];
    iir4 vrt[2];

    /* sides (0x600) and rear (0x30) allpass pairs (fn 0x1003e780), back centre allpass */
    apring sap[2], rap[2], bcap;
    float smult, rmult;

    iir4 fciir;

    /* outer ring logic (fn 0x1003a740) */
    float *inr[2];                  /* bs frames being collected */
    unsigned incount;
    float *outq[O_N];               /* output FIFO: bs frames of delay plus one block */
    unsigned outlen, outpos;

    float mem[];                    /* storage for the buffers above */
};

static float curve_y(float x)
{
    /* fn 0x1003e5a0 over the table at 0x100cc1e0: (-1, 1.9) (0, 1) (0.7, 0.7) (1, 0) */
    float y;
    if (x < 0.0f)
        y = 1.0f + (x - 0.0f) * ((1.9f - 1.0f) / (-1.0f - 0.0f));
    else if (x < 0.7f)
        y = 1.0f + (x - 0.0f) * ((0.7f - 1.0f) / (0.7f - 0.0f));
    else
        y = 0.7f + (x - 0.7f) * ((0.0f - 0.7f) / (1.0f - 0.7f));
    return (float)pow(y, 1.5);
}

/* frames per block at the output rate.  ALchemy uses 512 * ceil(rate / 24000), which only matches the
 * half-band tiers its process runs (512 << tiers) up to 48 kHz and at 72..96 kHz; at 48..72 kHz and above
 * 96 kHz its blocks come out garbled.  reAL takes the size the tiers need (the same everywhere else). */
static unsigned block_frames(uint32_t rate)
{
    return HOP << (rate > 96000 ? 3 : rate > 48000 ? 2 : rate > 24000 ? 1 : 0);
}

static void configure(rl_cmss3 *c)
{
    float rate = (float)c->rate;
    c->rate24 = rate < 24000.0f ? rate : 24000.0f;
    c->rate48 = rate < 48000.0f ? rate : 48000.0f;
    c->tiers = c->rate > 96000 ? 3 : c->rate > 48000 ? 2 : c->rate > 24000 ? 1 : 0;
    c->bs = block_frames(c->rate);
    c->n48 = c->bs > 1024 ? 1024 : c->bs;

    /* fn 0x1003b4d0: rear gain floor and the side/rear levels by layout */
    c->floor_lo = 0.4125f; c->rmult = 0.9f; c->smult = 0.44f;
    if ((c->mask & 0x630) == 0x630) {
        c->floor_lo = 0.4125f; c->rmult = 0.9f; c->smult = 0.44f;
    } else if ((c->mask & 0x600) == 0x600) {
        c->floor_lo = 0.583363f; c->smult = 1.0f;
    } else if ((c->mask & 0x30) == 0x30) {
        c->floor_lo = 0.583363f; c->rmult = 1.0f;
    }

    /* fns 0x1003e1c0, 0x1003f440: smoothing coefficients, given for 24 kHz */
    if (c->rate24 == 24000.0f) {
        c->fc_s5 = 0.568332016f; c->fc_s6 = 0.900570989f;
        c->am_a = 0.625427008f; c->am_p5 = 0.673992991f; c->am_p6 = 0.929485977f;
    } else {
        double e = 24000.0 / c->rate24;
        c->fc_s5 = (float)pow(0.568332016, e); c->fc_s6 = (float)pow(0.900570989, e);
        c->am_a = (float)pow(0.625427008, e); c->am_p5 = (float)pow(0.673992991, e);
        c->am_p6 = (float)pow(0.929485977, e);
    }
    /* fns 0x1003e830, 0x1003fbd0 */
    c->start_bin = rnd_int(floor((double)((float)FFT_N * 120.0f / c->rate24)));
    {
        float top = 12000.0f;
        if (0.5f * c->rate24 < 0.0f) top = 0.5f * c->rate24;
        c->fc_top = rnd_int(floor((double)(top * (float)FFT_N / c->rate24)));
    }

    /* fn 0x1003ef50: rotation by pi/10, 15 ms delay, -10 dB shelf at 8 kHz */
    c->rot_c = cosf((float)(M_PI / 10.0));
    c->rot_s = -sinf((float)(M_PI / 10.0));
    {
        float a = (float)pow(10.0, -10.0f / 20.0);
        float h = (a + 1.0f) * 0.5f;
        float w = (float)(8000.0f * 6.2831854820251465 / c->rate48);
        double sw = sin(w);
        double alpha = sinh(log(2.0) * 0.5 * sqrt(2.0) * w / sw) * sw;
        float al = (float)alpha;
        float aa = al * a;
        float q = (float)((double)aa / sqrt(a));
        float r = (1.0f - q) / (q + 1.0f);
        c->sh_b0 = h - -(h - 1.0f) * r;
        c->sh_b1 = -(h - 1.0f) - r * h;
        c->sh_r = r;
        /* 8 kHz at or above Nyquist (a 16 kHz device): sin w ~ 0 makes the design NaN (in ALchemy too) */
        if (!(8000.0f < 0.5f * c->rate48) || !(r == r)) { c->sh_b0 = 1.0f; c->sh_b1 = 0.0f; c->sh_r = 0.0f; }
    }

    /* fn 0x100705b0 with (1.4125, 8000, 2.5, 1.4125, 2368, 2.5): the centre's presence peaks */
    iir4_design_peaks(&c->fciir, 1.41253757f, 8000.0f, 2.5f, 1.41253757f, 2368.0f, 2.5f, c->rate48);
    for (int i = 0; i < 2; i++) {
        memcpy(c->vrt[i].d, k_cm3_virt[i], 4 * sizeof(float));
        memcpy(c->vrt[i].nm, k_cm3_virt[i] + 4, 4 * sizeof(float));
        c->vrt[i].nm[4] = 1.0f;
    }
}

static void reset_state(rl_cmss3 *c)
{
    memset(c->prev, 0, sizeof c->prev);
    memset(c->ola, 0, sizeof c->ola);
    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < 2; i++) {
            hb_init(&c->down[t][i], k_cm3_hb_down, k_cm3_hb_down_b, 0.5f);
            unsigned len = HOP << t;
            memset(c->hi[t][i], 0, len * sizeof(float));
            memset(c->hidl[t][i], 0, len * sizeof(float));
        }
        for (int o = 0; o < O_N; o++)
            hb_init(&c->up[t][o], k_cm3_hb_up, k_cm3_hb_up_b, 1.0f);
    }
    memset(c->pl, 0, sizeof c->pl); memset(c->pr, 0, sizeof c->pr);
    memset(c->cr, 0, sizeof c->cr); memset(c->ci, 0, sizeof c->ci); memset(c->coh, 0, sizeof c->coh);
    memset(c->g_fc, 0, sizeof c->g_fc);
    memset(c->spl, 0, sizeof c->spl); memset(c->spr, 0, sizeof c->spr);
    memset(c->scr, 0, sizeof c->scr); memset(c->sci, 0, sizeof c->sci);
    memset(c->amb, 0, sizeof c->amb); memset(c->g_amb, 0, sizeof c->g_amb);

    c->sh_z[0] = c->sh_z[1] = 0.0f;
    for (int i = 0; i < 2; i++)
        memset(c->vrt[i].w, 0, sizeof c->vrt[i].w);
    memset(c->fciir.w, 0, sizeof c->fciir.w);
    /* ring geometry (fns 0x1003e9b0, 0x1003e6e0/e280, b550's back centre) */
    ap_init(&c->rdl[0], c->rdl[0].buf, 960, 0.0f, 15.0f, c->rate48);
    ap_init(&c->rdl[1], c->rdl[1].buf, 960, 0.0f, 15.0f, c->rate48);
    ap_init(&c->rap[0], c->rap[0].buf, 480, -0.6465f, (float)(1.8f * 2.6400001049041748), c->rate48);
    ap_init(&c->rap[1], c->rap[1].buf, 480, -0.6465f, (float)(1.4f * 2.6400001049041748), c->rate48);
    ap_init(&c->sap[0], c->sap[0].buf, 480, -0.6465f, (float)(1.6f * 2.6400001049041748), c->rate48);
    ap_init(&c->sap[1], c->sap[1].buf, 480, -0.6465f, (float)(1.2f * 2.6400001049041748), c->rate48);
    ap_init(&c->bcap, c->bcap.buf, 480, -0.6465f, 4.22400045f, c->rate48);

    /* intensity: ALchemy constructs it at 0.99 and slews from there */
    c->it_tgt = c->it_cur = 0.99f;
    c->it_flag = 1;
    c->ap_inten = 0.0f;

    c->incount = 0;
    for (int o = 0; o < O_N; o++)
        memset(c->outq[o], 0, c->outlen * sizeof(float));
    c->outpos = 0;
}

rl_cmss3 *rl_cmss3_create(uint32_t rate, uint32_t mask, int virt)
{
    if (rate < 8000 || rate > 192000)
        return NULL;
    unsigned bs = block_frames(rate);
    unsigned outlen = 2 * bs;
    size_t floats = (size_t)O_N * 2 * bs + 2 * bs              /* o[], tmp */
                  + 3 * 2 * 2 * (HOP << 2)                       /* hi, hidl (upper bound) */
                  + 2 * 960 + 4 * 480 + 480                      /* rings */
                  + 2 * bs + (size_t)O_N * outlen;               /* inr, outq */
    rl_cmss3 *c = calloc(1, sizeof *c + floats * sizeof(float));
    if (!c)
        return NULL;
    c->rate = rate;
    c->outmask = mask;
    c->virt = 0;
    if ((mask & 0x7ff) == 0x3) {             /* stereo: ALchemy's quad path (fn 0x1003a2d0) */
        c->mask = 0x33;
        c->virt = virt != 0;
    } else {
        c->mask = mask & ~0x8u;
    }
    float *m = c->mem;
    for (int o = 0; o < O_N; o++) { c->o[o] = m; m += 2 * bs; }
    c->tmp = m; m += 2 * bs;
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < 2; i++) {
            c->hi[t][i] = m; m += HOP << 2;
            c->hidl[t][i] = m; m += HOP << 2;
        }
    c->rdl[0].buf = m; m += 960;
    c->rdl[1].buf = m; m += 960;
    c->rap[0].buf = m; m += 480;
    c->rap[1].buf = m; m += 480;
    c->sap[0].buf = m; m += 480;
    c->sap[1].buf = m; m += 480;
    c->bcap.buf = m; m += 480;
    c->inr[0] = m; m += bs;
    c->inr[1] = m; m += bs;
    c->outlen = outlen;
    for (int o = 0; o < O_N; o++) { c->outq[o] = m; m += outlen; }
    fft_plan(&c->fft);
    configure(c);
    reset_state(c);
    rl_cmss3_set_intensity(c, 0);
    return c;
}

void rl_cmss3_destroy(rl_cmss3 *c) { free(c); }
void rl_cmss3_reset(rl_cmss3 *c) { if (c) reset_state(c); }
uint32_t rl_cmss3_mask(const rl_cmss3 *c) { return c->mask; }
uint32_t rl_cmss3_latency(const rl_cmss3 *c) { return c->bs + (HOP << c->tiers); }

void rl_cmss3_set_intensity(rl_cmss3 *c, int percent)
{
    float x = (float)((double)percent / 100.0);   /* fn 0x1003a2d0 */
    if (!(1.0f > x))
        x = 0.99f;
    c->it_set = x;
    c->it_flag = 1;
}

/* fn 0x1003bfb0's prologue: slew the current intensity 0.1 per block toward the target */
static void intensity_step(rl_cmss3 *c)
{
    if (c->it_tgt != c->it_cur) {
        float d = fabsf(c->it_cur - c->it_tgt), step = 0.1f;
        float cur;
        if (!(c->it_tgt < c->it_cur)) {
            if (step < d) d = 0.1f;
            cur = d + c->it_cur;
        } else if (!(step < d)) {
            cur = c->it_cur - d;
        } else {
            cur = c->it_cur - 0.1f;
        }
        if (cur < -1.0f) cur = -1.0f;
        if (0.99f < cur) cur = 0.99f;
        if (fabsf(cur - c->it_tgt) < step) cur = c->it_tgt;
        c->it_cur = cur;
        c->it_flag = 1;
    }
    if (c->it_flag) {
        c->it_flag = 0;
        c->it_tgt = c->it_set;
        c->ap_inten = c->it_cur;
    }
}

/* fn 0x1003de20: centre gains from the level similarity of each bin */
static void fc_gains(rl_cmss3 *c)
{
    c->g_fc[0] = c->g_fc[NBIN] = 0.001f;
    for (unsigned k = 1; k < NBIN; k++) {
        float d = !(c->pl[k] < c->pr[k]) ? c->coh[k] - 1.0f : 1.0f - c->coh[k];
        float x = (d + 1.0f) * 0.5f * 55.0f;
        float v = k_cm3_fcbell[rnd_int(x)];
        float s = v < c->g_fc[k] ? c->fc_s5 : c->fc_s6;
        c->g_fc[k] = s * c->g_fc[k] + (1.0f - s) * v;
    }
}

/* fns 0x1006c370 + 0x1003f0e0: ambience of each bin, and the rear gains from it */
static void amb_gains(rl_cmss3 *c)
{
    float a = c->am_a, b = 1.0f - a, thr = 0.00160708081f;
    for (unsigned k = 0; k < NBIN; k++) {
        c->spl[k] = c->spl[k] * a + c->pl[k] * b;
        c->spr[k] = c->pr[k] * b + a * c->spr[k];
        c->scr[k] = c->scr[k] * a + c->cr[k] * b;
        c->sci[k] = a * c->sci[k] + c->ci[k] * b;
        if (!(c->spl[k] < thr) && !(c->spr[k] < thr)) {
            float num = sqrtf(c->scr[k] * c->scr[k] + c->sci[k] * c->sci[k]);
            float den = sqrtf(c->spl[k] * c->spr[k]);
            c->amb[k] = 1.0f - num / den;
        } else {
            c->amb[k] = 0.0f;
        }
    }
    for (unsigned k = 0; k < NBIN; k++) {
        float fac = !(c->coh[k] < 0.5f) ? 1.0f : 0.001f;
        float v = (fac + fac) * k_cm3_ambcurve[rnd_int((double)(c->amb[k] * 201.0f))];
        c->amb[k] = v;
        float s = v < c->g_amb[k] ? c->am_p5 : c->am_p6;
        c->g_amb[k] = s * c->g_amb[k] + (1.0f - s) * v;
    }
}

/* fn 0x1006c080 (via 0x1003e910): a rear spectrum from a front one */
static void rear_spec(rl_cmss3 *c, float *out, const float *in)
{
    float f1 = 2.04173994f * 0.96f, f2 = 2.04173994f * (1.0f - 0.96f);
    for (unsigned k = (unsigned)c->start_bin; k < NBIN; k++) {
        float g = c->g_amb[k] * f1 + f2;
        out[k] = in[k] * g;
        out[FFT_N - k] = g * in[FFT_N - k];
    }
    out[0] = in[0];
    for (unsigned k = 1; k < (unsigned)c->start_bin; k++) {
        out[k] = in[k] * c->floor_lo;
        out[FFT_N - k] = in[FFT_N - k] * c->floor_lo;
    }
    out[NBIN] = in[NBIN] * 0.001f;
}

/* fns 0x1003f9a0 + 0x1003f610: move the centred part of the front pair to the centre */
static void fc_extract(rl_cmss3 *c, float *fc, float *fl, float *fr)
{
    float *w = c->tmp;
    unsigned st = (unsigned)c->start_bin, top = (unsigned)c->fc_top;
    w[0] = w[NBIN] = 0.0f;
    for (unsigned k = 1; k < st; k++)
        w[k] = ((float)(st - k) * 0.0025f) / (float)(st - 1);
    for (unsigned k = st; k < top && k < NBIN; k++)
        w[k] = 1.0f;
    for (unsigned k = top; k < NBIN; k++)
        w[k] = ((float)(NBIN - k) + (float)(k - top) * 0.0025f) / (float)(NBIN - top);

    fc[0] = (fl[0] + fr[0]) * 0.0025f;
    fc[NBIN] = (fl[NBIN] + fr[NBIN]) * 0.0025f;
    for (unsigned k = 1; k < NBIN; k++) {
        float a = w[k] * c->g_fc[k];
        float g = a * 0.707106769f, h = 1.0f - a;
        fc[k] = (fl[k] + fr[k]) * g;
        fc[FFT_N - k] = (fl[FFT_N - k] + fr[FFT_N - k]) * g;
        fl[k] *= h; fl[FFT_N - k] *= h;
        fr[k] *= h; fr[FFT_N - k] *= h;
    }
}

/* fn 0x1003dcc0: inverse transform, overlap-add; leaves HOP frames at buf[0..] */
static void synth(rl_cmss3 *c, float *buf, float *tail)
{
    rfft_inv(&c->fft, buf);
    for (unsigned i = 0; i < FFT_N; i++)
        buf[i] *= k_cm3_win[i];
    for (unsigned i = 0; i < HOP; i++)
        buf[HOP + i] += tail[i];
    memcpy(tail, buf, HOP * sizeof(float));
    memcpy(buf, buf + HOP, HOP * sizeof(float));
    for (unsigned i = 0; i < HOP; i++)
        buf[i] *= k_cm3_olanorm[i];
}

/* fn 0x1003e780: an allpass pair and the intensity-dependent level */
static void side_stage(rl_cmss3 *c, apring ap[2], float mult, float *o0, float *o1, const float *i0,
                       const float *i1, unsigned n)
{
    ap_run(&ap[0], o0, i0, n);
    ap_run(&ap[1], o1, i1, n);
    float g = curve_y(c->ap_inten) * mult;
    for (unsigned i = 0; i < n; i++) { o0[i] *= g; o1[i] *= g; }
}

/* upsample one output buffer from len to 2 len frames at tier t */
static void tier_up(rl_cmss3 *c, int t, int o, unsigned len)
{
    if (o == O_FL || o == O_FR) {
        int i = o == O_FL ? 0 : 1;
        /* vtable 0xf4: this block's high band waits a block; the previous one is used now */
        float *sw = c->hi[t][i];
        c->hi[t][i] = c->hidl[t][i];
        c->hidl[t][i] = sw;
        hb_comb(&c->up[t][o], c->tmp, c->o[o], c->hi[t][i], len);
    } else {
        hb_up(&c->up[t][o], c->tmp, c->o[o], len);
    }
    memcpy(c->o[o], c->tmp, 2 * len * sizeof(float));
}

static void block(rl_cmss3 *c)
{
    uint32_t m = c->mask;
    float *L = c->o[O_FL], *R = c->o[O_FR];

    intensity_step(c);

    /* split off the high bands, top tier first */
    for (int t = c->tiers - 1; t >= 0; t--) {
        unsigned len = HOP << (t + 1);
        hb_down(&c->down[t][0], c->tmp, c->hi[t][0], L, len);
        memcpy(L, c->tmp, len / 2 * sizeof(float));
        hb_down(&c->down[t][1], c->tmp, c->hi[t][1], R, len);
        memcpy(R, c->tmp, len / 2 * sizeof(float));
    }

    /* frames [new half | old half], windowed */
    for (int i = 0; i < 2; i++) {
        float *b = c->o[i];
        memcpy(b + HOP, c->prev[i], HOP * sizeof(float));
        memcpy(c->prev[i], b, HOP * sizeof(float));
        for (unsigned k = 0; k < FFT_N; k++)
            b[k] *= k_cm3_win[k];
        rfft_fwd(&c->fft, b);
    }

    /* fn 0x1006bf20: power, cross spectrum and level similarity */
    c->pl[0] = c->pr[0] = c->cr[0] = c->ci[0] = 0.0f;
    c->coh[0] = 1.0f;
    for (unsigned k = 1; k < NBIN; k++) {
        float lr = L[k], li = L[FFT_N - k], rr = R[k], ri = R[FFT_N - k];
        c->pl[k] = lr * lr + li * li;
        c->pr[k] = rr * rr + ri * ri;
        c->cr[k] = lr * rr - li * ri;
        c->ci[k] = li * rr + lr * ri;
        float sum = c->pl[k] + c->pr[k];
        if (9.99999997e-07f <= sum) {
            float mag = sqrtf(c->cr[k] * c->cr[k] + c->ci[k] * c->ci[k]);
            c->coh[k] = (mag + mag) / sum;
        } else {
            c->coh[k] = 1.0f;
        }
    }

    if (m & 0x4)
        fc_gains(c);
    if (m & 0x630) {
        amb_gains(c);
        rear_spec(c, c->o[O_BL], L);
        rear_spec(c, c->o[O_BR], R);
    }
    if (m & 0x4)
        fc_extract(c, c->o[O_FC], L, R);

    synth(c, L, c->ola[0]);
    synth(c, R, c->ola[1]);
    if (m & 0x4)
        synth(c, c->o[O_FC], c->ola[2]);
    if (m & 0x630) {
        synth(c, c->o[O_BL], c->ola[3]);
        synth(c, c->o[O_BR], c->ola[4]);
    }

    /* back to <= 48 kHz */
    if (c->tiers >= 1) {
        tier_up(c, 0, O_FL, HOP);
        tier_up(c, 0, O_FR, HOP);
        if (m & 0x4)
            tier_up(c, 0, O_FC, HOP);
        if (m & 0x630) {
            tier_up(c, 0, O_BL, HOP);
            tier_up(c, 0, O_BR, HOP);
        }
    }

    unsigned n = c->n48;
    if (m & 0x630) {
        float *bl = c->o[O_BL], *br = c->o[O_BR];
        /* fn 0x1003f020: rotate, delay, shelve */
        for (unsigned i = 0; i < n; i++) {
            float a = bl[i], b = br[i];
            bl[i] = a * c->rot_c + c->rot_s * b;
            br[i] = c->rot_c * b + c->rot_s * a;
        }
        delay_run(&c->rdl[0], bl, n);
        delay_run(&c->rdl[1], br, n);
        for (int ch = 0; ch < 2; ch++) {
            float *x = ch ? br : bl, z = c->sh_z[ch];
            for (unsigned i = 0; i < n; i++) {
                float w = x[i] * 1.0f - -c->sh_r * z;
                x[i] = c->sh_b0 * w + c->sh_b1 * z;
                z = w;
            }
            c->sh_z[ch] = z;
        }
        if (c->virt) {
            iir4_run(&c->vrt[0], bl, n);
            iir4_run(&c->vrt[1], br, n);
        }
        const float *s0 = NULL, *s1 = NULL;
        if (m & 0x600) {
            side_stage(c, c->sap, c->smult, c->o[O_SL], c->o[O_SR], bl, br, n);
            s0 = c->o[O_SL]; s1 = c->o[O_SR];
        }
        if (m & 0x30) {
            side_stage(c, c->rap, c->rmult, bl, br, bl, br, n);
            s0 = bl; s1 = br;
        }
        if ((m & 0x100) && s0) {
            float *bc = c->o[O_BC];
            for (unsigned i = 0; i < n; i++)
                bc[i] = (s1[i] + s0[i]) * 0.707106769f;
            ap_run(&c->bcap, bc, bc, n);
        }
    }
    if ((m & 0x4) && c->rate48 >= 44100.0f)
        iir4_run(&c->fciir, c->o[O_FC], n);

    /* the upper tiers */
    static const uint32_t up_bits[O_N] = { 0, 0, 0x4, 0x10, 0x20, 0x100, 0x200, 0x400 };
    for (int t = 1; t < c->tiers; t++) {
        unsigned len = HOP << t;
        for (int o = 0; o < O_N; o++)
            if (o <= O_FR || (m & up_bits[o]))
                tier_up(c, t, o, len);
    }
}

void rl_cmss3_process(rl_cmss3 *c, const float *l, const float *r, float *const out_[12], uint32_t n)
{
    static const float dith[8] = { -1, -1, 1, -1, 1, 1, 1, -1 };
    float *out[12];
    int src[12];
    for (int b = 0; b < 12; b++) {
        out[b] = out_[b];
        src[b] = -1;
        for (int k = 0; k < O_N; k++)
            if (k_out_bit[k] == b && (c->mask & (1u << b)))
                src[b] = k;
    }
    while (n) {
        unsigned take = c->bs - c->incount;
        if (take > n)
            take = n;
        memcpy(c->inr[0] + c->incount, l, take * sizeof(float));
        memcpy(c->inr[1] + c->incount, r, take * sizeof(float));
        c->incount += take;

        /* the FIFO starts with bs frames of silence: output frame t is block output frame t - bs */
        for (int b = 0; b < 12; b++) {
            if (!out[b])
                continue;
            if (src[b] < 0) {
                memset(out[b], 0, take * sizeof(float));
            } else {
                const float *q = c->outq[src[b]];
                unsigned p = c->outpos;
                for (unsigned i = 0; i < take; i++) {
                    out[b][i] = q[p];
                    if (++p == c->outlen) p = 0;
                }
            }
            out[b] += take;
        }
        c->outpos = (c->outpos + take) % c->outlen;

        if (c->incount == c->bs) {
            /* fn 0x1003a740: one block, dithered by +-1e-8 (vtable 0x68) */
            for (int i = 0; i < 2; i++)
                for (unsigned k = 0; k < c->bs; k++)
                    c->o[i][k] = c->inr[i][k] + dith[k & 7] * 9.99999994e-09f;
            block(c);
            /* the frames read so far came from the previous block; this one is read from here on */
            unsigned wp = c->outpos;
            for (int o = 0; o < O_N; o++) {
                int on = (c->mask >> k_out_bit[o]) & 1;
                float *q = c->outq[o];
                unsigned p = wp;
                for (unsigned i = 0; i < c->bs; i++) {
                    q[p] = on ? c->o[o][i] : 0.0f;
                    if (++p == c->outlen) p = 0;
                }
            }
            c->incount = 0;
        }
        l += take;
        r += take;
        n -= take;
    }
}
