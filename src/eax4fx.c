/* reAL - EAX 4.0 effect properties other than reverb (AGC compressor, autowah, chorus, distortion,
 * echo, equalizer, flanger, frequency shifter, pitch shifter, ring modulator, vocal morpher).
 *
 * ALchemy's per-effect setters (slot handler 0x10028540, effect cases 1..12) accept exactly the EAX 4
 * SDK ranges, compared in float with float limits (NaN rejected); ALLPARAMETERS is all or nothing.
 * Verified against ALchemy in the emulator (re/gen_eax4fx_gt.py, tests/test_eax4.c).
 * Defaults are ALchemy's tables at .rdata 0x100b4444.. (= the SDK's, except the flanger delay: ALchemy
 * has 0.0002 s, the SDK 0.002 s; reAL uses the SDK value). */
#include "eax.h"
#include <string.h>

typedef struct fx_field { char kind; float lo, hi; float def; } fx_field;   /* 'i' int, 'u' unsigned, 'f' float */
typedef struct fx_spec { int n; fx_field f[10]; } fx_spec;

static const fx_spec k_fx[EAX4FX_COUNT] = {
    [EAX4FX_AGCCOMPRESSOR] = { 1, { { 'u', 0, 1, 1 } } },
    [EAX4FX_AUTOWAH] = { 4, { { 'f', 0.0001f, 1.0f, 0.06f }, { 'f', 0.0001f, 1.0f, 0.06f }, { 'i', 600, 6000, 6000 },
                              { 'i', -9000, 9000, 2100 } } },
    [EAX4FX_CHORUS] = { 6, { { 'u', 0, 1, 1 }, { 'i', -180, 180, 90 }, { 'f', 0, 10, 1.1f }, { 'f', 0, 1, 0.1f },
                             { 'f', -1, 1, 0.25f }, { 'f', 0, 0.016f, 0.016f } } },
    [EAX4FX_DISTORTION] = { 5, { { 'f', 0, 1, 0.2f }, { 'i', -6000, 0, -2600 }, { 'f', 80, 24000, 8000 },
                                 { 'f', 80, 24000, 3600 }, { 'f', 80, 24000, 3600 } } },
    [EAX4FX_ECHO] = { 5, { { 'f', 0, 0.207f, 0.1f }, { 'f', 0, 0.404f, 0.1f }, { 'f', 0, 0.99f, 0.5f }, { 'f', 0, 1, 0.5f },
                           { 'f', -1, 1, -1.0f } } },
    [EAX4FX_EQUALIZER] = { 10, { { 'i', -1800, 1800, 0 }, { 'f', 50, 800, 200 }, { 'i', -1800, 1800, 0 },
                                 { 'f', 200, 3000, 500 }, { 'f', 0.01f, 1, 1 }, { 'i', -1800, 1800, 0 },
                                 { 'f', 1000, 8000, 3000 }, { 'f', 0.01f, 1, 1 }, { 'i', -1800, 1800, 0 },
                                 { 'f', 4000, 16000, 6000 } } },
    [EAX4FX_FLANGER] = { 6, { { 'u', 0, 1, 1 }, { 'i', -180, 180, 0 }, { 'f', 0, 10, 0.27f }, { 'f', 0, 1, 1.0f },
                              { 'f', -1, 1, -0.5f }, { 'f', 0, 0.004f, 0.002f } } },
    [EAX4FX_FREQSHIFTER] = { 3, { { 'f', 0, 24000, 0 }, { 'u', 0, 2, 0 }, { 'u', 0, 2, 0 } } },
    [EAX4FX_PITCHSHIFTER] = { 2, { { 'i', -12, 12, 12 }, { 'i', -50, 50, 0 } } },
    [EAX4FX_RINGMODULATOR] = { 3, { { 'f', 0, 8000, 440 }, { 'f', 0, 24000, 800 }, { 'u', 0, 2, 0 } } },
    [EAX4FX_VOCALMORPHER] = { 6, { { 'u', 0, 29, 0 }, { 'i', -24, 24, 0 }, { 'u', 0, 29, 10 }, { 'i', -24, 24, 0 },
                                   { 'u', 0, 2, 0 }, { 'f', 0, 10, 1.41f } } },
};

