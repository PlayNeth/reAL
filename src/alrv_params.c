/* reAL - port of ALchemy's reverb parameter computation (CHRvbParamUpdate, fn 0x10060c50) and of its
 * EAX 1.0 -> property conversion (fn 0x1004a050).  See alrv.h.
 *
 * ALchemy evaluates these in x87 code with 53-bit precision and stores most intermediates as floats;
 * the float/double split below follows those stores.  All float->int conversions truncate. */
#include "alrv.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ properties */

void alrv_props_default(alrv_props *e)      /* fn 0x10043f40 */
{
    memset(e, 0, sizeof(*e));
    e->size = 1.0f;
    e->diffusion = 1.0f;
    e->room = 0.3162277638912201f;
    e->room_hf = 0.8912509083747864f;
    e->room_lf = 1.0f;
    e->decay = 1.49f;
    e->decay_hf = 1.2366999387741089f;
    e->decay_lf = 1.49f;
    e->reflections = 0.050003454089164734f;
    e->reflections_delay = 0.007f;
    e->reflections_pan[1] = 1.0f;
    e->reverb = 1.258925437927246f;
    e->reverb_delay = 0.011f;
    e->reverb_pan[1] = 1.0f;
    e->echo_time = 0.25f;
    e->mod_time = 0.25f;
    e->hf_ref = 5000.0f;
    e->lf_ref = 250.0f;
}

/* EAX 3 environment presets as ALchemy stores them (.rdata 0x100a8280, 0x70 bytes each); only the
 * fields the EAX 1.0 path uses */
typedef struct preset {
    float size, diffusion;
    int   room_lf;
    float lf_ratio;
    int   reflections;
    float reflections_delay;
    int   reverb;
    float reverb_delay;
    float echo_time, echo_depth, mod_time, mod_depth, hf_ref, lf_ref;
} preset;

#include "alrv_presets.h"   /* k_presets[26], k_damp1[26], k_damp2[26] */

static float mb_to_lin(int mb) { return mb < -9999 ? 0.0f : (float)pow(10.0, (double)(float)((float)mb * 0.0005f)); }

void alrv_props_from_eax1(alrv_props *e, uint32_t env, float volume, float decay, float damping)
{
    if (env > 25) env = 25;
    const preset *p = &k_presets[env];
    /* environment (fn 0x10044290 etc.) */
    float s = (float)pow(p->size, 3.0) * 0.0625f;
    e->size = s > 1.0f ? 1.0f : s;
    e->diffusion = p->diffusion;
    e->room_lf = mb_to_lin(p->room_lf);
    e->reflections = mb_to_lin(p->reflections + 300);
    e->reflections_delay = p->reflections_delay;
    e->reverb = mb_to_lin(p->reverb + 300);
    e->reverb_delay = p->reverb_delay;
    e->echo_time = p->echo_time;
    e->echo_depth = env == 24 ? 0.0f : p->echo_depth;
    e->mod_time = p->mod_time;
    e->mod_depth = p->mod_depth;
    e->hf_ref = p->hf_ref;
    e->lf_ref = p->lf_ref;
    /* volume and decay */
    e->room = volume;
    e->decay = decay;
    e->decay_lf = p->lf_ratio * decay;
    /* damping -> decay HF ratio and room HF */
    float t = damping;
    if (!(damping > 1.0f) && damping != 1.0f) {   /* damping < 1 (or NaN) */
        float d = damping > 1.1920928955078125e-07f ? damping : 1.1920928955078125e-07f;   /* 0x100b56c0 */
        t = 1.0f / (1.0f - (1.0f - 1.0f / d) * k_damp1[env]);
        if (t > 2.0f) t = 2.0f;                        /* 0x100a7dac */
        else if (t < 0.1f) t = 0.1f;                   /* 0x100a7da0 */
    }
    e->decay_hf = t * decay;
    int hf = (int)lrint((double)((1.0f / t - 1.0f) * k_damp2[env]) + 1.4999999464748726e-08);
    if (hf > 0) hf = 0;
    if (hf < -10000) hf = -10000;
    e->room_hf = mb_to_lin(hf);
}

