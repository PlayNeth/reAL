/* reAL: DirectSound's standard effects, ported from Microsoft's dsdmo.dll (the Windows 8+ build: no I3DL2 reverb).
 *
 * dsdmo's effects share a base class (CPCMDMO, 0x10004c80..0x100070d0): SetInputType checks the format (100070d0:
 * PCM 8/16 or float 32, mono or stereo) and calls the effect's Init; IMediaObjectInPlace::Process brings the
 * parameter envelopes up to date and calls the effect's FBRProcess on whole frames; Discontinuity resets the effect's
 * running state.  Parameters live in an IMediaParams store as floats (CParams 0x10018590): SetAllParameters converts
 * the DSFX structure's fields and calls SetParam for each, which checks the range and works out what the effect needs.
 * The store keeps, for each parameter, a list of envelope segments, newest first (IMediaParams::AddEnvelope);
 * SetParam makes the newest one a jump to its value, and GetParam reads the newest one's end value.  Process works in
 * quanta of 3 ms, each with its time stamp, and brings the parameters with envelopes up to it (UpdateActiveParams
 * 10018ac0) before the effect runs. */
#include "dsfx.h"
#include "music/wavesrev.h"
#include "i3dl2.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <uchar.h>

typedef struct { float min, max, def; } prange;
typedef struct dsfx_node { struct dsfx_node *next; dsfx_envseg s; } dsfx_node;
/* a delay line: written backwards, positions in 1/4096 samples */
typedef struct { float *buf; int32_t pos, len; } dline;

struct dsfx {
    int kind;
    unsigned tag, ch, rate, bits, is8;  /* +0x154.. the format; +0x130 rate, +0x134 channels, +0x138 8-bit */
    int hasfmt;
    float p[8];                         /* GetParam's values: the newest segment's end value (or the neutral value) */
    uint32_t fixes;
    dsfx_node *env[8];                  /* CParams +0x48: the segments, newest first */
    uint32_t active;                    /* +0x4c: the parameters with envelopes to follow */
    uint32_t qframes, qtime;            /* +0x174 +0x178: the processing quantum (3 ms, at least 16 frames) and its time */
    union {
        struct { uint32_t shape, hz, period, phase; } gargle;   /* +0x1ec +0x1f0 +0x1f4 +0x1f8 */
        struct {
            int32_t ldelay, rdelay;     /* +0x1ec +0x1f0 delays in 1/4096 samples */
            float st1, st2;             /* +0x1f4 +0x1f8 */
            float fbh, fbc, wet;        /* +0x1fc +0x200 +0x204: feedback / 2, sqrt(1 - feedback^2), wet mix */
            int32_t pan;                /* +0x208 */
            struct { float *buf; int32_t idx, len; } l, r;     /* +0x20c +0x218 */
            int32_t pan_param;          /* PanDelay as set (DSFX_FIX_ECHO_PANDELAY) */
        } echo;
        struct {
            float k1, k2, c1, c2, k3;   /* +0x1ec +0x1f0 +0x1f4 +0x1f8 +0x1fc */
            float gain, center, bw;     /* +0x200 +0x204 +0x208 */
            float z[2][2];              /* +0x214 +0x218 left, +0x21c +0x220 right */
        } eq;
        struct {
            float depth, wet, depthfix; /* +0x1ec +0x1f0 +0x1f4: depth / 100, wet mix, the sweep in 1/4096 samples */
            float inc, fb, freq, delay; /* +0x1f8 LFO step, +0x1fc feedback / 100, +0x200 +0x204 as set */
            float base;                 /* +0x208 the delay in 1/4096 samples (+2 samples) */
            int32_t shape, phase;       /* +0x20c +0x210 */
            float *buf;                 /* +0x214 */
            int32_t pos, len;           /* +0x218 +0x21c */
            float a, b;                 /* +0x220 +0x224 the LFO (sine: sin and cos, triangle: two phases) */
            int32_t d0, t[4];           /* +0x228 the base delay as of Discontinuity, +0x22c.. the taps */
        } chorus;
        struct {
            float inc, wet, fb, depthfix;   /* +0x1ec LFO step, +0x1f0, +0x1f4 feedback / 100, +0x1f8 sweep (1/4096 samples) */
            int32_t dl0, dr0;           /* +0x1fc +0x200 the delay in 1/4096 samples (+2 samples) */
            float depth;                /* +0x204 depth / 100 */
            int32_t phase;              /* +0x208 */
            float delay, freq;          /* +0x20c +0x210 as set */
            int32_t shape;              /* +0x214 */
            int32_t lt[3], rt[3];       /* +0x220 tap + 1 sample, +0x224 tap, +0x228 its fraction; +0x22c.. right */
            float a, b;                 /* +0x238 +0x23c the LFO */
            dline l, r, dl, dr;         /* +0x240 +0x24c the lines, +0x258 +0x264 the dry signal 2 samples late */
        } flanger;
        struct {
            int32_t edge;               /* +0x1ec the waveshaper's shift */
            float total, a, b, cw, k;   /* +0x1f0 gain * compensation, +0x1f4 +0x1f8 the lowpass, +0x1fc +0x200 the EQ */
            float edgef, gain, comp;    /* +0x204 Edge as 2..31, +0x208 the gain, +0x20c the EQ's compensation */
            float center, bw;           /* +0x210 +0x214 as set (cut to a third of the rate) */
            float lp[2];                /* +0x218 +0x21c */
            float s[2][2];              /* +0x220 +0x224 left, +0x228 +0x22c right */
        } dist;
        struct {
            float attack, release;      /* +0x1ec +0x1f0 the envelope's coefficients */
            float thresh, ratio;        /* +0x1f4 the threshold (log2 / 32), +0x1f8 1 - 1 / ratio */
            float gint, gfrac;          /* +0x1fc +0x200 the makeup gain's integer and fractional parts */
            float peak, mingain;        /* +0x204 +0x208 meters */
            int32_t pd[2];              /* +0x20c +0x210 the predelay in samples (+2) */
            float env, gain;            /* +0x214 +0x218 */
            dline l, r;                 /* +0x21c +0x228 */
        } comp;
        struct { wavesrev *w; float cur[4]; } waves;   /* +0x200 the reverb's state, made by Init; +0x1ec.. the values */
        i3dl2 *i3;                      /* XP's I3DL2 reverb (i3dl2.c) */
    } u;
};

/* the parameter tables (0x1001b000 ...): in IMediaParams index order */
static const prange s_ranges[DSFX_NKINDS][7] = {
    [DSFX_GARGLE] = { { 1, 1000, 20 }, { 0, 1, 0 } },
    [DSFX_ECHO] = { { 0, 100, 50 }, { 0, 100, 50 }, { 1, 2000, 500 }, { 1, 2000, 500 }, { 0, 1, 0 } },
    [DSFX_PARAMEQ] = { { 80, 16000, 8000 }, { 1, 36, 12 }, { -15, 15, 0 } },
    [DSFX_CHORUS] = { { 0, 100, 50 }, { 0, 100, 10 }, { 0, 10, 1.1f }, { 0, 1, 1 }, { 0, 4, 3 }, { -99, 99, 25 },
                      { 0, 20, 16 } },
    [DSFX_FLANGER] = { { 0, 100, 50 }, { 0, 1, 1 }, { 0, 10, 0.25f }, { 0, 100, 100 }, { 0, 4, 2 }, { -99, 99, -50 },
                       { 0, 4, 2 } },
    [DSFX_DISTORTION] = { { -60, 0, -18 }, { 0, 100, 15 }, { 100, 8000, 8000 }, { 100, 8000, 2400 }, { 100, 8000, 2400 } },
    [DSFX_COMPRESSOR] = { { -60, 60, 0 }, { 0.01f, 500, 10 }, { 50, 3000, 200 }, { -60, 0, -20 }, { 1, 100, 3 }, { 0, 4, 4 } },
    [DSFX_WAVES] = { { -96, 0, 0 }, { -96, 0, 0 }, { 0.001f, 3000, 1000 }, { 0.001f, 0.999f, 0.001f } },
};
/* the parameters' descriptions (dsdmo 0x1001b000 ...: MP_PARAMINFO and the enumerations), in index order */
typedef struct { uint32_t type; float min, max, neutral; const char16_t *unit, *label, *enums; } pdesc;
static const pdesc s_pdesc[DSFX_NKINDS][7] = {
    [DSFX_CHORUS] = {
        { 1, 0.0f, 100.0f, 50.0f, u"%", u"WetDryMix", u"" },
        { 1, 0.0f, 100.0f, 10.0f, u"", u"Depth", u"" },
        { 1, 0.0f, 10.0f, 1.100000023841858f, u"Hz", u"Frequency", u"" },
        { 3, 0.0f, 1.0f, 1.0f, u"", u"WaveShape", u"Triangle,Sine" },
        { 0, 0.0f, 4.0f, 3.0f, u"", u"Phase", u"" },
        { 1, -99.0f, 99.0f, 25.0f, u"", u"Feedback", u"" },
        { 1, 0.0f, 20.0f, 16.0f, u"", u"Delay", u"" },
    },
    [DSFX_COMPRESSOR] = {
        { 1, -60.0f, 60.0f, 0.0f, u"", u"Gain", u"" },
        { 1, 0.009999999776482582f, 500.0f, 10.0f, u"", u"Attack", u"" },
        { 1, 50.0f, 3000.0f, 200.0f, u"", u"Release", u"" },
        { 1, -60.0f, 0.0f, -20.0f, u"", u"Threshold", u"" },
        { 1, 1.0f, 100.0f, 3.0f, u"", u"Ratio", u"" },
        { 1, 0.0f, 4.0f, 4.0f, u"", u"Predelay", u"" },
    },
    [DSFX_DISTORTION] = {
        { 1, -60.0f, 0.0f, -18.0f, u"", u"Gain", u"" },
        { 1, 0.0f, 100.0f, 15.0f, u"", u"Edge", u"" },
        { 1, 100.0f, 8000.0f, 8000.0f, u"", u"PreLowpassCutoff", u"" },
        { 1, 100.0f, 8000.0f, 2400.0f, u"", u"PostEQCenterFrequency", u"" },
        { 1, 100.0f, 8000.0f, 2400.0f, u"", u"PostEQBandwidth", u"" },
    },
    [DSFX_PARAMEQ] = {
        { 1, 80.0f, 16000.0f, 8000.0f, u"", u"Center", u"" },
        { 1, 1.0f, 36.0f, 12.0f, u"", u"Bandwidth", u"" },
        { 1, -15.0f, 15.0f, 0.0f, u"", u"Gain", u"" },
    },
    [DSFX_ECHO] = {
        { 1, 0.0f, 100.0f, 50.0f, u"", u"WetDryMix", u"" },
        { 1, 0.0f, 100.0f, 50.0f, u"", u"Feedback", u"" },
        { 1, 1.0f, 2000.0f, 500.0f, u"", u"LeftDelay", u"" },
        { 1, 1.0f, 2000.0f, 500.0f, u"", u"RightDelay", u"" },
        { 2, 0.0f, 1.0f, 0.0f, u"", u"PanDelay", u"" },
    },
    [DSFX_FLANGER] = {
        { 1, 0.0f, 100.0f, 50.0f, u"%", u"WetDryMix", u"" },
        { 3, 0.0f, 1.0f, 1.0f, u"", u"WaveShape", u"Triangle,Sine" },
        { 1, 0.0f, 10.0f, 0.25f, u"Hz", u"Frequency", u"" },
        { 1, 0.0f, 100.0f, 100.0f, u"", u"Depth", u"" },
        { 0, 0.0f, 4.0f, 2.0f, u"", u"Phase", u"" },
        { 1, -99.0f, 99.0f, -50.0f, u"", u"Feedback", u"" },
        { 1, 0.0f, 4.0f, 2.0f, u"", u"Delay", u"" },
    },
    [DSFX_GARGLE] = {
        { 0, 1.0f, 1000.0f, 20.0f, u"Hz", u"Rate", u"" },
        { 3, 0.0f, 1.0f, 0.0f, u"", u"WaveShape", u"Triangle,Square" },
    },
    [DSFX_WAVES] = {
        { 1, -96.0f, 0.0f, 0.0f, u"dB", u"InGain", u"" },
        { 1, -96.0f, 0.0f, 0.0f, u"dB", u"ReverbMix", u"" },
        { 1, 0.0010000000474974513f, 3000.0f, 1000.0f, u"ms", u"ReverbTime", u"" },
        { 1, 0.0010000000474974513f, 0.9990000128746033f, 0.0010000000474974513f, u"", u"HighFreqRTRatio", u"" },
    },
};
static const int s_nparams[DSFX_NKINDS] = { [DSFX_GARGLE] = 2, [DSFX_ECHO] = 5, [DSFX_PARAMEQ] = 3, [DSFX_CHORUS] = 7,
                                            [DSFX_FLANGER] = 7, [DSFX_DISTORTION] = 5,
                                            [DSFX_COMPRESSOR] = 6, [DSFX_WAVES] = 4,
                                            [DSFX_I3DL2] = 13 };

