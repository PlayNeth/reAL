/* Waves reverb, ported from dsdmo.dll (10.0.26100, image base 0x10000000): coefficient setup 10003210, Q15
 * conversion 10002eb0, delay layout 100030d0, 16-bit stereo 10003b30 and mono 10003770, float mono 10003f30 and
 * stereo 100042b0 (the same network on floats in the same memory).  The setup is single
 * precision float (SSE) with pow/sqrt in double, then everything runs in 32-bit integers.  See wavesrev.h. */
#include "wavesrev.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* the 0x94-byte state; comments give the offsets */
struct wavesrev {
    float rate;             /* +0x08 */
    float fdry; int32_t kdry;           /* +0x0c +0x10 */
    float fwet; int32_t kwet;           /* +0x14 +0x18 */
    int32_t d[4];                       /* +0x1c..+0x28: delay line offsets (x4, interleaved) */
    int32_t ap[2];                      /* +0x2c +0x30: all-pass lengths (x2) */
    float fap; int32_t kap;             /* +0x34 +0x38: all-pass coefficient (0.618) */
    float fa0; int32_t ka0;             /* +0x3c +0x40 */
    float fa1; int32_t ka1;             /* +0x44 +0x48 */
    float f[8]; int32_t k[8];           /* +0x4c..+0x88: pairs (float, Q15) */
    float fnorm; int32_t knorm;         /* +0x8c +0x90 */
    float ingain, mix, time, hfratio;
    uint32_t *mem;                      /* 0x14008 bytes: two positions, 4 x 4096 delay, 2 x 2048 x 2 all-pass */
};

#define MEM_WORDS (0x14008 / 4)

static int32_t q15(float x)                      /* 10002eb0 */
{
    float v = x * 32768.0f + 0.5f, r = 32767.0f;
    if (v <= 32767.0f) r = v;
    if (r < -32768.0f) r = -32768.0f;
    return (int32_t)r;
}

static float p10(float x) { return (float)pow(10.0, (double)x); }

/* 10003210 */
static void setup(wavesrev *w, float ingain, float mix, float time, float hfratio)
{
    float hf = 0.999f < hfratio ? 0.999f : hfratio;   /* minss */
    if (!(0.0f < hf)) hf = 0.001f;
    float t = 0.001f > time ? 0.001f : time;          /* maxss */
    float k = 1.0f / hf - 1.0f;
    float g = 0.0f < ingain ? 0.0f : ingain;
    float m = 0.0f < mix ? 0.0f : mix;
    float rate = w->rate;
    if (1.0f > rate) { rate = 22050.0f; w->rate = 22050.0f; }
    float c = -3000.0f / (rate * t);
    int32_t d0 = w->ap[0] >> 1, d1 = w->ap[1] >> 1;
    float pa0 = p10((float)d0 * c);
    w->fa0 = pa0;
    float pa1 = p10((float)d1 * c);
    w->fa1 = pa1;
    float sq[4], poly3 = 0;
    for (int i = 0; i < 4; i++) {
        int32_t e = w->d[i] >> 2, dd = i < 2 ? d0 : d1;
        float a = p10((float)e * c), pa = i < 2 ? pa0 : pa1;
        float y = i < 3 ? a * pa : pa * a;
        float poly = (((y + 1.0f) * y + 1.0f) * y + 1.0f) * y + 1.0f;
        if (i < 3) sq[i] = poly * poly; else poly3 = poly;
        float b = p10((float)(e + dd) * c * k);
        float tt = (1.0f - b) * 0.5f;
        w->f[2 * i + 1] = a * tt;               /* +0x54 +0x64 +0x74 +0x84 */
        w->f[2 * i] = (1.0f - tt) * a;          /* +0x4c +0x5c +0x6c +0x7c */
    }
    float sum = sq[0] + 0.0f;
    sum += sq[1];
    sum += sq[2];
    sum += poly3 * poly3;
    float norm = (float)sqrt((double)sum);
    w->fnorm = norm;
    float G = p10(g * 0.05f), M = p10(m * 0.1f);
    w->fdry = (float)sqrt((double)(1.0f - M)) * G;
    w->fwet = (float)sqrt((double)M) * (4.0f / norm * G);
    w->kdry = q15(w->fdry); w->kwet = q15(w->fwet); w->kap = q15(w->fap);
    w->ka0 = q15(w->fa0); w->ka1 = q15(w->fa1);
    for (int i = 0; i < 8; i++) w->k[i] = q15(w->f[i]);
    w->knorm = q15(w->fnorm);
}

