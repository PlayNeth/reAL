/* reAL - EAX 2.0 / 3.0 property handling, ported from ALchemy (see eax.h).
 *
 * The arithmetic follows ALchemy's x87 code: intermediate results are rounded to float exactly where
 * ALchemy stores them, the rest is done in double (ALchemy runs the FPU at 53-bit precision).  Build
 * this file without -ffast-math. */
#include "eax.h"
#include <math.h>
#include <string.h>

#include "eax_presets.h"   /* k_eax3_presets[26], k_eax2_default */

/* ------------------------------------------------------------------ shared helpers */

/* millibels -> linear as ALchemy does it everywhere: 2^(log2(10) * (float)(mb * 0.0005)) */
static float mb_lin(int32_t mb)
{
    float x = (float)((double)mb * 0.0005000000237487257);
    return (float)exp2((double)3.321928024291992f * (double)x);
}

/* reverb property setters (fns 0x10043fe0 etc.): nothing below -9999 mB */
static float mb_lin_l(int32_t mb) { return mb < -9999 ? 0.0f : mb_lin(mb); }

/* the source conversions' gain: 0 at or below -10000 mB, 1 at or above 0 mB */
static float mb_gain(int32_t mb)
{
    if (mb <= -10000) return 0.0f;
    if (mb >= 0) return 1.0f;
    return mb_lin(mb);
}

/* ALchemy's float -> int for millibel sums: round-to-nearest of v -/+ (0.5 - 1.5e-8), i.e. truncation */
static int32_t trunc_mb(float v)
{
    volatile double t = v;
    if (v < 0.0f) t = (t + 0.5) - 1.4999999464748726e-08;
    else          t = (t - 0.5) + 1.4999999464748726e-08;
    t = t + 6755399441055744.0;
    t = t - 6755399441055744.0;
    return (int32_t)(int64_t)t;
}

/* _ftol: truncation toward zero */
static int32_t ftol(double v) { return (int32_t)v; }

static int in_range(float v, float lo, float hi) { return v >= lo && v <= hi; }   /* false for NaN */
static int mb_ok(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }
static int fne(float a, float b) { return !(a == b); }                              /* "changed" */

/* size -> reverb size (fn 0x10044290) */
static float rv_size(float s)
{
    float v = (float)((double)(float)pow((double)s, 3.0) * 0.0625);
    return v > 1.0f ? 1.0f : v;
}

/* pan vector -> (angle, spread) (fn 0x100442f0); used by the reverb's output panning (calc_pan in
 * alrv_params.c) */
static void rv_pan(float out[2], const float v[3])
{
    float x = v[0], y = v[1], z = v[2];
    float m = (float)(((double)x * x + (double)y * y) + (double)z * z);
    if (m > 1.0025000572204590f) {
        float l = (float)sqrt((double)m);
        x = (float)((double)x / l);
        z = (float)((double)z / l);
    }
    float f = (float)((double)x * x + (double)z * z);
    if (!(f < 0.0025000001769512892f)) {
        out[1] = f <= 0.9975000023841858 ? (float)(1.0 - (double)(float)sqrt((double)f)) : 0.0f;
        out[0] = (float)((double)(float)atan2((double)x, (double)z) * 57.2957763671875);
    } else {
        out[1] = 1.0f;
        out[0] = 0.0f;
    }
}

/* decay HF with EAXLISTENERFLAGS_DECAYHFLIMIT */
static float decay_hf(float ratio, float decay, float air, uint32_t flags)
{
    float a = (float)((double)ratio * (double)decay);
    if ((flags & EAXL_DECAYHFLIMIT) && air != 0.0f) {
        float b = (float)(-17.52438785685992 / (double)air);
        return b <= a ? b : a;
    }
    return a;
}

/* ------------------------------------------------------------------ EAX 2 listener */

void eax2l_reset(eax2_listener *l)
{
    memset(l, 0, sizeof(*l));
    l->p = k_eax2_default;
    l->s_reverb = l->s_reverb_delay = l->s_decay = l->s_reflections = l->s_refl_delay = l->p.env_size;
    l->b_reverb_delay = l->p.reverb_delay;
    l->b_decay = l->p.decay_time;
    l->b_refl_delay = l->p.reflections_delay;
    l->b_reverb = l->p.reverb;
    l->b_reflections = l->p.reflections;
    l->dirty = 0x80003fff;
    /* ALchemy converts the listener before the sources; reAL may convert a source first */
    l->decay_hf = decay_hf(l->p.decay_hf_ratio, l->p.decay_time, l->p.air_absorption_hf, l->p.flags);
}

/* environment preset (fn 0x1004b4b0) */
static void eax2l_env(eax2_listener *l, uint32_t env)
{
    const eax3_reverb *P = &k_eax3_presets[env];
    uint32_t d = 0;
    l->s_reverb = l->s_reverb_delay = l->s_decay = l->s_reflections = l->s_refl_delay = P->env_size;
    if (l->p.environment != env) { l->p.environment = env; d |= 0x200; }
    if (l->p.room != P->room) { l->p.room = P->room; d |= 1; }
    if (l->p.room_hf != P->room_hf) { l->p.room_hf = P->room_hf; d |= 2; }
    if (fne(P->room_rolloff, l->p.room_rolloff)) { d |= 4; l->p.room_rolloff = P->room_rolloff; }
    if (fne(P->decay_time, l->p.decay_time)) { d |= 8; l->b_decay = l->p.decay_time = P->decay_time; }
    if (fne(P->decay_hf_ratio, l->p.decay_hf_ratio)) { d |= 0x10; l->p.decay_hf_ratio = P->decay_hf_ratio; }
    if (l->p.reflections != P->reflections) { l->b_reflections = l->p.reflections = P->reflections; d |= 0x20; }
    if (fne(P->reflections_delay, l->p.reflections_delay)) { d |= 0x40; l->b_refl_delay = l->p.reflections_delay = P->reflections_delay; }
    if (l->p.reverb != P->reverb) { l->b_reverb = l->p.reverb = P->reverb; d |= 0x80; }
    if (fne(P->reverb_delay, l->p.reverb_delay)) { d |= 0x100; l->b_reverb_delay = l->p.reverb_delay = P->reverb_delay; }
    if (fne(P->env_size, l->p.env_size)) { d |= 0x400; l->p.env_size = P->env_size; }
    if (fne(P->env_diffusion, l->p.env_diffusion)) { d |= 0x800; l->p.env_diffusion = P->env_diffusion; }
    if (fne(P->air_absorption_hf, l->p.air_absorption_hf)) { d |= 0x1000; l->p.air_absorption_hf = P->air_absorption_hf; }
    if (l->p.flags != P->flags) {
        if ((P->flags ^ l->p.flags) & EAXL_DECAYHFLIMIT) d |= 0x10;
        l->p.flags = P->flags;
        d |= 0x2000;
    }
    l->dirty |= d;
}

/* environment size (fn 0x1004b660); EAX 3 has the same logic (fn 0x10044820) */
typedef struct size_ref {
    uint32_t flags;
    float *size;
    float *decay, *b_decay, *s_decay;
    float *refl_delay, *b_refl_delay, *s_refl_delay;
    int32_t *reflections, *b_reflections; float *s_reflections;
    int32_t *reverb, *b_reverb; float *s_reverb;
    float *reverb_delay, *b_reverb_delay, *s_reverb_delay;
    float *echo, *b_echo, *s_echo;
    float *mod, *b_mod, *s_mod;
    uint32_t *dirty;
    uint32_t d_decay, d_refl_delay, d_refl, d_reverb, d_reverb_delay, d_echo, d_mod, d_size;
} size_ref;

static float ratio(float size, float base, float base_size)   /* fmul base; fdiv base_size; fstp float */
{
    return (float)((double)size * (double)base / (double)base_size);
}

static int32_t log_mb(float size, float base_size, float k)       /* log10 ratio, scaled, truncated */
{
    float r = (float)((double)size / (double)base_size);
    float lg = (float)log10((double)r);
    return ftol((double)lg * (double)k);
}

static void scale_size(const size_ref *s, float size)
{
    uint32_t f = s->flags;
    if (f & EAXL_DECAYTIMESCALE) {
        float v = ratio(size, *s->b_decay, *s->s_decay);
        *s->decay = v < 0.1f ? 0.1f : v > 20.0f ? 20.0f : v;
        *s->dirty |= s->d_decay;
    }
    if (f & EAXL_REFLECTIONSDELAYSCALE) {
        float v = ratio(size, *s->b_refl_delay, *s->s_refl_delay);
        *s->refl_delay = v < 0.0f ? 0.0f : v > 0.3f ? 0.3f : v;
        *s->dirty |= s->d_refl_delay;
        if (f & EAXL_REFLECTIONSSCALE) {     /* ALchemy only scales reflections together with their delay */
            int32_t v2 = *s->b_reflections - log_mb(size, *s->s_reflections, 2000.0f);
            *s->reflections = v2 < -10000 ? -10000 : v2 > 1000 ? 1000 : v2;
            *s->dirty |= s->d_refl;
        }
    }
    if (f & EAXL_REVERBSCALE) {
        float k_new = 3000.0f;
        if (f & EAXL_DECAYTIMESCALE) {
            float old = ratio(*s->size, *s->b_decay, *s->s_decay);
            float nw = ratio(size, *s->b_decay, *s->s_decay);
            k_new = 2000.0f;
            if (nw > 20.0f && old > 20.0f) {
                k_new = 3000.0f;
            } else if (nw > 20.0f || old > 20.0f) {
                /* the decay time crosses its 20 s limit: rebase the reverb level at the crossing */
                float k_old = 2000.0f;
                if (old >= 20.0f) k_old = 3000.0f; else k_new = 3000.0f;
                float sx = (float)((double)*s->s_decay * 20.0 / (double)*s->b_decay);
                int32_t v = *s->b_reverb - log_mb(sx, *s->s_reverb, k_old);
                if (v < -10000) v = -10000;
                else if (v > 2000) v = 2000;
                *s->s_reverb = sx;
                *s->b_reverb = v;
                *s->reverb = v;
            }
        }
        int32_t v = *s->b_reverb - log_mb(size, *s->s_reverb, k_new);
        *s->reverb = v < -10000 ? -10000 : v > 2000 ? 2000 : v;
        *s->dirty |= s->d_reverb;
    }
    if (f & EAXL_REVERBDELAYSCALE) {
        float v = ratio(size, *s->b_reverb_delay, *s->s_reverb_delay);
        *s->reverb_delay = v < 0.0f ? 0.0f : v > 0.1f ? 0.1f : v;
        *s->dirty |= s->d_reverb_delay;
    }
    if (s->echo && (f & EAXL_ECHOTIMESCALE)) {
        float v = ratio(size, *s->b_echo, *s->s_echo);
        *s->echo = v < 0.075f ? 0.075f : v > 0.25f ? 0.25f : v;
        *s->dirty |= s->d_echo;
    }
    if (s->mod && (f & EAXL_MODULATIONTIMESCALE)) {
        float v = ratio(size, *s->b_mod, *s->s_mod);
        *s->mod = v < 0.04f ? 0.04f : v > 4.0f ? 4.0f : v;
        *s->dirty |= s->d_mod;
    }
    *s->size = size;
    *s->dirty |= s->d_size;
}

static void eax2l_size(eax2_listener *l, float size)
{
    size_ref s = { l->p.flags, &l->p.env_size,
        &l->p.decay_time, &l->b_decay, &l->s_decay,
        &l->p.reflections_delay, &l->b_refl_delay, &l->s_refl_delay,
        &l->p.reflections, &l->b_reflections, &l->s_reflections,
        &l->p.reverb, &l->b_reverb, &l->s_reverb,
        &l->p.reverb_delay, &l->b_reverb_delay, &l->s_reverb_delay,
        NULL, NULL, NULL, NULL, NULL, NULL, &l->dirty,
        0x8, 0x40, 0x20, 0x80, 0x100, 0, 0, 0x400 };
    scale_size(&s, size);
}

