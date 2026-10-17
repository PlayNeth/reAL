#include "speakers.h"
#include <math.h>
#include <string.h>

void rl_layout_init(rl_layout *l, uint32_t mask)
{
    l->mask = mask;
    l->n = 0;
    for (int b = 0; b < 12; b++) l->idx[b] = (mask >> b & 1) && l->n < RL_MAXOUT ? (int8_t)l->n++ : -1;
}

uint32_t rl_default_mask(uint32_t channels)
{
    switch (channels) {
    case 1: return RL_SPK_FC;
    case 2: return RL_LAYOUT_STEREO;
    case 3: return 0x7;
    case 4: return RL_LAYOUT_QUAD;
    case 5: return 0x37;
    case 6: return RL_LAYOUT_51;
    case 7: return 0x13f;
    default: return RL_LAYOUT_71;
    }
}

uint32_t rl_layout_for_channels(uint32_t channels)
{
    return channels >= 8 ? RL_LAYOUT_71 : channels >= 6 ? RL_LAYOUT_51 : channels >= 4 ? RL_LAYOUT_QUAD : RL_LAYOUT_STEREO;
}

/* fn 0x1001c1a0: the speaker configuration ALchemy reports (and renders) for each layout */
uint32_t rl_layout_speaker_config(uint32_t layout, int headphones)
{
    switch (layout) {
    case RL_LAYOUT_QUAD: return 3;      /* DSSPEAKER_QUAD */
    case RL_LAYOUT_51:   return 6;      /* DSSPEAKER_5POINT1 */
    case RL_LAYOUT_71:   return 7;      /* DSSPEAKER_7POINT1 */
    default:             return headphones ? 1 : 4;    /* DSSPEAKER_HEADPHONE / DSSPEAKER_STEREO */
    }
}

/* ------------------------------------------------------------------ channel converter (fn 0x10036740) */

#define K_707 0.7071067690849304f     /* 0x100b5158 */
#define K_HALF 0.5f                   /* 0x100b4e34 */
#define K_SIDE_F 0.3659999966621399f  /* 0x100a8f84: a side speaker into the front... */
#define K_SIDE_B 0.6340000033378601f  /* 0x100aad08: ...and the rear one */

typedef struct { const rl_layout *in, *out; float (*m)[RL_MAXOUT]; } conv;

static int has(uint32_t mask, uint32_t bit) { return (mask & bit) != 0; }
static int bitno(uint32_t bit) { int b = 0; while (bit >> b != 1) b++; return b; }

/* out[obit] += in[ibit] * g */
static void add(conv *c, uint32_t ibit, uint32_t obit, float g)
{
    int i = c->in->idx[bitno(ibit)], o = c->out->idx[bitno(obit)];
    if (i >= 0 && o >= 0) c->m[i][o] += g;
}

/* out[dst] = out[src] (a copy of everything routed to src so far) */
static void copy_out(conv *c, uint32_t src, uint32_t dst)
{
    int s = c->out->idx[bitno(src)], d = c->out->idx[bitno(dst)];
    if (s < 0 || d < 0) return;
    for (uint32_t i = 0; i < c->in->n; i++) c->m[i][d] = c->m[i][s];
}

static int conv_mode(uint32_t out)
{
    switch (out) {                    /* fn 0x10035fd0 */
    case 0x3: case 0xb: return 4;     /* (3 is the stereo virtualizer, used by the reverb: reverb.c) */
    case 0x7: case 0xf: return 5;
    case 0x33: case 0x3b: return 6;
    default: return 7;
    }
}