/* 100030d0 */
static void init(wavesrev *w, float rate)
{
    w->rate = rate;
    w->fap = 0.618034f;     /* 0x3f1e377a */
    const float f = 1.1892070770263672f;
    int32_t a = (int32_t)((double)rate * 0.045 + 0.5);
    int32_t c = (int32_t)((double)((float)a * f) + 0.5);
    int32_t b = (int32_t)((double)((float)c * f) + 0.5);
    int32_t d = (int32_t)((double)((float)b * f) + 0.5);
    int32_t i1 = (int32_t)((double)(float)(b + a) * 0.11546666920185089 + 0.5);
    int32_t i2 = (int32_t)((double)(float)(d + c) * 0.11546666920185089 + 0.5);
    w->d[0] = (a - i1) << 2; w->d[1] = (b - i1) << 2; w->d[2] = (c - i2) << 2; w->d[3] = (d - i2) << 2;
    w->ap[0] = i1 * 2; w->ap[1] = i2 * 2;
    setup(w, 0.0f, -10.0f, 1000.0f, 0.001f);
}

wavesrev *wavesrev_new(uint32_t rate)
{
    wavesrev *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->mem = calloc(MEM_WORDS, 4);
    if (!w->mem) { free(w); return NULL; }
    w->ingain = 0.0f; w->mix = 0.0f; w->time = 1000.0f; w->hfratio = 0.001f;
    init(w, (float)rate);
    setup(w, w->ingain, w->mix, w->time, w->hfratio);
    return w;
}

void wavesrev_free(wavesrev *w) { if (w) { free(w->mem); free(w); } }

void wavesrev_reset(wavesrev *w)
{
    memset(w->mem, 0, 0x14008);
    init(w, w->rate);
    setup(w, w->ingain, w->mix, w->time, w->hfratio);
}

int wavesrev_set(wavesrev *w, float ingain, float mix, float time, float hfratio)
{
    if (ingain < -96.0f || ingain > 0.0f || mix < -96.0f || mix > 0.0f ||      /* comiss: NaN passes */
        time < 0.001f || time > 3000.0f || hfratio < 0.001f || hfratio > 0.999f) return -1;
    w->ingain = ingain; w->mix = mix; w->time = time; w->hfratio = hfratio;
    setup(w, ingain, mix, time, hfratio);
    return 0;
}

#define MUL(a, b) ((int32_t)((uint32_t)(a) * (uint32_t)(b)))
#define SH(x) ((int32_t)(x) >> 15)

/* 10003b30 */
void wavesrev_stereo16(wavesrev *w, int16_t *buf, uint32_t frames)
{
    uint32_t *B = w->mem;
    uint32_t pa = B[1], p = B[0];
    uint32_t i18 = (w->d[0] + 4 + p) & 0x3fff, ic = (w->d[1] + 4 + p) & 0x3fff;
    uint32_t i1c = (p + w->d[2] + 4) & 0x3fff, i10 = (w->d[3] + p + 4) & 0x3fff;
    uint32_t L54 = i10 + 5, L44 = ic + 3;
    for (uint32_t n = 0; n < frames; n++) {
        uint32_t L40 = i18 + 2, L4c = i1c + 4;
        ic = (ic - 4) & 0x3fff; i10 = (i10 - 4) & 0x3fff; i18 = (i18 - 4) & 0x3fff; i1c = (i1c - 4) & 0x3fff;
        int32_t x0 = buf[2 * n], x1 = buf[2 * n + 1];
        int32_t kdry = w->kdry;
        int32_t a0 = SH(MUL(w->kwet, x0)), a1 = SH(MUL(w->kwet, x1));
        int32_t g = w->kap;
        uint32_t q = pa & 0x7ff, r = ((w->ap[0] + pa) & 0x7ff) + 0x4002;
        int32_t u8 = SH(MUL(B[ic + 3], g) + MUL(B[r], w->ka0));
        int32_t u10 = SH(MUL(B[r + 1], w->ka0) - MUL(B[i18 + 2], g));
        B[q + 0x4002] = (uint32_t)(SH(MUL(u10, g)) + (int32_t)B[i18 + 2]);
        B[q + 0x4003] = (uint32_t)((int32_t)B[ic + 3] - SH(MUL(u8, g)));
        B[i18 + 2] = (uint32_t)u8;
        B[ic + 3] = (uint32_t)u10;
        g = w->kap;
        r = ((w->ap[1] + pa) & 0x7ff) + 0x4802;
        u8 = SH(MUL(B[i10 + 5], g) + MUL(B[r], w->ka1));
        u10 = SH(MUL(B[r + 1], w->ka1) - MUL(B[i1c + 4], g));
        B[q + 0x4802] = (uint32_t)(SH(MUL(u10, g)) + (int32_t)B[i1c + 4]);
        B[q + 0x4803] = (uint32_t)((int32_t)B[i10 + 5] - SH(MUL(u8, g)));
        B[i1c + 4] = (uint32_t)u8;
        B[i10 + 5] = (uint32_t)u10;
        int32_t o0 = SH(MUL(kdry, x0)) + (int32_t)B[i1c + 4] + (int32_t)B[i18 + 2];
        int32_t o1 = SH(MUL(kdry, x1)) + (int32_t)B[ic + 3] + u10;
        uint32_t P = p + 2;
        B[P] = (uint32_t)(SH(MUL(w->k[1], B[L40]) + MUL(w->k[0], B[i18 + 2])) + a0);
        B[P + 1] = (uint32_t)(SH(MUL(w->k[2], B[ic + 3]) + MUL(B[L44], w->k[3])) + a1);
        B[P + 2] = (uint32_t)(SH(MUL(w->k[5], B[L4c]) + MUL(w->k[4], B[i1c + 4])) - a1);
        uint32_t v = B[L54];
        L54 = i10 + 5;
        pa = (pa - 2) & 0x7ff;
        B[P + 3] = (uint32_t)(SH(MUL(w->k[7], v) + MUL(w->k[6], B[L54])) + a0);
        p = (p - 4) & 0x3fff;
        buf[2 * n] = (int16_t)(o0 >= 0x8000 ? 0x7fff : o0 < -0x8000 ? -0x8000 : o0);
        buf[2 * n + 1] = (int16_t)(o1 >= 0x8000 ? 0x7fff : o1 < -0x8000 ? -0x8000 : o1);
        L44 = ic + 3;
    }
    B[1] = pa; B[0] = p;
}