int dsfx_nparams(int kind) { return kind >= 0 && kind < DSFX_NKINDS ? s_nparams[kind] : 0; }
int dsfx_kind(const dsfx *f) { return f->kind; }

/* 1001a090 (_ftol2_sse): float to integer, truncating */
static int32_t ftol(float v) { return (int32_t)v; }
/* (float)(double)(uint32_t), as the compiler's unsigned conversion */
static float u2f(uint32_t v) { return (float)(double)v; }

/* ------------------------------------------------------------------ Gargle (10015480 ...) */
/* 10015c20 SetParam */
static int gargle_param(dsfx *f, int i, float v)
{
    if (i == 0) {
        if (v < 1.0f || v > 1000.0f) return DSFX_E_INVALIDARG;
        uint32_t r = (uint32_t)ftol(v);
        if (r < 1) r = 1;
        if (r > 1000) r = 1000;
        f->u.gargle.hz = r;
        f->u.gargle.period = f->rate / r;
    } else if (i == 1) {
        if (v < 0.0f || v > 1.0f) return DSFX_E_INVALIDARG;
        f->u.gargle.shape = (uint32_t)ftol(v);
    }
    return DSFX_OK;
}
static void gargle_init(dsfx *f) { f->u.gargle.period = f->rate / f->u.gargle.hz; }     /* 100158a0 */
static void gargle_discont(dsfx *f) { f->u.gargle.phase = 0; }                         /* 100158f0 */
/* 10015910 FBRProcess */
static void gargle_process(dsfx *f, void *data, uint32_t n)
{
    uint32_t period = f->u.gargle.period, ch = f->ch;
    for (uint32_t i = 0; i < n; i++) {
        if (++f->u.gargle.phase > period) f->u.gargle.phase = 0;
        uint32_t ph = f->u.gargle.phase, half = period >> 1, m;
        if (f->u.gargle.shape == 0) m = ph > half ? period - ph : ph;
        else m = ph <= half ? half : 0;
        for (uint32_t c = 0; c < ch; c++) {
            uint32_t k = i * ch + c;
            if (f->tag == 1) {
                if (f->is8) {
                    int32_t v = ((int32_t)((uint8_t *)data)[k] - 128) * (int32_t)m * 2 / (int32_t)period;
                    if (v > 127) v = 127; else if (v < -128) v = -128;
                    ((uint8_t *)data)[k] = (uint8_t)(v + 128);
                } else {
                    int32_t v = ((int16_t *)data)[k] * (int32_t)m * 2 / (int32_t)period;
                    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                    ((int16_t *)data)[k] = (int16_t)v;
                }
            } else {
                float g = u2f(m);
                g = (g + g) / u2f(period);
                ((float *)data)[k] *= g;
            }
        }
    }
}

/* ------------------------------------------------------------------ Echo (100101c0 ...) */
/* 10011430 SetParam: needs a sample rate (the constructor's 22050 until a format is set) */
static int echo_param(dsfx *f, int i, float v)
{
    if (!f->rate) return DSFX_E_TYPE_NOT_ACCEPTED;
    switch (i) {
    case 0: if (v < 0.0f || v > 100.0f) return DSFX_E_INVALIDARG; f->u.echo.wet = v / 100.0f; break;
    case 1: {
        if (v < 0.0f || v > 100.0f) return DSFX_E_INVALIDARG;
        float x = v / 100.0f;
        f->u.echo.fbh = x * 0.5f;
        f->u.echo.fbc = (float)sqrt(1.0 - (double)(x * x));
        break;
    }
    case 2: case 3: {
        if (v < 1.0f || v > 2000.0f) return DSFX_E_INVALIDARG;
        float d = v * 4096.0f / 1000.0f * u2f(f->rate);
        if (i == 2) f->u.echo.ldelay = (int32_t)d; else f->u.echo.rdelay = (int32_t)d;
        break;
    }
    case 4: if (v < 0.0f || v > 1.0f) return DSFX_E_INVALIDARG; f->u.echo.pan = f->u.echo.pan_param = (int32_t)v; break;
    }
    return DSFX_OK;
}
/* 100104a0 Discontinuity (and the end of Init 10010320): the lines cleared, and PanDelay with them (the original
 * keeps the parameter, so GetAllParameters still has it, but echoes unpanned until it is set again) */
static void echo_discont(dsfx *f)
{
    f->u.echo.pan = (f->fixes & DSFX_FIX_ECHO_PANDELAY) ? f->u.echo.pan_param : 0;
    f->u.echo.st1 = f->u.echo.st2 = 0;
    if (f->u.echo.l.buf) memset(f->u.echo.l.buf, 0, f->u.echo.l.len * sizeof(float));
    if (f->u.echo.r.buf) memset(f->u.echo.r.buf, 0, f->u.echo.r.len * sizeof(float));
}
/* 10011a80: a line of 2 s and 16 samples */
static int echo_line(dsfx *f, int which)
{
    int32_t len = (int32_t)(2000u * f->rate / 1000u) + 16;
    float *b = calloc(len, sizeof(float));
    if (!b) return DSFX_E_OUTOFMEMORY;
    if (which == 0) { free(f->u.echo.l.buf); f->u.echo.l.buf = b; f->u.echo.l.len = len; f->u.echo.l.idx = 0; }
    else { free(f->u.echo.r.buf); f->u.echo.r.buf = b; f->u.echo.r.len = len; f->u.echo.r.idx = 0; }
    return DSFX_OK;
}
/* 10010320 Init: the parameters again for the new rate, new lines */
static int echo_init(dsfx *f)
{
    for (int i = 0; i < 4; i++) echo_param(f, i, f->p[i]);
    echo_param(f, 4, (float)(int32_t)f->p[4]);
    int hr = echo_line(f, 0);
    if (hr < 0 || (hr = echo_line(f, 1)) < 0) return hr;
    echo_discont(f);
    return DSFX_OK;
}
/* a line position: idx + delay, 20.12 fixed point, wrapped */
static int32_t echo_at(int32_t idx, int32_t delay, int32_t len)
{
    int32_t e = delay + idx;
    if (e < 0) { int32_t w = (int32_t)((uint32_t)len << 12); do e += w; while (e < 0); }
    e = (int32_t)((uint32_t)e >> 12);
    while (e >= len) e -= len;
    return e;
}
/* 10010420: both lines one sample on */
static void echo_step(dsfx *f)
{
    int32_t e = f->u.echo.l.idx;
    if (!e) e = (int32_t)((uint32_t)f->u.echo.l.len << 12);
    if (e < 0) e += (int32_t)((uint32_t)f->u.echo.l.len << 12);
    f->u.echo.l.idx = e - 0x1000;
    e = f->u.echo.r.idx;
    if (!e) e = (int32_t)((uint32_t)f->u.echo.r.len << 12);
    if (e < 0) f->u.echo.r.idx = (int32_t)((uint32_t)f->u.echo.r.len << 12) - 0x1000 + e;
    else f->u.echo.r.idx = e - 0x1000;
}
static float clamp_trunc16(float v)
{
    int32_t i = (int32_t)v;          /* cvttss2si */
    return (float)(i > 32767 ? 32767 : i < -32768 ? -32768 : i);
}
/* 10010510 FBRProcess and its five siblings (8/16-bit and float, mono and stereo): two lines, crossed by PanDelay; a
 * mono input goes through both and the two results are averaged */
