/* test_wavesrev <in.raw> <ref.raw> <rate> <channels> [ingain mix time hfratio] [chunk]: run reAL's Waves reverb
 * port on 16-bit PCM and count samples that differ from dsdmo.dll's output (tools/dmgt: dmgt wavesrev) */
#include "../src/music/wavesrev.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    FILE *f = fopen(argv[1], "rb"), *g = fopen(argv[2], "rb");
    if (!f || !g) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET);
    int16_t *a = malloc(n * 2), *r = malloc(n * 2);
    if (fread(a, 2, n, f) != (size_t)n || fread(r, 2, n, g) != (size_t)n) return 1;
    int ch = atoi(argv[4]);
    wavesrev *w = wavesrev_new(atoi(argv[3]));
    if (argc > 8 && wavesrev_set(w, atof(argv[5]), atof(argv[6]), atof(argv[7]), atof(argv[8])) < 0) { fprintf(stderr, "bad params\n"); return 1; }
    long chunk = (argc > 9 ? atol(argv[9]) : 441) * ch;
    for (long o = 0; o < n; o += chunk) {
        long m = n - o < chunk ? n - o : chunk;
        if (ch == 2) wavesrev_stereo16(w, a + o, m / 2); else wavesrev_mono16(w, a + o, m);
    }
    long bad = 0, first = -1; int maxd = 0;
    for (long i = 0; i < n; i++) if (a[i] != r[i]) { bad++; if (first < 0) first = i; int d = abs(a[i] - r[i]); if (d > maxd) maxd = d; }
    printf("%ld samples, %ld differ (first %ld, max diff %d)\n", n, bad, first, maxd);
    return bad != 0;
}
