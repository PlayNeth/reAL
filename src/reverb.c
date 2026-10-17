#include "reverb.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const rl_eax1_preset rl_eax1_presets[26] = {
    { 0.500f, 1.493f, 0.500f }, { 0.250f, 0.100f, 0.000f }, { 0.417f, 0.400f, 0.666f }, { 0.653f, 1.499f, 0.166f },
    { 0.208f, 0.478f, 0.000f }, { 0.500f, 2.309f, 0.888f }, { 0.403f, 4.279f, 0.500f }, { 0.500f, 3.961f, 0.500f },
    { 0.500f, 2.886f, 1.304f }, { 0.361f, 7.284f, 0.332f }, { 0.500f, 10.00f, 0.300f }, { 0.153f, 0.259f, 2.000f },
    { 0.361f, 1.493f, 0.000f }, { 0.444f, 2.697f, 0.638f }, { 0.250f, 1.752f, 0.776f }, { 0.111f, 3.145f, 0.472f },
    { 0.111f, 2.767f, 0.224f }, { 0.194f, 7.841f, 0.472f }, { 1.000f, 1.499f, 0.500f }, { 0.097f, 2.767f, 0.224f },
    { 0.208f, 1.652f, 1.500f }, { 0.652f, 2.886f, 0.250f }, { 1.000f, 1.499f, 0.000f }, { 0.875f, 8.392f, 1.388f },
    { 0.139f, 17.234f, 0.666f }, { 0.486f, 7.563f, 0.806f },
};

/* per-environment damping constants (.rdata 0x100a7e20 and 0x100a7e88) */
static const float k_damp1[26] = {
    0.2066f, 6.0646f, 0.4045f, 0.1654f, 0.9629f, 4.4585f, 0.7055f, 0.4342f, 1.0855f, 0.9916f, 1.4168f, 10.8844f, 0.7055f,
    0.4629f, 0.1628f, 0.8607f, 0.4856f, 3.6979f, 0.2066f, 0.2066f, 0.2024f, 2.0185f, 37.7223f, 0.4133f, 1.5424f, 0.4133f,
};
static const float k_damp2[26] = {
    -488.2f, -666.7f, -2216.6f, -1408.7f, -666.7f, -533.3f, -685.0f, -1166.7f, 0.0f, -343.8f, -298.7f, -444.4f, -431.7f,
    -891.6f, -1658.6f, -3873.9f, -1624.2f, -664.6f, -4882.4f, -2000.0f, 0.0f, -162.8f, -444.4f, 0.0f, -509.1f, -1526.8f,
};

float rl_eax1_damping_to_hf_ratio(uint32_t env, float d)
{
    if (env > 25) env = 25;
    if (d >= 1.0f) return d;
    if (d < 1.1920929e-07f) d = 1.1920929e-07f;
    float t = 1.0f / (1.0f - k_damp1[env] * (1.0f - 1.0f / d));
    if (t > 2.0f) t = 2.0f;
    else if (t < 0.1f) t = 0.1f;
    return t;
}

int rl_eax1_room_hf(uint32_t env, float t)
{
    if (env > 25) env = 25;
    float v = k_damp2[env] * (1.0f / t - 1.0f);
    int mb = (int)lrintf(v);
    if (mb > 0) mb = 0;
    if (mb < -10000) mb = -10000;
    return mb;
}

#define CX_TO_REAR 0.7070000171661377f       /* plug: rear pair += centre copy * 0.707 (0x100b54e4) */

/* ------------------------------------------------------------------------- reverb */

#define RAMP_LEN  1024                 /* input-gain ramp for level-only changes */
#define RING_MAX  2.0f                 /* the old environment's tail fades out over at most 2 s */

int rl_reverb_init(rl_reverb *r, uint32_t rate)
{
    memset(r, 0, sizeof(*r));
    r->rate = rate;
    if (!alrv_init(&r->eng[0], rate) || !alrv_init(&r->eng[1], rate)) { rl_reverb_free(r); return 0; }
    rl_virt_init(&r->virt, rate);
    alrv_props e;
    alrv_props_default(&e);
    alrv_props_from_eax1(&e, 0, rl_eax1_presets[0].volume, rl_eax1_presets[0].decay, rl_eax1_presets[0].damping);
    rl_reverb_set_props(r, &e);
    return 1;
}