static int valid(const fx_field *f, uint32_t w)
{
    switch (f->kind) {
    case 'f': { float v; memcpy(&v, &w, 4); return v >= f->lo && v <= f->hi; }
    case 'u': return w >= (uint32_t)f->lo && w <= (uint32_t)f->hi;
    default:  return (int32_t)w >= (int32_t)f->lo && (int32_t)w <= (int32_t)f->hi;
    }
}

/* store like ALchemy: a float is only written when it compares unequal (so -0 does not replace 0) */
static void store(const fx_field *f, uint32_t *dst, uint32_t w)
{
    if (f->kind == 'f') { float a, b; memcpy(&a, dst, 4); memcpy(&b, &w, 4); if (a == b) return; }
    *dst = w;
}

static uint32_t encode(const fx_field *f, float v)
{
    uint32_t w;
    if (f->kind == 'f') memcpy(&w, &v, 4);
    else w = (uint32_t)(int32_t)v;
    return w;
}

int eax4fx_effect_props(int type) { return type > 0 && type < EAX4FX_COUNT ? k_fx[type].n : 0; }

/* the value of field i as a float (for the DSP) */
float eax4fx_value(int type, const uint32_t fx[16], int i)
{
    const fx_field *f = &k_fx[type].f[i];
    if (f->kind == 'f') { float v; memcpy(&v, &fx[i], 4); return v; }
    if (f->kind == 'u') return (float)fx[i];
    return (float)(int32_t)fx[i];
}

void eax4fx_effect_defaults(int type, uint32_t fx[16])
{
    memset(fx, 0, 16 * 4);
    if (type <= 0 || type >= EAX4FX_COUNT) return;
    for (int i = 0; i < k_fx[type].n; i++) fx[i] = encode(&k_fx[type].f[i], k_fx[type].f[i].def);
}

int eax4fx_effect_set(int type, uint32_t fx[16], uint32_t id, const void *data, uint32_t len)
{
    if (type <= 0 || type >= EAX4FX_COUNT || type == EAX4FX_REVERB) return EAX_BADVALUE;
    const fx_spec *s = &k_fx[type];
    id &= ~EAX_DEFERRED;
    if (id == 0) return EAX_OK;
    if (id == 1) {
        uint32_t w[10];
        if (!data || len < 4u * s->n) return EAX_BADVALUE;
        memcpy(w, data, 4u * s->n);
        for (int i = 0; i < s->n; i++) if (!valid(&s->f[i], w[i])) return EAX_BADVALUE;
        for (int i = 0; i < s->n; i++) store(&s->f[i], &fx[i], w[i]);
        return EAX_OK;
    }
    if (id < 2 || id >= 2u + s->n || !data || len < 4) return EAX_BADVALUE;
    uint32_t w; memcpy(&w, data, 4);
    if (!valid(&s->f[id - 2], w)) return EAX_BADVALUE;
    store(&s->f[id - 2], &fx[id - 2], w);
    return EAX_OK;
}

int eax4fx_effect_get(int type, const uint32_t fx[16], uint32_t id, void *data, uint32_t len, uint32_t *ret)
{
    if (type <= 0 || type >= EAX4FX_COUNT || type == EAX4FX_REVERB) return EAX_BADVALUE;
    const fx_spec *s = &k_fx[type];
    id &= ~EAX_DEFERRED;
    uint32_t n;
    if (id == 0) n = 0;
    else if (id == 1) n = 4u * s->n;
    else if (id >= 2 && id < 2u + s->n) n = 4;
    else return EAX_BADVALUE;
    if (n && (!data || len < n)) return EAX_BADVALUE;
    if (id == 1) memcpy(data, fx, n);
    else if (n) memcpy(data, &fx[id - 2], 4);
    if (ret) *ret = n;
    return EAX_OK;
}
