/* Compare reAL's EAX 4 effect DSP with ALchemy (records from re/gen_fx_gt.py).
 * usage: test_fx <effect> <file> */
#include "../src/eax4dsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

typedef struct { const char *name; void *st; void (*init)(void *, uint32_t); void (*props)(void *, const uint32_t *);
                 void (*process)(void *, float *const ch[2], uint32_t); } fx_api;

static float U2F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static fx_distortion g_dist;
static void d_init(void *s, uint32_t r) { fxd_init(s, r); }
static void d_props(void *s, const uint32_t *p)
{ fxd_edge(s, U2F(p[0])); fxd_gain(s, (int32_t)p[1]); fxd_cutoff(s, U2F(p[2])); fxd_eq(s, U2F(p[3]), ((fx_distortion *)s)->bandwidth);
  fxd_eq(s, U2F(p[3]), U2F(p[4])); }
static void d_proc(void *s, float *const ch[2], uint32_t n) { fxd_process(s, ch, n); }

static fx_ringmod g_ring;
static void r_init(void *s, uint32_t r) { fxr_init(s, r); g_ring.alchemy_bug = 1; }
static void r_props(void *s, const uint32_t *p) { fxr_freq(s, U2F(p[0])); fxr_highpass(s, U2F(p[1])); fxr_waveform(s, p[2]); }
static void r_proc(void *s, float *const ch[2], uint32_t n) { fxr_process(s, ch, n); }

static fx_fshift g_fsh;
static void f_init(void *s, uint32_t r) { fxf_init(s, r); g_fsh.alchemy_bug = 1; }
static void f_props(void *s, const uint32_t *p) { fxf_freq(s, U2F(p[0])); fxf_dir(s, p[1], p[2]); }
static void f_proc(void *s, float *const ch[2], uint32_t n) { fxf_process(s, ch, n); }

static fx_agc g_agc;
static void a_init(void *s, uint32_t r) { fxa_init(s, r); g_agc.alchemy_bug = 1; }
static void a_props(void *s, const uint32_t *p) { fxa_onoff(s, p[0] != 0); }
static void a_proc(void *s, float *const ch[2], uint32_t n) { fxa_process(s, ch, n); }

static fx_eq g_eq;
static void e_init(void *s, uint32_t r) { fxe_init(s, r); }
static void e_props(void *s, const uint32_t *p)
{
    uint32_t d = p[10];
    if (d & 1) fxe_gain(s, 0, (int32_t)p[0]);
    if (d & 2) fxe_freq(s, 0, U2F(p[1]));
    if (d & 4) fxe_gain(s, 1, (int32_t)p[2]);
    if (d & 8) fxe_freq(s, 1, U2F(p[3]));
    if (d & 16) fxe_width(s, 1, U2F(p[4]));
    if (d & 32) fxe_gain(s, 2, (int32_t)p[5]);
    if (d & 64) fxe_freq(s, 2, U2F(p[6]));
    if (d & 128) fxe_width(s, 2, U2F(p[7]));
    if (d & 256) fxe_gain(s, 3, (int32_t)p[8]);
    if (d & 512) fxe_freq(s, 3, U2F(p[9]));
}
static void e_proc(void *s, float *const ch[2], uint32_t n) { fxe_process(s, ch, n); }

static fx_autowah g_aw;
static void w_init(void *s, uint32_t r) { fxw_init(s, r); }
static void w_props(void *s, const uint32_t *p)
{
    if (p[4] & 1) fxw_attack(s, U2F(p[0]));
    if (p[4] & 2) fxw_release(s, U2F(p[1]));
    if (p[4] & 4) fxw_resonance(s, (int32_t)p[2]);
    if (p[4] & 8) fxw_peak(s, (int32_t)p[3]);
}
static void w_proc(void *s, float *const ch[2], uint32_t n) { fxw_process(s, ch, n); }

static fx_echo g_echo;
static void o_init(void *s, uint32_t r) { fxo_free(s); fxo_init(s, r); }
static void o_props(void *s, const uint32_t *p)
{
    if (p[5] & 1) fxo_delay(s, U2F(p[0]));
    if (p[5] & 2) fxo_lrdelay(s, U2F(p[1]));
    if (p[5] & 4) fxo_damping(s, U2F(p[2]));
    if (p[5] & 8) fxo_feedback(s, U2F(p[3]));
    if (p[5] & 16) fxo_spread(s, U2F(p[4]));
}
static void o_proc(void *s, float *const ch[2], uint32_t n) { fxo_process(s, ch, n); }

static fx_chorus g_ch;
static void c_init(void *s, uint32_t r) { fxc_init(s, r); }
static void c_props(void *s, const uint32_t *p)
{
    if (p[6] & 1) fxc_waveform(s, p[0]);
    if (p[6] & 2) fxc_phase(s, (int32_t)p[1]);
    if (p[6] & 4) fxc_rate(s, U2F(p[2]));
    if (p[6] & 8) fxc_depth(s, U2F(p[3]));
    if (p[6] & 16) fxc_feedback(s, U2F(p[4]));
    if (p[6] & 32) fxc_delay(s, U2F(p[5]));
}
static void c_proc(void *s, float *const ch[2], uint32_t n) { fxc_process(s, ch, n); }