static void echo_process(dsfx *f, void *data, uint32_t n)
{
    float fbh = f->u.echo.fbh, fbc = f->u.echo.fbc, wet = f->u.echo.wet;
    for (uint32_t i = 0; i < n; i++) {
        float xl, xr;
        if (f->tag != 1) { xl = ((float *)data)[i * f->ch]; xr = f->ch == 2 ? ((float *)data)[i * 2 + 1] : xl; }
        else if (f->is8) { xl = (float)((((uint8_t *)data)[i * f->ch] - 128) << 8); xr = f->ch == 2 ? (float)((((uint8_t *)data)[i * 2 + 1] - 128) << 8) : xl; }
        else { xl = (float)((int16_t *)data)[i * f->ch]; xr = f->ch == 2 ? (float)((int16_t *)data)[i * 2 + 1] : xl; }
        float a, b;
        if (f->u.echo.pan) a = f->u.echo.r.buf[echo_at(f->u.echo.r.idx, f->u.echo.rdelay, f->u.echo.r.len)];
        else a = f->u.echo.l.buf[echo_at(f->u.echo.l.idx, f->u.echo.ldelay, f->u.echo.l.len)];
        float t = a * fbh + f->u.echo.st1;
        f->u.echo.l.buf[echo_at(f->u.echo.l.idx, 0, f->u.echo.l.len)] = xl * fbc + t;
        f->u.echo.st1 = fbh * a;
        float ol = (a - xl) * wet + xl;
        if (f->u.echo.pan) b = f->u.echo.l.buf[echo_at(f->u.echo.l.idx, f->u.echo.ldelay, f->u.echo.l.len)];
        else b = f->u.echo.r.buf[echo_at(f->u.echo.r.idx, f->u.echo.rdelay, f->u.echo.r.len)];
        float t2 = fbh * b + f->u.echo.st2;
        f->u.echo.r.buf[echo_at(f->u.echo.r.idx, 0, f->u.echo.r.len)] = xr * fbc + t2;
        f->u.echo.st2 = b * fbh;
        float orr = (b - xr) * wet + xr;
        echo_step(f);
        if (f->tag != 1) {
            if (f->ch == 2) { ((float *)data)[i * 2] = ol; ((float *)data)[i * 2 + 1] = orr; }
            else ((float *)data)[i] = (ol + orr) * 0.5f;
        } else {
            int32_t l = (int32_t)clamp_trunc16(ol), r = (int32_t)clamp_trunc16(orr);
            if (f->is8) {
                if (f->ch == 2) { ((uint8_t *)data)[i * 2] = (uint8_t)(l / 256 + 128); ((uint8_t *)data)[i * 2 + 1] = (uint8_t)(r / 256 + 128); }
                else ((uint8_t *)data)[i] = (uint8_t)((l + r) / 512 + 128);
            } else {
                if (f->ch == 2) { ((int16_t *)data)[i * 2] = (int16_t)l; ((int16_t *)data)[i * 2 + 1] = (int16_t)r; }
                else ((int16_t *)data)[i] = (int16_t)((l + r) / 2);
            }
        }
    }
}

/* ------------------------------------------------------------------ ParamEq (1000e990 ...): an allpass-based peaking filter */
/* 1000f400: the coefficients */
static void eq_coefs(dsfx *f)
{
    float a = (float)pow(10.0, (double)(f->u.eq.gain / 20.0f));
    if (a == 0.0f) a = 1e-5f;
    float p1 = (a + 1.0f) * 0.5f, m1 = (1.0f - a) * 0.5f;
    float mx = fabsf(m1) > fabsf(p1) ? fabsf(m1) : fabsf(p1), norm = 1.0f;
    if (mx > 1.0f) norm = (float)ceil((double)mx);
    double w = (double)(float)((double)f->u.eq.center * 6.283185307 / (double)f->rate);
    f->u.eq.k1 = m1 / norm;
    f->u.eq.k2 = p1 / norm;
    f->u.eq.k3 = norm * 4.0f;
    double sw = (double)(float)sin(w);
    double t = log(2.0) * 0.5 * (double)f->u.eq.bw / 12.0 * w / sw;
    float v = (float)(sinh(t) * sw);
    float q = v / (float)sqrt((double)a);
    f->u.eq.c1 = (1.0f - q) / (q + 1.0f);
    f->u.eq.c2 = -(float)cos(w);
}
/* 1000f630 SetParam: a center above a third of the rate is cut down to it (S_FALSE) */
static int eq_param(dsfx *f, int i, float *v)
{
    int r = DSFX_OK;
    switch (i) {
    case 0: {
        if (*v < 80.0f || *v > 16000.0f) return DSFX_E_INVALIDARG;
        float third = u2f(f->rate / 3);
        if (*v > third) { *v = third; r = DSFX_FALSE; }
        f->u.eq.center = *v;
        break;
    }
    case 1: if (*v < 1.0f || *v > 36.0f) return DSFX_E_INVALIDARG; f->u.eq.bw = *v; break;
    case 2: if (*v < -15.0f || *v > 15.0f) return DSFX_E_INVALIDARG; f->u.eq.gain = *v; break;
    }
    eq_coefs(f);
    return r;
}
static void eq_discont(dsfx *f) { memset(f->u.eq.z, 0, sizeof(f->u.eq.z)); }     /* 1000ec60 */
/* 1000eca0 FBRProcess and siblings: input scaled by 1/4, output by the normalisation */
static float eq_one(dsfx *f, float x, float *z)
{
    float xs = x * 0.25f;
    float a = f->u.eq.c1 * xs + z[1];
    float b = xs - f->u.eq.c1 * a;
    float c = f->u.eq.c2 * b + z[0];
    z[1] = c;
    z[0] = b - f->u.eq.c2 * c;
    return (a * f->u.eq.k1 + xs * f->u.eq.k2) * f->u.eq.k3;
}
static void eq_process(dsfx *f, void *data, uint32_t n)
{
    uint32_t total = n * f->ch;
    for (uint32_t k = 0; k < total; k++) {
        float *z = f->u.eq.z[f->ch == 2 ? (k & 1) : 0];
        if (f->tag != 1) ((float *)data)[k] = eq_one(f, ((float *)data)[k], z);
        else if (f->is8) {
            int32_t y = (int32_t)clamp_trunc16(eq_one(f, (float)((((uint8_t *)data)[k] - 128) << 8), z));
            ((uint8_t *)data)[k] = (uint8_t)(y / 256 + 128);
        } else ((int16_t *)data)[k] = (int16_t)clamp_trunc16(eq_one(f, (float)((int16_t *)data)[k], z));
    }
}

/* ------------------------------------------------------------------ frames in and out, as the effects' FBRProcess
 * siblings do it: 8-bit samples as (s - 128) << 8, integer results truncated and clamped to 16 bits; a mono result
 * made of two (Echo's lines, Chorus' and Flanger's taps) is their average */
static void frame_in(const dsfx *f, const void *data, uint32_t i, float *l, float *r)
{
    if (f->tag != 1) { *l = ((const float *)data)[i * f->ch]; *r = f->ch == 2 ? ((const float *)data)[i * 2 + 1] : *l; }
    else if (f->is8) {
        const uint8_t *p = (const uint8_t *)data + i * f->ch;
        *l = (float)((p[0] - 128) << 8); *r = f->ch == 2 ? (float)((p[1] - 128) << 8) : *l;
    } else { const int16_t *p = (const int16_t *)data + i * f->ch; *l = (float)p[0]; *r = f->ch == 2 ? (float)p[1] : *l; }
}
static void frame_out(const dsfx *f, void *data, uint32_t i, float l, float r)
{
    if (f->tag != 1) {
        if (f->ch == 2) { ((float *)data)[i * 2] = l; ((float *)data)[i * 2 + 1] = r; }
        else ((float *)data)[i] = (r + l) * 0.5f;
        return;
    }
    int32_t a = (int32_t)clamp_trunc16(l), b = (int32_t)clamp_trunc16(r);
    if (f->is8) {
        if (f->ch == 2) { ((uint8_t *)data)[i * 2] = (uint8_t)(a / 256 + 128); ((uint8_t *)data)[i * 2 + 1] = (uint8_t)(b / 256 + 128); }
        else ((uint8_t *)data)[i] = (uint8_t)((a + b) / 512 + 128);
    } else {
        if (f->ch == 2) { ((int16_t *)data)[i * 2] = (int16_t)a; ((int16_t *)data)[i * 2 + 1] = (int16_t)b; }
        else ((int16_t *)data)[i] = (int16_t)((a + b) / 2);
    }
}
/* a line position one sample on (the lines run backwards: 20.12 fixed point, 0 wraps to the end) */
static int32_t line_step(int32_t pos, int32_t len)
{
    int32_t e = pos;
    if (!e) e = (int32_t)((uint32_t)len << 12);
    if (e < 0) e = (int32_t)((uint32_t)e + ((uint32_t)len << 12));
    return (int32_t)((uint32_t)e - 0x1000);
}

/* ------------------------------------------------------------------ Chorus (10007960 ...): one line, two taps swept by an
 * LFO (the second's phase set by Phase), wet/dry mixed with the input */