void rl_reverb_free(rl_reverb *r)
{
    alrv_free(&r->eng[0]);
    alrv_free(&r->eng[1]);
    rl_virt_free(&r->virt);
    free(r->scratch);
    memset(r, 0, sizeof(*r));
}

static void get_gains(const alrv_params *p, float g[5])
{
    g[0] = p->refl_gain;
    for (int i = 0; i < 4; i++) g[1 + i] = p->late_tap_gain[i];
}

static void put_gains(alrv_params *p, const float g[5])
{
    p->refl_gain = g[0];
    for (int i = 0; i < 4; i++) p->late_tap_gain[i] = g[1 + i];
}

/* everything that sets the network's shape (delay lengths, modulation depth) */
static int same_structure(const alrv_params *a, const alrv_params *b)
{
    return a->refl_delay == b->refl_delay && a->mod_depth == b->mod_depth &&
           !memcmp(a->late_tap_delay, b->late_tap_delay, sizeof(a->late_tap_delay)) &&
           !memcmp(a->eap_delay, b->eap_delay, sizeof(a->eap_delay)) &&
           !memcmp(a->lap_delay, b->lap_delay, sizeof(a->lap_delay)) &&
           !memcmp(a->fdn_delay, b->fdn_delay, sizeof(a->fdn_delay));
}

void rl_reverb_set_props(rl_reverb *r, const alrv_props *props)
{
    if (!r->eng[0].mem) return;
    alrv_params p;
    alrv_compute_params(&p, props, r->rate);
    if (!memcmp(&p, &r->target, sizeof(p)) && r->active) return;
    alrv *cur = &r->eng[r->cur];
    if (!cur->have_params) {
        alrv_set_params(cur, &p);
    } else if (same_structure(&p, &r->target)) {
        /* same room, different levels/decay: update in place, ramp the input gains */
        float from[5];
        memcpy(from, r->gain_cur, sizeof(from));
        if (!r->ramp_left) get_gains(&cur->p, from);
        alrv_set_params(cur, &p);
        memcpy(r->gain_from, from, sizeof(from));
        r->ramp_left = RAMP_LEN;
        put_gains(&cur->p, from);
    } else {
        /* a different room: new input goes to a fresh network, the old one rings out */
        int old = r->cur;
        r->cur ^= 1;
        alrv *nw = &r->eng[r->cur];
        alrv_reset(nw);
        alrv_set_params(nw, &p);
        (void)old;
        float secs = r->decay < RING_MAX ? r->decay : RING_MAX;   /* the old room's decay */
        r->ring_total = r->ring_left = (uint32_t)((secs > 0.25f ? secs : 0.25f) * r->rate);
        r->ramp_left = 0;
    }
    r->target = p;
    r->decay = props->decay;
    r->active = props->room > 0.0f;
}

void rl_reverb_set_eax1(rl_reverb *r, uint32_t env, float volume, float decay, float damping)
{
    alrv_props e;
    alrv_props_default(&e);
    alrv_props_from_eax1(&e, env, volume, decay, damping);
    rl_reverb_set_props(r, &e);
}

#define CHUNK 64