static fx_flanger g_fl;
static void l_init(void *s, uint32_t r) { fxl_init(s, r); }
static void l_props(void *s, const uint32_t *p)
{
    if (p[6] & 1) fxl_waveform(s, p[0]);
    if (p[6] & 2) fxl_phase(s, (int32_t)p[1]);
    if (p[6] & 4) fxl_rate(s, U2F(p[2]));
    if (p[6] & 8) fxl_depth(s, U2F(p[3]));
    if (p[6] & 16) fxl_feedback(s, U2F(p[4]));
    if (p[6] & 32) fxl_delay(s, U2F(p[5]));
}
static void l_proc(void *s, float *const ch[2], uint32_t n) { fxl_process(s, ch, n); }

static fx_pshift g_ps;
static void p_init(void *s, uint32_t r) { fxp_init(s, r, 1); }
static void p_props(void *s, const uint32_t *p)
{
    fx_pshift *x = s;
    fxp_set(s, p[2] & 1 ? (int32_t)p[0] : x->semis, p[2] & 2 ? (int32_t)p[1] : x->cents);
}
static void p_proc(void *s, float *const ch[2], uint32_t n) { fxp_process(s, ch, n); }

static fx_vmorph g_vm;
static void v_init(void *s, uint32_t r) { fxv_init(s, r, 1); fxv_reset(s); }
static void v_props(void *s, const uint32_t *p)
{
    if (p[6] & 1) fxv_phoneme_a(s, p[0]);
    if (p[6] & 2) fxv_coarse_a(s, (int32_t)p[1]);
    if (p[6] & 4) fxv_phoneme_b(s, p[2]);
    if (p[6] & 8) fxv_coarse_b(s, (int32_t)p[3]);
    if (p[6] & 16) fxv_waveform(s, p[4]);
    if (p[6] & 32) fxv_rate(s, U2F(p[5]));
}
static void v_proc(void *s, float *const ch[2], uint32_t n) { fxv_process(s, ch, n); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_fx effect file\n"); return 2; }
    fx_api fx[] = { { "distortion", &g_dist, d_init, d_props, d_proc },
                    { "ringmod", &g_ring, r_init, r_props, r_proc },
                    { "fshift", &g_fsh, f_init, f_props, f_proc },
                    { "agc", &g_agc, a_init, a_props, a_proc },
                    { "eq", &g_eq, e_init, e_props, e_proc },
                    { "autowah", &g_aw, w_init, w_props, w_proc },
                    { "echo", &g_echo, o_init, o_props, o_proc },
                    { "chorus", &g_ch, c_init, c_props, c_proc },
                    { "flanger", &g_fl, l_init, l_props, l_proc },
                    { "pshift", &g_ps, p_init, p_props, p_proc },
                    { "vmorph", &g_vm, v_init, v_props, v_proc } };
    fx_api *a = NULL;
    for (size_t i = 0; i < sizeof(fx) / sizeof(fx[0]); i++) if (!strcmp(fx[i].name, argv[1])) a = &fx[i];
    if (!a) { fprintf(stderr, "unknown effect\n"); return 2; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
    uint32_t kind; double eref = 0, eerr = 0, worst = 1e9; int cases = 0, blocks = 0;
    double ce = 0, cr = 0; int nfirst = 0;
    static float in[2][4096], want[2][4096];
    while (rd(f, &kind, 4)) {
        if (kind == 0) {
            uint32_t rate; rd(f, &rate, 4);
            if (cases && cr > 0) { double s = 10 * log10(cr / (ce + 1e-30)); if (s < worst) worst = s; }
            ce = cr = 0; cases++;
            a->init(a->st, rate);
        } else if (kind == 1) {
            uint32_t p[16]; rd(f, p, 64); a->props(a->st, p);
        } else {
            uint32_t n; rd(f, &n, 4);
            rd(f, in[0], 4 * n); rd(f, in[1], 4 * n); rd(f, want[0], 4 * n); rd(f, want[1], 4 * n);
            float *ch[2] = { in[0], in[1] };
            float keep[2][8]; memcpy(keep[0], in[0], 32); memcpy(keep[1], in[1], 32);
            a->process(a->st, ch, n);
            if (getenv("FXDBG") && blocks < atoi(getenv("FXDBG")))
                for (int i = 0; i < 4; i++) printf("  blk %d i %d in %g -> got %g want %g\n", blocks, i, keep[0][i], in[0][i], want[0][i]);
            for (int c = 0; c < 2; c++) for (uint32_t i = 0; i < n; i++) {
                double d = (double)in[c][i] - want[c][i];
                eref += (double)want[c][i] * want[c][i]; eerr += d * d;
                cr += (double)want[c][i] * want[c][i]; ce += d * d;
            }
            if (getenv("FXFIRST")) {
                int shown = 0;
                for (uint32_t i = 0; i < n && shown < 3; i++) if (in[0][i] != want[0][i] || in[1][i] != want[1][i]) {
                    printf("  case %d blk %d i %u: got %.9g %.9g want %.9g %.9g\n", cases, blocks, i, in[0][i], in[1][i], want[0][i], want[1][i]);
                    shown++;
                }
                if (shown && atoi(getenv("FXFIRST")) <= ++nfirst) exit(1);
            }
            blocks++;
        }
    }
    if (cr > 0) { double s = 10 * log10(cr / (ce + 1e-30)); if (s < worst) worst = s; }
    double snr = 10 * log10(eref / (eerr + 1e-30));
    printf("%s: %d cases, %d blocks: SNR %.1f dB (worst case %.1f dB)%s\n", a->name, cases, blocks, snr, worst,
           worst < 60 ? "  <-- BAD" : "");
    return worst < 60;
}
