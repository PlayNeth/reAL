#include "hrtf.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

int rl_hrtf_load(rl_hrtf_set *set, const void *data, uint32_t size, uint32_t out_rate)
{
    const uint8_t *p = data, *end = p + size;
    memset(set, 0, sizeof(*set));
    if (size < 20 || memcmp(p, "RLHRTF01", 8)) return 0;
    uint32_t rate = rd32(p + 8), taps = rd32(p + 12), nr = rd32(p + 16);
    p += 20;
    if (!rate || !taps || taps > RL_HRTF_MAX_TAPS || !nr || nr > RL_HRTF_MAX_RINGS) return 0;
    uint32_t n = 0;
    for (uint32_t r = 0; r < nr; r++) {
        if (p + 8 > end) return 0;
        set->ring_el[r] = rdf(p);
        set->ring_naz[r] = rd32(p + 4);
        set->ring_first[r] = n;
        if (!set->ring_naz[r] || set->ring_naz[r] > 1024) return 0;
        n += set->ring_naz[r];
        p += 8;
    }
    if ((uint64_t)(end - p) < (uint64_t)n * (8 + 8 * taps)) return 0;
    set->dirs = calloc(n, sizeof(rl_hrtf_filter));
    if (!set->dirs) return 0;

    /* resampling of the filters if the device doesn't run at the measured rate */
    double ratio = out_rate ? (double)out_rate / rate : 1.0;
    uint32_t otaps = (uint32_t)ceil(taps * ratio);
    if (otaps > RL_HRTF_MAX_TAPS) otaps = RL_HRTF_MAX_TAPS;
    float tmp[RL_HRTF_MAX_TAPS];
    for (uint32_t i = 0; i < n; i++) {
        rl_hrtf_filter *f = &set->dirs[i];
        f->delay[0] = rdf(p) * (float)ratio;
        f->delay[1] = rdf(p + 4) * (float)ratio;
        p += 8;
        for (int ch = 0; ch < 2; ch++) {
            for (uint32_t k = 0; k < taps; k++) tmp[k] = rdf(p + 4 * k);
            p += 4 * taps;
            if (ratio == 1.0) {
                memcpy(f->coef[ch], tmp, taps * sizeof(float));
            } else {
                /* linear interpolation of the impulse response, scaled to keep the gain */
                for (uint32_t k = 0; k < otaps; k++) {
                    double t = k / ratio;
                    uint32_t i0 = (uint32_t)t;
                    double fr = t - i0;
                    float a = i0 < taps ? tmp[i0] : 0.0f, b = i0 + 1 < taps ? tmp[i0 + 1] : 0.0f;
                    f->coef[ch][k] = (float)((a + (b - a) * fr) / ratio);
                }
            }
        }
    }
    set->rate = out_rate ? out_rate : rate;
    set->taps = ratio == 1.0 ? taps : otaps;
    set->n_rings = nr;
    set->n_dirs = n;
    return 1;
}

void rl_hrtf_free(rl_hrtf_set *set)
{
    free(set->dirs);
    memset(set, 0, sizeof(*set));
}

static void mix_filter(rl_hrtf_filter *o, const rl_hrtf_filter *a, float w, uint32_t taps, int first)
{
    for (int ch = 0; ch < 2; ch++) {
        if (first) { o->delay[ch] = 0.0f; memset(o->coef[ch], 0, sizeof(o->coef[ch])); }
        o->delay[ch] += a->delay[ch] * w;
        for (uint32_t k = 0; k < taps; k++) o->coef[ch][k] += a->coef[ch][k] * w;
    }
}

/* filter on one ring at azimuth az (linear between the two neighbouring measurements) */
static void ring_lookup(const rl_hrtf_set *s, uint32_t r, float az, rl_hrtf_filter *o, float w, int first)
{
    uint32_t n = s->ring_naz[r];
    const rl_hrtf_filter *d = s->dirs + s->ring_first[r];
    if (n == 1) { mix_filter(o, d, w, s->taps, first); return; }
    float pos = az * n / 360.0f;
    uint32_t i0 = (uint32_t)floorf(pos) % n, i1 = (i0 + 1) % n;
    float fr = pos - floorf(pos);
    mix_filter(o, d + i0, w * (1.0f - fr), s->taps, first);
    mix_filter(o, d + i1, w * fr, s->taps, 0);
}