/* 10003770 */
void wavesrev_mono16(wavesrev *w, int16_t *buf, uint32_t frames)
{
    uint32_t *B = w->mem;
    uint32_t pa = B[1], p = B[0];
    uint32_t i14 = (w->d[0] + 4 + p) & 0x3fff, i10 = (w->d[1] + 4 + p) & 0x3fff;
    uint32_t i1c = (p + w->d[2] + 4) & 0x3fff, i24 = (w->d[3] + p + 4) & 0x3fff;
    for (uint32_t n = 0; n < frames; n++) {
        uint32_t L3c = i14 + 2, L40 = i10 + 3, L44 = i1c + 4, L48 = i24 + 5;
        i14 = (i14 - 4) & 0x3fff; i10 = (i10 - 4) & 0x3fff; i1c = (i1c - 4) & 0x3fff; i24 = (i24 - 4) & 0x3fff;
        int32_t x = buf[n];
        int32_t kdry = w->kdry;
        int32_t a = SH(MUL(w->kwet, x >> 1));
        int32_t g = w->kap;
        uint32_t q = pa & 0x7ff, r = ((w->ap[0] + pa) & 0x7ff) + 0x4002;
        int32_t u9 = SH(MUL(B[i10 + 3], g) + MUL(B[r], w->ka0));
        int32_t u7 = SH(MUL(B[r + 1], w->ka0) - MUL(B[i14 + 2], g));
        B[q + 0x4002] = (uint32_t)(SH(MUL(u7, g)) + (int32_t)B[i14 + 2]);
        B[q + 0x4003] = (uint32_t)((int32_t)B[i10 + 3] - SH(MUL(u9, g)));
        B[i14 + 2] = (uint32_t)u9;
        B[i10 + 3] = (uint32_t)u7;
        g = w->kap;
        r = ((w->ap[1] + pa) & 0x7ff) + 0x4802;
        u9 = SH(MUL(B[r], w->ka1) + MUL(B[i24 + 5], g));
        int32_t u8 = SH(MUL(B[r + 1], w->ka1) - MUL(B[i1c + 4], g));
        B[q + 0x4802] = (uint32_t)(SH(MUL(u8, g)) + (int32_t)B[i1c + 4]);
        B[q + 0x4803] = (uint32_t)((int32_t)B[i24 + 5] - SH(MUL(u9, g)));
        B[i1c + 4] = (uint32_t)u9;
        B[i24 + 5] = (uint32_t)u8;
        int32_t s4 = (int32_t)B[i1c + 4], s7 = (int32_t)B[i14 + 2], s9 = (int32_t)B[i10 + 3];
        uint32_t P = p + 2;
        B[P] = (uint32_t)(SH(MUL(w->k[0], s7) + MUL(w->k[1], B[L3c])) + a);
        B[P + 1] = (uint32_t)(SH(MUL(w->k[3], B[L40]) + MUL(w->k[2], B[i10 + 3])) + a);
        B[P + 2] = (uint32_t)(SH(MUL(w->k[4], B[i1c + 4]) + MUL(w->k[5], B[L44])) - a);
        B[P + 3] = (uint32_t)(SH(MUL(w->k[6], B[i24 + 5]) + MUL(B[L48], w->k[7])) + a);
        p = (p - 4) & 0x3fff;
        pa = (pa - 2) & 0x7ff;
        int32_t o = s7 + SH(MUL(kdry, x >> 1)) * 2 + s9 + s4 + u8;
        buf[n] = (int16_t)(o >= 0x8000 ? 0x7fff : o < -0x8000 ? -0x8000 : o);
    }
    B[0] = p; B[1] = pa;
}