/* ------------------------------------------------------------------ parameter computation */

typedef struct pu_state {      /* the CHRvbParamUpdate object's scratch fields */
    float norm;                /* +0x04 */
    float cos2;                /* +0x08 */
    float gain[8];             /* +0x0c */
    float delay[8];            /* +0x38 seconds: 4 FDN lines, 4 allpasses */
    float tan;                 /* +0x64 */
} pu_state;

static int32_t trunc_i(double v) { return (int32_t)v; }   /* FPU in chop mode */
static int32_t round8(double v) { return (trunc_i(v) + 7) & ~7; }

/* EAX 3 reflections / reverb panning (fn 0x10060250, callers 0x10060130 / 0x100601c0).
 * pan[0]: direction in degrees, pan[1]: 1 = all around .. 0 = fully directional.  A power mix of
 *   - an even part (the "all around" share),
 *   - 12 virtual sources 30 degrees apart around the direction, each fading out from 30 to 60 degrees off it,
 *     folded onto the outputs,
 *   - plain pairwise panning between output angles 0/30/90/150/180/210/270/330,
 * each normalised, then out = sqrt(power) * scale.  Output slots as in the parameter block (1..8). */
static void calc_pan(float out[9], const float pan[2], double scale)
{
    static const float tbl[10] = { 0, 0, 30, 90, 150, 180, 210, 270, 330, 360 };
    float near[8] = { 0 }, far[8] = { 0 }, w[12];
    int ang = trunc_i((double)pan[0] + 360.0) % 360;
    float c = (float)ang;
    float f = (float)(1.0 - (double)pan[1]);
    float a1 = (float)(1.0 - (double)f / 0.7070000171661377);
    if (!(a1 > 0.0f)) a1 = 0.0f;
    double D = a1;
    float g = (float)(1.0 - D);
    float b = (float)((double)f - 0.7070000171661377);
    if (!(b > 0.0f)) b = 0.0f;
    float q = (float)((double)b / 0.2930000126361847);
    float r = (float)(1.0 - (double)q);
    if (!(r > 0.0f)) r = 0.0f;
    float W1 = (float)((double)q * g), W2 = (float)((double)g * r);

    int hi = 0, lo = 0;
    for (int k = 2; k < 10; k++)
        if (!(tbl[k] < c)) { hi = k; lo = k - 1; break; }

    float sum_near = 0.0f, sum_far = 0.0f;
    for (int i = 0; i < 12; i++) {
        float d = fabsf((float)((double)c - (double)i * 30.0));
        float e = (float)(360.0 - d);
        if (!(e > d)) d = e;
        if (d < 30.0f) w[i] = 1.0f;
        else if (d < 60.0f) w[i] = (float)(1.0 - ((double)d - 30.0) / 30.0);
        else w[i] = 0.0f;
        switch (i) {      /* the 12 virtual sources onto the 8 outputs */
        case 0:  near[0] += w[i]; break;
        case 1:  near[1] += w[i]; break;
        case 2:  near[1] += w[i] * 0.5f; near[2] += w[i] * 0.5f; break;
        case 3:  near[2] += w[i]; break;
        case 4:  near[2] += w[i] * 0.5f; near[3] += w[i] * 0.5f; break;
        case 5:  near[3] += w[i]; break;
        case 6:  near[4] += w[i]; break;
        case 7:  near[5] += w[i]; break;
        case 8:  near[5] += w[i] * 0.5f; near[6] += w[i] * 0.5f; break;
        case 9:  near[6] += w[i]; break;
        case 10: near[6] += w[i] * 0.5f; near[7] += w[i] * 0.5f; break;
        default: near[7] += w[i]; break;
        }
        sum_near += w[i];
    }
    for (int m = 1; m < 9; m++) {   /* far[m-1] is the output at tbl[m] */
        if (m == hi) far[m - 1] = (float)(((double)c - tbl[m - 1]) / ((double)tbl[m] - tbl[m - 1]));
        else if (m == lo) far[m - 1] = (float)(((double)tbl[m + 1] - c) / ((double)tbl[m + 1] - tbl[m]));
        sum_far += far[m - 1];
    }
    if (hi == 9) {                  /* between 330 and 360: the other side is the 0 degree output */
        if (c == 0.0f) c = 360.0f;
        far[0] = (float)(((double)c - 330.0) / 30.0);
        sum_far += far[0];
    }
    if (!(sum_near > 0.0f)) sum_near = 1.0f;
    double kn = (double)W2 / sum_near, kf = (double)W1 / sum_far;
    double even = (double)(float)(D * 0.125);
    out[0] = 0.0f;
    for (int i = 0; i < 8; i++) {
        float n = (float)(near[i] * kn), fa = (float)(far[i] * kf);
        float pw = (float)(((double)n + even) + fa);
        out[1 + i] = (float)((double)(float)sqrt((double)pw) * scale);
    }
}

