#include "reverb.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const rl_eax1_preset rl_eax1_presets[26] = {
    { 0.500f, 1.493f, 0.500f }, /*  0 generic */         { 0.250f, 0.100f, 0.000f }, /*  1 padded cell */
    { 0.417f, 0.400f, 0.666f }, /*  2 room */            { 0.653f, 1.499f, 0.166f }, /*  3 bathroom */
    { 0.208f, 0.478f, 0.000f }, /*  4 living room */     { 0.500f, 2.309f, 0.888f }, /*  5 stone room */
    { 0.403f, 4.279f, 0.500f }, /*  6 auditorium */      { 0.500f, 3.961f, 0.500f }, /*  7 concert hall */
    { 0.500f, 2.886f, 1.304f }, /*  8 cave */            { 0.361f, 7.284f, 0.332f }, /*  9 arena */
    { 0.500f, 10.00f, 0.300f }, /* 10 hangar */          { 0.153f, 0.259f, 2.000f }, /* 11 carpeted hallway */
    { 0.361f, 1.493f, 0.000f }, /* 12 hallway */         { 0.444f, 2.697f, 0.638f }, /* 13 stone corridor */
    { 0.250f, 1.752f, 0.776f }, /* 14 alley */           { 0.111f, 3.145f, 0.472f }, /* 15 forest */
    { 0.111f, 2.767f, 0.224f }, /* 16 city */            { 0.194f, 7.841f, 0.472f }, /* 17 mountains */
    { 1.000f, 1.499f, 0.500f }, /* 18 quarry */          { 0.097f, 2.767f, 0.224f }, /* 19 plain */
    { 0.208f, 1.652f, 1.500f }, /* 20 parking lot */     { 0.652f, 2.886f, 0.250f }, /* 21 sewer pipe */
    { 1.000f, 1.499f, 0.000f }, /* 22 underwater */      { 0.875f, 8.392f, 1.388f }, /* 23 drugged */
    { 0.139f, 17.234f, 0.666f }, /* 24 dizzy */          { 0.486f, 7.563f, 0.806f }, /* 25 psychotic */
};

/* mutually prime-ish line lengths in ms */
static const float k_line_ms[RL_FDN_LINES] = { 29.7f, 37.1f, 41.1f, 43.7f };
static const float k_ap_ms[RL_AP_STAGES] = { 4.77f, 3.59f };
static const float k_pre_ms = 12.0f;
static const float k_ap_g = 0.6f;

int rl_reverb_init(rl_reverb *r, uint32_t rate)
{
    memset(r, 0, sizeof(*r));
    r->rate = rate;
    uint32_t total = 0;
    for (int i = 0; i < RL_FDN_LINES; i++) { r->len[i] = (uint32_t)(k_line_ms[i] * rate / 1000.0f); total += r->len[i]; }
    for (int i = 0; i < RL_AP_STAGES; i++) { r->ap_len[i] = (uint32_t)(k_ap_ms[i] * rate / 1000.0f); total += r->ap_len[i]; }
    r->pre_len = (uint32_t)(k_pre_ms * rate / 1000.0f);
    total += r->pre_len;
    r->mem = calloc(total, sizeof(float));
    if (!r->mem) return 0;
    float *p = r->mem;
    for (int i = 0; i < RL_FDN_LINES; i++) { r->line[i] = p; p += r->len[i]; }
    for (int i = 0; i < RL_AP_STAGES; i++) { r->ap[i] = p; p += r->ap_len[i]; }
    r->pre = p;
    rl_reverb_set(r, 1.493f, 0.5f, 0.0f);
    return 1;
}

void rl_reverb_free(rl_reverb *r)
{
    free(r->mem);
    memset(r, 0, sizeof(*r));
}

/* one-pole lowpass magnitude at normalised angular frequency w */
static float lp_mag(float a, float w)
{
    return (1.0f - a) / sqrtf(1.0f - 2.0f * a * cosf(w) + a * a);
}

void rl_reverb_set(rl_reverb *r, float decay, float hf_ratio, float volume)
{
    if (decay < 0.1f) decay = 0.1f; else if (decay > 20.0f) decay = 20.0f;
    if (hf_ratio < 0.1f) hf_ratio = 0.1f; else if (hf_ratio > 2.0f) hf_ratio = 2.0f;
    if (volume < 0.0f) volume = 0.0f; else if (volume > 1.0f) volume = 1.0f;

    const float w = 2.0f * 3.14159265f * 5000.0f / (float)r->rate;  /* EAX HF reference 5 kHz */
    for (int i = 0; i < RL_FDN_LINES; i++) {
        float secs = (float)r->len[i] / (float)r->rate;
        float g  = powf(10.0f, -3.0f * secs / decay);
        float gh = powf(10.0f, -3.0f * secs / (decay * hf_ratio));
        r->fb[i] = g;
        float target = hf_ratio >= 1.0f ? 1.0f : gh / g;   /* extra HF loss per pass */
        float lo = 0.0f, hi = 0.999f;
        if (target >= 0.9999f) hi = 0.0f;
        for (int it = 0; it < 30 && hi > 0.0f; it++) {
            float mid = 0.5f * (lo + hi);
            if (lp_mag(mid, w) > target) lo = mid; else hi = mid;
        }
        r->lp_a[i] = hi > 0.0f ? 0.5f * (lo + hi) : 0.0f;
    }
    /* keep perceived level roughly independent of decay length */
    r->out_gain = volume * 0.35f;
    r->active = volume > 0.0f;
}

void rl_reverb_process(rl_reverb *r, const float *in, float *out, uint32_t frames)
{
    if (!r->mem) return;
    for (uint32_t n = 0; n < frames; n++) {
        /* pre-delay */
        float x = r->pre[r->pre_pos];
        r->pre[r->pre_pos] = in[n];
        if (++r->pre_pos >= r->pre_len) r->pre_pos = 0;

        /* input diffusion */
        for (int i = 0; i < RL_AP_STAGES; i++) {
            float d = r->ap[i][r->ap_pos[i]];
            float y = d - k_ap_g * x;
            r->ap[i][r->ap_pos[i]] = x + k_ap_g * y;
            if (++r->ap_pos[i] >= r->ap_len[i]) r->ap_pos[i] = 0;
            x = y;
        }

        /* FDN read + damping */
        float d[RL_FDN_LINES], sum = 0.0f;
        for (int i = 0; i < RL_FDN_LINES; i++) {
            float s = r->line[i][r->pos[i]];
            r->lp_z[i] = s + r->lp_a[i] * (r->lp_z[i] - s);   /* (1-a)s + a*z */
            d[i] = r->lp_z[i] * r->fb[i];
            sum += d[i];
        }
        /* Householder feedback matrix: I - 2/N * 11^T */
        float h = sum * (2.0f / RL_FDN_LINES);
        for (int i = 0; i < RL_FDN_LINES; i++) {
            r->line[i][r->pos[i]] = d[i] - h + x;
            if (++r->pos[i] >= r->len[i]) r->pos[i] = 0;
        }

        if (r->active) {
            out[n * 2]     += (d[0] - d[1] + d[2]) * r->out_gain;
            out[n * 2 + 1] += (d[1] + d[2] - d[3]) * r->out_gain;
        }
    }
}
