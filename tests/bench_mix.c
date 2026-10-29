/* bench_mix - what the mixer's stages cost, block by block as the device runs them (src/device.c mix()):
 * voices of each kind through rl_voice_mix_n, then the EAX reverb.  48 kHz output, 10 ms blocks; each line is the
 * time per block and the share of the block's real time (one core).
 *
 *   bench_mix [seconds per case] [reAL_hrtf.bin]
 *
 * Built like the DLL (32-bit, SSE2, the same fast-math split) and run under Wine, whose arithmetic runs native. */
#include "../src/mixcore.h"
#include "../src/hrtf.h"
#include "../src/reverb.h"
#include "../src/speakers.h"
#include "../src/alrv.h"
#include "../src/cmss3.h"
#include "../src/virt.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RATE  48000
#define BLOCK 480

static unsigned s_seed = 12345;
static unsigned rnd(void) { s_seed = s_seed * 1103515245u + 12345u; return s_seed >> 8; }
static void nosig(void *e) { (void)e; }
static double now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static rl_hrtf_set s_hrtf;
static int s_have_hrtf;
static double s_secs = 2.0;

/* one sound: 2 s of noise-ish tone, looping */
static uint8_t *make_data(rl_sfmt sf, int ch, int rate, uint32_t *size)
{
    int bps = sf == RL_U8 ? 1 : sf == RL_S16 ? 2 : sf == RL_S24 ? 3 : 4;
    uint32_t frames = (uint32_t)rate * 2;
    *size = frames * ch * bps;
    uint8_t *d = malloc(*size);
    for (uint32_t i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) {
            float x = 0.5f * sinf((float)i * (0.03f + 0.01f * c)) + 0.1f * ((float)(rnd() % 1000) / 1000.0f - 0.5f);
            uint8_t *p = d + ((size_t)i * ch + c) * bps;
            switch (sf) {
            case RL_U8: *p = (uint8_t)(128 + x * 127); break;
            case RL_S16: { int16_t v = (int16_t)(x * 32767); memcpy(p, &v, 2); break; }
            case RL_S24: { int32_t v = (int32_t)(x * 8388607); p[0] = v; p[1] = v >> 8; p[2] = v >> 16; break; }
            case RL_S32: { int32_t v = (int32_t)(x * 2147483647.0); memcpy(p, &v, 4); break; }
            default: memcpy(p, &x, 4); break;
            }
        }
    return d;
}

typedef struct {
    const char *name;
    rl_sfmt sf; int ch, rate, interp;   /* source */
    int kind;                           /* 0 2D, 1 3D panned, 2 3D HRTF, 3 2D CMSS-3D */
    int send;                           /* feeds the reverb bus */
    float pitch;                        /* Doppler etc.: 0 = none */
    int lp;                             /* EAX occlusion HF filter on */
} vcase;

static double run(const vcase *c, int n, int nout, int reverb, double *rev_part)
{
    rl_voice *v = calloc(n, sizeof *v);
    uint8_t **data = calloc(n, sizeof *data);
    rl_hrtf_voice **hv = calloc(n, sizeof *hv);
    for (int i = 0; i < n; i++) {
        uint32_t size;
        data[i] = make_data(c->sf, c->ch, c->rate, &size);
        rl_voice *a = &v[i];
        a->data = data[i]; a->size = size; a->channels = c->ch;
        a->block = c->ch * (c->sf == RL_U8 ? 1 : c->sf == RL_S16 ? 2 : c->sf == RL_S24 ? 3 : 4);
        a->sfmt = c->sf; a->rate = c->rate; a->interp = c->interp; a->looping = 1;
        a->pitch = c->pitch ? c->pitch + 0.001f * (float)(i % 7) : 0.0f;
        a->lp_a = c->lp ? 0.4f : 1.0f;
        for (int k = 0; k < RL_SENDS; k++) a->send_lp_a[k] = c->lp ? 0.6f : 1.0f;
        a->gain[0] = 0.3f; a->gain[1] = 0.2f;
        if (c->send) a->send_gain[0] = 0.25f;
        a->pos = (uint64_t)(rnd() % (size / a->block)) << 32;
        if (c->kind == 2 && s_have_hrtf) {
            hv[i] = calloc(1, sizeof **hv);
            rl_hrtf_filter f;
            rl_hrtf_lookup(&s_hrtf, (float)(i * 37 % 360) - 180.0f, 0.0f, &f);
            rl_hrtf_voice_set(hv[i], &f);
            a->hrtf = hv[i]; a->hrtf_set = &s_hrtf;
            a->gain[0] = a->gain[1] = 0.3f;
        }
        if (c->kind == 3) {
            a->cmss = rl_cmss3_create(RATE, nout == 6 ? 0x3f : 0x3, nout == 2);
            for (int b = 0; b < 12; b++) a->cmss_idx[b] = nout == 6 && b < 6 ? b : -1;
            if (nout == 2) { a->cmss_virt = calloc(1, sizeof(rl_virt)); rl_virt_init(a->cmss_virt, RATE); }
        }
        if (nout == 6 && (c->kind == 1 || c->kind == 2)) {
            a->route_on = 1;
            for (int k = 0; k < 3; k++) for (int ch = 0; ch < 6; ch++) a->rt[k][ch] = 0.2f + 0.1f * k;
        }
        rl_voice_start(a);
    }
    float *out = calloc((size_t)BLOCK * nout, 4), *bus = calloc(BLOCK, 4);
    float *sends[RL_SENDS] = { bus };
    rl_reverb rv;
    rl_layout lay;
    rl_layout_init(&lay, nout == 6 ? 0x3f : 0x3);
    if (reverb) {
        rl_reverb_init(&rv, RATE);
        rl_reverb_set_eax1(&rv, 0, 0.5f, 1.49f, 0.5f);
    }
    int blocks = (int)(s_secs * RATE / BLOCK);
    double t_mix = 0, t_rev = 0;
    for (int b = 0; b < blocks; b++) {
        memset(out, 0, (size_t)BLOCK * nout * 4);
        memset(bus, 0, BLOCK * 4);
        double t0 = now();
        for (int i = 0; i < n; i++) rl_voice_mix_n(&v[i], out, nout, reverb ? sends : NULL, BLOCK, RATE, nosig);
        double t1 = now();
        if (reverb) rl_reverb_process_n(&rv, bus, out, &lay, BLOCK);
        double t2 = now();
        t_mix += t1 - t0; t_rev += t2 - t1;
    }
    if (reverb) rl_reverb_free(&rv);
    for (int i = 0; i < n; i++) {
        free(data[i]); free(hv[i]);
        if (v[i].cmss) rl_cmss3_destroy(v[i].cmss);
        if (v[i].cmss_virt) { rl_virt_free(v[i].cmss_virt); free(v[i].cmss_virt); }
    }
    free(v); free(data); free(hv); free(out); free(bus);
    if (rev_part) *rev_part = t_rev / blocks;
    return (t_mix + t_rev) / blocks;
}

