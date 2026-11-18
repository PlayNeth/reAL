/* test_outconv - the mix converted to the device's integer formats (src/outconv.c).
 *
 * The rounding is ALchemy's (tools/convref.py compares it with the DLL's kernels in an emulator; here the cases
 * that tell rounding apart from cutting off, and the edges).  Then what the dither is for: a sound too quiet for the
 * steps is still there, and the error no longer follows the signal. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../src/outconv.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* how much of a tone of `cycles` cycles in n samples there is in x (its amplitude) */
static double tone(const double *x, int n, double cycles)
{
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) { re += x[i] * cos(2 * M_PI * cycles * i / n); im += x[i] * sin(2 * M_PI * cycles * i / n); }
    return 2 * sqrt(re * re + im * im) / n;
}

int main(void)
{
    static const struct { float x; int s16; } r16[] = {
        { 0.0f, 0 }, { 1.0f, 32767 }, { -1.0f, -32768 }, { 2.0f, 32767 }, { -2.0f, -32768 }, { 1e30f, 32767 }, { -1e30f, -32768 },
        { 0.4f / 32768, 0 }, { 0.5f / 32768, 1 }, { 0.6f / 32768, 1 }, { -0.4f / 32768, 0 }, { -0.5f / 32768, 0 }, { -0.6f / 32768, -1 },
        { 1.5f / 32768, 2 }, { -1.5f / 32768, -1 }, { 2.5f / 32768, 3 }, { 32766.5f / 32768, 32767 }, { 32767.4f / 32768, 32767 },
        { -32767.5f / 32768, -32767 }, { -32767.6f / 32768, -32768 }, { 0.25f, 8192 }, { -0.25f, -8192 },
    };
    for (unsigned i = 0; i < sizeof(r16) / sizeof(*r16); i++) {
        int16_t o;
        rl_out_s16(&o, &r16[i].x, 1, NULL);
        CHECK(o == r16[i].s16 && rl_out_s16_one(r16[i].x, NULL) == o, "16 bits: %g gives %d, not %d", r16[i].x * 32768.0, o, r16[i].s16);
    }
    static const struct { float x; int32_t fixed, alchemy; } r32[] = {
        { 0.0f, 0, 0 }, { 0.5f, 1073741824, 1073741824 }, { -1.0f, -2147483647 - 1, -2147483647 - 1 }, { -2.0f, -2147483647 - 1, -2147483647 - 1 },
        { 1.0f, 2147483647, -2147483647 - 1 }, { 1.0000001f, 2147483647, 2147483647 }, { 0.99999994f, 2147483520, 2147483520 },
        { 2.0f, 2147483647, 2147483647 }, { 0.5f / 2147483648.0f, 1, 1 }, { -0.5f / 2147483648.0f, 0, 0 }, { 0.4f / 2147483648.0f, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof(r32) / sizeof(*r32); i++) {
        int32_t f, a;
        rl_out_s32(&f, &r32[i].x, 1, 1);
        rl_out_s32(&a, &r32[i].x, 1, 0);
        CHECK(f == r32[i].fixed && a == r32[i].alchemy, "32 bits: %.9g gives %d and %d, not %d and %d", r32[i].x, f, a, r32[i].fixed, r32[i].alchemy);
    }
    {
        const float nan = NAN, inf = INFINITY, minf = -INFINITY;
        uint32_t d = 0;
        CHECK(rl_out_s16_one(nan, NULL) == 0 && rl_out_s16_one(nan, &d) == 0 && rl_out_s16_one(inf, &d) == 32767 && rl_out_s16_one(minf, &d) == -32768
              && rl_out_s32_one(nan, 1) == 0 && rl_out_s32_one(inf, 0) == 2147483647 && rl_out_s32_one(minf, 0) == -2147483647 - 1,
              "a NaN is nothing, an infinity the end of the range");
    }

    enum { N = 1 << 16 };
    static float in[N];
    static int16_t out[N];
    static double e[N];
    /* silence stays silence, and a start from 0 is as good as any */
    {
        uint32_t d = 0;
        int quiet = 1;
        memset(in, 0, sizeof(in));
        rl_out_s16(out, in, N, &d);
        for (int i = 0; i < N; i++) quiet &= out[i] == 0;
        CHECK(quiet && d == 0, "dither: silence came out with noise in it");
        in[0] = 0.5f;
        rl_out_s16(out, in, 1, &d);
        CHECK(d != 0, "dither: the generator did not start");
    }
    /* the noise alone (on a level that sits on a step): no offset, never more than a step either way, and half a
     * step in all (96 dB below full scale) */
    {
        uint32_t d = 1;
        double sum = 0, sq = 0;
        int lo = 0, hi = 0;
        for (int i = 0; i < N; i++) in[i] = 100.0f / 32768;
        rl_out_s16(out, in, N, &d);
        for (int i = 0; i < N; i++) { const int v = out[i] - 100; sum += v; sq += v * v; if (v < lo) lo = v; if (v > hi) hi = v; }
        const double mean = sum / N, rms = sqrt(sq / N);
        printf("     the noise on a steady level: mean %.4f of a step, rms %.3f, from %d to %d\n", mean, rms, lo, hi);
        CHECK(fabs(mean) < 0.01 && rms > 0.48 && rms < 0.52 && lo == -1 && hi == 1, "dither: mean %.4f, rms %.3f, %d to %d", mean, rms, lo, hi);
    }
    /* a tone of 0.4 of a step: rounded, nothing at all; with the noise, there at its level */
    {
        uint32_t d = 1;
        for (int i = 0; i < N; i++) in[i] = (float)(0.4 / 32768 * sin(2 * M_PI * 37 * i / N));
        rl_out_s16(out, in, N, NULL);
        for (int i = 0; i < N; i++) e[i] = out[i];
        const double plain = tone(e, N, 37);
        rl_out_s16(out, in, N, &d);
        for (int i = 0; i < N; i++) e[i] = out[i];
        const double with = tone(e, N, 37);
        printf("     a tone of 0.4 of a step: %.3f of a step out rounded, %.3f with the noise\n", plain, with);
        CHECK(plain == 0 && with > 0.37 && with < 0.43, "dither: a tone of 0.4 of a step came out at %.3f and %.3f", plain, with);
    }
    /* a tone of 1.3 steps: rounded, its third and fifth harmonics are there with it; with the noise they are gone
     * into it (what is left at any one frequency is the noise's share: some 0.004 of a step) */
    {
        uint32_t d = 1;
        for (int i = 0; i < N; i++) in[i] = (float)(1.3 / 32768 * sin(2 * M_PI * 37 * i / N));
        rl_out_s16(out, in, N, NULL);
        for (int i = 0; i < N; i++) e[i] = out[i];
        const double p1 = tone(e, N, 37), p3 = tone(e, N, 111), p5 = tone(e, N, 185);
        rl_out_s16(out, in, N, &d);
        for (int i = 0; i < N; i++) e[i] = out[i];
        const double w1 = tone(e, N, 37), w3 = tone(e, N, 111), w5 = tone(e, N, 185);
        printf("     a tone of 1.3 steps: rounded %.3f with harmonics of %.3f and %.3f; with the noise %.3f, %.3f and %.3f\n", p1, p3, p5, w1, w3, w5);
        CHECK(p3 > 0.1 && w3 < 0.015 && w5 < 0.015 && w1 > 1.27 && w1 < 1.33, "dither: harmonics %.3f and %.3f, the tone %.3f", w3, w5, w1);
    }
    /* loud: the same samples but for a step either way, and nothing past the ends */
    {
        uint32_t d = 7;
        static int16_t plain[N];
        int worst = 0;
        for (int i = 0; i < N; i++) in[i] = (float)(1.2 * sin(2 * M_PI * 201 * i / N));
        rl_out_s16(plain, in, N, NULL);
        rl_out_s16(out, in, N, &d);
        for (int i = 0; i < N; i++) { const int v = abs(out[i] - plain[i]); if (v > worst) worst = v; }
        CHECK(worst == 1, "dither: a loud tone moved by %d steps", worst);
    }
    if (fails) { printf("test_outconv: %d FAILED\n", fails); return 1; }
    printf("test_outconv: all passed\n");
    return 0;
}