void rl_hrtf_lookup(const rl_hrtf_set *s, float az, float el, rl_hrtf_filter *out)
{
    az = fmodf(az, 360.0f);
    if (az < 0.0f) az += 360.0f;
    uint32_t nr = s->n_rings;
    if (el <= s->ring_el[0]) { ring_lookup(s, 0, az, out, 1.0f, 1); return; }
    if (el >= s->ring_el[nr - 1]) { ring_lookup(s, nr - 1, az, out, 1.0f, 1); return; }
    uint32_t r = 0;
    while (r + 1 < nr && s->ring_el[r + 1] < el) r++;
    float fr = (el - s->ring_el[r]) / (s->ring_el[r + 1] - s->ring_el[r]);
    ring_lookup(s, r, az, out, 1.0f - fr, 1);
    ring_lookup(s, r + 1, az, out, fr, 0);
}

void rl_hrtf_voice_reset(rl_hrtf_voice *v)
{
    memset(v, 0, sizeof(*v));
}

void rl_hrtf_voice_restart(rl_hrtf_voice *v)
{
    memset(v->hist, 0, sizeof(v->hist));
    v->w = 0;
    if (v->have_cur) { v->cur = v->target; v->dirty = 0; }
}

void rl_hrtf_voice_set(rl_hrtf_voice *v, const rl_hrtf_filter *f)
{
    /* reversed (coef[RL_HRTF_MAX_TAPS - 1 - k] = h[k]) so the convolution reads both arrays forwards */
    v->target.delay[0] = f->delay[0];
    v->target.delay[1] = f->delay[1];
    for (int ch = 0; ch < 2; ch++)
        for (int k = 0; k < RL_HRTF_MAX_TAPS; k++)
            v->target.coef[ch][RL_HRTF_MAX_TAPS - 1 - k] = f->coef[ch][k];
    if (!v->have_cur) { v->cur = *f; v->have_cur = 1; v->dirty = 0; }
    else v->dirty = 1;
}

static inline void conv(const rl_hrtf_voice *v, const rl_hrtf_filter *f, uint32_t taps, uint32_t w, float *l, float *r)
{
    const uint32_t mask = RL_HRTF_HIST - 1;
    float acc[2];
    for (int ch = 0; ch < 2; ch++) {
        uint32_t d = (uint32_t)(f->delay[ch] + 0.5f);
        /* x[w - d - k] for k < taps is contiguous: x0[j] = x[w - d - (taps - 1) + j] */
        const float *x0 = v->hist + ((w - d) & mask) + RL_HRTF_HIST - (taps - 1);
        const float *c = f->coef[ch] + RL_HRTF_MAX_TAPS - taps;          /* reversed: c[j] = h[taps - 1 - j] */
        float a = 0.0f;
        for (uint32_t j = 0; j < taps; j++) a += c[j] * x0[j];
        acc[ch] = a;
    }
    *l = acc[0];
    *r = acc[1];
}

void rl_hrtf_render(rl_hrtf_voice *v, const rl_hrtf_set *s, const float *in, float *out, uint32_t frames)
{
    const uint32_t mask = RL_HRTF_HIST - 1, taps = s->taps;
    /* crossfade from cur to target across this block (at most ~5 ms) */
    uint32_t xf = 0;
    if (v->dirty) {
        xf = s->rate / 200;
        if (xf > frames) xf = frames;
        if (xf < 1) xf = 1;
    }
    for (uint32_t n = 0; n < frames; n++) {
        v->hist[v->w & mask] = in[n];
        v->hist[(v->w & mask) + RL_HRTF_HIST] = in[n];
        float l, r;
        conv(v, &v->cur, taps, v->w, &l, &r);
        if (n < xf) {
            float l2, r2, t = (float)(n + 1) / (float)xf;
            conv(v, &v->target, taps, v->w, &l2, &r2);
            l += (l2 - l) * t;
            r += (r2 - r) * t;
            if (n + 1 == xf) { v->cur = v->target; v->dirty = 0; }
        }
        out[n * 2] += l;
        out[n * 2 + 1] += r;
        v->w++;
    }
}