/* EAXLISTENER_FLAGS change: the scaled properties restart from their current values (fn 0x1004ba90) */
static void eax2l_flags(eax2_listener *l, uint32_t v)
{
    if (l->p.flags == v) return;
    uint32_t x = l->p.flags ^ v;
    if (x & EAXL_DECAYHFLIMIT) l->dirty |= 0x10;
    if (x & EAXL_REVERBSCALE) { l->s_reverb = l->p.env_size; l->b_reverb = l->p.reverb; }
    if (x & EAXL_REVERBDELAYSCALE) { l->b_reverb_delay = l->p.reverb_delay; l->s_reverb_delay = l->p.env_size; }
    if (x & EAXL_DECAYTIMESCALE) { l->b_decay = l->p.decay_time; l->s_decay = l->p.env_size; }
    if (x & EAXL_REFLECTIONSSCALE) { l->s_reflections = l->p.env_size; l->b_reflections = l->p.reflections; }
    if (x & EAXL_REFLECTIONSDELAYSCALE) { l->b_refl_delay = l->p.reflections_delay; l->s_refl_delay = l->p.env_size; }
    l->p.flags = v;
    l->dirty |= 0x2000;
}

int eax2l_set(eax2_listener *l, uint32_t id, const void *data, uint32_t len)
{
    id &= ~EAX_DEFERRED;
    if (id == EAX2L_NONE) return EAX_OK;
    if (id >= EAX2L_COUNT) return EAX_BADID;
    if (!data || len < (id == EAX2L_ALL ? sizeof(eax2_reverb) : 4)) return EAX_BADSIZE;
    int32_t iv; float fv; uint32_t uv;
    memcpy(&iv, data, 4); memcpy(&fv, data, 4); memcpy(&uv, data, 4);
    switch (id) {
    case EAX2L_ALL: {           /* fn 0x1004ae70 */
        eax2_reverb r; memcpy(&r, data, sizeof(r));
        if (!mb_ok(r.room, -10000, 0) || !mb_ok(r.room_hf, -10000, 0) || !in_range(r.room_rolloff, 0.0f, 10.0f) ||
            !in_range(r.decay_time, 0.1f, 20.0f) || !in_range(r.decay_hf_ratio, 0.1f, 2.0f) ||
            !mb_ok(r.reflections, -10000, 1000) || !in_range(r.reflections_delay, 0.0f, 0.3f) ||
            !mb_ok(r.reverb, -10000, 2000) || !in_range(r.reverb_delay, 0.0f, 0.1f) || r.environment >= 26 ||
            isnan(r.env_size) || !in_range(r.env_diffusion, 0.0f, 1.0f) ||
            !in_range(r.air_absorption_hf, -100.0f, 0.0f) || (r.flags & ~0x3fu))
            return EAX_BADVALUE;
        uint32_t d = 0;
        float sz = r.env_size < 1.0f ? 1.0f : r.env_size > 100.0f ? 100.0f : r.env_size;
        /* the sizes the scaling divides by and takes log10 of: a size of (next to) 0, below or infinite (which
         * the size itself is clamped from) would make them inf / NaN and the next size change's levels undefined */
        float ss = r.env_size >= 1e-30f && isfinite(r.env_size) ? r.env_size : sz;
        l->s_reverb = l->s_reverb_delay = l->s_decay = l->s_reflections = l->s_refl_delay = ss;
        if (l->p.room != r.room) { l->p.room = r.room; d |= 1; }
        if (l->p.room_hf != r.room_hf) { l->p.room_hf = r.room_hf; d |= 2; }
        if (fne(r.room_rolloff, l->p.room_rolloff)) { d |= 4; l->p.room_rolloff = r.room_rolloff; }
        if (fne(r.decay_time, l->p.decay_time)) { d |= 8; l->b_decay = l->p.decay_time = r.decay_time; }
        if (fne(r.decay_hf_ratio, l->p.decay_hf_ratio)) { d |= 0x10; l->p.decay_hf_ratio = r.decay_hf_ratio; }
        if (l->p.reflections != r.reflections) { l->b_reflections = l->p.reflections = r.reflections; d |= 0x20; }
        if (fne(r.reflections_delay, l->p.reflections_delay)) { d |= 0x40; l->b_refl_delay = l->p.reflections_delay = r.reflections_delay; }
        if (l->p.reverb != r.reverb) { l->b_reverb = l->p.reverb = r.reverb; d |= 0x80; }
        if (fne(r.reverb_delay, l->p.reverb_delay)) { d |= 0x100; l->b_reverb_delay = l->p.reverb_delay = r.reverb_delay; }
        if (l->p.environment != r.environment) { l->p.environment = r.environment; d |= 0x200; }
        if (fne(sz, l->p.env_size)) { l->p.env_size = sz; d |= 0x400; }
        if (fne(r.env_diffusion, l->p.env_diffusion)) { d |= 0x800; l->p.env_diffusion = r.env_diffusion; }
        if (fne(r.air_absorption_hf, l->p.air_absorption_hf)) { d |= 0x1000; l->p.air_absorption_hf = r.air_absorption_hf; }
        if (l->p.flags != r.flags) {
            if ((l->p.flags ^ r.flags) & EAXL_DECAYHFLIMIT) d |= 0x10;
            l->p.flags = r.flags;
            d |= 0x2000;
        }
        l->dirty |= d;
        return EAX_OK;
    }
    case EAX2L_ROOM:
        if (!mb_ok(iv, -10000, 0)) return EAX_BADVALUE;
        if (l->p.room != iv) { l->p.room = iv; l->dirty |= 1; }
        return EAX_OK;
    case EAX2L_ROOMHF:
        if (!mb_ok(iv, -10000, 0)) return EAX_BADVALUE;
        if (l->p.room_hf != iv) { l->p.room_hf = iv; l->dirty |= 2; }
        return EAX_OK;
    case EAX2L_ROOMROLLOFF:
        if (!in_range(fv, 0.0f, 10.0f)) return EAX_BADVALUE;
        if (fne(fv, l->p.room_rolloff)) { l->p.room_rolloff = fv; l->dirty |= 4; }
        return EAX_OK;
    case EAX2L_DECAYTIME:
        if (!in_range(fv, 0.1f, 20.0f)) return EAX_BADVALUE;
        l->s_decay = l->p.env_size;
        if (fne(fv, l->p.decay_time)) { l->b_decay = l->p.decay_time = fv; l->dirty |= 8; }
        return EAX_OK;
    case EAX2L_DECAYHFRATIO:
        if (!in_range(fv, 0.1f, 2.0f)) return EAX_BADVALUE;
        if (fne(fv, l->p.decay_hf_ratio)) { l->p.decay_hf_ratio = fv; l->dirty |= 0x10; }
        return EAX_OK;
    case EAX2L_REFLECTIONS:
        if (!mb_ok(iv, -10000, 1000)) return EAX_BADVALUE;
        l->s_reflections = l->p.env_size;
        if (l->p.reflections != iv) { l->b_reflections = l->p.reflections = iv; l->dirty |= 0x20; }
        return EAX_OK;
    case EAX2L_REFLECTIONSDELAY:
        if (!in_range(fv, 0.0f, 0.3f)) return EAX_BADVALUE;
        l->s_refl_delay = l->p.env_size;
        if (fne(fv, l->p.reflections_delay)) { l->b_refl_delay = l->p.reflections_delay = fv; l->dirty |= 0x40; }
        return EAX_OK;
    case EAX2L_REVERB:
        if (!mb_ok(iv, -10000, 2000)) return EAX_BADVALUE;
        l->s_reverb = l->p.env_size;
        if (l->p.reverb != iv) { l->b_reverb = l->p.reverb = iv; l->dirty |= 0x80; }
        return EAX_OK;
    case EAX2L_REVERBDELAY:
        if (!in_range(fv, 0.0f, 0.1f)) return EAX_BADVALUE;
        l->s_reverb_delay = l->p.env_size;
        if (fne(fv, l->p.reverb_delay)) { l->b_reverb_delay = l->p.reverb_delay = fv; l->dirty |= 0x100; }
        return EAX_OK;
    case EAX2L_ENVIRONMENT:
        if (uv >= 26) return EAX_BADVALUE;
        eax2l_env(l, uv);
        return EAX_OK;
    case EAX2L_ENVIRONMENTSIZE:
        if (!in_range(fv, 1.0f, 100.0f)) return EAX_BADVALUE;
        if (fv == l->p.env_size) return EAX_OK;
        eax2l_size(l, fv);
        return EAX_OK;
    case EAX2L_ENVIRONMENTDIFFUSION:
        if (!in_range(fv, 0.0f, 1.0f)) return EAX_BADVALUE;
        if (fne(fv, l->p.env_diffusion)) { l->p.env_diffusion = fv; l->dirty |= 0x800; }
        return EAX_OK;
    case EAX2L_AIRABSORPTIONHF:
        if (!in_range(fv, -100.0f, 0.0f)) return EAX_BADVALUE;
        if (fne(fv, l->p.air_absorption_hf)) { l->p.air_absorption_hf = fv; l->dirty |= 0x1000; }
        return EAX_OK;
    case EAX2L_FLAGS:
        if (uv & ~0x3fu) return EAX_BADVALUE;
        eax2l_flags(l, uv);
        return EAX_OK;
    }
    return EAX_BADID;
}

int eax2l_get(const eax2_listener *l, uint32_t id, void *data, uint32_t len, uint32_t *ret)
{
    id &= ~EAX_DEFERRED;
    if (id == EAX2L_NONE) { if (ret) *ret = 0; return EAX_OK; }
    if (id >= EAX2L_COUNT) return EAX_BADID;
    uint32_t n = id == EAX2L_ALL ? sizeof(eax2_reverb) : 4;
    if (!data || len < n) return EAX_BADSIZE;
    const char *p = (const char *)&l->p;
    memcpy(data, id == EAX2L_ALL ? p : p + 4 * (id - 2), n);
    if (ret) *ret = n;
    return EAX_OK;
}

int eax2l_apply(eax2_listener *l, alrv_props *e)
{
    uint32_t d = l->dirty;
    l->dirty = 0;
    l->taken = d;
    if (!d) return 0;
    if ((int32_t)d < 0) alrv_props_default(e);
    uint32_t env = l->p.environment > 25 ? 25 : l->p.environment;
    const eax3_reverb *P = &k_eax3_presets[env];
    if (d & 0x200) {
        e->room_lf = mb_lin_l(P->room_lf);
        e->echo_time = P->echo_time;
        e->echo_depth = env == 24 ? 0.0f : P->echo_depth;
        e->mod_time = P->mod_time;
        e->mod_depth = P->mod_depth;
        e->hf_ref = P->hf_reference;
        e->lf_ref = P->lf_reference;
    }
    if (d & 0x400) e->size = rv_size(l->p.env_size);
    if (d & 0x800) e->diffusion = l->p.env_diffusion;
    if (d & 1) e->room = mb_lin_l(l->p.room);
    if (d & 2) e->room_hf = mb_lin_l(l->p.room_hf);
    if (d & 8) e->decay = l->p.decay_time;
    if (d & 0x1018) {
        l->decay_hf = decay_hf(l->p.decay_hf_ratio, l->p.decay_time, l->p.air_absorption_hf, l->p.flags);
        l->taken |= 0x1000000;
        e->decay_hf = l->decay_hf;
    }
    if (d & 0x208) e->decay_lf = (float)((double)P->decay_lf_ratio * (double)l->p.decay_time);
    if (d & 0x20) e->reflections = mb_lin_l(l->p.reflections);
    if (d & 0x40) e->reflections_delay = l->p.reflections_delay;
    if (d & 0x80) e->reverb = mb_lin_l(l->p.reverb);
    if (d & 0x100) e->reverb_delay = l->p.reverb_delay;
    return 1;
}

/* ------------------------------------------------------------------ EAX 3 listener */

