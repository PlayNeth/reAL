/* Compare the alrv port against ground truth produced by running ALchemy's own reverb in an emulator.
 * usage: test_alrv <gt dir> <case...>   (files <case>.P/.E/.in/.out, see re/gen_gt.py)
 *   -p : also recompute the parameter block from the property struct (.E) and compare it */
#include "../src/alrv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) exit(2); fclose(f);
    if (size) *size = n;
    return b;
}

static const char *chn[ALRV_NOUT] = { "FL", "FR", "FC", "BL", "BR", "Cx", "SL", "SR" };

static int cmp_params(const alrv_params *a, const alrv_params *b)
{
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    int bad = 0;
    for (unsigned i = 0; i < sizeof(alrv_params) / 4; i++) {
        float fx, fy; memcpy(&fx, &x[i], 4); memcpy(&fy, &y[i], 4);
        int isint = (x[i] && x[i] < 0x01000000) || (y[i] && y[i] < 0x01000000);
        int ok = x[i] == y[i] || (!isint && fabsf(fx - fy) <= 2e-5f * fmaxf(1.0f, fabsf(fy)));
        if (!ok) {
            if (isint) printf("    P+0x%03x: got %d want %d\n", i * 4, (int)x[i], (int)y[i]);
            else printf("    P+0x%03x: got %.7g want %.7g\n", i * 4, fx, fy);
            bad++;
        }
    }
    return bad;
}

int main(int argc, char **argv)
{
    int params = 0, argi = 1, fails = 0;
    if (argc > 1 && !strcmp(argv[1], "-p")) { params = 1; argi++; }
    if (argc - argi < 2) { fprintf(stderr, "usage: test_alrv [-p] dir case...\n"); return 2; }
    const char *dir = argv[argi++];
    for (; argi < argc; argi++) {
        char path[512];
        size_t n_in, n_out, n_p;
        snprintf(path, sizeof(path), "%s/%s.P", dir, argv[argi]);
        alrv_params *P = load(path, &n_p);
        if (n_p != sizeof(alrv_params)) { fprintf(stderr, "bad P size\n"); return 2; }
        snprintf(path, sizeof(path), "%s/%s.in", dir, argv[argi]);
        float *in = load(path, &n_in); n_in /= 4;
        snprintf(path, sizeof(path), "%s/%s.out", dir, argv[argi]);
        float *ref = load(path, &n_out); n_out /= 4 * ALRV_NOUT;
        if (n_out != n_in) { fprintf(stderr, "length mismatch\n"); return 2; }

        printf("%s:\n", argv[argi]);
        if (params) {
            snprintf(path, sizeof(path), "%s/%s.E", dir, argv[argi]);
            alrv_props *E = load(path, NULL);
            alrv_params mine;
            alrv_compute_params(&mine, E, 48000);
            int bad = cmp_params(&mine, P);
            printf("  params: %s (%d fields differ)\n", bad ? "MISMATCH" : "ok", bad);
            if (bad) fails++;
            free(E);
        }

        alrv r;
        if (!alrv_init(&r, 48000)) return 3;
        alrv_set_params(&r, P);
        /* ALchemy quirk: on the first (immediate) parameter apply the shelf's inner allpass keeps its
         * constructor coefficient 0.5 (the new value only propagates on crossfaded updates). The
         * ground truth was captured right after the first apply. */
        for (int i = 0; i < 4; i++) r.shelf_a[i] = 0.5f;
        float *out[ALRV_NOUT];
        for (int c = 0; c < ALRV_NOUT; c++) out[c] = calloc(n_in, sizeof(float));
        alrv_process(&r, in, out, (uint32_t)n_in);
        for (int c = 0; c < ALRV_NOUT; c++) {
            const float *y = ref + (size_t)c * n_in;
            double e_ref = 0, e_err = 0; size_t first = (size_t)-1;
            for (size_t i = 0; i < n_in; i++) {
                double d = (double)out[c][i] - y[i];
                e_ref += (double)y[i] * y[i]; e_err += d * d;
                if (first == (size_t)-1 && fabs(d) > 1e-4 * (1 + fabs(y[i]))) first = i;
            }
            double snr = e_err > 0 ? 10 * log10(e_ref / e_err) : 999;
            printf("  %s: ref energy %10.4f  SNR %6.1f dB%s", chn[c], e_ref, snr, snr < 60 ? "  <-- BAD" : "");
            if (first != (size_t)-1 && snr < 60) printf(" (first diff at %zu: got %g want %g)", first, out[c][first], y[first]);
            printf("\n");
            if (snr < 60) fails++;
            free(out[c]);
        }
        alrv_free(&r);
        free(P); free(in); free(ref);
    }
    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
