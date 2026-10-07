/* Compare alrv_props_from_eax1() and alrv_compute_params() with ALchemy (records from re/gen_params.py).
 * usage: test_alrv_params params.bin [-v] */
#include "../src/alrv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct rec {
    int32_t env; float vol, decay, damp;      /* env < 0: random property set */
    uint8_t E[0x60];
    uint8_t P[0x15c];
} rec;
#pragma pack(pop)

static int close_enough(uint32_t a, uint32_t b, int isint, float tol)
{
    if (a == b) return 1;
    if (isint) return 0;
    float x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    return fabsf(x - y) <= tol * fmaxf(1e-6f, fabsf(y));
}

/* which parameter-block words are integers */
static int p_isint(unsigned off)
{
    return (off >= 0x64 && off < 0x88) || (off >= 0x9c && off < 0xac) || (off >= 0xc0 && off < 0xe0) || off == 0x10c;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_alrv_params params.bin [-v]\n"); return 2; }
    int verbose = argc > 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    rec r;
    int n = 0, bad_e = 0, bad_p = 0;
    int per_field[0x15c / 4] = { 0 };
    const float tol = 1e-4f;
    while (fread(&r, sizeof(r), 1, f) == 1) {
        n++;
        alrv_props want_e; memcpy(&want_e, r.E, sizeof(want_e));
        if (r.env >= 0) {
            alrv_props e; alrv_props_default(&e);
            alrv_props_from_eax1(&e, (uint32_t)r.env, r.vol, r.decay, r.damp);
            int bad = 0;
            for (unsigned i = 0; i < sizeof(alrv_props) / 4; i++) {
                uint32_t a, b; memcpy(&a, (char *)&e + 4 * i, 4); memcpy(&b, r.E + 4 * i, 4);
                if (!close_enough(a, b, 0, 1e-6f)) {
                    if (verbose || bad_e < 10) { float x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4);
                        printf("rec %d env %d: E+0x%02x got %.9g want %.9g\n", n, r.env, 4 * i, x, y); }
                    bad = 1;
                }
            }
            bad_e += bad;
        }
        alrv_params p;
        alrv_compute_params(&p, &want_e, 48000);
        int bad = 0;
        for (unsigned i = 0; i < sizeof(alrv_params) / 4; i++) {
            uint32_t a, b; memcpy(&a, (char *)&p + 4 * i, 4); memcpy(&b, r.P + 4 * i, 4);
            if (!close_enough(a, b, p_isint(4 * i), tol)) {
                per_field[i]++;
                if (verbose) {
                    float x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4);
                    if (p_isint(4 * i)) printf("rec %d: P+0x%03x got %d want %d\n", n, 4 * i, (int)a, (int)b);
                    else printf("rec %d: P+0x%03x got %.9g want %.9g\n", n, 4 * i, x, y);
                }
                bad = 1;
            }
        }
        bad_p += bad;
    }
    fclose(f);
    printf("%d records: property mismatches %d, parameter mismatches %d\n", n, bad_e, bad_p);
    for (unsigned i = 0; i < sizeof(alrv_params) / 4; i++)
        if (per_field[i]) printf("  P+0x%03x differs in %d records\n", 4 * i, per_field[i]);
    return bad_e || bad_p;
}