/* reflections delay and late-reverb feed taps (fn 0x1005fcc0) */
static void calc_feed(alrv_params *P, const alrv_props *E, double fs)
{
    int32_t rd = trunc_i((double)E->reflections_delay * fs);
    if ((uint32_t)rd < 8) rd = 8;
    float v = E->reverb_delay;
    if (v > 0.019999999552965164f) v = (float)((v - 0.019999999552965164) * 0.8500000238418579 + 0.019999999552965164);
    if (v < 0.0024999999441206455f) v = 0.0024999999441206455f;
    int32_t ld = trunc_i((double)v * fs);
    if ((uint32_t)ld < 8) ld = 8;
    int32_t base = ld + rd;
    P->refl_delay = (rd + 7) & ~7;
    float k = 1.0f - E->echo_depth * 0.9900000095367432f;
    static const double off[4] = { -0.0, -23.170000076293945, -51.099998474121094, -75.69999694824219 };
    for (int i = 0; i < 4; i++)
        P->late_tap_delay[i] = (base - trunc_i(((double)(float)(fs * k) * off[i]) / 1000.0) + 7) & ~7;
}

/* early allpasses (fns 0x1005f190, 0x1005f330) */
static void calc_early(alrv_params *P, const alrv_props *E, double fs)
{
    float c = E->reverb_delay / 0.05f;
    float lc = 1.0f;
    if (c <= 1.0f) { lc = c; if (c < 0.6f) lc = 0.6f; }
    static const double us[4] = { 6910.0, 10040.0, 12630.0, 27826.0 };
    for (int i = 0; i < 4; i++) {
        P->eap_delay[i] = (int32_t)((7 - trunc_i(((us[i] * lc + 1000.0) * fs) / -1000000.0)) & ~7);
        P->eap_gain[i] = 1.0f;
    }
    float r = E->reverb_delay / 0.05f;
    if (r > 1.0f) P->eap_coef = (float)(0.4142135679721832 * 1.0);
    else {
        if (r < 1.0f) r = (float)(r * 0.4000000059604645 + 0.6000000238418579);
        P->eap_coef = (float)(r * 0.4142135679721832);
    }
}

static void sort4(float *d)       /* fn 0x1005fbf0: insertion sort, ascending */
{
    for (int i = 1; i < 4; i++) {
        float v = d[i]; int j = i - 1;
        while (j >= 0 && v < d[j]) { d[j + 1] = d[j]; j--; }
        d[j + 1] = v;
    }
}

