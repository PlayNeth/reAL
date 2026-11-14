/* SOFA files as HRTF sets (src/hrtf_sofa.c, libmysofa's reader in src/sofa).
 *   test_sofa file.sofa ...       loads each, checks it is the right way round, prints what it made of it
 *   test_sofa -fuzz file.sofa n seed out.bin
 *                                 n damaged copies of the file (out.bin: the one being tried, kept if it crashes);
 *                                 build with -fsanitize=address,undefined to check the reader
 * libmysofa's tests/ folder has SOFA files of most layouts (CIPIC, ARI, LISTEN, SADIE, FHK, TU Berlin, Mesh2HRTF). */
#include "../src/hrtf.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char *read_all(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);
    unsigned char *b = malloc(*n ? *n : 1);
    if (b && fread(b, 1, *n, f) != (size_t)*n) { free(b); b = NULL; }
    fclose(f);
    return b;
}
static double energy(const rl_hrtf_filter *f, int ch, uint32_t taps)
{
    double e = 0.0;
    for (uint32_t k = 0; k < taps; k++) e += (double)f->coef[ch][k] * f->coef[ch][k];
    return e;
}

static unsigned s_r;
static unsigned rn(unsigned n) { s_r = s_r * 1103515245u + 12345u; return n ? ((s_r >> 8) & 0xffffff) % n : 0; }
static const char *s_out;
static unsigned char *s_cur;
static long s_len;
static void on_alarm(int x)
{
    (void)x;
    FILE *f = fopen(s_out, "wb");
    if (f) { fwrite(s_cur, 1, s_len, f); fclose(f); }
    fprintf(stderr, "HANG: kept in %s\n", s_out);
    _exit(3);
}
static int fuzz(const char *path, int n, unsigned seed, const char *out)
{
    long len;
    unsigned char *b = read_all(path, &len), *d = b ? malloc(len) : NULL;
    if (!d) { printf("%s: cannot read\n", path); return 1; }
    s_r = seed; s_out = out;
    signal(SIGALRM, on_alarm);
    int loaded = 0;
    for (int i = 0; i < n; i++) {
        memcpy(d, b, len);
        long m = len;
        for (int k = 1 + rn(16); k--; ) {
            long p = rn(m);
            switch (rn(7)) {
            case 0: d[p] ^= 1 << rn(8); break;
            case 1: d[p] = (unsigned char)(rn(2) ? 0xff : rn(256)); break;
            case 2: if (p + 4 <= m) {
                        unsigned v = !rn(3) ? 0xffffffffu : !rn(3) ? 0x7fffffffu : rn(65536);
                        memcpy(d + p, &v, 4);
                    }
                    break;
            case 3: if (p + 8 <= m) {
                        unsigned long long v = rn(2) ? ~0ULL : (unsigned long long)rn(1 << 20) << 20;
                        memcpy(d + p, &v, 8);
                    }
                    break;
            case 4: if (p + 2 <= m) d[p] = d[p + 1] = 0; break;
            case 5: { long q = rn(m), c = 1 + rn(64); if (p + c <= m && q + c <= m) memmove(d + p, d + q, c); } break;
            case 6: if (!rn(8)) m = p + 1; break;
            }
        }
        FILE *f = fopen(out, "wb");
        if (f) { fwrite(d, 1, m, f); fclose(f); }
        s_cur = d; s_len = m;
        alarm(30);
        rl_hrtf_set s;
        char why[160];
        if (rl_hrtf_load_sofa(&s, d, (uint32_t)m, 48000, why, sizeof(why))) { loaded++; rl_hrtf_free(&s); }
        alarm(0);
    }
    printf("%s: %d damaged copies, %d still loaded\n", path, n, loaded);
    free(b); free(d);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-fuzz"))
        return argc < 6 ? 2 : fuzz(argv[2], atoi(argv[3]), (unsigned)atoi(argv[4]), argv[5]);
    int fails = 0;
    for (int i = 1; i < argc; i++) {
        long n;
        unsigned char *b = read_all(argv[i], &n);
        const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        if (!b) { printf("%s: cannot read\n", name); fails++; continue; }
        rl_hrtf_set s;
        char why[160] = "";
        int ok = rl_hrtf_load_sofa(&s, b, (uint32_t)n, 48000, why, sizeof(why));
        free(b);
        if (!ok) { printf("%-50s not loaded: %s\n", name, why); continue; }
        rl_hrtf_filter l, r, f;
        rl_hrtf_lookup(&s, 270.0f, 0.0f, &l);      /* (reAL's azimuth: clockwise from the front) */
        rl_hrtf_lookup(&s, 90.0f, 0.0f, &r);
        rl_hrtf_lookup(&s, 0.0f, 0.0f, &f);
        double ll = 10 * log10(energy(&l, 0, s.taps) / energy(&l, 1, s.taps));
        double rr = 10 * log10(energy(&r, 1, s.taps) / energy(&r, 0, s.taps));
        /* the near ear louder and sooner on either side (a set of one response everywhere has neither) */
        int flat = fabs(ll) < 0.01 && fabs(rr) < 0.01;
        int good = flat || (ll > 3 && rr > 3 && l.delay[0] < l.delay[1] && r.delay[1] < r.delay[0]);
        if (!good) fails++;
        printf("%-50s %s %u rings (%.0f..%.0f), %u directions; side: %.1f / %.1f dB louder, %.0f / %.0f samples "
               "sooner%s\n", name, good ? "ok  " : "FAIL", s.n_rings, s.ring_el[0], s.ring_el[s.n_rings - 1], s.n_dirs,
               ll, rr, l.delay[1] - l.delay[0], r.delay[0] - r.delay[1], flat ? " (the same response everywhere)" : "");
        rl_hrtf_free(&s);
    }
    printf(fails ? "sofa tests: %d FAILED\n" : "sofa tests: all passed\n", fails);
    return fails != 0;
}
