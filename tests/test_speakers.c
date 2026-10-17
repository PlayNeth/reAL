/* speakers.c against ALchemy's own converter (fn 0x10036740) and 3D speaker gains (fn 0x10034900), run in the
 * emulator by tools/gen_speakers_gt.py:  build/test_speakers tests/gt/speakers_gt.txt */
#include "../src/speakers.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "tests/gt/speakers_gt.txt", "r");
    if (!f) { perror("ground truth"); return 2; }
    char line[4096];
    int nconv = 0, ng = 0, bad = 0;
    double worst = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *e;
        if (!strncmp(line, "conv ", 5)) {
            p += 5;
            unsigned im = strtoul(p, &e, 16), om = strtoul(e, &e, 16);
            rl_layout in, out;
            rl_layout_init(&in, im); rl_layout_init(&out, om);
            float m[RL_MAXOUT][RL_MAXOUT];
            int redirect;
            rl_convert_matrix(&in, &out, m, &redirect);
            for (uint32_t i = 0; i < in.n; i++)
                for (uint32_t o = 0; o < out.n; o++) {
                    double want = strtod(e, &e);
                    if (fabs(want - m[i][o]) > 1e-7) {
                        if (bad++ < 20) printf("conv %x -> %x: [%u][%u] %.9g, want %.9g\n", im, om, i, o, m[i][o], want);
                    }
                }
            nconv++;
        } else if (!strncmp(line, "g3d ", 4)) {
            p += 4;
            unsigned lay = strtoul(p, &e, 16), n = strtoul(e, &e, 10);
            float az = strtof(e, &e), el = strtof(e, &e);
            double dist = strtod(e, &e);
            float g[12];
            rl_speaker_gains3d(lay, n, az, el, dist, g);
            for (int k = 0; k < 12; k++) {
                double want = strtod(e, &e), d = fabs(want - g[k]);
                if (d > worst) worst = d;
                if (d > 2e-6) {
                    if (bad++ < 20) printf("g3d %x n%u az %.6g el %.6g dist %.6g: g[%d] %.9g, want %.9g\n", lay, n, az, el, dist, k, g[k], want);
                }
            }
            ng++;
        }
    }
    fclose(f);
    printf("speakers: %d converter cases, %d 3D cases, largest 3D difference %.3g, %d mismatches\n", nconv, ng, worst, bad);
    return bad != 0;
}