/* FDN and allpass delay lengths from size / echo (fn 0x1005f5f0) */
static void calc_delays(pu_state *pu, alrv_params *P, const alrv_props *E, double fs)
{
    static const float cap[4] = { 0.23499999940395355f, 0.2409999966621399f, 0.24500000476837158f, 0.3411458134651184f };
    float size = E->size, fsz;
    if (!(size < 0.800000011920929f)) fsz = 0.8f;
    else if (!(0.0625f < size)) fsz = 0.0625f;
    else fsz = size;
    float echo = E->echo_depth;
    float *d = pu->delay;
    d[0] = (float)((fsz / 0.800000011920929) * 0.07936637848615646);
    float et = 0.0f, total;
    if (echo != 0.0f) {
        et = E->echo_time;
        float a = (float)(1.284000039100647 - 1.215999960899353 * et);
        float b = (float)(0.16500000655651093 - et * 1.3200000524520874);
        float t2 = (float)((double)echo * a + b);
        if (t2 > 1.0f) t2 = 1.0f; else if (t2 < 0.0f) t2 = 0.0f;
        float l2 = (float)log(0.20000000298023224), l5 = (float)log(0.5);
        float ex = l2 / l5;
        float t6 = (float)pow(t2, ex);
        float t7 = (float)(t6 * (0.890999972820282 * et) + (1.0 - t6) * (0.6000000238418579 * et));
        d[0] = t7;
        if (fabsf(t7 - et) <= 0.006f) d[0] = (float)(et - 0.006000000052154064);
        total = (float)(et * 4.0);
    } else {
        total = fsz;
    }
    /* d0*(1 + x + x^2 + x^3) = total, solved with Cardano */
    float t11 = (float)(1.0 - (double)total / d[0]);   /* x87: the division is not rounded to float */
    float q = (float)((t11 * 27.0 - 7.0) / 54.0);
    float s = sqrtf((float)((double)q * q - -0.010973937145149432));
    float u = (float)(s + (double)q);
    float p = -(float)pow(u, 0.3333333432674408);
    float v = (float)(-0.2222222238779068 / p);
    float x = (float)((p + v) - 0.3333333333333333);
    d[1] = d[0] * x;
    d[2] = x * d[1];
    d[3] = x * d[2];
    if (echo != 0.0f) {
        float lim = (float)((1.0f - echo) * fsz - et * 0.5);
        if (total < lim) {
            d[3] = d[3] + et;
            total = total + et;
            if (total < lim) {
                float step = et + et, acc = d[3];
                do { d[2] = d[2] + et; acc = acc + et; d[3] = acc; total = step + total; } while (total < lim);
            }
        }
    }
    sort4(d);
    for (int k = 2; k >= 1; k--) {          /* lines over their cap hand the excess to the shorter ones */
        int i = k + 1;
        if (cap[i] < d[i]) {
            float ex = d[i] - cap[i];
            d[i] = cap[i];
            for (int j = 1; j <= k; j++) d[j] = (float)(d[j] + ex / (double)k);
        }
    }
    sort4(d);
    if (fs != 0.0)
        for (int i = 0; i < 4; i++) if (cap[i] < d[i]) d[i] = cap[i];
    d[1] = (float)(d[1] - 0.006000000052154064);
    calc_feed(P, E, fs);
    static const float apd[4] = { 0.0024399999529123306f, 0.006089999806135893f, 0.009209999814629555f, 0.021929999813437462f };
    for (int i = 0; i < 4; i++) {
        d[4 + i] = apd[i];
        P->lap_delay[i] = (trunc_i(fs * apd[i]) + 7) & ~7;
    }
    if (echo > 0.0f)
        for (int i = 0; i < 4; i++) d[i] = d[i] - d[4 + i];
    for (int i = 0; i < 4; i++) P->fdn_delay[i] = round8(fs * d[i]);
}