void rl_convert_matrix(const rl_layout *in, const rl_layout *out, float m[RL_MAXOUT][RL_MAXOUT], int *lfe_redirect)
{
    memset(m, 0, sizeof(float) * RL_MAXOUT * RL_MAXOUT);
    conv c = { in, out, m };
    const uint32_t im = in->mask, om = out->mask;
    switch (conv_mode(om)) {
    case 4:                           /* 0x10036c10: everything folded onto two speakers */
        if (has(im, RL_SPK_FC)) add(&c, RL_SPK_FC, RL_SPK_FL, K_707);
        if (has(im, RL_SPK_BC)) add(&c, RL_SPK_BC, RL_SPK_FL, K_707);
        if (has(im, RL_SPK_TC)) add(&c, RL_SPK_TC, RL_SPK_FL, K_707);
        copy_out(&c, RL_SPK_FL, RL_SPK_FR);
        add(&c, RL_SPK_FL, RL_SPK_FL, 1.0f); add(&c, RL_SPK_BL, RL_SPK_FL, 1.0f); add(&c, RL_SPK_SL, RL_SPK_FL, 1.0f);
        add(&c, RL_SPK_FR, RL_SPK_FR, 1.0f); add(&c, RL_SPK_BR, RL_SPK_FR, 1.0f); add(&c, RL_SPK_SR, RL_SPK_FR, 1.0f);
        break;
    case 5:                           /* 0x10036d90: three front speakers */
        if (has(im, RL_SPK_FC)) add(&c, RL_SPK_FC, RL_SPK_FC, 1.0f);
        if (has(im, RL_SPK_BC)) add(&c, RL_SPK_BC, RL_SPK_FL, K_707);
        if (has(im, RL_SPK_TC)) add(&c, RL_SPK_TC, RL_SPK_FL, K_707);
        copy_out(&c, RL_SPK_FL, RL_SPK_FR);
        add(&c, RL_SPK_FL, RL_SPK_FL, 1.0f); add(&c, RL_SPK_BL, RL_SPK_FL, 1.0f); add(&c, RL_SPK_SL, RL_SPK_FL, 1.0f);
        add(&c, RL_SPK_FR, RL_SPK_FR, 1.0f); add(&c, RL_SPK_BR, RL_SPK_FR, 1.0f); add(&c, RL_SPK_SR, RL_SPK_FR, 1.0f);
        break;
    case 6:                           /* 0x10036f00: quad */
        if (has(im, RL_SPK_TC)) add(&c, RL_SPK_TC, RL_SPK_FL, K_HALF);
        copy_out(&c, RL_SPK_FL, RL_SPK_BL);
        if (has(im, RL_SPK_FC)) add(&c, RL_SPK_FC, RL_SPK_FL, K_707);
        copy_out(&c, RL_SPK_FL, RL_SPK_FR);
        if (has(im, RL_SPK_BC)) add(&c, RL_SPK_BC, RL_SPK_BL, K_707);
        copy_out(&c, RL_SPK_BL, RL_SPK_BR);
        add(&c, RL_SPK_FL, RL_SPK_FL, 1.0f); add(&c, RL_SPK_FR, RL_SPK_FR, 1.0f);
        add(&c, RL_SPK_BL, RL_SPK_BL, 1.0f); add(&c, RL_SPK_BR, RL_SPK_BR, 1.0f);
        if (has(im, RL_SPK_SL)) { add(&c, RL_SPK_SL, RL_SPK_FL, K_707); add(&c, RL_SPK_SL, RL_SPK_BL, K_707); }
        if (has(im, RL_SPK_SR)) { add(&c, RL_SPK_SR, RL_SPK_FR, K_707); add(&c, RL_SPK_SR, RL_SPK_BR, K_707); }
        break;
    default:                          /* 0x100370e0: 5.1, 7.1 and anything else */
        for (int b = 0; b < 12; b++)
            if (has(om, 1u << b) && has(im, 1u << b)) add(&c, 1u << b, 1u << b, 1.0f);
        if (has(im, RL_SPK_FC) && !has(om, RL_SPK_FC)) {
            if (has(im, RL_SPK_FL)) add(&c, RL_SPK_FC, RL_SPK_FL, K_707);
            if (has(im, RL_SPK_FR)) add(&c, RL_SPK_FC, RL_SPK_FR, K_707);
        }
        if (has(im, RL_SPK_TC)) {
            add(&c, RL_SPK_TC, RL_SPK_FL, K_HALF); add(&c, RL_SPK_TC, RL_SPK_FR, K_HALF);
            if (has(om, RL_SPK_BL) && has(om, RL_SPK_BR)) { add(&c, RL_SPK_TC, RL_SPK_BL, K_HALF); add(&c, RL_SPK_TC, RL_SPK_BR, K_HALF); }
            else if (has(om, RL_SPK_SL) && has(om, RL_SPK_SR)) { add(&c, RL_SPK_TC, RL_SPK_SL, K_HALF); add(&c, RL_SPK_TC, RL_SPK_SR, K_HALF); }
        }
        if (has(im, RL_SPK_BC) && !has(om, RL_SPK_BC)) {
            if (has(om, RL_SPK_BL) && has(om, RL_SPK_BR)) { add(&c, RL_SPK_BC, RL_SPK_BL, K_707); add(&c, RL_SPK_BC, RL_SPK_BR, K_707); }
            else if (has(om, RL_SPK_SL) && has(om, RL_SPK_SR)) { add(&c, RL_SPK_BC, RL_SPK_SL, K_707); add(&c, RL_SPK_BC, RL_SPK_SR, K_707); }
            else if (has(om, RL_SPK_FL) && has(om, RL_SPK_FR)) { add(&c, RL_SPK_BC, RL_SPK_FL, K_707); add(&c, RL_SPK_BC, RL_SPK_FR, K_707); }
        }
        if (has(im, RL_SPK_SL) && !has(om, RL_SPK_SL) && has(om, RL_SPK_FL) && has(om, RL_SPK_BL)) {
            add(&c, RL_SPK_SL, RL_SPK_FL, K_SIDE_F); add(&c, RL_SPK_SL, RL_SPK_BL, K_SIDE_B);
        }
        if (has(im, RL_SPK_SR) && !has(om, RL_SPK_SR) && has(om, RL_SPK_FR) && has(om, RL_SPK_BR)) {
            add(&c, RL_SPK_SR, RL_SPK_FR, K_SIDE_F); add(&c, RL_SPK_SR, RL_SPK_BR, K_SIDE_B);
        }
        if (has(im, RL_SPK_BL) && has(om, RL_SPK_SL) && !has(om, RL_SPK_BL)) add(&c, RL_SPK_BL, RL_SPK_SL, 1.0f);
        if (has(im, RL_SPK_BR) && has(om, RL_SPK_SR) && !has(om, RL_SPK_BR)) add(&c, RL_SPK_BR, RL_SPK_SR, 1.0f);
        break;
    }
    /* 0x10036690, after every mode: the front left/right-of-centre speakers */
    if (has(im, RL_SPK_FLC) && !has(om, RL_SPK_FLC)) {
        if (has(om, RL_SPK_FC)) { add(&c, RL_SPK_FLC, RL_SPK_FL, K_707); add(&c, RL_SPK_FLC, RL_SPK_FC, K_707); }
        else add(&c, RL_SPK_FLC, RL_SPK_FL, 1.0f);
    }
    if (has(im, RL_SPK_FRC) && !has(om, RL_SPK_FRC)) {
        if (has(om, RL_SPK_FC)) { add(&c, RL_SPK_FRC, RL_SPK_FR, K_707); add(&c, RL_SPK_FRC, RL_SPK_FC, K_707); }
        else add(&c, RL_SPK_FRC, RL_SPK_FR, 1.0f);
    }
    if (lfe_redirect) *lfe_redirect = has(im, RL_SPK_LFE) && !has(om, RL_SPK_LFE);
}