static const int32_t s_chorus_lsign[5] = { 1, 1, 1, 1, -1 };      /* 10002700 */
static const int32_t s_chorus_rsel[5] = { 0, 1, 0, 1, 0 };        /* 100026d8: which LFO output the right tap uses */
static const int32_t s_chorus_rsign[5] = { -1, -1, 1, 1, 1 };     /* 100026ec */
/* 10009450 SetParam */
static void chorus_inc(dsfx *f)
{
    double rate = (double)f->rate;
    if (f->u.chorus.shape == 0) { float x = f->u.chorus.freq / (float)rate; f->u.chorus.inc = x + x; }
    else f->u.chorus.inc = (float)(sin((double)f->u.chorus.freq * 3.1415926535 / rate) * 2.0);
}
static int chorus_param(dsfx *f, int i, float v)
{
    if (!f->rate) return DSFX_E_TYPE_NOT_ACCEPTED;
    if (v < s_ranges[DSFX_CHORUS][i].min || v > s_ranges[DSFX_CHORUS][i].max) return DSFX_E_INVALIDARG;
    float frate = (float)(double)f->rate;
    switch (i) {
    case 0: f->u.chorus.wet = v / 100.0f; break;
    case 1: {
        f->u.chorus.depth = v / 100.0f;
        double d = (double)(frate * f->u.chorus.delay / 1000.0f);
        f->u.chorus.depthfix = (float)(d * (double)f->u.chorus.depth * 0.5) * 4096.0f;
        break;
    }
    case 6: {
        f->u.chorus.delay = v;
        double d = (double)(frate * v / 1000.0f);
        f->u.chorus.depthfix = (float)((double)f->u.chorus.depth * d * 0.5 * 4096.0);
        f->u.chorus.base = (float)((d + 2.0) * 4096.0);
        break;
    }
    case 2: f->u.chorus.freq = v; chorus_inc(f); break;
    case 3: {
        int32_t s = (int32_t)v;
        if (s != f->u.chorus.shape) { f->u.chorus.a = 0.0f; f->u.chorus.b = s ? 1.0f : 0.5f; }
        f->u.chorus.shape = s;
        chorus_inc(f);
        break;
    }
    case 4: f->u.chorus.phase = (int32_t)v; break;
    case 5: f->u.chorus.fb = v / 100.0f; break;
    }
    return DSFX_OK;
}
/* 10007bb0 Discontinuity */
static void chorus_discont(dsfx *f)
{
    f->u.chorus.a = 0.0f;
    f->u.chorus.b = f->u.chorus.shape ? 1.0f : 0.5f;
    if (f->u.chorus.buf) memset(f->u.chorus.buf, 0, (size_t)f->u.chorus.len * sizeof(float));
    int32_t d = (int32_t)f->u.chorus.base;
    f->u.chorus.d0 = f->u.chorus.t[0] = f->u.chorus.t[1] = f->u.chorus.t[2] = f->u.chorus.t[3] = d;
}
/* 10007ab0 Init: the parameters again for the rate (10009780, stopping at the first failure), a line of 3.84 s */
static int chorus_init(dsfx *f)
{
    int hr;
    float v[7];
    memcpy(v, f->p, sizeof(v));
    v[3] = (float)(int32_t)v[3]; v[4] = (float)(int32_t)v[4];
    for (int i = 0; i < 7; i++) if ((hr = chorus_param(f, i, v[i])) < 0) return hr;
    int32_t len = (int32_t)(3840u * f->rate / 1000u) + 3;      /* 10009a90 */
    float *b = calloc((size_t)len, sizeof(float));
    if (!b) return DSFX_E_OUTOFMEMORY;
    free(f->u.chorus.buf);
    f->u.chorus.buf = b; f->u.chorus.len = len; f->u.chorus.pos = 0;
    chorus_discont(f);
    return DSFX_OK;
}
/* a tap of the line, linear between the samples at t and t2 (t + 1 sample, but as of Discontinuity, t) */
static float chorus_tap(const dsfx *f, int32_t t, int32_t t2)
{
    const float *buf = f->u.chorus.buf;
    int32_t pos = f->u.chorus.pos, len = f->u.chorus.len;
    float s1 = buf[echo_at(pos, t, len)], s2 = buf[echo_at(pos, t2, len)];
    float frac = (float)(t & 0xfff) * (1.0f / 4096.0f);
    return frac * (s2 - s1) + s1;
}
/* 10007c40 FBRProcess (8/16-bit and float, mono and stereo alike) */
static void chorus_process(dsfx *f, void *data, uint32_t n)
{
    int32_t ph = f->u.chorus.phase;
    for (uint32_t i = 0; i < n; i++) {
        float xl, xr;
        frame_in(f, data, i, &xl, &xr);
        float *buf = f->u.chorus.buf;
        int32_t len = f->u.chorus.len, pos = f->u.chorus.pos;
        float in = f->ch == 2 ? xr + xl : xl + xl;
        float fbk = buf[echo_at(pos, f->u.chorus.d0, len)] * f->u.chorus.fb;
        buf[echo_at(pos, 0, len)] = fbk + in * 0.5f;
        float lfo[2], inc = f->u.chorus.inc;
        if (f->u.chorus.shape == 0) {
            float a = f->u.chorus.a + inc;
            if (a > 1.0f) a -= 2.0f;
            f->u.chorus.a = a;
            float b = inc + f->u.chorus.b;
            if (b > 1.0f) b -= 2.0f;
            f->u.chorus.b = b;
            lfo[0] = fabsf(a) + fabsf(a) - 1.0f;
            lfo[1] = fabsf(b) + fabsf(b) - 1.0f;
        } else {
            float a = f->u.chorus.a + f->u.chorus.b * inc;
            float b = f->u.chorus.b - a * inc;
            f->u.chorus.a = a; f->u.chorus.b = b;
            lfo[0] = a; lfo[1] = b;
        }
        int32_t *t = f->u.chorus.t, d0 = f->u.chorus.d0;
        float yl = chorus_tap(f, t[0], t[1]);
        t[0] = d0 + (int32_t)(lfo[0] * f->u.chorus.depthfix) * s_chorus_lsign[ph];
        t[1] = t[0] + 0x1000;
        float ol = (yl - xl) * f->u.chorus.wet + xl;
        float yr = chorus_tap(f, t[2], t[3]);
        t[2] = d0 + (int32_t)(lfo[s_chorus_rsel[ph]] * f->u.chorus.depthfix) * s_chorus_rsign[ph];
        t[3] = t[2] + 0x1000;
        float orr = (yr - xr) * f->u.chorus.wet + xr;
        f->u.chorus.pos = line_step(pos, len);
        frame_out(f, data, i, ol, orr);
    }
}

/* ------------------------------------------------------------------ Flanger (10012440 ...): Chorus' sweep on a line
 * per channel, mixed with the dry signal two samples late (the shortest delay) */
static float lfo_inc(unsigned rate, int32_t shape, float freq)    /* as Chorus' */
{
    if (shape == 0) { float x = freq / (float)(double)rate; return x + x; }
    return (float)(sin((double)freq * 3.1415926535 / (double)rate) * 2.0);
}
/* 100147a0 SetParam */
static int flanger_param(dsfx *f, int i, float v)
{
    if (!f->rate) return DSFX_E_TYPE_NOT_ACCEPTED;
    if (v < s_ranges[DSFX_FLANGER][i].min || v > s_ranges[DSFX_FLANGER][i].max) return DSFX_E_INVALIDARG;
    float frate = (float)(double)f->rate;
    switch (i) {
    case 0: f->u.flanger.wet = v / 100.0f; break;
    case 1: {
        int32_t s = (int32_t)v;
        if (s != f->u.flanger.shape) { f->u.flanger.a = 0.0f; f->u.flanger.b = s ? 1.0f : 0.5f; }
        f->u.flanger.shape = s;
        f->u.flanger.inc = lfo_inc(f->rate, s, f->u.flanger.freq);
        break;
    }
    case 2: f->u.flanger.freq = v; f->u.flanger.inc = lfo_inc(f->rate, f->u.flanger.shape, v); break;
    case 3: {
        f->u.flanger.depth = v / 100.0f;
        double d = (double)(frate * f->u.flanger.delay / 1000.0f);
        f->u.flanger.depthfix = (float)(d * (double)f->u.flanger.depth * 0.5 * 4096.0);
        break;
    }
    case 4: f->u.flanger.phase = (int32_t)v; break;
    case 5: f->u.flanger.fb = v / 100.0f; break;
    case 6: {
        f->u.flanger.delay = v;
        double d = (double)(frate * v / 1000.0f);
        f->u.flanger.depthfix = (float)((double)f->u.flanger.depth * d * 0.5 * 4096.0);
        f->u.flanger.dl0 = f->u.flanger.dr0 = (int32_t)((d + 2.0) * 4096.0);
        break;
    }
    }
    return DSFX_OK;
}
static void dline_clear(dline *d) { if (d->buf) memset(d->buf, 0, (size_t)d->len * sizeof(float)); }
/* 10012850 Discontinuity */
static void flanger_discont(dsfx *f)
{
    dline_clear(&f->u.flanger.l); dline_clear(&f->u.flanger.r); dline_clear(&f->u.flanger.dl); dline_clear(&f->u.flanger.dr);
    for (int k = 0; k < 3; k++) { f->u.flanger.lt[k] = f->u.flanger.dl0; f->u.flanger.rt[k] = f->u.flanger.dr0; }
    f->u.flanger.a = 0.0f;
    f->u.flanger.b = f->u.flanger.shape ? 1.0f : 0.5f;
}
/* 10014ee0 / 10014df0 */
static int dline_alloc(dline *d, uint32_t ms, unsigned rate, int32_t extra)
{
    int32_t len = (int32_t)(ms * rate / 1000u) + extra;
    float *b = calloc((size_t)len, sizeof(float));
    if (!b) return DSFX_E_OUTOFMEMORY;
    free(d->buf);
    d->buf = b; d->len = len; d->pos = 0;
    return DSFX_OK;
}
/* 100125d0 Init: the parameters again (10014ae0), lines of 768 ms and of 3 samples */
static int flanger_init(dsfx *f)
{
    int hr;
    float v[7];
    memcpy(v, f->p, sizeof(v));
    v[1] = (float)(int32_t)v[1]; v[4] = (float)(int32_t)v[4];
    for (int i = 0; i < 7; i++) if ((hr = flanger_param(f, i, v[i])) < 0) return hr;
    if ((hr = dline_alloc(&f->u.flanger.l, 768, f->rate, 3)) < 0 || (hr = dline_alloc(&f->u.flanger.r, 768, f->rate, 3)) < 0 ||
        (hr = dline_alloc(&f->u.flanger.dl, 0, f->rate, 3)) < 0 || (hr = dline_alloc(&f->u.flanger.dr, 0, f->rate, 3)) < 0)
        return hr;
    flanger_discont(f);
    return DSFX_OK;
}
/* one channel's input: the dry sample kept, the line fed back */
static void flanger_chan(const dsfx *f, dline *ln, dline *dry, int32_t d0, float x)
{
    dry->buf[echo_at(dry->pos, 0, dry->len)] = x;
    float fbk = ln->buf[echo_at(ln->pos, d0, ln->len)] * f->u.flanger.fb;
    ln->buf[echo_at(ln->pos, 0, ln->len)] = fbk + x;
}
/* one channel's output: the swept tap, mixed with the dry sample 2 samples late */
static float flanger_tap(const dsfx *f, const dline *ln, const dline *dry, const int32_t *t)
{
    float s1 = ln->buf[echo_at(ln->pos, t[1], ln->len)], s2 = ln->buf[echo_at(ln->pos, t[0], ln->len)];
    float y = (s2 - s1) * ((float)(t[2] & 0xfff) * (1.0f / 4096.0f)) + s1;
    float d = dry->buf[echo_at(dry->pos, 0x2000, dry->len)];
    return (y - d) * f->u.flanger.wet + d;
}
/* 10012940 FBRProcess (8/16-bit and float, mono and stereo alike) */
static void flanger_process(dsfx *f, void *data, uint32_t n)
{
    int32_t ph = f->u.flanger.phase;
    for (uint32_t i = 0; i < n; i++) {
        float xl, xr;
        frame_in(f, data, i, &xl, &xr);
        flanger_chan(f, &f->u.flanger.l, &f->u.flanger.dl, f->u.flanger.dl0, xl);
        flanger_chan(f, &f->u.flanger.r, &f->u.flanger.dr, f->u.flanger.dr0, xr);
        float lfo[2], inc = f->u.flanger.inc;
        if (f->u.flanger.shape == 0) {
            float a = inc + f->u.flanger.a;
            if (a > 1.0f) a -= 2.0f;
            f->u.flanger.a = a;
            float b = inc + f->u.flanger.b;
            if (b > 1.0f) b -= 2.0f;
            f->u.flanger.b = b;
            lfo[0] = fabsf(a) + fabsf(a) - 1.0f;
            lfo[1] = fabsf(b) + fabsf(b) - 1.0f;
        } else {
            float a = inc * f->u.flanger.b + f->u.flanger.a;
            float b = f->u.flanger.b - inc * a;
            f->u.flanger.a = a; f->u.flanger.b = b;
            lfo[0] = a; lfo[1] = b;
        }
        float ol = flanger_tap(f, &f->u.flanger.l, &f->u.flanger.dl, f->u.flanger.lt);
        float orr = flanger_tap(f, &f->u.flanger.r, &f->u.flanger.dr, f->u.flanger.rt);
        int32_t t = f->u.flanger.dl0 + (int32_t)(f->u.flanger.depthfix * lfo[0]) * s_chorus_lsign[ph];
        f->u.flanger.lt[2] = f->u.flanger.lt[1] = t; f->u.flanger.lt[0] = t + 0x1000;
        t = f->u.flanger.dr0 + (int32_t)(f->u.flanger.depthfix * lfo[s_chorus_rsel[ph]]) * s_chorus_rsign[ph];
        f->u.flanger.rt[2] = f->u.flanger.rt[1] = t; f->u.flanger.rt[0] = t + 0x1000;
        dline *ls[4] = { &f->u.flanger.l, &f->u.flanger.r, &f->u.flanger.dl, &f->u.flanger.dr };     /* 10012780 */
        for (int k = 0; k < 4; k++) ls[k]->pos = line_step(ls[k]->pos, ls[k]->len);
        frame_out(f, data, i, ol, orr);
    }
}

