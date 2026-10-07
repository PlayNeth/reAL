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

/* From ALchemy's EAX3 preset table (.rdata 0x100a8280, 0x70 bytes/entry), the fields the
 * EAX 1.0 path copies: size, diffusion, reflections, reflections delay, reverb, reverb delay. */
static const struct { float size, diff; int refl; float refl_dly; int rev; float rev_dly; } k_env[26] = {
    {   7.5f, 1.00f, -2602, 0.007f,   200, 0.011f }, {   1.4f, 1.00f, -1204, 0.001f,   207, 0.002f },
    {   1.9f, 1.00f, -1646, 0.002f,    53, 0.003f }, {   1.4f, 1.00f,  -370, 0.007f,  1030, 0.011f },
    {   2.5f, 1.00f, -1376, 0.003f, -1104, 0.004f }, {  11.6f, 1.00f,  -711, 0.012f,    83, 0.017f },
    {  21.6f, 1.00f,  -789, 0.020f,  -289, 0.030f }, {  19.6f, 1.00f, -1230, 0.020f,    -2, 0.029f },
    {  14.6f, 1.00f,  -602, 0.015f,  -302, 0.022f }, {  36.2f, 1.00f, -1166, 0.020f,    16, 0.030f },
    {  50.3f, 1.00f,  -602, 0.020f,   198, 0.030f }, {   1.9f, 1.00f, -1831, 0.002f, -1630, 0.030f },
    {   1.8f, 1.00f, -1219, 0.007f,   441, 0.011f }, {  13.5f, 1.00f, -1214, 0.013f,   395, 0.020f },
    {   7.5f, 0.30f, -1204, 0.007f,    -4, 0.011f }, {  38.0f, 0.30f, -2560, 0.162f,  -229, 0.088f },
    {   7.5f, 0.50f, -2273, 0.007f, -1691, 0.011f }, { 100.0f, 0.27f, -2780, 0.300f, -1434, 0.100f },
    {  17.5f, 1.00f,-10000, 0.061f,   500, 0.025f }, {  42.5f, 0.21f, -2466, 0.179f, -1926, 0.100f },
    {   8.3f, 1.00f, -1363, 0.008f, -1153, 0.012f }, {   1.7f, 0.80f,   429, 0.014f,  1023, 0.021f },
    {   1.8f, 1.00f,  -449, 0.007f,  1700, 0.011f }, {   1.9f, 0.50f,  -115, 0.002f,   985, 0.030f },
    {   1.8f, 0.60f, -1713, 0.020f,  -613, 0.030f }, {   1.0f, 0.50f,  -626, 0.020f,   774, 0.030f },
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

void rl_eax1_to_params(uint32_t env, float volume, float decay, float damping, rl_reverb_params *p)
{
    if (env > 25) env = 25;
    if (volume < 0.0f) volume = 0.0f; else if (volume > 1.0f) volume = 1.0f;
    if (decay < 0.1f) decay = 0.1f; else if (decay > 20.0f) decay = 20.0f;
    float t = rl_eax1_damping_to_hf_ratio(env, damping);
    p->room = volume;
    p->room_hf = rl_eax1_room_hf(env, t);
    p->decay = decay;
    p->decay_hf_ratio = t;
    p->reflections = k_env[env].refl;
    p->reflections_delay = k_env[env].refl_dly;
    p->reverb = k_env[env].rev;
    p->reverb_delay = k_env[env].rev_dly;
    p->env_size = k_env[env].size;
    p->diffusion = k_env[env].diff;
    p->hf_ref = 5000.0f;
}

/* ------------------------------------------------------------------------- DSP */

static const float k_line_ms[RL_FDN_LINES] = { 29.7f, 37.1f, 41.1f, 43.7f };
static const float k_ap_ms[RL_AP_STAGES] = { 4.77f, 3.59f };
static const float k_er_ms[RL_ER_TAPS] = { 0.0f, 3.1f, 5.3f, 8.9f, 12.7f, 17.3f };   /* relative, scaled by size */
static const float k_er_pan[RL_ER_TAPS] = { -0.7f, 0.6f, -0.3f, 0.9f, -0.9f, 0.4f };
#define MAX_SCALE 2.5f
#define MAX_PRE_S 0.5f
#define RL_LATE_CAL 0.68f   /* measured: makes steady-state late energy per channel == input at 0 mB */

static float db100(int mb) { return mb <= -10000 ? 0.0f : powf(10.0f, (float)mb / 2000.0f); }

int rl_reverb_init(rl_reverb *r, uint32_t rate)
{
    memset(r, 0, sizeof(*r));
    r->rate = rate;
    uint32_t total = 0;
    for (int i = 0; i < RL_FDN_LINES; i++) { r->cap[i] = (uint32_t)(k_line_ms[i] * MAX_SCALE * rate / 1000.0f) + 1; total += r->cap[i]; }
    for (int i = 0; i < RL_AP_STAGES; i++) { r->ap_cap[i] = (uint32_t)(k_ap_ms[i] * MAX_SCALE * rate / 1000.0f) + 1; total += r->ap_cap[i]; }
    r->pre_cap = (uint32_t)(MAX_PRE_S * rate) + 1;
    total += r->pre_cap;
    r->mem = calloc(total, sizeof(float));
    if (!r->mem) return 0;
    float *p = r->mem;
    for (int i = 0; i < RL_FDN_LINES; i++) { r->line[i] = p; p += r->cap[i]; r->len[i] = r->cap[i]; }
    for (int i = 0; i < RL_AP_STAGES; i++) { r->ap[i] = p; p += r->ap_cap[i]; r->ap_len[i] = r->ap_cap[i]; }
    r->pre = p;
    rl_reverb_params def;
    rl_eax1_to_params(0, 0.0f, 1.493f, 0.5f, &def);
    rl_reverb_set_params(r, &def);
    return 1;
}

void rl_reverb_free(rl_reverb *r)
{
    free(r->mem);
    memset(r, 0, sizeof(*r));
}

/* one-pole coefficient with -3 dB at angular frequency w (used as the shelf crossover) */
static float lp_coef_for_cutoff(float w)
{
    float c = 2.0f - cosf(w);
    return c - sqrtf(c * c - 1.0f);
}

void rl_reverb_set_params(rl_reverb *r, const rl_reverb_params *p)
{
    if (!r->mem) return;
    const float fs = (float)r->rate;
    float decay = p->decay < 0.1f ? 0.1f : p->decay > 20.0f ? 20.0f : p->decay;
    float hfr = p->decay_hf_ratio < 0.1f ? 0.1f : p->decay_hf_ratio > 2.0f ? 2.0f : p->decay_hf_ratio;
    float hfref = p->hf_ref > 1000.0f && p->hf_ref < fs * 0.45f ? p->hf_ref : 5000.0f;
    float w = 2.0f * 3.14159265f * hfref / fs;
    r->xover_a = lp_coef_for_cutoff(w);

    /* environment size scales the network (7.5 m = generic) */
    float scale = p->env_size / 7.5f;
    if (scale < 0.4f) scale = 0.4f; else if (scale > MAX_SCALE) scale = MAX_SCALE;

    float gsum2 = 0.0f;
    for (int i = 0; i < RL_FDN_LINES; i++) {
        uint32_t len = (uint32_t)(k_line_ms[i] * scale * fs / 1000.0f);
        if (len < 8) len = 8;
        if (len > r->cap[i]) len = r->cap[i];
        if (r->len[i] != len) { r->len[i] = len; if (r->pos[i] >= len) r->pos[i] = 0; }
        float secs = (float)len / fs;
        float g  = powf(10.0f, -3.0f * secs / decay);
        float gh = powf(10.0f, -3.0f * secs / (decay * hfr));
        r->fb[i] = g;
        r->hf_g[i] = hfr >= 1.0f ? 1.0f : gh / g;     /* extra per-pass HF gain (shelf above hf_ref) */
        gsum2 += g * g;
    }
    for (int i = 0; i < RL_AP_STAGES; i++) {
        uint32_t len = (uint32_t)(k_ap_ms[i] * scale * fs / 1000.0f);
        if (len < 4) len = 4;
        if (len > r->ap_cap[i]) len = r->ap_cap[i];
        if (r->ap_len[i] != len) { r->ap_len[i] = len; if (r->ap_pos[i] >= len) r->ap_pos[i] = 0; }
    }
    float diff = p->diffusion < 0.0f ? 0.0f : p->diffusion > 1.0f ? 1.0f : p->diffusion;
    r->ap_g = 0.75f * diff;

    /* early reflections: a cluster of taps starting at reflections_delay */
    float rd = p->reflections_delay < 0.0f ? 0.0f : p->reflections_delay;
    float er = db100(p->reflections) / sqrtf((float)RL_ER_TAPS);
    for (int i = 0; i < RL_ER_TAPS; i++) {
        float t = rd + k_er_ms[i] * scale * 0.001f;
        uint32_t off = (uint32_t)(t * fs) + 1;
        if (off >= r->pre_cap) off = r->pre_cap - 1;
        r->er_off[i] = off;
        float pan = k_er_pan[i];
        r->er_gain[i][0] = er * sqrtf(0.5f * (1.0f - pan));
        r->er_gain[i][1] = er * sqrtf(0.5f * (1.0f + pan));
    }

    /* late reverb starts reverb_delay after the reflections */
    float ld = rd + (p->reverb_delay < 0.0f ? 0.0f : p->reverb_delay);
    uint32_t loff = (uint32_t)(ld * fs) + 1;
    if (loff >= r->pre_cap) loff = r->pre_cap - 1;
    r->late_off = loff;
    /* normalise the network's energy gain (~1/(1-g^2)) so `reverb` sets the late level */
    float gmean2 = gsum2 / RL_FDN_LINES;
    r->late_gain = db100(p->reverb) * sqrtf(1.0f - gmean2) * RL_LATE_CAL;

    /* room HF: high shelf on the input */
    r->in_hf_g = db100(p->room_hf);
    r->room = p->room < 0.0f ? 0.0f : p->room > 1.0f ? 1.0f : p->room;
    r->active = r->room > 0.0f;
}

void rl_reverb_process(rl_reverb *r, const float *in, float *out, uint32_t frames)
{
    if (!r->mem) return;
    const float room = r->room;
    for (uint32_t n = 0; n < frames; n++) {
        /* room + room HF on the input */
        float x = in[n] * room;
        r->in_z = x + r->xover_a * (r->in_z - x);            /* low part */
        r->pre[r->pre_pos] = r->in_z + r->in_hf_g * (x - r->in_z);

        /* early reflections */
        float el = 0.0f, err = 0.0f;
        for (int i = 0; i < RL_ER_TAPS; i++) {
            uint32_t k = r->pre_pos >= r->er_off[i] ? r->pre_pos - r->er_off[i] : r->pre_pos + r->pre_cap - r->er_off[i];
            float s = r->pre[k];
            el += s * r->er_gain[i][0];
            err += s * r->er_gain[i][1];
        }

        /* late reverb input */
        uint32_t k = r->pre_pos >= r->late_off ? r->pre_pos - r->late_off : r->pre_pos + r->pre_cap - r->late_off;
        float lx = r->pre[k];
        if (++r->pre_pos >= r->pre_cap) r->pre_pos = 0;

        for (int i = 0; i < RL_AP_STAGES; i++) {
            float d = r->ap[i][r->ap_pos[i]];
            float y = d - r->ap_g * lx;
            r->ap[i][r->ap_pos[i]] = lx + r->ap_g * y;
            if (++r->ap_pos[i] >= r->ap_len[i]) r->ap_pos[i] = 0;
            lx = y;
        }

        float d[RL_FDN_LINES], sum = 0.0f;
        for (int i = 0; i < RL_FDN_LINES; i++) {
            float s = r->line[i][r->pos[i]];
            r->lp_z[i] = s + r->xover_a * (r->lp_z[i] - s);
            d[i] = (r->lp_z[i] + r->hf_g[i] * (s - r->lp_z[i])) * r->fb[i];
            sum += d[i];
        }
        float h = sum * (2.0f / RL_FDN_LINES);
        for (int i = 0; i < RL_FDN_LINES; i++) {
            r->line[i][r->pos[i]] = d[i] - h + lx;
            if (++r->pos[i] >= r->len[i]) r->pos[i] = 0;
        }

        if (r->active) {
            out[n * 2]     += el + (d[0] - d[1] + d[2]) * r->late_gain;
            out[n * 2 + 1] += err + (d[1] + d[2] - d[3]) * r->late_gain;
        }
    }
}