/* load a whole property set (fn 0x100459b0) */
static void eax3l_load(eax3_listener *l, const eax3_reverb *r)
{
    uint32_t d = 0;
    l->p.environment = r->environment;
    l->env_copy = r->environment;
    l->s_reverb = l->s_reverb_delay = l->s_decay = l->s_reflections = l->s_refl_delay = l->s_echo = l->s_mod = r->env_size;
    if (fne(r->env_size, l->p.env_size)) { d |= 2; l->p.env_size = r->env_size; }
    if (fne(r->env_diffusion, l->p.env_diffusion)) { d |= 4; l->p.env_diffusion = r->env_diffusion; }
    if (l->p.room != r->room) { l->p.room = r->room; d |= 8; }
    if (l->p.room_hf != r->room_hf) { l->p.room_hf = r->room_hf; d |= 0x10; }
    if (l->p.room_lf != r->room_lf) { l->p.room_lf = r->room_lf; d |= 0x20; }
    if (fne(r->decay_time, l->p.decay_time)) { d |= 0x40; l->b_decay = l->p.decay_time = r->decay_time; }
    if (fne(r->decay_hf_ratio, l->p.decay_hf_ratio)) { d |= 0x80; l->p.decay_hf_ratio = r->decay_hf_ratio; }
    if (fne(r->decay_lf_ratio, l->p.decay_lf_ratio)) { d |= 0x100; l->p.decay_lf_ratio = r->decay_lf_ratio; }
    if (l->p.reflections != r->reflections) { l->b_reflections = l->p.reflections = r->reflections; d |= 0x200; }
    if (fne(r->reflections_delay, l->p.reflections_delay)) { d |= 0x400; l->b_refl_delay = l->p.reflections_delay = r->reflections_delay; }
    if (fne(r->reflections_pan[0], l->p.reflections_pan[0]) || fne(r->reflections_pan[1], l->p.reflections_pan[1]) ||
        fne(r->reflections_pan[2], l->p.reflections_pan[2])) {
        memcpy(l->p.reflections_pan, r->reflections_pan, sizeof(r->reflections_pan));
        d |= 0x800;
    }
    if (l->p.reverb != r->reverb) { l->b_reverb = l->p.reverb = r->reverb; d |= 0x1000; }
    if (fne(r->reverb_delay, l->p.reverb_delay)) { d |= 0x2000; l->b_reverb_delay = l->p.reverb_delay = r->reverb_delay; }
    if (fne(r->reverb_pan[0], l->p.reverb_pan[0]) || fne(r->reverb_pan[1], l->p.reverb_pan[1]) ||
        fne(r->reverb_pan[2], l->p.reverb_pan[2])) {
        memcpy(l->p.reverb_pan, r->reverb_pan, sizeof(r->reverb_pan));
        d |= 0x4000;
    }
    if (fne(r->echo_time, l->p.echo_time)) { d |= 0x8000; l->b_echo = l->p.echo_time = r->echo_time; }
    if (fne(r->echo_depth, l->p.echo_depth)) { d |= 0x10000; l->p.echo_depth = r->echo_depth; }
    if (fne(r->mod_time, l->p.mod_time)) { d |= 0x20000; l->b_mod = l->p.mod_time = r->mod_time; }
    if (fne(r->mod_depth, l->p.mod_depth)) { d |= 0x40000; l->p.mod_depth = r->mod_depth; }
    if (fne(r->air_absorption_hf, l->p.air_absorption_hf)) { d |= 0x80000; l->p.air_absorption_hf = r->air_absorption_hf; }
    if (fne(r->hf_reference, l->p.hf_reference)) { d |= 0x100000; l->p.hf_reference = r->hf_reference; }
    if (fne(r->lf_reference, l->p.lf_reference)) { d |= 0x200000; l->p.lf_reference = r->lf_reference; }
    if (fne(r->room_rolloff, l->p.room_rolloff)) { d |= 0x400000; l->p.room_rolloff = r->room_rolloff; }
    if (l->p.flags != r->flags) {
        if ((r->flags ^ l->p.flags) & EAXL_DECAYHFLIMIT) d |= 0x80;
        l->p.flags = r->flags;
        d |= 0x800000;
    }
    l->dirty |= d;
}

void eax3l_reset(eax3_listener *l)
{
    uint32_t env_copy = l->env_copy;        /* fn 0x100456b0 leaves it alone */
    memset(l, 0, sizeof(*l));
    l->env_copy = env_copy;
    l->p = k_eax3_presets[0];
    l->s_reverb = l->s_reverb_delay = l->s_decay = l->s_reflections = l->s_refl_delay = l->s_echo = l->s_mod = l->p.env_size;
    l->b_reverb_delay = l->p.reverb_delay;
    l->b_decay = l->p.decay_time;
    l->b_reverb = l->p.reverb;
    l->b_refl_delay = l->p.reflections_delay;
    l->b_reflections = l->p.reflections;
    l->b_echo = l->p.echo_time;
    l->b_mod = l->p.mod_time;
    l->dirty = 0x80ffffff;
    l->decay_hf = decay_hf(l->p.decay_hf_ratio, l->p.decay_time, l->p.air_absorption_hf, l->p.flags);
}

static void eax3l_size(eax3_listener *l, float size)
{
    size_ref s = { l->p.flags, &l->p.env_size,
        &l->p.decay_time, &l->b_decay, &l->s_decay,
        &l->p.reflections_delay, &l->b_refl_delay, &l->s_refl_delay,
        &l->p.reflections, &l->b_reflections, &l->s_reflections,
        &l->p.reverb, &l->b_reverb, &l->s_reverb,
        &l->p.reverb_delay, &l->b_reverb_delay, &l->s_reverb_delay,
        &l->p.echo_time, &l->b_echo, &l->s_echo, &l->p.mod_time, &l->b_mod, &l->s_mod, &l->dirty,
        0x40, 0x400, 0x200, 0x1000, 0x2000, 0x8000, 0x20000, 0x2 };
    scale_size(&s, size);
    l->p.environment = 26;
}

static void eax3l_flags(eax3_listener *l, uint32_t v)       /* fn 0x100455b0 */
{
    if (l->p.flags == v) return;
    uint32_t x = l->p.flags ^ v;
    if (x & EAXL_DECAYHFLIMIT) l->dirty |= 0x80;
    if (x & EAXL_REVERBSCALE) { l->s_reverb = l->p.env_size; l->b_reverb = l->p.reverb; }
    if (x & EAXL_REVERBDELAYSCALE) { l->b_reverb_delay = l->p.reverb_delay; l->s_reverb_delay = l->p.env_size; }
    if (x & EAXL_DECAYTIMESCALE) { l->b_decay = l->p.decay_time; l->s_decay = l->p.env_size; }
    if (x & EAXL_REFLECTIONSSCALE) { l->s_reflections = l->p.env_size; l->b_reflections = l->p.reflections; }
    if (x & EAXL_REFLECTIONSDELAYSCALE) { l->b_refl_delay = l->p.reflections_delay; l->s_refl_delay = l->p.env_size; }
    if (x & EAXL_ECHOTIMESCALE) { l->b_echo = l->p.echo_time; l->s_echo = l->p.env_size; }
    if (x & EAXL_MODULATIONTIMESCALE) { l->b_mod = l->p.mod_time; l->s_mod = l->p.env_size; }
    l->p.flags = v;
    l->p.environment = 26;
    l->dirty |= 0x800000;
}

/* the common shape of the single-value setters: range check, record, mark the environment custom */
#define SET3F(field, lo, hi, bit) do { \
        if (!in_range(fv, lo, hi)) return EAX_BADVALUE; \
        if (fne(fv, l->p.field)) { l->p.field = fv; l->p.environment = 26; l->dirty |= (bit); } \
        return EAX_OK; } while (0)
#define SET3I(field, lo, hi, bit) do { \
        if (!mb_ok(iv, lo, hi)) return EAX_BADVALUE; \
        if (l->p.field != iv) { l->p.field = iv; l->p.environment = 26; l->dirty |= (bit); } \
        return EAX_OK; } while (0)

int eax3l_set(eax3_listener *l, uint32_t id, const void *data, uint32_t len)
{
    id &= ~EAX_DEFERRED;
    if (id == EAX3L_NONE) return EAX_OK;
    if (id >= EAX3L_COUNT) return EAX_BADID;
    uint32_t need = id == EAX3L_ALL ? sizeof(eax3_reverb) : (id == EAX3L_REFLECTIONSPAN || id == EAX3L_REVERBPAN) ? 12 : 4;
    if (!data || len < need) return EAX_BADSIZE;
    int32_t iv; float fv; uint32_t uv;
    memcpy(&iv, data, 4); memcpy(&fv, data, 4); memcpy(&uv, data, 4);
    switch (id) {
    case EAX3L_ALL: {           /* fn 0x100444f0 */
        eax3_reverb r; memcpy(&r, data, sizeof(r));
        if (r.environment >= 27 || !(r.env_size >= 1.0f && r.env_size <= 100.0f) ||
            !in_range(r.env_diffusion, 0.0f, 1.0f) || !mb_ok(r.room, -10000, 0) || !mb_ok(r.room_hf, -10000, 0) ||
            !mb_ok(r.room_lf, -10000, 0) || !in_range(r.decay_time, 0.1f, 20.0f) ||
            !in_range(r.decay_hf_ratio, 0.1f, 2.0f) || !in_range(r.decay_lf_ratio, 0.1f, 2.0f) ||
            !mb_ok(r.reflections, -10000, 1000) || !in_range(r.reflections_delay, 0.0f, 0.3f) ||
            !mb_ok(r.reverb, -10000, 2000) || !in_range(r.reverb_delay, 0.0f, 0.1f) ||
            !in_range(r.echo_time, 0.075f, 0.25f) || !in_range(r.echo_depth, 0.0f, 1.0f) ||
            !in_range(r.mod_time, 0.04f, 4.0f) || !in_range(r.mod_depth, 0.0f, 1.0f) ||
            !in_range(r.air_absorption_hf, -100.0f, 0.0f) || !in_range(r.hf_reference, 1000.0f, 20000.0f) ||
            !in_range(r.lf_reference, 20.0f, 1000.0f) || !in_range(r.room_rolloff, 0.0f, 10.0f) || (r.flags & ~0xffu))
            return EAX_BADVALUE;
        eax3l_load(l, &r);
        return EAX_OK;
    }
    case EAX3L_ENVIRONMENT:     /* fn 0x10044800: a preset is (re)loaded unless it is already current */
        if (uv >= 26) return EAX_BADVALUE;
        if (l->p.environment != uv) eax3l_load(l, &k_eax3_presets[uv]);
        return EAX_OK;
    case EAX3L_ENVIRONMENTSIZE:
        if (!in_range(fv, 1.0f, 100.0f)) return EAX_BADVALUE;
        if (fv == l->p.env_size) return EAX_OK;
        eax3l_size(l, fv);
        return EAX_OK;
    case EAX3L_ENVIRONMENTDIFFUSION: SET3F(env_diffusion, 0.0f, 1.0f, 4);
    case EAX3L_ROOM:   SET3I(room, -10000, 0, 8);
    case EAX3L_ROOMHF: SET3I(room_hf, -10000, 0, 0x10);
    case EAX3L_ROOMLF: SET3I(room_lf, -10000, 0, 0x20);
    case EAX3L_DECAYTIME:
        if (!in_range(fv, 0.1f, 20.0f)) return EAX_BADVALUE;
        l->s_decay = l->p.env_size;
        if (fne(fv, l->p.decay_time)) { l->b_decay = l->p.decay_time = fv; l->p.environment = 26; l->dirty |= 0x40; }
        return EAX_OK;
    case EAX3L_DECAYHFRATIO: SET3F(decay_hf_ratio, 0.1f, 2.0f, 0x80);
    case EAX3L_DECAYLFRATIO: SET3F(decay_lf_ratio, 0.1f, 2.0f, 0x100);
    case EAX3L_REFLECTIONS:
        if (!mb_ok(iv, -10000, 1000)) return EAX_BADVALUE;
        l->s_reflections = l->p.env_size;
        if (l->p.reflections != iv) { l->b_reflections = l->p.reflections = iv; l->p.environment = 26; l->dirty |= 0x200; }
        return EAX_OK;
    case EAX3L_REFLECTIONSDELAY:
        if (!in_range(fv, 0.0f, 0.3f)) return EAX_BADVALUE;
        l->s_refl_delay = l->p.env_size;
        if (fne(fv, l->p.reflections_delay)) { l->b_refl_delay = l->p.reflections_delay = fv; l->p.environment = 26; l->dirty |= 0x400; }
        return EAX_OK;
    case EAX3L_REFLECTIONSPAN:
    case EAX3L_REVERBPAN: {
        float v[3]; memcpy(v, data, 12);
        if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) return EAX_BADVALUE;
        float *p = id == EAX3L_REFLECTIONSPAN ? l->p.reflections_pan : l->p.reverb_pan;
        if (fne(v[0], p[0]) || fne(v[1], p[1]) || fne(v[2], p[2])) {
            memcpy(p, v, 12);
            l->p.environment = 26;
            l->dirty |= id == EAX3L_REFLECTIONSPAN ? 0x800 : 0x4000;
        }
        return EAX_OK;
    }
    case EAX3L_REVERB:
        if (!mb_ok(iv, -10000, 2000)) return EAX_BADVALUE;
        l->s_reverb = l->p.env_size;
        if (l->p.reverb != iv) { l->b_reverb = l->p.reverb = iv; l->p.environment = 26; l->dirty |= 0x1000; }
        return EAX_OK;
    case EAX3L_REVERBDELAY:
        if (!in_range(fv, 0.0f, 0.1f)) return EAX_BADVALUE;
        l->s_reverb_delay = l->p.env_size;
        if (fne(fv, l->p.reverb_delay)) { l->b_reverb_delay = l->p.reverb_delay = fv; l->p.environment = 26; l->dirty |= 0x2000; }
        return EAX_OK;
    case EAX3L_ECHOTIME:
        if (!in_range(fv, 0.075f, 0.25f)) return EAX_BADVALUE;
        l->s_echo = l->p.env_size;
        if (fne(fv, l->p.echo_time)) { l->b_echo = l->p.echo_time = fv; l->p.environment = 26; l->dirty |= 0x8000; }
        return EAX_OK;
    case EAX3L_ECHODEPTH: SET3F(echo_depth, 0.0f, 1.0f, 0x10000);
    case EAX3L_MODULATIONTIME:
        if (!in_range(fv, 0.04f, 4.0f)) return EAX_BADVALUE;
        l->s_mod = l->p.env_size;
        if (fne(fv, l->p.mod_time)) { l->b_mod = l->p.mod_time = fv; l->p.environment = 26; l->dirty |= 0x20000; }
        return EAX_OK;
    case EAX3L_MODULATIONDEPTH: SET3F(mod_depth, 0.0f, 1.0f, 0x40000);
    case EAX3L_AIRABSORPTIONHF: {
        /* ALchemy (fn 0x100453f0) stores the value but only flags a nonzero one, so going back to 0
         * leaves the reverb's HF decay limited by the old value until something else changes.  reAL
         * also flags a change to 0 (setting 0 when it already is 0 stays a no-op). */
        if (!in_range(fv, -100.0f, 0.0f)) return EAX_BADVALUE;
        float old = l->p.air_absorption_hf;
        l->p.air_absorption_hf = fv;
        if (fv == 0.0f && old == 0.0f) return EAX_OK;
        }
        l->p.environment = 26;
        l->dirty |= 0x80000;
        return EAX_OK;
    case EAX3L_HFREFERENCE: SET3F(hf_reference, 1000.0f, 20000.0f, 0x100000);
    case EAX3L_LFREFERENCE: SET3F(lf_reference, 20.0f, 1000.0f, 0x200000);
    case EAX3L_ROOMROLLOFFFACTOR: SET3F(room_rolloff, 0.0f, 10.0f, 0x400000);
    case EAX3L_FLAGS:
        if (uv & ~0xffu) return EAX_BADVALUE;
        eax3l_flags(l, uv);
        return EAX_OK;
    }
    return EAX_BADID;
}