/* ------------------------------------------------------------------ Distortion (1000cd90 ...): a one-pole lowpass, a
 * waveshaper working on the bits of the sample (100183c0), and an allpass-based post-EQ */
static const double s_dist_comp[32] = {     /* 100027c8: the EQ's gain compensation, by Edge (2..31) */
    1.0, 1.0, 1.5, 1.0, 1.75, 1.4, 1.17, 1.0, 1.88, 1.76, 1.5, 1.36, 1.25, 1.15, 1.07, 1.0,
    1.94, 1.82, 1.72, 1.63, 1.55, 1.48, 1.41, 1.35, 1.29, 1.24, 1.19, 1.15, 1.11, 1.07, 1.03, 1.0 };
/* 100183c0: the sample as a little floating point number: shifted up by up to edge bits, the shift count in its top
 * bits, the rest shifted down to fit (the sign kept apart) */
static float dist_shape(float v, int32_t edge)
{
    uint32_t i = (uint32_t)(int32_t)v;         /* cvttss2si */
    uint32_t sign = i & 0x80000000u;
    if (sign) i = 0u - i;
    uint32_t e = (uint32_t)edge;
    while (i < 0x80000000u && e) { i += i; e--; }
    uint32_t m = i, top = e;
    if (i >= 0x80000000u) { m = i & 0x7fffffffu; top = e + 1; }
    uint32_t sh = (uint32_t)edge > 15 ? 5 : (uint32_t)edge > 7 ? 4 : (uint32_t)edge > 3 ? 3 : 2;
    uint32_t r = (top << (31 - sh)) | (m >> sh);
    if (sign) r = ~r;
    r |= sign;
    return (float)(int32_t)r;
}
/* 1000d8d0 SetParam: center frequencies above a third of the rate are cut to it (S_FALSE); internal calls (from
 * Edge's) leave the parameter store alone */
static int dist_param(dsfx *f, int i, float *pv)
{
    float v = *pv;
    if (v < s_ranges[DSFX_DISTORTION][i].min || v > s_ranges[DSFX_DISTORTION][i].max) return DSFX_E_INVALIDARG;
    int r = DSFX_OK;
    double rate = (double)f->rate;
    if (i >= 2) {
        float third = u2f(f->rate / 3);
        if (v > third) { v = third; r = DSFX_FALSE; }
    }
    switch (i) {
    case 0:
        f->u.dist.gain = (float)pow(10.0, (double)(v / 20.0f));
        f->u.dist.total = f->u.dist.gain * f->u.dist.comp;
        break;
    case 1: {
        f->u.dist.edgef = v / 100.0f * 29.0f + 2.0f;
        f->u.dist.edge = (int32_t)(int64_t)f->u.dist.edgef;
        float c = f->u.dist.center, b = f->u.dist.bw;
        dist_param(f, 3, &c);
        dist_param(f, 4, &b);
        break;
    }
    case 2: {
        double c = (cos((double)v * 6.283185307 / rate) * 2.0 + 3.0) / 5.0;
        float a = (float)sqrt(c);
        f->u.dist.a = a;
        f->u.dist.b = (float)sqrt((double)(1.0f - a * a));
        break;
    }
    case 3: case 4: {
        double cw, k;
        if (i == 3) {
            f->u.dist.center = v;
            cw = -cos((double)v * 6.283185307 / rate);
            double t = (double)f->u.dist.bw * 6.283185307 / rate * 0.5;
            k = (1.0 - tan(t)) / (tan(t) + 1.0);
        } else {
            f->u.dist.bw = v;
            cw = (double)(float)-cos((double)f->u.dist.center * 6.283185307 / rate);
            double t = (double)v * 6.283185307 / rate * 0.5;
            k = (double)(float)((1.0 - tan(t)) / (tan(t) + 1.0));
        }
        double s1 = sqrt(1.0 - k * k), s2 = sqrt(1.0 - cw * cw);
        f->u.dist.comp = (float)((double)(float)(s1 * s2) * s_dist_comp[(int32_t)f->u.dist.edgef]);
        if (i == 3) f->u.dist.cw = (float)cw; else f->u.dist.k = (float)k;
        f->u.dist.total = f->u.dist.comp * f->u.dist.gain;
        break;
    }
    }
    *pv = v;
    return r;
}
static void dist_discont(dsfx *f) { memset(f->u.dist.lp, 0, sizeof(f->u.dist.lp)); memset(f->u.dist.s, 0, sizeof(f->u.dist.s)); }
/* 1000cf80 Init: the parameters again (1000dea0: all tried, the first not S_OK) */
static int dist_init(dsfx *f)
{
    float v[5];
    memcpy(v, f->p, sizeof(v));
    for (int i = 0; i < 5; i++) { int hr = dist_param(f, i, &v[i]); if (hr < 0) return hr; f->p[i] = v[i]; }
    dist_discont(f);
    return DSFX_OK;
}
static float dist_one(dsfx *f, float x, int c)
{
    float lp = x * f->u.dist.b + f->u.dist.lp[c] * f->u.dist.a;
    f->u.dist.lp[c] = lp;
    float r = dist_shape(lp * 32768.0f, f->u.dist.edge);
    float *st = f->u.dist.s[c], cw = f->u.dist.cw;
    float y = r * (1.0f / 32768.0f) * f->u.dist.total - f->u.dist.k * st[0] - cw * st[1];
    st[0] = cw * y + st[1];
    st[1] = y;
    return y;
}
/* fistp under the default control word: to nearest; out of range: the integer indefinite */
static int32_t fistp(float v)
{
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return INT32_MIN;
    return (int32_t)lrintf(v);
}
/* 1000d080 FBRProcess and siblings: mono integer output rounds (x87), stereo truncates */
static void dist_process(dsfx *f, void *data, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        float xl, xr;
        frame_in(f, data, i, &xl, &xr);
        float yl = dist_one(f, xl, 0);
        if (f->ch == 2) { frame_out(f, data, i, yl, dist_one(f, xr, 1)); continue; }
        if (f->tag != 1) { ((float *)data)[i] = yl; continue; }
        int32_t y = fistp(yl);
        if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
        if (f->is8) ((uint8_t *)data)[i] = (uint8_t)(y / 256 + 128); else ((int16_t *)data)[i] = (int16_t)y;
    }
}

