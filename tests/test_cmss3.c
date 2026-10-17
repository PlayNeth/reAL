/* CMSS-3D upmixer (src/cmss3.c) against ALchemy's CCMSS3Upmix run in the emulator (tools/gen_cmss3_gt.py).
 *   test_cmss3 tests/gt/cmss3_gt.bin */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/cmss3.h"

static unsigned seed;
static float rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return (float)((int)(seed >> 9 & 0xffff) - 32768) / 32768.0f;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "tests/gt/cmss3_gt.bin", "rb");
    if (!f) { printf("cmss3: no ground truth, skipped\n"); return 0; }
    uint32_t ncase;
    if (fread(&ncase, 4, 1, f) != 1) return 1;
    int fails = 0;
    for (uint32_t k = 0; k < ncase; k++) {
        int32_t hdr[3]; uint32_t s0, count, nbits;
        if (fread(hdr, 4, 3, f) != 3 || fread(&s0, 4, 1, f) != 1 || fread(&count, 4, 1, f) != 1 ||
            fread(&nbits, 4, 1, f) != 1) return 1;
        uint32_t rate = (uint32_t)hdr[0], mask = (uint32_t)hdr[1] & 0x7fffffff;
        int hp = (uint32_t)hdr[1] >> 31;
        int inten = hdr[2];
        uint32_t n = s0 + count;
        float *l = malloc(n * 4), *r = malloc(n * 4), *o[12] = { 0 };
        seed = 1;
        for (uint32_t i = 0; i < n; i++) {
            float c = rnd(), a = rnd(), b = rnd();
            l[i] = 0.35f * c + 0.25f * a;
            r[i] = 0.35f * c + 0.25f * b;
        }
        for (int b = 0; b < 12; b++) o[b] = calloc(n, 4);
        rl_cmss3 *c = rl_cmss3_create(rate, mask, mask == 0x3 && !hp);   /* stereo: the quad, rear allpasses on speakers */
        rl_cmss3_set_intensity(c, inten);
        /* uneven chunks, as a mixer would call it */
        for (uint32_t p = 0, step = 1; p < n; step = step * 7 % 997 + 64) {
            uint32_t m = n - p < step ? n - p : step;
            float *op[12];
            for (int b = 0; b < 12; b++) op[b] = o[b] + p;
            rl_cmss3_process(c, l + p, r + p, op, m);
            p += m;
        }
        double worst = 0.0;
        for (uint32_t j = 0; j < nbits; j++) {
            uint32_t bit;
            float *gt = malloc(count * 4);
            if (fread(&bit, 4, 1, f) != 1 || fread(gt, 4, count, f) != count) return 1;
            double e = 0, s = 0;
            for (uint32_t i = 0; i < count; i++) {
                double d = (double)o[bit][s0 + i] - gt[i];
                e += d * d; s += (double)gt[i] * gt[i];
            }
            double db = 10.0 * log10(e / (s + 1e-30) + 1e-30);
            if (j == 0 || db > worst) worst = db;
            if (db > -70.0) { printf("cmss3: %u Hz mask %03x intensity %d: speaker bit %u at %.1f dB\n", rate, mask, inten, bit, db); fails++; }
            free(gt);
        }
        printf("cmss3: %6u Hz mask %03x%s intensity %4d: worst speaker %.1f dB from ALchemy\n", rate, mask, hp ? " (headphones)" : "", inten, worst);
        rl_cmss3_destroy(c);
        for (int b = 0; b < 12; b++) free(o[b]);
        free(l); free(r);
    }
    fclose(f);
    printf(fails ? "cmss3: FAILED\n" : "cmss3: all cases match\n");
    return fails != 0;
}