/* the 8 reverb channels of this block (centre copy folded into the rear pair), or NULL when silent */
static float *render(rl_reverb *r, const float *in, uint32_t frames, float *ch[ALRV_NOUT])
{
    if (!r->eng[0].mem || (!r->active && !r->ring_left)) return NULL;
    if (r->scratch_frames < frames) {
        free(r->scratch);
        r->scratch = calloc((size_t)frames * 19, sizeof(float));
        if (!r->scratch) { r->scratch_frames = 0; return NULL; }
        r->scratch_frames = frames;
    }
    float *och[ALRV_NOUT];
    for (int c = 0; c < ALRV_NOUT; c++) { ch[c] = r->scratch + c * frames; och[c] = r->scratch + (8 + c) * frames; }
    float *zero = r->scratch + 16 * frames, *tmp = r->scratch + 17 * frames;

    /* current environment, with the input-gain ramp applied chunk by chunk */
    alrv *cur = &r->eng[r->cur];
    for (uint32_t pos = 0; pos < frames;) {
        uint32_t n = frames - pos;
        if (r->ramp_left) {
            if (n > CHUNK) n = CHUNK;
            float tg[5], g[5], k = 1.0f - (float)(r->ramp_left > n ? r->ramp_left - n : 0) / RAMP_LEN;
            get_gains(&r->target, tg);
            for (int i = 0; i < 5; i++) g[i] = r->gain_from[i] + (tg[i] - r->gain_from[i]) * k;
            put_gains(&cur->p, g);
            memcpy(r->gain_cur, g, sizeof(g));
            r->ramp_left = r->ramp_left > n ? r->ramp_left - n : 0;
        }
        float *sub[ALRV_NOUT];
        for (int c = 0; c < ALRV_NOUT; c++) sub[c] = ch[c] + pos;
        alrv_process(cur, in + pos, sub, n);
        pos += n;
    }

    /* previous environment ringing out, faded */
    if (r->ring_left) {
        alrv *old = &r->eng[r->cur ^ 1];
        uint32_t n = frames < r->ring_left ? frames : r->ring_left;
        memset(zero, 0, n * sizeof(float));
        alrv_process(old, zero, och, n);
        for (uint32_t i = 0; i < n; i++) {
            float w = (float)(r->ring_left - i) / (float)r->ring_total;
            for (int c = 0; c < ALRV_NOUT; c++) ch[c][i] += och[c][i] * w;
        }
        r->ring_left -= n;
    }

    for (uint32_t i = 0; i < frames; i++) {
        ch[ALRV_BL][i] += ch[ALRV_CX][i] * CX_TO_REAR;
        ch[ALRV_BR][i] += ch[ALRV_CX][i] * CX_TO_REAR;
    }
    return tmp;
}

void rl_reverb_process(rl_reverb *r, const float *in, float *out, uint32_t frames)
{
    float *ch[ALRV_NOUT], *tmp = render(r, in, frames, ch);
    if (tmp) rl_virt_process(&r->virt, ch, out, frames, tmp);
}

/* the speaker bit of each reverb output (the centre copy is folded away before this) */
static const uint32_t k_alrv_bit[ALRV_NOUT] = { RL_SPK_FL, RL_SPK_FR, RL_SPK_FC, RL_SPK_BL, RL_SPK_BR, 0, RL_SPK_SL, RL_SPK_SR };

void rl_reverb_process_n(rl_reverb *r, const float *in, float *out, const rl_layout *lay, uint32_t frames)
{
    if (lay->n <= 2) { rl_reverb_process(r, in, out, frames); return; }
    float *ch[ALRV_NOUT], *tmp = render(r, in, frames, ch);
    if (!tmp) return;
    /* more speakers: the plug's converter (modes 6 and 7) instead of the virtualizer */
    rl_layout src;
    rl_layout_init(&src, RL_SPK_FL | RL_SPK_FR | RL_SPK_FC | RL_SPK_BL | RL_SPK_BR | RL_SPK_SL | RL_SPK_SR);
    float m[RL_MAXOUT][RL_MAXOUT];
    rl_convert_matrix(&src, lay, m, NULL);
    for (int c = 0; c < ALRV_NOUT; c++) {
        if (!k_alrv_bit[c]) continue;
        int i = src.idx[__builtin_ctz(k_alrv_bit[c])];
        for (uint32_t o = 0; o < lay->n; o++) {
            float g = m[i][o];
            if (g == 0.0f) continue;
            const float *x = ch[c];
            for (uint32_t n = 0; n < frames; n++) out[n * lay->n + o] += x[n] * g;
        }
    }
}