/* ------------------------------------------------------------------ bass limiter */

void rl_bass_limiter_init(rl_bass_limiter *b, uint32_t rate)
{
    float k = (float)rate / 48000.0f;
    b->att = (float)(0.029999999329447746 / (double)(k * 10.0f));
    b->rel = (float)(1.0 - 0.009999999776482582 / (double)(k * 50.0f));
    b->thr = 0.17782793939113617f;
    b->env = 0.0f;
}

float rl_bass_limit(rl_bass_limiter *b, float x)
{
    float d = fabsf(x) - b->env;
    if (d < 0.0f) d = 0.0f;
    b->env = d * b->att + b->rel * b->env;
    return b->thr < b->env ? x * b->thr / b->env : x;
}

/* ------------------------------------------------------------------ 3D sources on speakers (fn 0x10034900) */

/* 0x100ae7f8: 2^-1/4 cos(2i degrees); 0x100ae8b0: 2^-1/2 cos^2(2i degrees) */
static const float k_lobe[45] = {
    0.840896428f, 0.840384185f, 0.838848054f, 0.836289883f, 0.832712889f, 0.828121305f, 0.822520792f, 0.815918207f,
    0.808321536f, 0.799740016f, 0.79018414f, 0.779665589f, 0.768197119f, 0.755792677f, 0.742467463f, 0.728237629f,
    0.71312058f, 0.697134733f, 0.680299461f, 0.662635446f, 0.644164026f, 0.624907792f, 0.604890287f, 0.584135711f,
    0.562669516f, 0.540517807f, 0.517707527f, 0.49426651f, 0.470223308f, 0.445607215f, 0.420448214f, 0.39477694f,
    0.368624717f, 0.342023373f, 0.315005332f, 0.287603527f, 0.259851277f, 0.231782466f, 0.203431249f, 0.174832195f,
    0.146020129f, 0.117030159f, 0.0878976136f, 0.0586579703f, 0.0293468609f
};
static const float k_duck[45] = {
    0.707106769f, 0.706245542f, 0.703666031f, 0.699380815f, 0.693410695f, 0.685784876f, 0.676540494f, 0.665722489f,
    0.653383672f, 0.639584064f, 0.624391019f, 0.607878387f, 0.590126812f, 0.571222603f, 0.551257908f, 0.530330062f,
    0.508540988f, 0.485996813f, 0.462807387f, 0.439085692f, 0.414947301f, 0.390509784f, 0.365892231f, 0.341214567f,
    0.316596985f, 0.292159498f, 0.268021077f, 0.244299382f, 0.221109957f, 0.198565781f, 0.176776692f, 0.155848846f,
    0.135884196f, 0.116979994f, 0.0992283672f, 0.0827157795f, 0.0675226897f, 0.0537231117f, 0.041384276f, 0.0305662975f,
    0.0213218778f, 0.0136960587f, 0.00772598991f, 0.00344075728f, 0.000861238281f
};