int eax3l_get(const eax3_listener *l, uint32_t id, void *data, uint32_t len, uint32_t *ret)
{
    static const uint8_t off[EAX3L_COUNT] = { 0, 0, 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28,
        0x2c, 0x38, 0x3c, 0x40, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64, 0x68, 0x6c };
    id &= ~EAX_DEFERRED;
    if (id == EAX3L_NONE) { if (ret) *ret = 0; return EAX_OK; }
    if (id >= EAX3L_COUNT) return EAX_BADID;
    uint32_t n = id == EAX3L_ALL ? sizeof(eax3_reverb) : (id == EAX3L_REFLECTIONSPAN || id == EAX3L_REVERBPAN) ? 12 : 4;
    if (!data || len < n) return EAX_BADSIZE;
    memcpy(data, (const char *)&l->p + off[id], n);
    if (ret) *ret = n;
    return EAX_OK;
}

int eax3l_apply(eax3_listener *l, alrv_props *e)
{
    uint32_t d = l->dirty;
    l->dirty = 0;
    l->taken = d;
    if (!d) return 0;
    if ((int32_t)d < 0) alrv_props_default(e);
    if (d & 2) e->size = rv_size(l->p.env_size);
    if (d & 4) {
        float s = l->p.env_diffusion;
        e->diffusion = l->env_copy == 23 ? (float)((double)s * s * s) : s;
    }
    if (d & 8) e->room = mb_lin_l(l->p.room);
    if (d & 0x10) e->room_hf = mb_lin_l(l->p.room_hf);
    if (d & 0x20) e->room_lf = mb_lin_l(l->p.room_lf);
    if (d & 0x40) e->decay = l->env_copy == 23 ? (float)((double)l->p.decay_time * 0.75) : l->p.decay_time;
    if (d & 0x800c0) {
        l->decay_hf = decay_hf(l->p.decay_hf_ratio, l->p.decay_time, l->p.air_absorption_hf, l->p.flags);
        l->taken |= 0x1000000;
        e->decay_hf = l->decay_hf;
    }
    if (d & 0x140) e->decay_lf = (float)((double)l->p.decay_lf_ratio * (double)l->p.decay_time);
    if (d & 0x200) e->reflections = mb_lin_l(l->p.reflections);
    if (d & 0x400) e->reflections_delay = l->p.reflections_delay;
    if (d & 0x800) rv_pan(e->reflections_pan, l->p.reflections_pan);
    if (d & 0x1000) e->reverb = mb_lin_l(l->p.reverb);
    if (d & 0x2000) e->reverb_delay = l->p.reverb_delay;
    if (d & 0x4000) rv_pan(e->reverb_pan, l->p.reverb_pan);
    if (d & 0x8000) e->echo_time = l->p.echo_time;
    if (d & 0x10000) e->echo_depth = l->env_copy == 24 ? 0.0f : l->p.echo_depth;
    if (d & 0x20000) e->mod_time = l->p.mod_time;
    if (d & 0x40000) e->mod_depth = l->p.mod_depth;
    if (d & 0x100000) e->hf_ref = l->p.hf_reference;
    if (d & 0x200000) e->lf_ref = l->p.lf_reference;
    return 1;
}

/* ------------------------------------------------------------------ buffers */

void eax2b_defaults(eax2_buffer *b)           /* .rdata 0x100b4680 */
{
    memset(b, 0, sizeof(*b));
    b->occlusion_lf = 0.25f;
    b->occlusion_room = 0.5f;
    b->air_absorption = 0.0f;
    b->flags = 7;
}

void eax3b_defaults(eax3_buffer *b)           /* .rdata 0x100b46f0 */
{
    memset(b, 0, sizeof(*b));
    b->occlusion_lf = 0.25f;
    b->occlusion_room = 1.5f;
    b->occlusion_direct = 1.0f;
    b->exclusion_lf = 1.0f;
    b->flags = 7;
}

#define BSETI(field, lo, hi) do { if (!mb_ok(iv, lo, hi)) return EAX_BADVALUE; b->field = iv; return EAX_OK; } while (0)
#define BSETF(field, lo, hi) do { if (!in_range(fv, lo, hi)) return EAX_BADVALUE; b->field = fv; return EAX_OK; } while (0)

int eax2b_set(eax2_buffer *b, uint32_t id, const void *data, uint32_t len)
{
    id &= ~EAX_DEFERRED;
    if (id == EAX2B_NONE) return EAX_OK;
    if (id >= EAX2B_COUNT) return EAX_BADID;
    if (!data || len < (id == EAX2B_ALL ? sizeof(eax2_buffer) : 4)) return EAX_BADSIZE;
    int32_t iv; float fv; uint32_t uv;
    memcpy(&iv, data, 4); memcpy(&fv, data, 4); memcpy(&uv, data, 4);
    switch (id) {
    case EAX2B_ALL: {
        eax2_buffer r; memcpy(&r, data, sizeof(r));
        if (!mb_ok(r.direct, -10000, 1000) || !mb_ok(r.direct_hf, -10000, 0) || !mb_ok(r.room, -10000, 1000) ||
            !mb_ok(r.room_hf, -10000, 0) || !in_range(r.room_rolloff, 0.0f, 10.0f) ||
            !mb_ok(r.obstruction, -10000, 0) || !in_range(r.obstruction_lf, 0.0f, 1.0f) ||
            !mb_ok(r.occlusion, -10000, 0) || !in_range(r.occlusion_lf, 0.0f, 1.0f) ||
            !in_range(r.occlusion_room, 0.0f, 10.0f) || !mb_ok(r.outside_volume_hf, -10000, 0) ||
            !in_range(r.air_absorption, 0.0f, 10.0f) || (r.flags & ~7u))
            return EAX_BADVALUE;
        *b = r;
        return EAX_OK;
    }
    case EAX2B_DIRECT:         BSETI(direct, -10000, 1000);
    case EAX2B_DIRECTHF:       BSETI(direct_hf, -10000, 0);
    case EAX2B_ROOM:           BSETI(room, -10000, 1000);
    case EAX2B_ROOMHF:         BSETI(room_hf, -10000, 0);
    case EAX2B_ROOMROLLOFF:    BSETF(room_rolloff, 0.0f, 10.0f);
    case EAX2B_OBSTRUCTION:    BSETI(obstruction, -10000, 0);
    case EAX2B_OBSTRUCTIONLF:  BSETF(obstruction_lf, 0.0f, 1.0f);
    case EAX2B_OCCLUSION:      BSETI(occlusion, -10000, 0);
    case EAX2B_OCCLUSIONLF:    BSETF(occlusion_lf, 0.0f, 1.0f);
    case EAX2B_OCCLUSIONROOM:  BSETF(occlusion_room, 0.0f, 10.0f);
    case EAX2B_OUTSIDEVOLUMEHF: BSETI(outside_volume_hf, -10000, 0);
    case EAX2B_AIRABSORPTION:  BSETF(air_absorption, 0.0f, 10.0f);
    case EAX2B_FLAGS:
        if (uv & ~7u) return EAX_BADVALUE;
        b->flags = uv;
        return EAX_OK;
    }
    return EAX_BADID;
}

int eax2b_get(const eax2_buffer *b, uint32_t id, void *data, uint32_t len, uint32_t *ret)
{
    id &= ~EAX_DEFERRED;
    if (id == EAX2B_NONE) { if (ret) *ret = 0; return EAX_OK; }
    if (id >= EAX2B_COUNT) return EAX_BADID;
    uint32_t n = id == EAX2B_ALL ? sizeof(eax2_buffer) : 4;
    if (!data || len < n) return EAX_BADSIZE;
    memcpy(data, id == EAX2B_ALL ? (const char *)b : (const char *)b + 4 * (id - 2), n);
    if (ret) *ret = n;
    return EAX_OK;
}