/* ------------------------------------------------------------------ Compressor (1000a800 ...): an envelope follower and
 * gain computer in a log2 domain (fixed point tricks on the sample's bits), the gain applied to the input delayed by
 * Predelay - in 1/4096 samples: the original adds samples to a 20.12 position, so it is next to no delay at all */
/* 1000be00 SetParam */
static int comp_param(dsfx *f, int i, float v)
{
    if (!f->rate) return DSFX_E_TYPE_NOT_ACCEPTED;
    if (v < s_ranges[DSFX_COMPRESSOR][i].min || v > s_ranges[DSFX_COMPRESSOR][i].max) return DSFX_E_INVALIDARG;
    float frate = (float)(double)f->rate;
    switch (i) {
    case 0: {
        double ip, fp = modf((double)(float)pow(10.0, (double)v / 20.0), &ip);
        f->u.comp.gint = (float)ip; f->u.comp.gfrac = (float)fp;
        break;
    }
    case 1: case 2: {
        float t = frate * v / 1000.0f;
        float c = (float)pow(10.0, (double)(-1.0f / t));
        if (i == 1) f->u.comp.attack = c; else f->u.comp.release = c;
        break;
    }
    case 3: {
        double t = (double)(float)pow(10.0, (double)(v / 20.0f));
        double l = log(pow(2.0, 31.0) * t);
        l = l * pow(2.0, 26.0) / log(2.0) + pow(2.0, 26.0);
        float a = (float)l, b = (float)(pow(2.0, 31.0) - 1.0);
        f->u.comp.thresh = (a < b ? a : b) * (1.0f / 2147483648.0f);
        break;
    }
    case 4: f->u.comp.ratio = (float)(1.0 - 1.0 / (double)v); break;
    case 5: f->u.comp.pd[0] = f->u.comp.pd[1] = (int32_t)((float)((double)(frate * v) / 1000.0) + 2.0f); break;
    }
    return DSFX_OK;
}
/* 1000ab60 Discontinuity */
static void comp_discont(dsfx *f)
{
    dline_clear(&f->u.comp.l);
    if (f->ch == 2) dline_clear(&f->u.comp.r);
    f->u.comp.env = f->u.comp.gain = 0.0f;
}
/* 1000a960 Init: the parameters again, 200 ms lines (the right one for stereo only) */
static int comp_init(dsfx *f)
{
    int hr;
    float v[6];
    memcpy(v, f->p, sizeof(v));
    for (int i = 0; i < 6; i++) if ((hr = comp_param(f, i, v[i])) < 0) return hr;
    if ((hr = dline_alloc(&f->u.comp.l, 200, f->rate, 0)) < 0) return hr;          /* 1000c480 */
    if (f->ch == 2 && (hr = dline_alloc(&f->u.comp.r, 200, f->rate, 0)) < 0) return hr;
    comp_discont(f);
    return DSFX_OK;
}
/* the level in the log2 domain: the shift count in the top bits, the mantissa below, over 2^31 */
static float comp_log(float lv)
{
    uint32_t i = (uint32_t)(int32_t)lv;        /* cvttss2si */
    uint32_t sign = i & 0x80000000u;
    if (sign) i = 0u - i;
    uint32_t e = 31;
    while (i < 0x80000000u && e) { i += i; e--; }
    uint32_t m = i, top = e;
    if (i >= 0x80000000u) { m = i & 0x7fffffffu; top = e + 1; }
    uint32_t code = (top << 26) | (m >> 5);
    if (sign) code = ~code;
    code |= sign;
    return fabsf((float)(int32_t)code) * (1.0f / 2147483648.0f);
}
static float comp_exp(int32_t gi)
{
    uint32_t a = (uint32_t)(gi < 0 ? -gi : gi);
    int32_t ex = (int32_t)a >> 26;
    uint32_t mant = a << 5, val = mant;
    if (ex) { ex--; val = mant | 0x80000000u; }
    val >>= (31 - ex) & 31;
    if (gi < 0) val = ~val;
    return (float)(double)val * (1.0f / 2147483648.0f);
}
/* the line read at the predelay: samples added to a 20.12 position (and the line's length with them) */
static float comp_tap(const dline *d, int32_t pd)
{
    int32_t e = (int32_t)((uint32_t)pd - 1u + (uint32_t)d->pos + (uint32_t)d->len);
    if (e < 0) { do e = (int32_t)((uint32_t)e + ((uint32_t)d->len << 12)); while (e < 0); }
    e /= 4096;
    while (e >= d->len) e -= d->len;
    return d->buf[e];
}
/* 1000abd0 FBRProcess and siblings: a stereo pair shares the envelope (the mean of the two levels) */
static void comp_process(dsfx *f, void *data, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        float xl, xr;
        frame_in(f, data, i, &xl, &xr);
        dline *l = &f->u.comp.l, *r = &f->u.comp.r;
        l->buf[echo_at(l->pos, 0, l->len)] = xl;
        float lv;
        if (f->ch == 2) { r->buf[echo_at(r->pos, 0, r->len)] = xr; lv = (fabsf(xr) + fabsf(xl)) * 0.5f * 32768.0f; }
        else lv = fabsf(xl) * 32768.0f;
        float lg = comp_log(lv), env = f->u.comp.env;
        f->u.comp.peak = lg > f->u.comp.peak ? lg : f->u.comp.peak;
        float c = f->u.comp.attack;
        if (!(lg >= env)) c = -c;
        if (0.0f >= c) c = f->u.comp.release;
        env = (env - lg) * c + lg;
        f->u.comp.env = env;
        float th = f->u.comp.thresh, e2 = env > th ? env : th;
        float g = (th - e2) * f->u.comp.ratio + 0.99999988f;   /* 10002970: 0x3f7ffffe */
        f->u.comp.mingain = g < f->u.comp.mingain ? g : f->u.comp.mingain;
        float gain = comp_exp((int32_t)(g * 2147483648.0f));
        f->u.comp.gain = gain;
        float yl = gain * comp_tap(l, f->u.comp.pd[0]), yr = 0.0f;
        float ol = f->u.comp.gint * yl + f->u.comp.gfrac * yl, orr;
        if (f->ch == 2) { yr = gain * comp_tap(r, f->u.comp.pd[1]); orr = f->u.comp.gint * yr + f->u.comp.gfrac * yr; }
        else orr = ol;
        l->pos = line_step(l->pos, l->len);
        if (f->ch == 2) r->pos = line_step(r->pos, r->len);
        if (f->ch == 2 || f->tag != 1) frame_out(f, data, i, ol, orr);
        else {
            int32_t y = (int32_t)clamp_trunc16(ol);
            if (f->is8) ((uint8_t *)data)[i] = (uint8_t)(y / 256 + 128); else ((int16_t *)data)[i] = (int16_t)y;
        }
    }
}

/* ------------------------------------------------------------------ Waves reverb (10016770 ...): the network is in
 * music/wavesrev.c; 16-bit and float only */
static void record(dsfx *f, int i, float v);
/* Init and Discontinuity: GetAllParameters, then SetAllParameters with them (so the newest segments become jumps) */
static void waves_params_again(dsfx *f)
{
    for (int i = 0; i < 4; i++) { f->u.waves.cur[i] = f->p[i]; record(f, i, f->p[i]); }
    float *q = f->u.waves.cur;
    wavesrev_set(f->u.waves.w, q[0], q[1], q[2], q[3]);
}
/* 10016c90 SetParam: the coefficients made again once Init has run */
static int waves_param(dsfx *f, int i, float v)
{
    if (v < s_ranges[DSFX_WAVES][i].min || v > s_ranges[DSFX_WAVES][i].max) return DSFX_E_INVALIDARG;
    float *q = f->u.waves.cur;
    q[i] = v;
    if (f->u.waves.w) wavesrev_set(f->u.waves.w, q[0], q[1], q[2], q[3]);
    return DSFX_OK;
}
/* 100168f0 Init / 10016ad0 Discontinuity: the memory cleared, the delays laid out for the rate, the parameters set */
static int waves_init(dsfx *f)
{
    if (!f->u.waves.w) {
        if (!(f->u.waves.w = wavesrev_new(f->rate))) return DSFX_E_OUTOFMEMORY;
    } else {
        wavesrev_free(f->u.waves.w);
        if (!(f->u.waves.w = wavesrev_new(f->rate))) return DSFX_E_OUTOFMEMORY;
    }
    waves_params_again(f);
    return DSFX_OK;
}
static void waves_discont(dsfx *f)
{
    if (!f->u.waves.w) return;
    wavesrev_reset(f->u.waves.w);
    waves_params_again(f);
}
static void waves_process(dsfx *f, void *data, uint32_t n)
{
    if (!f->u.waves.w) return;
    if (f->tag == 1) {
        if (f->ch == 2) wavesrev_stereo16(f->u.waves.w, data, n); else wavesrev_mono16(f->u.waves.w, data, n);
    } else {
        if (f->ch == 2) wavesrev_stereo_float(f->u.waves.w, data, n); else wavesrev_mono_float(f->u.waves.w, data, n);
    }
}

