/* Compare src/eax.c with ALchemy (records from re/gen_eax_gt.py, run in the emulator).
 * usage: test_eax eax.bin [-v] */
#include "../src/eax.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int verbose;

static int read_exact(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static int cmp_words(const char *what, int rec, const void *got, const void *want, size_t bytes, float tol)
{
    int bad = 0;
    for (size_t i = 0; i < bytes / 4; i++) {
        uint32_t a, b; float x, y;
        memcpy(&a, (const char *)got + 4 * i, 4); memcpy(&b, (const char *)want + 4 * i, 4);
        if (a == b) continue;
        memcpy(&x, &a, 4); memcpy(&y, &b, 4);
        if (tol > 0 && isfinite(x) && isfinite(y) && fabsf(x - y) <= tol * fmaxf(1e-30f, fabsf(y))) continue;
        if (verbose || bad == 0)
            printf("rec %d %s +0x%02zx: got %08x (%g / %d) want %08x (%g / %d)\n", rec, what, 4 * i, a, x, (int)a, b, y, (int)b);
        bad = 1;
    }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_eax eax.bin [-v]\n"); return 2; }
    verbose = argc > 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    int n[32] = { 0 }, bad[32] = { 0 }, rec = 0, badret = 0;
    uint32_t hdr[3];
    while (read_exact(f, hdr, 12)) {
        rec++;
        uint32_t type = hdr[0];
        if (type == 2 || type == 3) {
            struct { uint32_t id, r; uint8_t payload[0x70]; } ops[16];
            uint32_t nops = hdr[1];
            if (nops > 16 || !read_exact(f, ops, nops * sizeof(ops[0]))) return 3;
            uint8_t want_state[0xc0], want_props[0x60];
            if (!read_exact(f, want_state, 0xc0) || !read_exact(f, want_props, 0x60)) return 3;
            alrv_props e; alrv_props_default(&e);
            int b = 0;
            if (type == 3) {
                eax3_listener l; memset(&l, 0, sizeof(l));
                memcpy(&l.env_copy, want_state + 0x70, 4);
                eax3l_reset(&l);
                for (uint32_t i = 0; i < nops; i++) {
                    uint32_t len = ops[i].id == EAX3L_ALL ? 0x70 : (ops[i].id == EAX3L_REFLECTIONSPAN || ops[i].id == EAX3L_REVERBPAN) ? 12 : 4;
                    int r = eax3l_set(&l, ops[i].id, ops[i].payload, len) == EAX_OK;
                    if (r != (int)ops[i].r) {
                        printf("rec %d: EAX3 listener op %u (id %u) returned %d, ALchemy %u\n", rec, i, ops[i].id, r, ops[i].r);
                        badret++;
                    }
                }
                eax3l_apply(&l, &e);
                b |= cmp_words("eax3 state", rec, &l, want_state, sizeof(l), 0);
            } else {
                eax2_listener l;
                eax2l_reset(&l);
                for (uint32_t i = 0; i < nops; i++) {
                    uint32_t len = ops[i].id == EAX2L_ALL ? 0x38 : 4;
                    int r = eax2l_set(&l, ops[i].id, ops[i].payload, len) == EAX_OK;
                    if (r != (int)ops[i].r) {
                        printf("rec %d: EAX2 listener op %u (id %u) returned %d, ALchemy %u\n", rec, i, ops[i].id, r, ops[i].r);
                        badret++;
                    }
                }
                eax2l_apply(&l, &e);
                b |= cmp_words("eax2 state", rec, &l, want_state, sizeof(l), 0);
            }
            b |= cmp_words("props", rec, &e, want_props, sizeof(e), 0);
            if (b && verbose) {
                for (uint32_t i = 0; i < nops; i++) {
                    int32_t iv; float fv; memcpy(&iv, ops[i].payload, 4); memcpy(&fv, ops[i].payload, 4);
                    printf("    op id %u: %d / %g -> %u\n", ops[i].id, iv, fv, ops[i].r);
                }
            }
            n[type]++; bad[type] += b;
        } else if (type == 12 || type == 13) {
            uint8_t st[0xc0], bp[0x50];
            struct { float x, cf; uint32_t inner, outer; int32_t ov; float df, ro; } g;
            float want[4];
            if (!read_exact(f, st, 0xc0) || !read_exact(f, bp, 0x50) || !read_exact(f, &g, sizeof(g)) || !read_exact(f, want, 16)) return 3;
            eax_geom geo = { g.x, g.cf, g.inner, g.outer, g.ov, g.df, g.ro };
            eax_gains o;
            if (type == 13) {
                eax3_listener l; eax3_buffer bb;
                memcpy(&l, st, sizeof(l)); memcpy(&bb, bp, sizeof(bb));
                eax3_source_gains(&bb, &l, &geo, &o);
            } else {
                eax2_listener l; eax2_buffer bb;
                memcpy(&l, st, sizeof(l)); memcpy(&bb, bp, sizeof(bb));
                eax2_source_gains(&bb, &l, &geo, &o);
            }
            int b = cmp_words(type == 13 ? "eax3 source" : "eax2 source", rec, &o, want, 16, 0);
            if (b && verbose) printf("    geom x %g cf %g cone %u/%u ov %d df %g ro %g\n", g.x, g.cf, g.inner, g.outer, g.ov, g.df, g.ro);
            n[type]++; bad[type] += b;
        } else if (type == 20) {
            struct { float g, r; int32_t q; } c;
            if (!read_exact(f, &c, sizeof(c))) return 3;
            float a = eax_lp_coef(c.g, c.r);
            int32_t q = (int32_t)lrintf(a * 32768.0f);
            int b = q != c.q;
            if (b && (verbose || bad[20] < 5)) printf("rec %d: lp coef gain %g ratio %g: got %d want %d\n", rec, c.g, c.r, q, c.q);
            n[20]++; bad[20] += b;
        } else {
            printf("unknown record type %u\n", type);
            return 3;
        }
    }
    fclose(f);
    printf("EAX3 listener %d/%d, EAX2 listener %d/%d, EAX3 source %d/%d, EAX2 source %d/%d, lp coef %d/%d identical; "
           "%d setter results differ\n",
           n[3] - bad[3], n[3], n[2] - bad[2], n[2], n[13] - bad[13], n[13], n[12] - bad[12], n[12], n[20] - bad[20], n[20], badret);

    /* reAL fix: setting EAX 3 air absorption back to 0 lifts the DECAYHFLIMIT cap on the HF decay
     * (ALchemy ignores the change until another property is set) */
    int fixfail = 0;
    {
        eax3_listener l; alrv_props e;
        eax3l_reset(&l); alrv_props_default(&e);
        float decay = 20.0f, ratio = 2.0f, air = -100.0f, zero = 0.0f;
        eax3l_set(&l, EAX3L_DECAYTIME, &decay, 4); eax3l_set(&l, EAX3L_DECAYHFRATIO, &ratio, 4);
        eax3l_set(&l, EAX3L_AIRABSORPTIONHF, &air, 4);
        eax3l_apply(&l, &e);
        float capped = e.decay_hf;
        eax3l_set(&l, EAX3L_AIRABSORPTIONHF, &zero, 4);
        int changed = eax3l_apply(&l, &e);
        printf("air absorption -> 0: decay HF %.3f s -> %.3f s (want %.3f)\n", capped, e.decay_hf, ratio * decay);
        fixfail = !(capped < ratio * decay && changed && e.decay_hf == ratio * decay);
        if (fixfail) printf("  FAILED\n");
    }
    return (bad[2] || bad[3] || bad[12] || bad[13] || bad[20] || badret || fixfail) ? 1 : 0;
}
