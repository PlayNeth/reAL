/* test_hrtf - the built-in HRTF set (hrtf=builtin, MIT KEMAR): unfolds and loads at every device rate, its left half
 * mirrors the right, it lateralises the right way round, its level is the one tools/hrtf_kemar.py set, and a voice
 * renders through it from its first direction (set with no Play in between: the first set used to leave the voice
 * silent, its coefficients the wrong way round, until the next change of direction or a restart). */
#include "../src/hrtf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); s_fail++; } } while (0)

static int cmp_double(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return a < b ? -1 : a > b; }
static double energy(const float *h, uint32_t n) { double e = 0; for (uint32_t k = 0; k < n; k++) e += (double)h[k] * h[k]; return e; }
/* |H(f)| in dB */
static double mag_db(const float *h, uint32_t n, double f, double rate)
{
    double re = 0, im = 0;
    for (uint32_t k = 0; k < n; k++) { re += h[k] * cos(2 * M_PI * f * k / rate); im -= h[k] * sin(2 * M_PI * f * k / rate); }
    return 10 * log10(re * re + im * im);
}

int main(void)
{
    rl_hrtf_set s;
    CHECK(rl_hrtf_load_builtin(&s, 48000), "load at 48000");
    CHECK(s.rate == 48000 && s.taps == 64 && s.n_rings == 14 && s.n_dirs == 710, "48000: rate %u taps %u rings %u dirs %u",
          s.rate, s.taps, s.n_rings, s.n_dirs);

    /* the left half is the right half with the ears swapped, through the interpolation too */
    double worst = 0;
    rl_hrtf_filter a, b;
    for (float el = -45.0f; el <= 90.0f; el += 7.3f)
        for (float az = 0.0f; az < 360.0f; az += 3.7f) {
            rl_hrtf_lookup(&s, az, el, &a);
            rl_hrtf_lookup(&s, 360.0f - az, el, &b);
            for (int ch = 0; ch < 2; ch++) {
                double d = fabs(a.delay[ch] - b.delay[ch ^ 1]);
                if (d > worst) worst = d;
                for (uint32_t k = 0; k < s.taps; k++) {
                    d = fabs(a.coef[ch][k] - b.coef[ch ^ 1][k]);
                    if (d > worst) worst = d;
                }
            }
        }
    CHECK(worst < 1e-5, "mirror symmetry: worst difference %g", worst);

    /* azimuth clockwise from the front: 90 is the right */
    rl_hrtf_lookup(&s, 90.0f, 0.0f, &a);
    double el_ = energy(a.coef[0], s.taps), er = energy(a.coef[1], s.taps);
    printf("az 90: energy L %.3f R %.3f, delay L %.1f R %.1f\n", el_, er, a.delay[0], a.delay[1]);
    CHECK(er > 8 * el_ && a.delay[1] + 10 < a.delay[0], "az 90 is not on the right");
    rl_hrtf_lookup(&s, 0.0f, 0.0f, &a);
    CHECK(fabs(energy(a.coef[0], s.taps) - energy(a.coef[1], s.taps)) < 1e-6 && a.delay[0] == a.delay[1], "front is not centred");

    /* level: the horizontal ring's mean energy, both ears (0.82 in tools/hrtf_kemar.py, after 16-bit rounding) */
    double e0 = 0;
    for (int k = 0; k < 72; k++) {
        rl_hrtf_lookup(&s, k * 5.0f, 0.0f, &a);
        e0 += energy(a.coef[0], s.taps) + energy(a.coef[1], s.taps);
    }
    e0 /= 72;
    printf("horizontal ring mean energy %.4f\n", e0);
    CHECK(fabs(e0 - 0.82) < 0.01, "level %.4f", e0);

    /* the other rates: the filters resampled band-limited, their response kept up to 40% of the lower rate (both
     * ears, a grid of directions; notches aside, so the 95th percentile of the differences) */
    static const uint32_t rates[] = { 22050, 32000, 44100, 88200, 96000, 192000 };
    static double dif[40000];
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        rl_hrtf_set r;
        int ok = rl_hrtf_load_builtin(&r, rates[i]);
        CHECK(ok && r.rate == rates[i] && r.taps <= RL_HRTF_MAX_TAPS, "load at %u", rates[i]);
        if (!ok) continue;
        const double lim = 0.4 * (rates[i] < 48000 ? rates[i] : 48000);
        int n = 0;
        for (float az = 0.0f; az < 360.0f; az += 30.0f)
            for (float el = -30.0f; el <= 60.0f; el += 30.0f) {
                rl_hrtf_lookup(&s, az, el, &a);
                rl_hrtf_lookup(&r, az, el, &b);
                for (int ch = 0; ch < 2; ch++)
                    for (double f = 200.0; f < lim && n < 40000; f *= 1.1)
                        dif[n++] = fabs(mag_db(b.coef[ch], r.taps, f, rates[i]) - mag_db(a.coef[ch], s.taps, f, 48000));
            }
        qsort(dif, n, sizeof(double), cmp_double);
        printf("%6u Hz: response within %.2f dB of 48 kHz's (95%% of %d points to %.0f Hz)\n", rates[i], dif[n * 95 / 100], n, lim);
        CHECK(dif[n * 95 / 100] < 0.5, "%u Hz: 95th percentile %.2f dB", rates[i], dif[n * 95 / 100]);
        rl_hrtf_free(&r);
    }

    /* a voice on the right renders louder on the right, finite throughout */
    rl_hrtf_voice *v = calloc(1, sizeof *v);
    rl_hrtf_voice_reset(v);
    rl_hrtf_lookup(&s, 80.0f, 0.0f, &a);
    rl_hrtf_voice_set(v, &a);
    enum { N = 4800 };
    float *in = malloc(N * sizeof(float)), *out = calloc(2 * N, sizeof(float));
    unsigned seed = 1;
    for (int i = 0; i < N; i++) { seed = seed * 1103515245u + 12345u; in[i] = (float)((seed >> 9) & 0xffff) / 32768.0f - 1.0f; }
    for (int i = 0; i < N; i += 480) rl_hrtf_render(v, &s, in + i, out + 2 * i, 480);
    double l = 0, rr = 0;
    int finite = 1;
    for (int i = 0; i < N; i++) {
        l += (double)out[2 * i] * out[2 * i];
        rr += (double)out[2 * i + 1] * out[2 * i + 1];
        if (!isfinite(out[2 * i]) || !isfinite(out[2 * i + 1])) finite = 0;
    }
    printf("render az 80: L %.1f dB, R %.1f dB\n", 10 * log10(l / N), 10 * log10(rr / N));
    CHECK(finite && rr > 4 * l, "render");
    free(in); free(out); free(v);
    rl_hrtf_free(&s);

    printf(s_fail ? "test_hrtf: %d FAILED\n" : "test_hrtf: all passed\n", s_fail);
    return s_fail != 0;
}