/* ------------------------------------------------------------------ the common part */
dsfx *dsfx_new(int kind)
{
    if (kind < 0 || kind >= DSFX_NKINDS || !s_nparams[kind]) return NULL;
    dsfx *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->kind = kind;
    if (kind == DSFX_I3DL2) {           /* its parameters are the translated original's */
        if (!(f->u.i3 = i3dl2_new(0))) { free(f); return NULL; }
        return f;
    }
    switch (kind) {
    case DSFX_GARGLE: f->u.gargle.hz = 20; f->u.gargle.shape = 0; break;
    case DSFX_ECHO: f->rate = 22050; break;                         /* 100102e3 */
    case DSFX_PARAMEQ: f->rate = 48000; break;                      /* 1000eab3 */
    case DSFX_CHORUS: f->rate = 22050; f->u.chorus.len = 3; break; /* 10007a83 */
    case DSFX_FLANGER:                                              /* 10012563 */
        f->rate = 22050;
        f->u.flanger.l.len = f->u.flanger.r.len = f->u.flanger.dl.len = f->u.flanger.dr.len = 3;
        break;
    case DSFX_DISTORTION: f->rate = 44010; break;                   /* 1000ceb3 */
    case DSFX_COMPRESSOR: f->rate = 22050; break;                   /* 1000a923 */
    }
    for (int i = 0; i < s_nparams[kind]; i++) { f->p[i] = s_ranges[kind][i].def; dsfx_set_param(f, i, f->p[i]); }
    return f;
}
void dsfx_free(dsfx *f)
{
    if (!f) return;
    if (f->kind == DSFX_ECHO) { free(f->u.echo.l.buf); free(f->u.echo.r.buf); }
    if (f->kind == DSFX_CHORUS) free(f->u.chorus.buf);
    if (f->kind == DSFX_COMPRESSOR) { free(f->u.comp.l.buf); free(f->u.comp.r.buf); }
    if (f->kind == DSFX_WAVES) wavesrev_free(f->u.waves.w);
    if (f->kind == DSFX_I3DL2) i3dl2_free(f->u.i3);
    for (int i = 0; i < 8; i++) while (f->env[i]) { dsfx_node *n = f->env[i]; f->env[i] = n->next; free(n); }
    if (f->kind == DSFX_FLANGER) { free(f->u.flanger.l.buf); free(f->u.flanger.r.buf); free(f->u.flanger.dl.buf); free(f->u.flanger.dr.buf); }
    free(f);
}
void dsfx_set_fixes(dsfx *f, uint32_t fixes)
{
    if (f->kind == DSFX_I3DL2 && (fixes & DSFX_FIX_I3DL2_QUALITY) && !(f->fixes & DSFX_FIX_I3DL2_QUALITY)) {
        uint32_t fx = I3DL2_FIX_QUALITY;    /* made again with the fix: only before anything is set */
        i3dl2 *n = i3dl2_new(fx);
        if (n) { i3dl2_free(f->u.i3); f->u.i3 = n; }
    }
    f->fixes = fixes;
}
int dsfx_i3dl2_set_preset(dsfx *f, uint32_t p) { return f->kind == DSFX_I3DL2 ? i3dl2_set_preset(f->u.i3, p) : DSFX_E_INVALIDARG; }
int dsfx_i3dl2_get_preset(dsfx *f, uint32_t *p) { return f->kind == DSFX_I3DL2 ? i3dl2_get_preset(f->u.i3, p) : DSFX_E_INVALIDARG; }
int dsfx_i3dl2_set_quality(dsfx *f, int32_t q) { return f->kind == DSFX_I3DL2 ? i3dl2_set_quality(f->u.i3, q) : DSFX_E_INVALIDARG; }
void dsfx_i3dl2_frame(dsfx *f, uint32_t fn, void *out) { if (f->kind == DSFX_I3DL2) i3dl2_frame(f->u.i3, fn, out); }
void dsfx_i3dl2_object(dsfx *f, void *out) { if (f->kind == DSFX_I3DL2) i3dl2_object(f->u.i3, out); }
int dsfx_i3dl2_get_quality(dsfx *f, int32_t *q) { return f->kind == DSFX_I3DL2 ? i3dl2_get_quality(f->u.i3, q) : DSFX_E_INVALIDARG; }

static int set_format_init(dsfx *f);
int dsfx_set_format(dsfx *f, unsigned tag, unsigned ch, unsigned rate, unsigned bits)
{
    if (tag == 1) { if (bits != 8 && bits != 16) return DSFX_E_TYPE_NOT_ACCEPTED; }
    else if (tag != 3 || bits != 32) return DSFX_E_TYPE_NOT_ACCEPTED;
    if (ch != 1 && ch != 2) return DSFX_E_TYPE_NOT_ACCEPTED;
    if (f->kind == DSFX_WAVES && bits == 8) return DSFX_E_TYPE_NOT_ACCEPTED;      /* 16-bit and float only */
    if (f->kind == DSFX_I3DL2) {
        int hr = i3dl2_set_format(f->u.i3, tag, ch, rate, bits);
        if (hr >= 0) { f->tag = tag; f->ch = ch; f->rate = rate; f->bits = bits; f->is8 = bits == 8; f->hasfmt = 1; }
        return hr;
    }
    f->tag = tag; f->ch = ch; f->rate = rate; f->bits = bits; f->is8 = bits == 8; f->hasfmt = 1;
    f->qframes = rate * 3 / 1000;       /* 100070d0 */
    if (f->qframes < 16) f->qframes = 16;
    f->qtime = (uint32_t)((uint64_t)f->qframes * 10000000 / rate);
    int hr = set_format_init(f);
    /* Init sets the parameters again through SetParam, Gargle's excepted (Waves' does it itself) */
    if (hr >= 0 && f->kind != DSFX_GARGLE && f->kind != DSFX_WAVES)
        for (int i = 0; i < s_nparams[f->kind]; i++) record(f, i, f->p[i]);
    return hr;
}
static int set_format_init(dsfx *f)
{
    switch (f->kind) {
    case DSFX_GARGLE: gargle_init(f); break;
    case DSFX_ECHO: return echo_init(f);
    case DSFX_CHORUS: return chorus_init(f);
    case DSFX_FLANGER: return flanger_init(f);
    case DSFX_DISTORTION: return dist_init(f);
    case DSFX_COMPRESSOR: return comp_init(f);
    case DSFX_WAVES: return waves_init(f);
    case DSFX_PARAMEQ: {                /* 1000eb40: the parameters again for the new rate */
        for (int i = 0; i < 3; i++) { float v = f->p[i]; eq_param(f, i, &v); f->p[i] = v; }
        eq_discont(f);
        break;
    }
    }
    return DSFX_OK;
}

/* the effect's SetParam: what the effect needs worked out (*v: the value as kept, for the ones that adjust it) */
static int apply(dsfx *f, int i, float *v)
{
    switch (f->kind) {
    case DSFX_GARGLE: return gargle_param(f, i, *v);
    case DSFX_ECHO: return echo_param(f, i, *v);
    case DSFX_PARAMEQ: return eq_param(f, i, v);
    case DSFX_CHORUS: return chorus_param(f, i, *v);
    case DSFX_FLANGER: return flanger_param(f, i, *v);
    case DSFX_DISTORTION: return dist_param(f, i, v);
    case DSFX_COMPRESSOR: return comp_param(f, i, *v);
    case DSFX_WAVES: return waves_param(f, i, *v);
    }
    return DSFX_E_INVALIDARG;
}
/* 10018e30: SetParam's value kept - the newest segment made a jump to it (a segment over all time when there is none) */
static void record(dsfx *f, int i, float v)
{
    dsfx_node *n = f->env[i];
    if (!n) {
        if (!(n = calloc(1, sizeof(*n)))) return;
        n->s.start = INT64_MIN; n->s.end = INT64_MAX;
        f->env[i] = n;
    }
    n->s.v0 = n->s.v1 = v;
    n->s.curve = DSFX_CURVE_JUMP;
    f->p[i] = v;
}
int dsfx_set_param(dsfx *f, int i, float v)
{
    if (i < 0 || i >= s_nparams[f->kind]) return DSFX_E_INVALIDARG;
    if (f->kind == DSFX_I3DL2) return i3dl2_set_param(f->u.i3, i, v);
    int hr = apply(f, i, &v);
    if (hr >= 0) record(f, i, v);
    return hr;
}
float dsfx_get_param(const dsfx *f, int i)
{
    if (f->kind == DSFX_I3DL2) return i3dl2_get_param(f->u.i3, i);
    return i >= 0 && i < s_nparams[f->kind] ? f->p[i] : 0.0f;
}

/* 10018ef0 AddEnvelope: the segments pushed in order (the last one newest), their values clamped to the range */
int dsfx_add_envelope(dsfx *f, int i, uint32_t n, const dsfx_envseg *segs)
{
    if (!segs) return DSFX_E_POINTER;
    if (f->kind == DSFX_I3DL2 || i < 0 || i >= s_nparams[f->kind]) return DSFX_E_INVALIDARG;
    const prange *r = &s_ranges[f->kind][i];
    for (uint32_t k = 0; k < n; k++) {
        dsfx_node *e = malloc(sizeof(*e));
        if (!e) return DSFX_E_OUTOFMEMORY;
        e->s = segs[k];
        e->s.v1 = r->min <= e->s.v1 && e->s.v1 <= r->max ? e->s.v1 : r->min <= e->s.v1 ? r->max : r->min;
        e->s.v0 = r->min <= e->s.v0 && e->s.v0 <= r->max ? e->s.v0 : r->min <= e->s.v0 ? r->max : r->min;
        e->next = f->env[i];
        f->env[i] = e;
        f->active |= 1u << i;
        f->p[i] = e->s.v1;
    }
    return DSFX_OK;
}
/* 10018990 (IMediaObjectInPlace::Clone): the segments copied, in order; nothing followed yet */
int dsfx_copy_envelopes(dsfx *dst, const dsfx *src)
{
    if (dst->kind != src->kind || dst->kind == DSFX_I3DL2) return DSFX_E_INVALIDARG;
    for (int i = 0; i < s_nparams[dst->kind]; i++) {
        while (dst->env[i]) { dsfx_node *n = dst->env[i]; dst->env[i] = n->next; free(n); }
        dsfx_node **pp = &dst->env[i];
        for (const dsfx_node *n = src->env[i]; n; n = n->next) {
            if (!(*pp = malloc(sizeof(**pp)))) return DSFX_E_OUTOFMEMORY;
            (*pp)->s = n->s; (*pp)->next = NULL;
            pp = &(*pp)->next;
        }
        dst->p[i] = src->p[i];
    }
    return DSFX_OK;
}
/* 10019030 FlushEnvelope: the segments wholly inside [start, end] dropped */
int dsfx_flush_envelope(dsfx *f, int i, int64_t start, int64_t end)
{
    if (f->kind == DSFX_I3DL2 || i < 0 || i >= s_nparams[f->kind] || start >= end) return DSFX_E_INVALIDARG;
    dsfx_node **pp = &f->env[i];
    while (*pp) {
        dsfx_node *e = *pp;
        if (e->s.start >= start && e->s.end <= end) { *pp = e->next; free(e); }
        else pp = &e->next;
    }
    f->p[i] = f->env[i] ? f->env[i]->s.v1 : s_ranges[f->kind][i].def;
    return DSFX_OK;
}
/* 10018ac0 UpdateActiveParams: each parameter with an envelope set to its value at t (the effect's SetParam, nothing
 * kept); a parameter whose newest segment is over is followed no more */