static void line(const char *what, double per_block)
{
    printf("  %-52s %8.1f us/block  %6.2f%% of a core\n", what, per_block * 1e6, per_block / ((double)BLOCK / RATE) * 100.0);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc > 1) s_secs = atof(argv[1]);
    if (argc > 2) {
        FILE *f = fopen(argv[2], "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            void *d = malloc(n);
            if (fread(d, 1, n, f) == (size_t)n) s_have_hrtf = rl_hrtf_load(&s_hrtf, d, (uint32_t)n, RATE);
            fclose(f); free(d);
        }
    }
    rl_mix_prepare();
    printf("48 kHz, %d-frame blocks; HRTF %s\n", BLOCK, s_have_hrtf ? "loaded" : "absent (3D HRTF cases skipped)");

    static const vcase cases[] = {
        { "2D s16 stereo 44.1k sinc",           RL_S16, 2, 44100, RL_INTERP_SINC,   0, 0, 0, 0 },
        { "2D s16 stereo 44.1k linear",         RL_S16, 2, 44100, RL_INTERP_LINEAR, 0, 0, 0, 0 },
        { "2D s16 stereo 48k sinc (no resampling)", RL_S16, 2, 48000, RL_INTERP_SINC, 0, 0, 0, 0 },
        { "2D u8 mono 22.05k sinc",             RL_U8,  1, 22050, RL_INTERP_SINC,   0, 0, 0, 0 },
        { "2D u8 stereo 22.05k sinc",           RL_U8,  2, 22050, RL_INTERP_SINC,   0, 0, 0, 0 },
        { "2D float stereo 44.1k sinc",         RL_F32, 2, 44100, RL_INTERP_SINC,   0, 0, 0, 0 },
        { "2D s24 stereo 44.1k sinc", RL_S24, 2, 44100, RL_INTERP_SINC, 0, 0, 0, 0 },
        { "2D s32 mono 32k sinc",               RL_S32, 1, 32000, RL_INTERP_SINC,   0, 0, 0, 0 },
        { "3D s16 mono 22.05k sinc, Doppler",   RL_S16, 1, 22050, RL_INTERP_SINC,   1, 0, 1.03f, 0 },
        { "3D s16 mono 22.05k sinc, Doppler, EAX send+HF", RL_S16, 1, 22050, RL_INTERP_SINC, 1, 1, 1.03f, 1 },
        { "3D s16 mono 22.05k sinc, HRTF",      RL_S16, 1, 22050, RL_INTERP_SINC,   2, 0, 1.03f, 0 },
        { "3D s16 mono 22.05k sinc, HRTF, EAX send", RL_S16, 1, 22050, RL_INTERP_SINC, 2, 1, 1.03f, 1 },
        { "2D s16 stereo 44.1k sinc, CMSS-3D",  RL_S16, 2, 44100, RL_INTERP_SINC,   3, 0, 0, 0 },
    };
    for (int nout = 2; nout <= 6; nout += 4) {
        printf("\n%s output:\n", nout == 2 ? "stereo" : "5.1");
        for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
            const vcase *c = &cases[k];
            if (c->kind == 2 && !s_have_hrtf) continue;
            int n = c->kind == 3 ? 4 : 32;
            char what[128];
            snprintf(what, sizeof what, "%2d x %s", n, c->name);
            line(what, run(c, n, nout, c->send, NULL));
        }
        double rev;
        static const vcase silent = { "", RL_S16, 1, 48000, RL_INTERP_LINEAR, 1, 1, 0, 0 };
        run(&silent, 1, nout, 1, &rev);
        line("EAX reverb (one engine)", rev);
    }
    /* a heavy game scene: 64 3D voices with EAX sends + 8 2D streams + reverb */
    printf("\nscene, stereo output: 64 3D (EAX) + 8 2D streams + reverb\n");
    double a = run(&cases[9], 64, 2, 1, NULL), b = run(&cases[0], 8, 2, 0, NULL);
    line("total", a + b);
    if (s_have_hrtf) {
        double h = run(&cases[11], 64, 2, 1, NULL);
        line("total, the 3D voices on HRTF", h + b);
    }
    return 0;
}