/* 10003f30 (mono) and 100042b0 (stereo): the network in float; the input gets 1e-30 added (no denormals), a mono
 * input is halved and its dry part added to both halves of the sum */
static void wavesrev_float(wavesrev *w, float *buf, uint32_t frames, int stereo)
{
    uint32_t *B = w->mem;
    float *F = (float *)(void *)w->mem;
    const float tiny = 1.0000000031710769e-30f;     /* 10002940 */
    uint32_t pa = B[1], p = B[0];
    uint32_t i0 = (w->d[0] + 4 + p) & 0x3fff, i1 = (w->d[1] + 4 + p) & 0x3fff;
    uint32_t i2 = (w->d[2] + 4 + p) & 0x3fff, i3 = (w->d[3] + 4 + p) & 0x3fff;
    float *L0 = &F[i0 + 2], *L1 = &F[i1 + 3], *L2 = &F[i2 + 4], *L3 = &F[i3 + 5];
    for (uint32_t n = 0; n < frames; n++) {
        i0 = (i0 - 4) & 0x3fff; i1 = (i1 - 4) & 0x3fff; i2 = (i2 - 4) & 0x3fff; i3 = (i3 - 4) & 0x3fff;
        float dl, dr, a0, a1;
        if (stereo) {
            float xl = buf[2 * n] + tiny, xr = buf[2 * n + 1] + tiny;
            a0 = w->fwet * xl; dr = w->fdry * xr; a1 = w->fwet * xr; dl = w->fdry * xl;
        } else {
            float x = buf[n] * 0.5f + tiny;
            dl = dr = x * w->fdry; a0 = a1 = x * w->fwet;
        }
        float *c0 = &F[i0 + 2], *c1 = &F[i1 + 3], *c2 = &F[i2 + 4], *c3 = &F[i3 + 5];
        float g = w->fap, k = w->fa0;
        uint32_t q = pa & 0x7ff, r = (w->ap[0] + pa) & 0x7ff;
        float u9 = k * F[r + 0x4002] + g * *c1;
        float u7 = k * F[r + 0x4003] - *c0 * g;
        F[q + 0x4002] = u7 * g + *c0;
        F[q + 0x4003] = *c1 - u9 * g;
        *c0 = u9; *c1 = u7;
        k = w->fa1;
        r = (w->ap[1] + pa) & 0x7ff;
        float v9 = k * F[r + 0x4802] + g * *c3;
        float u8 = k * F[r + 0x4803] - *c2 * g;
        F[q + 0x4802] = u8 * g + *c2;
        F[q + 0x4803] = *c3 - v9 * g;
        *c2 = v9; *c3 = u8;
        float ol = *c0 + *c2 + dl, orr = u8 + *c1 + dr;
        F[p + 2] = *c0 * w->f[0] + a0 + w->f[1] * *L0;
        F[p + 3] = w->f[2] * *c1 + a1 + w->f[3] * *L1;
        F[p + 4] = *c2 * w->f[4] - a1 + w->f[5] * *L2;
        F[p + 5] = w->f[6] * *c3 + a0 + *L3 * w->f[7];
        L0 = c0; L1 = c1; L2 = c2; L3 = c3;
        p = (p - 4) & 0x3fff;
        pa = (pa - 2) & 0x7ff;
        if (stereo) { buf[2 * n] = ol; buf[2 * n + 1] = orr; }
        else buf[n] = orr + ol;
    }
    B[1] = pa; B[0] = p;
}
void wavesrev_mono_float(wavesrev *w, float *buf, uint32_t frames) { wavesrev_float(w, buf, frames, 0); }
void wavesrev_stereo_float(wavesrev *w, float *buf, uint32_t frames) { wavesrev_float(w, buf, frames, 1); }