static void update_params(dsfx *f, int64_t t)
{
    for (int i = 0; i < s_nparams[f->kind] && f->active; i++) {
        if (!(f->active & (1u << i))) continue;
        float def = s_ranges[f->kind][i].def, v = def;
        dsfx_node *head = f->env[i], *e;
        if (!head) { apply(f, i, &v); f->active &= ~(1u << i); continue; }
        for (e = head; e && e->s.start > t; e = e->next) ;
        if (!e) v = def;
        else if (e->s.end < t) {
            v = e->s.v1;
            if (e == head) f->active &= ~(1u << i);
        } else if (e->s.curve & DSFX_CURVE_JUMP) v = e->s.v1;
        else {
            float x = (float)(double)(t - e->s.start) / (float)(double)(e->s.end - e->s.start);
            switch (e->s.curve) {
            case DSFX_CURVE_SQUARE: x = x * x; break;
            case DSFX_CURVE_INVSQUARE: x = (float)sqrt((double)x); break;
            case DSFX_CURVE_SINE: x = (float)(sin((double)x * 3.1415926535 - 1.57079632675) + 1.0) * 0.5f; break;
            }
            float v0 = e->s.v0;
            if (e->s.flags & DSFX_ENVF_NEUTRAL) v0 = def;
            if (e->s.flags & DSFX_ENVF_CURRENT)
                for (dsfx_node *o = e->next; o; o = o->next) if (o->s.end <= t) { v0 = o->s.v1; break; }
            v = (e->s.v1 - v0) * x + v0;
        }
        apply(f, i, &v);
    }
}
int dsfx_params_size(int kind)
{
    static const int sz[DSFX_NKINDS] = { 8, 28, 28, 20, 20, 24, 12, 16, 48 };
    return kind >= 0 && kind < DSFX_NKINDS ? sz[kind] : 0;
}

/* SetAllParameters: the structure's fields to IMediaParams indices */
int dsfx_set_all(dsfx *f, const void *params)
{
    const uint32_t *w = params;
    int hr = DSFX_OK;
    switch (f->kind) {
    case DSFX_I3DL2: return i3dl2_set_all(f->u.i3, params);
    case DSFX_GARGLE:                   /* 10015ae0 */
        if ((hr = dsfx_set_param(f, 0, u2f(w[0]))) < 0) return hr;
        return dsfx_set_param(f, 1, u2f(w[1]));
    case DSFX_ECHO:                     /* 10011640 */
        for (int i = 0; i < 4; i++) { float v; memcpy(&v, &w[i], 4); if ((hr = dsfx_set_param(f, i, v)) < 0) return hr; }
        return dsfx_set_param(f, 4, (float)(int32_t)w[4]);
    case DSFX_COMPRESSOR: case DSFX_WAVES:     /* 1000c1c0 / 10016da0: in order until one fails */
        for (int i = 0; i < dsfx_nparams(f->kind); i++) { float v; memcpy(&v, &w[i], 4); if ((hr = dsfx_set_param(f, i, v)) < 0) return hr; }
        return hr;
    case DSFX_PARAMEQ: case DSFX_DISTORTION: {  /* 1000f770 / 1000dea0: in order until one fails; the first not S_OK */
        static const int fld[2][5] = { { 0, 1, 2 }, { 0, 1, 4, 2, 3 } };
        int d = f->kind == DSFX_DISTORTION, np = d ? 5 : 3, r[5] = { 0 };
        for (int i = 0; i < np; i++) { float v; memcpy(&v, &w[fld[d][i]], 4); if ((r[i] = dsfx_set_param(f, i, v)) < 0) return r[i]; }
        for (int i = 0; i < np; i++) if (r[i]) return r[i];
        return DSFX_OK;
    }
    case DSFX_CHORUS: case DSFX_FLANGER: {  /* 10009780 / 10014ae0: the DSFXChorus/DSFXFlanger fields, parameter order */
        static const int fld[2][7] = { { 0, 1, 3, 4, 6, 2, 5 }, { 0, 4, 3, 1, 6, 2, 5 } };
        const int *fl = fld[f->kind == DSFX_FLANGER];
        for (int i = 0; i < 7; i++) {
            float v;
            if (fl[i] == 4 || fl[i] == 6) v = (float)(int32_t)w[fl[i]]; else memcpy(&v, &w[fl[i]], 4);
            if ((hr = dsfx_set_param(f, i, v)) < 0) return hr;
        }
        return hr;
    }
    }
    return DSFX_E_INVALIDARG;
}
int dsfx_get_all(const dsfx *f, void *params)
{
    uint32_t *w = params;
    switch (f->kind) {
    case DSFX_I3DL2: return i3dl2_get_all(((dsfx *)f)->u.i3, params);
    case DSFX_GARGLE:                   /* 10015b90 */
        w[0] = (uint32_t)ftol(f->p[0]); w[1] = (uint32_t)ftol(f->p[1]);
        return DSFX_OK;
    case DSFX_ECHO:                     /* 100118c0 */
        memcpy(w, f->p, 4 * sizeof(float)); w[4] = (uint32_t)ftol(f->p[4]);
        return DSFX_OK;
    case DSFX_PARAMEQ: memcpy(w, f->p, 3 * sizeof(float)); return DSFX_OK;
    case DSFX_CHORUS:                   /* 10009850 */
        memcpy(&w[0], &f->p[0], 4); memcpy(&w[1], &f->p[1], 4); memcpy(&w[2], &f->p[5], 4); memcpy(&w[3], &f->p[2], 4);
        w[4] = (uint32_t)ftol(f->p[3]); memcpy(&w[5], &f->p[6], 4); w[6] = (uint32_t)ftol(f->p[4]);
        return DSFX_OK;
    case DSFX_COMPRESSOR: memcpy(w, f->p, 6 * sizeof(float)); return DSFX_OK;   /* 1000c280 */
    case DSFX_WAVES: memcpy(w, f->p, 4 * sizeof(float)); return DSFX_OK;        /* 10016ee0 */
    case DSFX_DISTORTION:               /* 1000dfb0 */
        memcpy(&w[0], &f->p[0], 4); memcpy(&w[1], &f->p[1], 4); memcpy(&w[2], &f->p[3], 4); memcpy(&w[3], &f->p[4], 4);
        memcpy(&w[4], &f->p[2], 4);
        return DSFX_OK;
    case DSFX_FLANGER:                  /* 10014bb0 */
        memcpy(&w[0], &f->p[0], 4); memcpy(&w[1], &f->p[3], 4); memcpy(&w[2], &f->p[5], 4); memcpy(&w[3], &f->p[2], 4);
        w[4] = (uint32_t)ftol(f->p[1]); memcpy(&w[5], &f->p[6], 4); w[6] = (uint32_t)ftol(f->p[4]);
        return DSFX_OK;
    }
    return DSFX_E_INVALIDARG;
}

void dsfx_discontinuity(dsfx *f)
{
    switch (f->kind) {
    case DSFX_GARGLE: gargle_discont(f); break;
    case DSFX_ECHO: echo_discont(f); break;
    case DSFX_PARAMEQ: eq_discont(f); break;
    case DSFX_CHORUS: chorus_discont(f); break;
    case DSFX_FLANGER: flanger_discont(f); break;
    case DSFX_DISTORTION: dist_discont(f); break;
    case DSFX_COMPRESSOR: comp_discont(f); break;
    case DSFX_WAVES: waves_discont(f); break;
    case DSFX_I3DL2: i3dl2_discontinuity(f->u.i3); break;
    }
}

static void process(dsfx *f, void *data, uint32_t frames)
{
    switch (f->kind) {
    case DSFX_GARGLE: gargle_process(f, data, frames); break;
    case DSFX_ECHO: echo_process(f, data, frames); break;
    case DSFX_PARAMEQ: eq_process(f, data, frames); break;
    case DSFX_CHORUS: chorus_process(f, data, frames); break;
    case DSFX_FLANGER: flanger_process(f, data, frames); break;
    case DSFX_DISTORTION: dist_process(f, data, frames); break;
    case DSFX_COMPRESSOR: comp_process(f, data, frames); break;
    case DSFX_WAVES: waves_process(f, data, frames); break;
    }
}
/* 10006d70: whole quanta while more than one is left, then the rest; each with its time */
int dsfx_param_info(int kind, int i, dsfx_pinfo *o)
{
    if (kind < 0 || kind >= DSFX_NKINDS || kind == DSFX_I3DL2 || i < 0 || i >= s_nparams[kind]) return DSFX_E_INVALIDARG;
    const pdesc *d = &s_pdesc[kind][i];
    o->type = d->type; o->caps = 0x1f; o->min = d->min; o->max = d->max; o->neutral = d->neutral;
    o->unit = (const uint16_t *)d->unit; o->label = (const uint16_t *)d->label; o->enums = (const uint16_t *)d->enums;
    return DSFX_OK;
}
void dsfx_process_at(dsfx *f, void *data, uint32_t frames, int64_t t)
{
    if (!f->hasfmt || !frames) return;
    if (f->kind == DSFX_I3DL2) { i3dl2_process(f->u.i3, data, frames); return; }
    unsigned fb = f->ch * f->bits / 8;
    uint8_t *d = data;
    uint32_t done = 0, q = f->qframes;
    if (frames > q)
        do {
            if (f->active) update_params(f, t);
            process(f, d, q);
            t += f->qtime; d += q * fb; done += q;
        } while (done < frames - q);
    if (f->active) update_params(f, t);
    process(f, d, frames - done);
}
void dsfx_process(dsfx *f, void *data, uint32_t frames) { dsfx_process_at(f, data, frames, 0); }