int eax3b_set(eax3_buffer *b, uint32_t id, const void *data, uint32_t len)
{
    static const uint8_t need[EAX3B_COUNT] = { 0, sizeof(eax3_buffer), 8, 16, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4 };
    id &= ~EAX_DEFERRED;
    if (id == EAX3B_NONE) return EAX_OK;
    if (id >= EAX3B_COUNT) return EAX_BADID;
    if (!data || len < need[id]) return EAX_BADSIZE;
    int32_t iv; float fv; uint32_t uv;
    memcpy(&iv, data, 4); memcpy(&fv, data, 4); memcpy(&uv, data, 4);
    switch (id) {
    case EAX3B_ALL: {           /* fn 0x1004bdb0 */
        eax3_buffer r; memcpy(&r, data, sizeof(r));
        if (!mb_ok(r.direct, -10000, 1000) || !mb_ok(r.direct_hf, -10000, 0) || !mb_ok(r.room, -10000, 1000) ||
            !mb_ok(r.room_hf, -10000, 0) || !mb_ok(r.obstruction, -10000, 0) || !in_range(r.obstruction_lf, 0.0f, 1.0f) ||
            !mb_ok(r.occlusion, -10000, 0) || !in_range(r.occlusion_lf, 0.0f, 1.0f) ||
            !in_range(r.occlusion_room, 0.0f, 10.0f) || !in_range(r.occlusion_direct, 0.0f, 10.0f) ||
            !mb_ok(r.exclusion, -10000, 0) || !in_range(r.exclusion_lf, 0.0f, 1.0f) ||
            !mb_ok(r.outside_volume_hf, -10000, 0) || !in_range(r.doppler, 0.0f, 10.0f) ||
            !in_range(r.rolloff, 0.0f, 10.0f) || !in_range(r.room_rolloff, 0.0f, 10.0f) ||
            !in_range(r.air_absorption, 0.0f, 10.0f) || (r.flags & ~7u))
            return EAX_BADVALUE;
        *b = r;
        return EAX_OK;
    }
    case EAX3B_OBSTRUCTIONPARAMS: {
        struct { int32_t o; float lf; } v; memcpy(&v, data, 8);
        if (!mb_ok(v.o, -10000, 0) || !in_range(v.lf, 0.0f, 1.0f)) return EAX_BADVALUE;
        b->obstruction = v.o; b->obstruction_lf = v.lf;
        return EAX_OK;
    }
    case EAX3B_OCCLUSIONPARAMS: {
        struct { int32_t o; float lf, room, direct; } v; memcpy(&v, data, 16);
        if (!mb_ok(v.o, -10000, 0) || !in_range(v.lf, 0.0f, 1.0f) || !in_range(v.room, 0.0f, 10.0f) ||
            !in_range(v.direct, 0.0f, 10.0f))
            return EAX_BADVALUE;
        b->occlusion = v.o; b->occlusion_lf = v.lf; b->occlusion_room = v.room; b->occlusion_direct = v.direct;
        return EAX_OK;
    }
    case EAX3B_EXCLUSIONPARAMS: {
        struct { int32_t e; float lf; } v; memcpy(&v, data, 8);
        if (!mb_ok(v.e, -10000, 0) || !in_range(v.lf, 0.0f, 1.0f)) return EAX_BADVALUE;
        b->exclusion = v.e; b->exclusion_lf = v.lf;
        return EAX_OK;
    }
    case EAX3B_DIRECT:          BSETI(direct, -10000, 1000);
    case EAX3B_DIRECTHF:        BSETI(direct_hf, -10000, 0);
    case EAX3B_ROOM:            BSETI(room, -10000, 1000);
    case EAX3B_ROOMHF:          BSETI(room_hf, -10000, 0);
    case EAX3B_OBSTRUCTION:     BSETI(obstruction, -10000, 0);
    case EAX3B_OBSTRUCTIONLF:   BSETF(obstruction_lf, 0.0f, 1.0f);
    case EAX3B_OCCLUSION:       BSETI(occlusion, -10000, 0);
    case EAX3B_OCCLUSIONLF:     BSETF(occlusion_lf, 0.0f, 1.0f);
    case EAX3B_OCCLUSIONROOM:   BSETF(occlusion_room, 0.0f, 10.0f);
    case EAX3B_OCCLUSIONDIRECT: BSETF(occlusion_direct, 0.0f, 10.0f);
    case EAX3B_EXCLUSION:       BSETI(exclusion, -10000, 0);
    case EAX3B_EXCLUSIONLF:     BSETF(exclusion_lf, 0.0f, 1.0f);
    case EAX3B_OUTSIDEVOLUMEHF: BSETI(outside_volume_hf, -10000, 0);
    case EAX3B_DOPPLER:         BSETF(doppler, 0.0f, 10.0f);
    case EAX3B_ROLLOFF:         BSETF(rolloff, 0.0f, 10.0f);
    case EAX3B_ROOMROLLOFF:     BSETF(room_rolloff, 0.0f, 10.0f);
    case EAX3B_AIRABSORPTION:   BSETF(air_absorption, 0.0f, 10.0f);
    case EAX3B_FLAGS:
        if (uv & ~7u) return EAX_BADVALUE;
        b->flags = uv;
        return EAX_OK;
    }
    return EAX_BADID;
}

int eax3b_get(const eax3_buffer *b, uint32_t id, void *data, uint32_t len, uint32_t *ret)
{
    static const uint8_t off[EAX3B_COUNT] = { 0, 0, 0x10, 0x18, 0x28, 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
        0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44 };
    static const uint8_t size[EAX3B_COUNT] = { 0, sizeof(eax3_buffer), 8, 16, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4 };
    id &= ~EAX_DEFERRED;
    if (id == EAX3B_NONE) { if (ret) *ret = 0; return EAX_OK; }
    if (id >= EAX3B_COUNT) return EAX_BADID;
    uint32_t n = size[id];
    if (!data || len < n) return EAX_BADSIZE;
    memcpy(data, (const char *)b + off[id], n);
    if (ret) *ret = n;
    return EAX_OK;
}

/* ------------------------------------------------------------------ source conversions */

float eax_cone_auto(const eax_geom *g, int32_t extra_mb)
{
    float t0 = (float)((double)(g->cone_outside_volume + extra_mb) / 1000.0);
    float t1 = (float)pow(10.0, (double)t0);
    double outer = (double)g->cone_outside, inner = (double)g->cone_inside;
    float t2 = (float)(((outer * t1 + inner) * 0.008726646192371845) / ((double)t1 + 1.0));
    float t3 = (float)cos((double)t2);
    float t4 = (float)(((1.0 - t3) + ((double)t3 + 1.0) * t1) * 0.5);
    float lg = (float)log10((double)t4);
    return (float)((double)lg * 1000.0);
}

static float occ_mix(float a, float b)        /* max(a b, a + b - 1) */
{
    double p = (double)a * b, s = ((double)a + b) - 1.0;
    return (float)(s >= p ? s : p);
}

void eax2_source_gains(const eax2_buffer *b, const eax2_listener *l, const eax_geom *g, eax_gains *out)
{
    const double X = g->x;
    /* direct: obstruction and occlusion at LF (fn 0x1004a8a0) */
    float v = (float)((double)b->occlusion * b->occlusion_lf + (double)b->obstruction * b->obstruction_lf);
    out->direct = mb_gain(b->direct + trunc_mb(v));
    /* direct HF: what is lost above LF, air absorption, outside cone */
    float h = (float)((double)b->occlusion * (1.0 - b->occlusion_lf) + (double)b->obstruction * (1.0 - b->obstruction_lf));
    h = (float)((double)l->p.air_absorption_hf * b->air_absorption * g->distance_factor * g->rolloff_factor * X + h);
    if (b->flags & EAXB_DIRECTHFAUTO) h = (float)((double)b->outside_volume_hf * g->cone_factor + h);
    out->direct_hf = mb_gain(b->direct_hf + trunc_mb(h));
    /* room */
    float rl = (float)((double)b->occlusion_lf * b->occlusion * b->occlusion_room);
    float aut = (b->flags & EAXB_ROOMAUTO) ? eax_cone_auto(g, 0) : 0.0f;
    if (!(rl <= aut)) rl = aut;
    float q = (float)(((double)l->p.room_rolloff + b->room_rolloff) * X + 1.0);
    float lg = (float)log10((double)q);
    float r = (float)((double)rl - (double)lg * 2000.0);
    if (b->flags & EAXB_ROOMAUTO)
        r = (float)((double)g->distance_factor * -6000.0 * g->rolloff_factor * X / ((double)l->p.decay_time * 342.3800048828125) + r);
    out->room = mb_gain(b->room + trunc_mb(r));
    /* room HF */
    float rh = (float)((b->occlusion_room + 1.0) * b->occlusion);
    float a2 = (b->flags & EAXB_ROOMHFAUTO) ? eax_cone_auto(g, b->outside_volume_hf) : aut;
    if (a2 <= rh) rh = a2;
    rh = (float)((double)rh - rl);
    if (b->flags & EAXB_ROOMAUTO)
        rh = (float)((double)g->distance_factor * -6000.0 * g->rolloff_factor * X / ((double)l->decay_hf * 342.3800048828125) + rh);
    rh = (float)((double)l->p.air_absorption_hf * b->air_absorption * g->distance_factor * g->rolloff_factor * X + rh);
    out->room_hf = mb_gain(b->room_hf + trunc_mb(rh));
}

void eax3_source_gains(const eax3_buffer *b, const eax3_listener *l, const eax_geom *g, eax_gains *out)
{
    const double X = g->x;
    const double rsum_d = (double)g->rolloff_factor + b->rolloff;
    /* direct (fn 0x1004c890): obstruction and occlusion at LF plus the distance roll-off */
    float a = (float)((double)b->obstruction * b->obstruction_lf);
    float m = occ_mix(b->occlusion_direct, b->occlusion_lf);
    float v1 = (float)((double)m * b->occlusion + a);
    float rs = (float)rsum_d;
    float q = (float)((double)rs * X + 1.0);
    float lg = (float)log10((double)q);
    float lg2 = (float)((double)lg * 2000.0);
    float v = (float)((double)v1 - lg2);
    out->direct = mb_gain(b->direct + trunc_mb(v));
    /* direct HF */
    float c = (float)((double)b->occlusion * b->occlusion_direct + b->obstruction);
    double air = (double)l->p.air_absorption_hf * b->air_absorption * g->distance_factor * rsum_d * X;
    float dd = (float)((double)c - (double)m * b->occlusion);
    float h = (float)(air + dd);
    if (b->flags & EAXB_DIRECTHFAUTO) h = (float)((double)b->outside_volume_hf * g->cone_factor + h);
    out->direct_hf = mb_gain(b->direct_hf + trunc_mb(h));
    /* room: occlusion through the room ratio, exclusion */
    float m2 = occ_mix(b->occlusion_lf, b->occlusion_room);
    float rl = (float)((double)m2 * b->occlusion);
    rl = (float)((double)b->exclusion_lf * b->exclusion + rl);
    float aut = (b->flags & EAXB_ROOMAUTO) ? eax_cone_auto(g, 0) : 0.0f;
    if (!(rl <= aut)) rl = aut;
    float rr = (float)((double)l->p.room_rolloff + b->room_rolloff);
    float q2 = (float)((double)rr * X + 1.0);
    float lg3 = (float)log10((double)q2);
    float r = (float)((double)rl - (double)(float)((double)lg3 * 2000.0));
    if (b->flags & EAXB_ROOMAUTO)
        r = (float)((double)g->distance_factor * -6000.0 * rsum_d * X / ((double)l->p.decay_time * 342.3800048828125) + r);
    out->room = mb_gain(b->room + trunc_mb(r));
    /* room HF */
    float rh0 = (float)((double)b->occlusion * b->occlusion_room);
    float rh = (float)((double)rh0 + b->exclusion);
    float a2 = (b->flags & EAXB_ROOMHFAUTO) ? eax_cone_auto(g, b->outside_volume_hf) : aut;
    if (a2 <= rh) rh = a2;
    rh = (float)((double)rh - rl);
    if (b->flags & EAXB_ROOMAUTO)
        rh = (float)((double)g->distance_factor * -6000.0 * rsum_d * X / ((double)l->decay_hf * 342.3800048828125) + rh);
    rh = (float)((double)l->p.air_absorption_hf * b->air_absorption * g->distance_factor * rsum_d * X + rh);
    out->room_hf = mb_gain(b->room_hf + trunc_mb(rh));
}

float eax_lp_coef(float gain, float f_over_fs)
{
    if (gain == 1.0f) return 1.0f;
    if (f_over_fs == 0.0f) return 0.0f;
    if (f_over_fs > 0.5f) f_over_fs = 0.5f;
    double g = gain;
    float c = (float)cos((double)(float)((double)f_over_fs * 6.2831854820251465));
    double t2 = 1.0 - c * g;
    float t3 = (float)(g * (((1.0 - c) + (1.0 - c)) - (1.0 - (double)c * c) * g));
    float t4 = (float)sqrt((double)t3);
    float t5 = (float)((1.0 - (t2 - t4) / (1.0 - g)) * 32768.0);
    double q = ((double)t5 + 1.4999999464748726e-08) + 6755399441055744.0;
    q -= 6755399441055744.0;
    return (float)(q / 32768.0);
}

/* ================================================================== EAX 4.0 */

const eax_guid EAX_NULL_GUID = { { 0, 0, 0, 0 } };
#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { { (uint32_t)(a), (uint32_t)(b) | (uint32_t)(c) << 16, (uint32_t)(d0) | (uint32_t)(d1) << 8 | (uint32_t)(d2) << 16 | (uint32_t)(d3) << 24, \
        (uint32_t)(d4) | (uint32_t)(d5) << 8 | (uint32_t)(d6) << 16 | (uint32_t)(d7) << 24 } }