/* decay: per-line gains, LF shelves, HF damping inputs, energy normalisation (fn 0x1005ed10) */
static void calc_decay(pu_state *pu, alrv_params *P, const alrv_props *E, double fs)
{
    double decay = E->decay;
    float hf_ratio = (float)(E->decay_hf / decay);
    float acc_l = 0.0f, acc_a = 0.0f;
    for (int i = 0; i < 8; i++) {
        float d = pu->delay[i];
        if (i == 1) d = (float)(d + 0.006000000052154064);
        double t2 = (d * -60.0) / decay;                 /* dB per pass at mid frequencies */
        if (i < 4) {
            float tot = d + pu->delay[i + 4];
            double t4 = (-60.0 * tot) / (double)E->decay_lf;
            float g;
            P->line_shelf_f[i] = E->lf_ref / (float)fs;
            if (t4 > t2) {                                /* LF decays slower: boost LF */
                pu->gain[i] = (float)pow(10.0, (t4 + t4 - t2) / 20.0);
                g = (float)pow(10.0, t2 / 20.0);
                P->line_shelf_g[i] = (float)pow(10.0, ((t2 - t4) + (t2 - t4)) / 20.0);
                P->line_shelf_high[i] = 1;
            } else {
                g = (float)pow(10.0, t2 / 20.0);
                pu->gain[i] = g;
                P->line_shelf_g[i] = (float)pow(10.0, ((t4 - t2) + (t4 - t2)) / 20.0);
                P->line_shelf_high[i] = 0;
            }
            acc_l = g * g + acc_l;
            P->line_g[i] = g;
            P->line_hf_f[i] = E->hf_ref / (float)fs;
            P->line_hf_ratio[i] = hf_ratio;
        } else {
            float g = (float)pow(10.0, t2 / 20.0);
            pu->gain[i] = g;
            acc_a = g * g + acc_a;
        }
    }
    float qa = (float)(acc_l * 0.25), qb = (float)(acc_a * 0.25);
    pu->norm = qb == 0.0f ? 1.0f : sqrtf((float)((1.0 - (double)qa * qb) / qb));
    float t = E->reverb * P->room;
    t = (float)(t * 0.10000000149011612);
    t = (float)(t * (pu->norm * 0.5));
    t = t * pu->cos2;
    for (int i = 0; i < 4; i++) P->late_tap_gain[i] = t;
}

void alrv_compute_params(alrv_params *P, const alrv_props *E, uint32_t rate)
{
    pu_state pu;
    double fs = (double)rate;
    memset(P, 0, sizeof(*P));
    memset(&pu, 0, sizeof(pu));

    P->room = E->room;
    P->in_lp_gain = E->room_hf;
    P->in_lp_f = E->hf_ref / (float)fs;
    P->in_hp_gain = E->room_lf;
    P->in_hp_f = E->lf_ref / (float)fs;

    /* diffusion -> rotation angle (fn 0x1005ff30) */
    float dif = E->diffusion;
    if (!(dif < 1.0f)) dif = 1.0f; else if (!(0.0f < dif)) dif = 0.0f;
    float d8 = (float)pow(dif, 8.0);
    float th = (float)((float)(d8 * 0.5 + 0.5 * dif) * 3.1415927410125732 * 0.25);
    pu.cos2 = (float)pow((float)cos(th), 2.0);
    pu.tan = (float)tan(th);

    P->refl_gain = (float)(E->reflections * P->room * 0.10000000149011612);
    calc_early(P, E, fs);

    /* modulation (fns 0x100608a0, 0x100608f0) */
    P->mod_freq = (float)((float)sin((float)(3.1415927410125732 / ((float)fs * E->mod_time))) * 2.0);
    {
        float f = 1.0f / E->mod_time;
        float c = f > 0.25f ? f : 0.25f;
        float k = (float)(E->mod_depth * 4.0 / c);
        if (!(k < 1.0f)) k = 1.0f;
        P->mod_depth = trunc_i(((float)fs * 0.012000000104308128 * 0.5 - 4.0) * k);
    }

    /* panning of the reflections and the late reverb over the 8 outputs (EAX 3 pans) */
    calc_pan(P->early_out, E->reflections_pan, 0.45649999380111694);
    calc_pan(P->late_out, E->reverb_pan, 0.9129999876022339);

    calc_delays(&pu, P, E, fs);
    calc_decay(&pu, P, E, fs);
    for (int i = 0; i < 4; i++) {
        P->fdn_gain[i] = pu.gain[i] * pu.cos2;
        P->lap_gain[i] = pu.gain[4 + i] * pu.cos2;
    }
    P->fdn_rot = P->lap_rot = pu.tan;
    P->lap_coef = (float)(E->diffusion * 0.6180340051651001);
}