/* |round(a -+ 0.5 +- 1.5e-8)|: the angle's whole degrees, as the x87 code rounds them */
static uint32_t whole_degrees(float a)
{
    const double eps = 1.4999999464748726e-08;
    double x = a >= 0.0f ? ((double)a - 0.5) + eps : ((double)a + 0.5) - eps;
    double r = nearbyint(x);
    return (uint32_t)(r < 0 ? -r : r);
}

void rl_speaker_gains3d(uint32_t layout, uint32_t n, float az, float el, double dist, float g[12])
{
    memset(g, 0, 12 * sizeof(float));
    float g0 = 1.0f, g1 = 1.0f, top = 0.0f;
    if (n > 5) {
        top = g1 < g0 ? g0 : g1;
        g[3] = top * 0.5f;
    }
    /* front/rear split: 0.75 straight ahead, 0.25 behind, 0.5 to the side (and overhead) */
    float p = (float)(((1.0 - fabsf(el) / 90.0) * (fabsf(az) / 90.0 - 1.0) + 1.0) * 0.5);
    if (p < 0.25f) p = 0.25f;
    else if (0.75f < p) p = 0.75f;
    if (dist < 0.5) {                 /* closer than half a metre: towards an even split */
        float s = (float)(dist * 2.0);
        p = (float)(((double)p - 0.5) * s + 0.5);
    }
    float f = (float)((0.75 - p) * 2.0), r = 1.0f - f;
    g[4] = r * g0;
    g[5] = r * g1;
    g[0] = g0 * f;
    g[1] = g1 * f;
    if (n >= 6) {
        uint32_t ia = whole_degrees(az), ie = whole_degrees(el);
        float fmax = g[1] < g[0] ? g[0] : g[1];
        if (ia <= 44 && ie <= 44) {
            if (layout == 0xff) g[2] = ia < 23 ? k_lobe[2 * ia] * fmax * k_lobe[ie] : 0.0f;
            else g[2] = k_lobe[ia] * fmax * k_lobe[ie];
            float d = 1.0f - k_duck[ie] * k_duck[ia];
            g[0] *= d;
            g[1] *= d;
        }
        if (n >= 8) {
            if (layout != RL_LAYOUT_71) {         /* 7.1 wide: the front left/right-of-centre speakers */
                if (ia < 45 && ie < 45 && ia != 0) {
                    uint32_t k = ia * 2 > 45 ? ia * 2 - 45 : 45 - ia * 2;
                    float v = k < 45 ? k_lobe[k] * top * k_lobe[ie] : 0.0f;
                    if (az < 0.0f) g[6] = v; else g[7] = v;
                }
            } else if (ia >= 46 && ia <= 134 && ie < 45) {
                uint32_t k = ia > 90 ? ia - 90 : 90 - ia;
                float v = k_lobe[k] * top * k_lobe[ie];
                if (az >= 0.0f) g[10] = v; else g[9] = v;
                float d = 1.0f - k_duck[k] * k_duck[ie];
                g[0] *= d; g[1] *= d; g[4] *= d; g[5] *= d;
            }
        }
    }
    g[3] *= 0.5f;                     /* 0x10035780 halves the LFE once more */
}