const eax_guid EAX_PRIMARY_SLOT_GUID = G(0xf317866d, 0x924c, 0x450c, 0x86, 0x1b, 0xe6, 0xda, 0xa2, 0x5e, 0x7c, 0x20);
const eax_guid EAX4_SLOT_GUID[4] = {
    G(0xc4d79f1e, 0xf1ac, 0x436b, 0xa8, 0x1d, 0xa7, 0x38, 0xe7, 0x04, 0x54, 0x69),
    G(0x08c00e96, 0x74be, 0x4491, 0x93, 0xaa, 0xe8, 0xad, 0x35, 0xa4, 0x91, 0x17),
    G(0x1d433b88, 0xf0f6, 0x4637, 0x91, 0x9f, 0x60, 0xe7, 0xe0, 0x6b, 0x5e, 0xdd),
    G(0xefff08ea, 0xc7d8, 0x44ab, 0x93, 0xad, 0x6d, 0xbd, 0x5f, 0x91, 0x00, 0x64),
};
const eax_guid EAX4_EFFECT_GUID[13] = {
    { { 0, 0, 0, 0 } },
    G(0xbfb7a01e, 0x7825, 0x4039, 0x92, 0x7f, 0x03, 0xaa, 0xbd, 0xa0, 0xc5, 0x60),   /* AGC compressor */
    G(0xec3130c0, 0xac7a, 0x11d2, 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1),   /* autowah */
    G(0xde6d6fe0, 0xac79, 0x11d2, 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1),   /* chorus */
    G(0x975a4ce0, 0xac7e, 0x11d2, 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1),   /* distortion */
    G(0x0e9f1bc0, 0xac82, 0x11d2, 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1),   /* echo */
    G(0x65f94ce0, 0x9793, 0x11d3, 0x93, 0x9d, 0x00, 0xc0, 0xf0, 0x2d, 0xd6, 0xf0),   /* equalizer */
    G(0xa70007c0, 0x07d2, 0x11d3, 0x9b, 0x1e, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1),   /* flanger */
    G(0xdc3e1880, 0x9212, 0x11d3, 0x93, 0x9d, 0x00, 0xc0, 0xf0, 0x2d, 0xd6, 0xf0),   /* frequency shifter */
    G(0xe7905100, 0xafb2, 0x11d2, 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1),   /* pitch shifter */
    G(0x0cf95c8f, 0xa3cc, 0x4849, 0xb0, 0xb6, 0x83, 0x2e, 0xcc, 0x18, 0x22, 0xdf),   /* reverb */
    G(0x0b89fe60, 0xafb5, 0x11d2, 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1),   /* ring modulator */
    G(0xe41cf10c, 0x3383, 0x11d2, 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1),   /* vocal morpher */
};
#undef G

static int geq(const eax_guid *a, const eax_guid *b) { return !memcmp(a, b, sizeof(*a)); }

int eax4_slot_index(const eax_guid *g)                     /* fn 0x10027270 */
{
    for (int i = 0; i < 4; i++) if (geq(g, &EAX4_SLOT_GUID[i])) return i;
    return -1;
}

static int effect_type(const eax_guid *g)                  /* fn 0x1004cfb0 */
{
    for (int t = 0; t < EAX4FX_COUNT; t++) if (geq(g, &EAX4_EFFECT_GUID[t])) return t;
    return -1;
}

static int fail4(eax4_context *c, int32_t err)
{
    if (c) c->last_error = err;
    return EAX_BADVALUE;
}

/* ------------------------------------------------------------------ context */

void eax4_context_reset(eax4_context *c)
{
    c->primary = EAX4_SLOT_GUID[0];
    c->distance_factor = 1.0f;
    c->air_absorption_hf = -5.0f;
    c->hf_reference = 5000.0f;
    c->last_error = 0;
    c->dirty = 0xf;
}

static int primary_ok(const eax_guid *g) { return geq(g, &EAX_NULL_GUID) || eax4_slot_index(g) >= 0; }

int eax4c_set(eax4_context *c, uint32_t id, const void *data, uint32_t len)
{
    float f = 0.0f;
    id &= ~EAX_DEFERRED;
    if (id >= EAX4C_DISTANCEFACTOR && id <= EAX4C_HFREFERENCE) {
        if (!data || len < 4) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&f, data, 4);
    }
    switch (id) {
    case EAX4C_NONE:
        return EAX_OK;
    case EAX4C_ALL: {                                      /* fn 0x1004f700 (the distance factor is not checked) */
        struct { eax_guid g; float df, air, hf; } v;
        if (!data || len < sizeof(v)) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&v, data, sizeof(v));
        if (!primary_ok(&v.g) || !in_range(v.air, -100.0f, 0.0f) || !in_range(v.hf, 1000.0f, 20000.0f))
            return fail4(c, EAX4ERR_INVALID_VALUE);
        if (!geq(&v.g, &c->primary)) { c->primary = v.g; c->dirty |= 1; }
        if (fne(v.df, c->distance_factor)) { c->distance_factor = v.df; c->dirty |= 2; }
        if (fne(v.air, c->air_absorption_hf)) { c->air_absorption_hf = v.air; c->dirty |= 4; }
        if (fne(v.hf, c->hf_reference)) { c->hf_reference = v.hf; c->dirty |= 8; }
        return EAX_OK;
    }
    case EAX4C_PRIMARYFXSLOTID: {
        eax_guid g;
        if (!data || len < 16) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&g, data, 16);
        if (!primary_ok(&g)) return fail4(c, EAX4ERR_INVALID_VALUE);
        if (!geq(&g, &c->primary)) { c->primary = g; c->dirty |= 1; }
        return EAX_OK;
    }
    case EAX4C_DISTANCEFACTOR:
        if (!in_range(f, 1.1754943508222875e-38f, 3.4028234663852886e+38f)) return fail4(c, EAX4ERR_INVALID_VALUE);
        if (fne(f, c->distance_factor)) { c->distance_factor = f; c->dirty |= 2; }
        return EAX_OK;
    case EAX4C_AIRABSORPTIONHF:
        if (!in_range(f, -100.0f, 0.0f)) return fail4(c, EAX4ERR_INVALID_VALUE);
        if (fne(f, c->air_absorption_hf)) { c->air_absorption_hf = f; c->dirty |= 4; }
        return EAX_OK;
    case EAX4C_HFREFERENCE:
        if (!in_range(f, 1000.0f, 20000.0f)) return fail4(c, EAX4ERR_INVALID_VALUE);
        if (fne(f, c->hf_reference)) { c->hf_reference = f; c->dirty |= 8; }
        return EAX_OK;
    default:                                               /* incl. LASTERROR (read only) */
        return fail4(c, EAX4ERR_INVALID_VALUE);
    }
}

int eax4c_get(eax4_context *c, uint32_t id, void *data, uint32_t len, uint32_t *ret)
{
    uint32_t n;
    id &= ~EAX_DEFERRED;
    switch (id) {
    case EAX4C_NONE: n = 0; break;
    case EAX4C_ALL: n = 28; break;
    case EAX4C_PRIMARYFXSLOTID: n = 16; break;
    case EAX4C_DISTANCEFACTOR: case EAX4C_AIRABSORPTIONHF: case EAX4C_HFREFERENCE: case EAX4C_LASTERROR: n = 4; break;
    default: return fail4(c, EAX4ERR_INVALID_VALUE);
    }
    if (n && (!data || len < n)) return fail4(c, EAX4ERR_INVALID_VALUE);
    switch (id) {
    case EAX4C_ALL: memcpy(data, c, 28); break;
    case EAX4C_PRIMARYFXSLOTID: memcpy(data, &c->primary, 16); break;
    case EAX4C_DISTANCEFACTOR: memcpy(data, &c->distance_factor, 4); break;
    case EAX4C_AIRABSORPTIONHF: memcpy(data, &c->air_absorption_hf, 4); break;
    case EAX4C_HFREFERENCE: memcpy(data, &c->hf_reference, 4); break;
    case EAX4C_LASTERROR: memcpy(data, &c->last_error, 4); c->last_error = 0; break;
    }
    if (ret) *ret = n;
    return EAX_OK;
}

/* ------------------------------------------------------------------ FX slots */

static void effect_defaults(eax4_slot *s)                 /* the effect's reset for EAX 4 */
{
    memset(s->fx, 0, sizeof(s->fx));
    if (s->type == EAX4FX_REVERB) eax3l_reset(&s->reverb);
    else eax4fx_effect_defaults(s->type, s->fx);
}

/* load an effect type (fn 0x1004ffc0); the same type stays loaded with its properties */
static int load_effect(eax4_slot *s, int type, int reset)
{
    if (s->system_lock) return 0;
    if (s->type != type) {
        s->type = type;
        s->effect = EAX4_EFFECT_GUID[type];
        /* ALchemy only resets a new effect's properties when it is loaded through LOADEFFECT; one
         * loaded through ALLPARAMETERS keeps uninitialised (zero) properties (F.E.A.R. reads them back,
         * changes a few and gets the write rejected).  reAL always starts it from the defaults. */
        effect_defaults(s);
    } else if (reset) {
        effect_defaults(s);
    }
    s->dirty |= 1;
    return 1;
}

void eax4_slots_init(eax4_slot slots[4])
{
    for (int i = 0; i < 4; i++) {
        eax4_slot *s = &slots[i];
        if (i < 2) {                                         /* fn 0x10042220(4) */
            s->system_lock = 0;
            load_effect(s, i == 0 ? EAX4FX_REVERB : EAX4FX_CHORUS, 0);
            s->system_lock = 1;
            s->lock = 1;
        }
        s->volume = 0;                                       /* fn 0x10050850 */
        s->flags = EAX4SLOT_ENVIRONMENT;
        s->dirty = 0x1000000a;
        effect_defaults(s);
    }
}

int eax4fx_set(eax4_slot *s, eax4_context *c, uint32_t id, const void *data, uint32_t len)
{
    id &= ~EAX_DEFERRED;
    switch (id) {
    case EAX4FXS_NONE:
        return EAX_OK;
    case EAX4FXS_ALL: {                                      /* fn 0x1004d3a0 */
        struct { eax_guid g; int32_t volume, lock; uint32_t flags; } v;
        if (!data || len < sizeof(v)) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&v, data, sizeof(v));
        int type = effect_type(&v.g);
        if (type < 0 || !mb_ok(v.volume, -10000, 0) || (v.flags & ~1u) || s->system_lock)
            return fail4(c, EAX4ERR_INVALID_VALUE);
        s->lock = v.lock;
        load_effect(s, type, 0);
        if (s->volume != v.volume) { s->volume = v.volume; s->dirty |= 2; }
        if (s->flags != v.flags) { s->flags = v.flags; s->dirty |= 8; }
        return EAX_OK;
    }
    case EAX4FXS_LOADEFFECT: {                               /* fn 0x1004d470: always resets the effect */
        eax_guid g;
        if (!data || len < 16) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&g, data, 16);
        int type = effect_type(&g);
        if (type < 0 || !load_effect(s, type, 1)) return fail4(c, EAX4ERR_UNKNOWN_EFFECT);
        return EAX_OK;
    }
    case EAX4FXS_VOLUME: {
        int32_t v;
        if (!data || len < 4) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&v, data, 4);
        if (!mb_ok(v, -10000, 0)) return fail4(c, EAX4ERR_INVALID_VALUE);
        if (s->volume != v) { s->volume = v; s->dirty |= 2; }
        return EAX_OK;
    }
    case EAX4FXS_LOCK: {                                     /* fn 0x100507e0: any value; not for system slots */
        int32_t v;
        if (!data || len < 4) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&v, data, 4);
        if (s->system_lock) return fail4(c, EAX4ERR_INVALID_OPERATION);
        s->lock = v; s->dirty |= 4;
        return EAX_OK;
    }
    case EAX4FXS_FLAGS: {
        uint32_t v;
        if (!data || len < 4) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&v, data, 4);
        if (v & ~1u) return fail4(c, EAX4ERR_INVALID_VALUE);
        if (s->flags != v) { s->flags = v; s->dirty |= 8; }
        return EAX_OK;
    }
    }
    if (id >= 0x10000) return fail4(c, EAX4ERR_INVALID_VALUE);
    if (s->type == EAX4FX_NONE) return fail4(c, EAX4ERR_NO_EFFECT);
    int r;
    if (s->type == EAX4FX_REVERB) r = eax3l_set(&s->reverb, id, data, len);
    else r = eax4fx_effect_set(s->type, s->fx, id, data, len);
    if (r != EAX_OK) return fail4(c, EAX4ERR_INVALID_VALUE);
    s->dirty |= 0x10;
    return EAX_OK;
}

