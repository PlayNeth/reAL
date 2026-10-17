/* The stereo virtualizer (src/virt.c) against ALchemy's channel converter run in the emulator
 * (tools/gen_virt_gt.py): headphones (mode 0, CMSS-3D's) and speakers (mode 3).
 *   test_virt tests/gt/virt_gt.bin */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/virt.h"

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "tests/gt/virt_gt.bin", "rb");
    if (!f) { printf("virt: no ground truth, skipped\n"); return 0; }
    uint32_t ncase, fails = 0;
    if (fread(&ncase, 4, 1, f) != 1) return 1;
    for (uint32_t k = 0; k < ncase; k++) {
        uint32_t h[5];
        if (fread(h, 4, 5, f) != 5) return 1;
        uint32_t rate = h[0], hp = h[1], chunk = h[2], n = h[3], keep = h[4];
        float *gt = malloc(2 * keep * 4), *x[4], *out = calloc(2 * n, 4), *tmp = malloc(2 * chunk * 4), *z = calloc(chunk, 4);
        if (fread(gt, 4, 2 * keep, f) != 2 * keep) return 1;
        unsigned seed = 1;
        for (int c = 0; c < 4; c++) x[c] = malloc(n * 4);
        for (uint32_t i = 0; i < n; i++)
            for (int c = 0; c < 4; c++) {
                seed = seed * 1103515245u + 12345u;
                x[c][i] = (float)((int)(seed >> 9 & 0xffff) - 32768) / 32768.0f * 0.25f;
            }
        rl_virt v;
        if (hp) rl_virt_init_hp(&v, rate); else rl_virt_init(&v, rate);
        /* longer blocks than ALchemy's 128: its block-wise early-reflection reads, as modelled */
        if (hp && chunk > 128) rl_virt_hp_alchemy_er(&v, 1);
        for (uint32_t s = 0; s < n; s += chunk) {
            float *ch[ALRV_NOUT] = { x[0] + s, x[1] + s, z, x[2] + s, x[3] + s, z, z, z };
            rl_virt_process(&v, ch, out + 2 * s, chunk, tmp);
        }
        double e = 0, p = 0;
        for (uint32_t i = 0; i < keep; i++)
            for (int c = 0; c < 2; c++) {
                double g = gt[c * keep + i], d = out[2 * (n - keep + i) + c] - g;
                e += d * d; p += g * g;
            }
        double db = 10 * log10(e / p + 1e-30);
        printf("virt: %5u Hz %-10s blocks of %3u: %.1f dB from ALchemy\n", rate, hp ? "headphones" : "speakers", chunk, db);
        if (db > -90.0) fails++;
        rl_virt_free(&v);
        for (int c = 0; c < 4; c++) free(x[c]);
        free(gt); free(out); free(tmp); free(z);
    }
    fclose(f);
    printf(fails ? "virt: FAILED\n" : "virt: all cases match\n");
    return fails != 0;
}