int eax4fx_get(const eax4_slot *s, eax4_context *c, uint32_t id, void *data, uint32_t len, uint32_t *ret)
{
    id &= ~EAX_DEFERRED;
    uint32_t n = 0;
    switch (id) {
    case EAX4FXS_NONE: break;
    case EAX4FXS_ALL: n = 28; break;
    case EAX4FXS_LOADEFFECT: n = 16; break;
    case EAX4FXS_VOLUME: case EAX4FXS_LOCK: case EAX4FXS_FLAGS: n = 4; break;
    default: {
        if (id >= 0x10000) return fail4(c, EAX4ERR_INVALID_VALUE);
        if (s->type == EAX4FX_NONE) return fail4(c, EAX4ERR_NO_EFFECT);
        int r = s->type == EAX4FX_REVERB ? eax3l_get(&s->reverb, id, data, len, ret)
                                         : eax4fx_effect_get(s->type, s->fx, id, data, len, ret);
        return r == EAX_OK ? EAX_OK : fail4(c, EAX4ERR_INVALID_VALUE);
    }
    }
    if (n && (!data || len < n)) return fail4(c, EAX4ERR_INVALID_VALUE);
    switch (id) {
    case EAX4FXS_ALL: {
        struct { eax_guid g; int32_t volume, lock; uint32_t flags; } v = { s->effect, s->volume, s->lock, s->flags };
        memcpy(data, &v, sizeof(v));
        break;
    }
    case EAX4FXS_LOADEFFECT: memcpy(data, &s->effect, 16); break;
    case EAX4FXS_VOLUME: memcpy(data, &s->volume, 4); break;
    case EAX4FXS_LOCK: memcpy(data, &s->lock, 4); break;
    case EAX4FXS_FLAGS: memcpy(data, &s->flags, 4); break;
    }
    if (ret) *ret = n;
    return EAX_OK;
}

/* ------------------------------------------------------------------ sources */

void eax4_source_defaults(eax4_source *s)
{
    memset(s, 0, sizeof(*s));
    static const eax3_buffer def = { 0, 0, 0, 0, 0, 0.0f, 0, 0.25f, 1.5f, 1.0f, 0, 1.0f, 0, 1.0f, 0.0f, 0.0f, 0.0f, 7 };
    static const eax4_send sdef = { 0, 0, 0, 0.25f, 1.5f, 1.0f, 0, 1.0f };
    s->p = def;
    for (int i = 0; i < 4; i++) { s->send[i] = sdef; s->send_dirty[i] = 0xff; }
    s->active[0] = EAX_NULL_GUID;
    s->active[1] = EAX_PRIMARY_SLOT_GUID;
    s->dirty = 0x7ffff;
}

/* single-property setters (fns 0x1004dd20 .. 0x1004e270); offsets index eax3_buffer as 32-bit words */
static int set_i4(eax4_source *s, int word, int32_t v, int32_t lo, int32_t hi)
{
    int32_t *p = (int32_t *)&s->p + word;
    if (!mb_ok(v, lo, hi)) return 0;
    if (*p != v) { *p = v; s->dirty |= 1u << word; }
    return 1;
}
static int set_f4(eax4_source *s, int word, float v, float lo, float hi)
{
    float *p = (float *)&s->p + word;
    if (!in_range(v, lo, hi)) return 0;
    if (fne(*p, v)) { *p = v; s->dirty |= 1u << word; }
    return 1;
}
static int occl_lf_ok(float v) { return v >= 0.0f && v <= 1.0f; }      /* rejects NaN, like ALchemy */

static int send_set(eax4_source *s, int slot, int kind, const uint8_t *v)
{
    eax4_send *d = &s->send[slot];
    int32_t i0, i1, i2; float f[5];
    uint32_t b = 0;
    switch (kind) {
    case EAX4S_SENDPARAMS:                                   /* fn 0x1004e590 */
        memcpy(&i0, v, 4); memcpy(&i1, v + 4, 4);
        if (!mb_ok(i0, -10000, 0) || !mb_ok(i1, -10000, 0)) return 0;
        if (d->room != i0) { d->room = i0; b |= 1; }
        if (d->room_hf != i1) { d->room_hf = i1; b |= 2; }
        break;
    case EAX4S_ALLSENDPARAMS:                                /* fn 0x1004e3b0 */
        memcpy(&i0, v, 4); memcpy(&i1, v + 4, 4); memcpy(&i2, v + 8, 4); memcpy(f, v + 12, 12);
        { int32_t ex; float exlf; memcpy(&ex, v + 24, 4); memcpy(&exlf, v + 28, 4);
          if (!mb_ok(i0, -10000, 0) || !mb_ok(i1, -10000, 0) || !mb_ok(i2, -10000, 0) || !in_range(f[0], 0.0f, 1.0f) ||
              !in_range(f[1], 0.0f, 10.0f) || !in_range(f[2], 0.0f, 10.0f) || !mb_ok(ex, -10000, 0) || !in_range(exlf, 0.0f, 1.0f))
              return 0;
          if (d->room != i0) { d->room = i0; b |= 1; }
          if (d->room_hf != i1) { d->room_hf = i1; b |= 2; }
          if (d->occlusion != i2) { d->occlusion = i2; b |= 4; }
          if (fne(f[0], d->occlusion_lf)) { d->occlusion_lf = f[0]; b |= 8; }
          if (fne(f[1], d->occlusion_room_ratio)) { d->occlusion_room_ratio = f[1]; b |= 0x10; }
          if (fne(f[2], d->occlusion_direct_ratio)) { d->occlusion_direct_ratio = f[2]; b |= 0x20; }
          if (d->exclusion != ex) { d->exclusion = ex; b |= 0x40; }
          if (fne(exlf, d->exclusion_lf)) { d->exclusion_lf = exlf; b |= 0x80; } }
        break;
    case EAX4S_OCCLUSIONSENDPARAMS:                          /* fn 0x1004e600 */
        memcpy(&i0, v, 4); memcpy(f, v + 4, 12);
        if (!mb_ok(i0, -10000, 0) || !occl_lf_ok(f[0]) || !in_range(f[1], 0.0f, 10.0f) || !in_range(f[2], 0.0f, 10.0f)) return 0;
        if (d->occlusion != i0) { d->occlusion = i0; b |= 4; }
        if (fne(f[0], d->occlusion_lf)) { d->occlusion_lf = f[0]; b |= 8; }
        if (fne(f[1], d->occlusion_room_ratio)) { d->occlusion_room_ratio = f[1]; b |= 0x10; }
        if (fne(f[2], d->occlusion_direct_ratio)) { d->occlusion_direct_ratio = f[2]; b |= 0x20; }
        break;
    case EAX4S_EXCLUSIONSENDPARAMS:                          /* fn 0x1004e740 */
        memcpy(&i0, v, 4); memcpy(f, v + 4, 4);
        if (!mb_ok(i0, -10000, 0) || !in_range(f[0], 0.0f, 1.0f)) return 0;
        if (d->exclusion != i0) { d->exclusion = i0; b |= 0x40; }
        if (fne(f[0], d->exclusion_lf)) { d->exclusion_lf = f[0]; b |= 0x80; }
        break;
    }
    s->send_dirty[slot] |= b;
    return 1;
}

/* record sizes of the send arrays: GUID + values */
static uint32_t send_rec(uint32_t id)
{
    switch (id) {
    case EAX4S_SENDPARAMS: case EAX4S_EXCLUSIONSENDPARAMS: return 24;
    case EAX4S_ALLSENDPARAMS: return 48;
    case EAX4S_OCCLUSIONSENDPARAMS: return 32;
    }
    return 0;
}

int eax4s_set(eax4_source *s, eax4_context *c, uint32_t id, const void *data, uint32_t len)
{
    const uint8_t *d = data;
    int32_t iv = 0; float fv = 0.0f;
    id &= ~EAX_DEFERRED;
    if (id >= EAX4S_DIRECT && id <= EAX4S_FLAGS) {
        if (!d || len < 4) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&iv, d, 4); memcpy(&fv, d, 4);
    }
    int ok = 1;
    switch (id) {
    case EAX4S_NONE: return EAX_OK;
    case EAX4S_ALL: {                                        /* fn 0x1004d730 */
        eax3_buffer v;
        if (!d || len < sizeof(v)) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&v, d, sizeof(v));
        if (!mb_ok(v.direct, -10000, 1000) || !mb_ok(v.direct_hf, -10000, 0) || !mb_ok(v.room, -10000, 1000) ||
            !mb_ok(v.room_hf, -10000, 0) || !mb_ok(v.obstruction, -10000, 0) || !in_range(v.obstruction_lf, 0, 1) ||
            !mb_ok(v.occlusion, -10000, 0) || !in_range(v.occlusion_lf, 0, 1) || !in_range(v.occlusion_room, 0, 10) ||
            !in_range(v.occlusion_direct, 0, 10) || !mb_ok(v.exclusion, -10000, 0) || !in_range(v.exclusion_lf, 0, 1) ||
            !mb_ok(v.outside_volume_hf, -10000, 0) || !in_range(v.doppler, 0, 10) || !in_range(v.rolloff, 0, 10) ||
            !in_range(v.room_rolloff, 0, 10) || !in_range(v.air_absorption, 0, 10) || (v.flags & ~7u))
            return fail4(c, EAX4ERR_INVALID_VALUE);
        const uint32_t *nw = (const uint32_t *)&v; uint32_t *ow = (uint32_t *)&s->p;
        static const uint32_t isflt = 0x1eba0;         /* words holding floats (compared as floats) */
        for (int i = 0; i < 18; i++) {
            float a, b; memcpy(&a, &nw[i], 4); memcpy(&b, &ow[i], 4);
            int changed = (isflt >> i & 1) ? fne(a, b) : nw[i] != ow[i];
            if (changed) { ow[i] = nw[i]; s->dirty |= 1u << i; }
        }
        return EAX_OK;
    }
    case EAX4S_OBSTRUCTIONPARAMS: {                          /* fn 0x1004dae0 */
        int32_t o; float lf;
        if (!d || len < 8) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&o, d, 4); memcpy(&lf, d + 4, 4);
        if (!mb_ok(o, -10000, 0) || !in_range(lf, 0, 1)) return fail4(c, EAX4ERR_INVALID_VALUE);
        set_i4(s, 4, o, -10000, 0); set_f4(s, 5, lf, 0, 1);
        return EAX_OK;
    }
    case EAX4S_OCCLUSIONPARAMS: {                            /* fn 0x1004db60 */
        int32_t o; float f[3];
        if (!d || len < 16) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&o, d, 4); memcpy(f, d + 4, 12);
        if (!mb_ok(o, -10000, 0) || !occl_lf_ok(f[0]) || !in_range(f[1], 0, 10) || !in_range(f[2], 0, 10))
            return fail4(c, EAX4ERR_INVALID_VALUE);
        set_i4(s, 6, o, -10000, 0); set_f4(s, 7, f[0], 0, 1); set_f4(s, 8, f[1], 0, 10); set_f4(s, 9, f[2], 0, 10);
        return EAX_OK;
    }
    case EAX4S_EXCLUSIONPARAMS: {                            /* fn 0x1004dc90 */
        int32_t o; float lf;
        if (!d || len < 8) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(&o, d, 4); memcpy(&lf, d + 4, 4);
        if (!mb_ok(o, -10000, 0) || !in_range(lf, 0, 1)) return fail4(c, EAX4ERR_INVALID_VALUE);
        set_i4(s, 10, o, -10000, 0); set_f4(s, 11, lf, 0, 1);
        return EAX_OK;
    }
    case EAX4S_DIRECT:               ok = set_i4(s, 0, iv, -10000, 1000); break;
    case EAX4S_DIRECTHF:             ok = set_i4(s, 1, iv, -10000, 0); break;
    case EAX4S_ROOM:                 ok = set_i4(s, 2, iv, -10000, 1000); break;
    case EAX4S_ROOMHF:               ok = set_i4(s, 3, iv, -10000, 0); break;
    case EAX4S_OBSTRUCTION:          ok = set_i4(s, 4, iv, -10000, 0); break;
    case EAX4S_OBSTRUCTIONLF:        ok = set_f4(s, 5, fv, 0, 1); break;
    case EAX4S_OCCLUSION:            ok = set_i4(s, 6, iv, -10000, 0); break;
    case EAX4S_OCCLUSIONLF:          ok = set_f4(s, 7, fv, 0, 1); break;
    case EAX4S_OCCLUSIONROOMRATIO:   ok = set_f4(s, 8, fv, 0, 10); break;
    case EAX4S_OCCLUSIONDIRECTRATIO: ok = set_f4(s, 9, fv, 0, 10); break;
    case EAX4S_EXCLUSION:            ok = set_i4(s, 10, iv, -10000, 0); break;
    case EAX4S_EXCLUSIONLF:          ok = set_f4(s, 11, fv, 0, 1); break;
    case EAX4S_OUTSIDEVOLUMEHF:      ok = set_i4(s, 12, iv, -10000, 0); break;
    case EAX4S_DOPPLERFACTOR:        ok = set_f4(s, 13, fv, 0, 10); break;
    case EAX4S_ROLLOFFFACTOR:        ok = set_f4(s, 14, fv, 0, 10); break;
    case EAX4S_ROOMROLLOFFFACTOR:    ok = set_f4(s, 15, fv, 0, 10); break;
    case EAX4S_AIRABSORPTIONFACTOR:  ok = set_f4(s, 16, fv, 0, 10); break;
    case EAX4S_FLAGS:
        if ((uint32_t)iv & ~7u) ok = 0;
        else if (s->p.flags != (uint32_t)iv) { s->p.flags = (uint32_t)iv; s->dirty |= 0x20000; }
        break;
    case EAX4S_SENDPARAMS: case EAX4S_ALLSENDPARAMS: case EAX4S_OCCLUSIONSENDPARAMS: case EAX4S_EXCLUSIONSENDPARAMS: {
        /* arrays of {slot GUID, values}; records before a bad one stay applied */
        uint32_t rs = send_rec(id), n = d ? len / rs : 0;
        if (n > 4) n = 4;
        if (!n) return fail4(c, EAX4ERR_INVALID_VALUE);
        for (uint32_t i = 0; i < n; i++) {
            eax_guid g; memcpy(&g, d + i * rs, 16);
            int slot = eax4_slot_index(&g);
            if (slot < 0 || !send_set(s, slot, id, d + i * rs + 16)) return fail4(c, EAX4ERR_INVALID_VALUE);
        }
        return EAX_OK;
    }
    case EAX4S_ACTIVEFXSLOTID: {                             /* fn 0x1004e2e0: no dirty bit */
        uint32_t n = d ? len / 16 : 0;
        if (n > 2) n = 2;
        if (!n) return fail4(c, EAX4ERR_INVALID_VALUE);
        for (uint32_t i = 0; i < n; i++) {
            eax_guid g; memcpy(&g, d + i * 16, 16);
            if (!geq(&g, &EAX_NULL_GUID) && !geq(&g, &EAX_PRIMARY_SLOT_GUID) && eax4_slot_index(&g) < 0)
                return fail4(c, EAX4ERR_INVALID_VALUE);
            s->active[i] = g;
        }
        return EAX_OK;
    }
    default:
        return fail4(c, EAX4ERR_INVALID_VALUE);
    }
    return ok ? EAX_OK : fail4(c, EAX4ERR_INVALID_VALUE);
}

int eax4s_get(const eax4_source *s, eax4_context *c, uint32_t id, void *data, uint32_t len, uint32_t *ret)
{
    uint8_t *d = data;
    id &= ~EAX_DEFERRED;
    uint32_t n = 0;
    switch (id) {
    case EAX4S_NONE: break;
    case EAX4S_ALL:
        if (!d || len < sizeof(s->p)) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(d, &s->p, sizeof(s->p)); n = sizeof(s->p);
        break;
    case EAX4S_OBSTRUCTIONPARAMS: case EAX4S_EXCLUSIONPARAMS:
        if (!d || len < 8) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(d, (const uint32_t *)&s->p + (id == EAX4S_OBSTRUCTIONPARAMS ? 4 : 10), 8); n = 8;
        break;
    case EAX4S_OCCLUSIONPARAMS:
        if (!d || len < 16) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(d, (const uint32_t *)&s->p + 6, 16); n = 16;
        break;
    case EAX4S_SENDPARAMS: case EAX4S_ALLSENDPARAMS: case EAX4S_OCCLUSIONSENDPARAMS: case EAX4S_EXCLUSIONSENDPARAMS: {
        /* ALchemy fills record i with slot i, whatever GUID the caller put there */
        uint32_t rs = send_rec(id), cnt = d ? len / rs : 0, off = id == EAX4S_OCCLUSIONSENDPARAMS ? 8 :
                      id == EAX4S_EXCLUSIONSENDPARAMS ? 24 : 0;
        if (cnt > 4) cnt = 4;
        if (!cnt) return fail4(c, EAX4ERR_INVALID_VALUE);
        for (uint32_t i = 0; i < cnt; i++) {
            memcpy(d + i * rs, &EAX4_SLOT_GUID[i], 16);
            memcpy(d + i * rs + 16, (const uint8_t *)&s->send[i] + off, rs - 16);
        }
        n = cnt * rs;
        break;
    }
    case EAX4S_ACTIVEFXSLOTID: {
        uint32_t cnt = d ? len / 16 : 0;
        if (cnt > 2) cnt = 2;
        if (!cnt) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(d, s->active, cnt * 16); n = cnt * 16;
        break;
    }
    default:
        if (id < EAX4S_DIRECT || id > EAX4S_FLAGS) return fail4(c, EAX4ERR_INVALID_VALUE);
        if (!d || len < 4) return fail4(c, EAX4ERR_INVALID_VALUE);
        memcpy(d, (const uint32_t *)&s->p + (id - EAX4S_DIRECT), 4); n = 4;
        break;
    }
    if (ret) *ret = n;
    return EAX_OK;
}

/* ------------------------------------------------------------------ source conversion (fn 0x1004e7e0) */

static double occ_max(float a, float b)                    /* max(a b, a + b - 1), kept in extended precision */
{
    double p = (double)a * b, s = ((double)a + b) - 1.0;
    return s >= p ? s : p;
}

static float mb_gain_nc(int32_t mb) { return mb <= -10000 ? 0.0f : mb_lin(mb); }   /* no clamp at 0 mB */
static double rs_d(const eax_geom *g, const eax3_buffer *b) { return (double)g->rolloff_factor + b->rolloff; }

void eax4_source_gains(const eax4_source *s, const eax4_context *c, const eax4_slot slots[4], const eax_geom *g,
                       float ds3d_doppler, eax4_gains *out)
{
    const eax3_buffer *b = &s->p;
    const int pi = eax4_slot_index(&c->primary);
    const eax4_slot *ps = pi >= 0 ? &slots[pi] : NULL;
    const int penv = ps && (ps->flags & EAX4SLOT_ENVIRONMENT);
    const eax3_listener *prv = ps && ps->type == EAX4FX_REVERB ? &ps->reverb : NULL;
    const float X = g->x;

    /* direct */
    float a = (float)((double)b->obstruction * b->obstruction_lf);
    float v1 = (float)(occ_max(b->occlusion_direct, b->occlusion_lf) * b->occlusion + a);
    if (penv) {
        const eax4_send *ss = &s->send[pi];
        v1 = (float)(occ_max(ss->occlusion_direct_ratio, ss->occlusion_lf) * ss->occlusion + v1);
    }
    float rs = (float)((double)g->rolloff_factor + b->rolloff);
    float q = (float)((double)rs * X + 1.0);
    float lg = (float)((double)(float)log10((double)q) * 2000.0);
    float dv = (float)((double)v1 - lg);
    int32_t dx = trunc_mb(dv);
    out->direct = mb_gain(b->direct + dx);

    /* direct HF */
    float occ = (float)b->occlusion;
    float hc = (float)((double)occ * b->occlusion_direct + b->obstruction);
    float h = (float)((double)hc - occ_max(b->occlusion_direct, b->occlusion_lf) * occ);
    float air_hf = c->air_absorption_hf;
    if (penv) {
        const eax4_send *ss = &s->send[pi];
        float socc = (float)ss->occlusion;
        h = (float)((double)h + (double)ss->occlusion_direct_ratio * socc);
        h = (float)((double)h - occ_max(ss->occlusion_direct_ratio, ss->occlusion_lf) * socc);
        if (prv) air_hf = prv->p.air_absorption_hf;
    }
    h = (float)((double)(float)air_hf * b->air_absorption * g->distance_factor * c->distance_factor * rs_d(g, b) * X + h);
    if (b->flags & EAXB_DIRECTHFAUTO) h = (float)((double)b->outside_volume_hf * g->cone_factor + h);
    int32_t hx = trunc_mb(h);
    out->direct_hf = mb_gain(b->direct_hf + hx);

    out->hf_reference = penv && prv ? prv->p.hf_reference : c->hf_reference;

    /* active slots */
    unsigned mask = 0;
    for (int k = 0; k < 2; k++) {
        if (geq(&s->active[k], &EAX_PRIMARY_SLOT_GUID)) { if (pi >= 0) mask |= 1u << pi; }
        else { int i = eax4_slot_index(&s->active[k]); if (i >= 0) mask |= 1u << i; }
    }

    for (int i = 0; i < 4; i++) {
        const eax4_send *ss = &s->send[i];
        const eax4_slot *sl = &slots[i];
        out->send[i] = out->send_hf[i] = 0.0f;
        if (!(mask >> i & 1)) continue;
        if (!(sl->flags & EAX4SLOT_ENVIRONMENT)) {
            /* a plain send: follows the direct path's attenuation */
            out->send[i] = mb_gain_nc(ss->room + dx);
            out->send_hf[i] = mb_gain(ss->room_hf + hx);
            continue;
        }
        const eax3_listener *rv = sl->type == EAX4FX_REVERB ? &sl->reverb : NULL;
        /* room */
        float rl = 0.0f;
        if (i == pi) {
            float ex = (float)((double)b->exclusion * b->exclusion_lf + 0.0);
            rl = (float)(occ_max(b->occlusion_room, b->occlusion_lf) * b->occlusion + ex);
        }
        float t = (float)(occ_max(ss->occlusion_lf, ss->occlusion_room_ratio) * ss->occlusion + rl);
        float rlev = (float)((double)ss->exclusion_lf * ss->exclusion + t);
        float aut = (b->flags & EAXB_ROOMAUTO) ? eax_cone_auto(g, 0) : 0.0f;
        if (!(rlev <= aut)) rlev = aut;
        float rr = rv ? rv->p.room_rolloff : 0.0f;
        rr = (float)((double)b->room_rolloff + rr);
        float q2 = (float)((double)rr * X + 1.0);
        float lg2 = (float)((double)(float)log10((double)q2) * 2000.0);
        float r = (float)((double)rlev - lg2);
        if ((b->flags & EAXB_ROOMAUTO) && rv)
            r = (float)((double)g->distance_factor * -6000.0 * c->distance_factor * rs_d(g, b) * X /
                        ((double)rv->p.decay_time * 342.3800048828125) + r);
        out->send[i] = mb_gain(ss->room + b->room + trunc_mb(r));
        /* room HF */
        float rh0 = 0.0f;
        if (i == pi) {
            float o = (float)((double)b->occlusion * b->occlusion_room + 0.0);
            rh0 = (float)((double)o + b->exclusion);
        }
        float rh1 = (float)((double)ss->occlusion * ss->occlusion_room_ratio + rh0);
        float rh = (float)((double)rh1 + ss->exclusion);
        float a2 = (b->flags & EAXB_ROOMHFAUTO) ? eax_cone_auto(g, b->outside_volume_hf) : aut;
        if (a2 <= rh) rh = a2;
        rh = (float)((double)rh - rlev);
        if (rv) {
            if (b->flags & EAXB_ROOMAUTO) {
                float dh = (float)((double)rv->p.decay_hf_ratio * rv->p.decay_time);
                if (rv->p.flags & EAXL_DECAYHFLIMIT) {
                    double lim = -6000.0 / ((double)rv->p.air_absorption_hf * 342.3800048828125);
                    if (!((double)dh < lim)) dh = (float)lim;
                }
                rh = (float)((double)g->distance_factor * -6000.0 * c->distance_factor * rs_d(g, b) * X /
                             ((double)dh * 342.3800048828125) + rh);
            }
            rh = (float)((double)rv->p.air_absorption_hf * b->air_absorption * g->distance_factor * c->distance_factor *
                         rs_d(g, b) * X + rh);
        }
        out->send_hf[i] = mb_gain(ss->room_hf + b->room_hf + trunc_mb(rh));
    }

    /* Doppler: listener x source factor (max 10) and the distance factor for the speed of sound */
    float dop = (float)((double)ds3d_doppler * b->doppler);
    if (10.0f < dop) dop = 10.0f;
    out->doppler_factor = dop;
    double df = (double)g->distance_factor * c->distance_factor;
    /* (ALL does not check the context's factor: a NaN one is capped like an infinite one, not passed on) */
    out->distance_factor = (float)(!(df <= 3.4028234663852886e+38) ? 3.4028234663852886e+38 : df);
}
