#include "spatial.h"
#include "mixcore.h"
#include <math.h>

static rl_vec3 v3(float x, float y, float z) { rl_vec3 r = { x, y, z }; return r; }
static rl_vec3 sub(rl_vec3 a, rl_vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float   dot(rl_vec3 a, rl_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static rl_vec3 cross(rl_vec3 a, rl_vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static float   len(rl_vec3 a) { return sqrtf(dot(a, a)); }
static rl_vec3 norm(rl_vec3 a, rl_vec3 fallback)
{
    float l = len(a);
    return l > 1e-6f ? v3(a.x / l, a.y / l, a.z / l) : fallback;
}

void rl_listener_defaults(rl_listener3d *l)
{
    l->pos = v3(0, 0, 0); l->vel = v3(0, 0, 0);
    l->front = v3(0, 0, 1); l->top = v3(0, 1, 0);
    l->distance_factor = 1.0f; l->rolloff_factor = 1.0f; l->doppler_factor = 1.0f;
}

void rl_source_defaults(rl_source3d *s)
{
    s->pos = v3(0, 0, 0); s->vel = v3(0, 0, 0); s->cone_orient = v3(0, 0, 1);
    s->cone_inside = 360; s->cone_outside = 360; s->cone_outside_volume = 0;
    s->min_dist = 1.0f; s->max_dist = 1000000000.0f;
    s->mode = RL_DS3DMODE_NORMAL; s->mute_at_max = 0;
}

void rl_spatial_geometry(const rl_listener3d *l, const rl_source3d *s, rl_geom *g)
{
    g->pan[0] = g->pan[1] = 1.0f;
    g->dist_gain = g->cone_gain = 1.0f;
    g->x = 0.0f;
    g->muted = 0;
    if (s->mode == RL_DS3DMODE_DISABLE) { g->cone_factor = 1.0f; return; }   /* as ALchemy */
    g->cone_factor = 0.0f;

    /* DirectSound is left-handed: +x right, +y up, +z forward. right = top x front. */
    rl_vec3 front = norm(l->front, v3(0, 0, 1));
    rl_vec3 top   = norm(l->top, v3(0, 1, 0));
    rl_vec3 right = norm(cross(top, front), v3(1, 0, 0));

    rl_vec3 rel; /* source position in listener space */
    if (s->mode == RL_DS3DMODE_HEADRELATIVE) {
        rel = s->pos;
    } else {
        rl_vec3 d = sub(s->pos, l->pos);
        rel = v3(dot(d, right), dot(d, top), dot(d, front));
    }
    float dist = len(rel);

    /* distance attenuation, DS3D inverse model with rolloff */
    if (s->mute_at_max && dist > s->max_dist) {
        g->dist_gain = 0.0f;
        g->muted = 1;
    } else {
        float d = dist < s->min_dist ? s->min_dist : dist;
        if (d > s->max_dist) d = s->max_dist;
        float denom = s->min_dist + l->rolloff_factor * (d - s->min_dist);
        if (denom > 1e-6f) g->dist_gain = s->min_dist / denom;
        if (g->dist_gain > 1.0f) g->dist_gain = 1.0f;
    }
    if (dist > s->min_dist && s->max_dist > s->min_dist && s->min_dist > 0.0f)
        g->x = (dist < s->max_dist ? dist : s->max_dist) / s->min_dist - 1.0f;

    /* sound cone: ALchemy interpolates the outside volume in millibels */
    if ((s->cone_inside < 360 || s->cone_outside < 360) && dist > 1e-6f) {
        rl_vec3 to_listener = (s->mode == RL_DS3DMODE_HEADRELATIVE)
            ? v3(-rel.x, -rel.y, -rel.z) : sub(l->pos, s->pos);
        rl_vec3 o = norm(s->cone_orient, v3(0, 0, 1));
        float c = dot(norm(to_listener, o), o);
        if (c > 1.0f) c = 1.0f; else if (c < -1.0f) c = -1.0f;
        float angle = acosf(c) * (360.0f / 3.14159265f); /* full cone angle equivalent */
        float in = (float)s->cone_inside, out = (float)s->cone_outside;
        if (angle >= out) g->cone_factor = 1.0f;
        else if (angle > in && out > in) g->cone_factor = (angle - in) / (out - in);
        g->cone_gain = rl_db100_to_gain((int32_t)lrintf((float)s->cone_outside_volume * g->cone_factor));
    }

    /* equal power pan from azimuth */
    float x = dist > 1e-6f ? rel.x / dist : 0.0f;
    float a = (x + 1.0f) * 0.78539816f; /* 0..pi/2 */
    g->pan[0] = cosf(a) * 1.41421356f;
    g->pan[1] = sinf(a) * 1.41421356f;
    if (g->pan[0] > 1.0f) g->pan[0] = 1.0f; /* keep centre at unity, sides <= unity */
    if (g->pan[1] > 1.0f) g->pan[1] = 1.0f;
}

float rl_spatial_doppler(const rl_listener3d *l, const rl_source3d *s, float factor)
{
    if (s->mode == RL_DS3DMODE_DISABLE || factor == 0.0f) return 1.0f;
    rl_vec3 d, lv;
    if (s->mode == RL_DS3DMODE_HEADRELATIVE) {   /* listener at the origin, not moving */
        d = s->pos;
        lv = v3(0, 0, 0);
    } else {
        d = sub(s->pos, l->pos);
        lv = l->vel;
    }
    float dist = len(d);
    if (!(dist > 0.0f)) return 1.0f;
    rl_vec3 u = v3(d.x / dist, d.y / dist, d.z / dist);   /* listener -> source */
    double vs = (double)dot(s->vel, u);                   /* source moving away */
    double vl = (double)dot(lv, u);                       /* listener moving towards the source */
    if (vs == vl) return 1.0f;
    float df = l->distance_factor > 0.0f ? l->distance_factor : 1.0f;
    float c = (float)(342.3800048828125 / df);
    float r = (float)((c + factor * vl) / (factor * vs + c));
    if (r < 0.0f) return 0.0f;
    return r > 10.0f ? 10.0f : r;
}

static int is_zero(rl_vec3 v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

uint32_t rl_spatial_doppler_ds(const rl_listener3d *l, const rl_source3d *s, uint32_t freq)
{
    float df = l->doppler_factor;
    if (!(df > 0.0f) || (is_zero(l->vel) && is_zero(s->vel))) return freq;
    float v = 0.0f;
    if (s->mode == RL_DS3DMODE_NORMAL || s->mode == RL_DS3DMODE_HEADRELATIVE) {
        rl_vec3 lp = v3(0, 0, 0), lv = v3(0, 0, 0);
        if (s->mode == RL_DS3DMODE_NORMAL) { lp = l->pos; lv = l->vel; }
        rl_vec3 a = sub(s->pos, lp), b = sub(s->vel, lv);         /* listener -> source; relative velocity */
        if (!is_zero(a)) {
            float d = (float)((double)a.y * b.y + (double)a.x * b.x + (double)a.z * b.z);
            float n = (float)sqrt((double)a.x * a.x + (double)a.y * a.y + (double)a.z * a.z);
            v = (float)((double)d / n);                           /* positive: moving apart */
        }
    }
    double w = (double)v * l->distance_factor * 1000.0 * df;     /* mm/s */
    if (!(w <= 3.4028234663852886e+38)) w = 3.4028234663852886e+38;
    if (w < -3.4028234663852886e+38) w = -3.4028234663852886e+38;
    v = (float)w;
    if (v > 179830.0f) v = 179830.0f;
    if (v < -179830.0f) v = -179830.0f;
    float f = (float)((double)(float)freq * 359660.0 / (359660.0 + v));
    return (uint32_t)(int64_t)f;
}

int32_t rl_sink_cents(uint32_t freq, uint32_t rate)
{
    return freq && rate ? (int32_t)(log10((double)freq / (double)rate) * 3986.3137138648344) : 0;
}

void rl_spatialize(const rl_listener3d *l, const rl_source3d *s, float gains[2], float *dist_gain)
{
    rl_geom g;
    rl_spatial_geometry(l, s, &g);
    float gain = g.dist_gain * g.cone_gain;
    if (dist_gain) *dist_gain = gain;
    gains[0] = g.pan[0] * gain;
    gains[1] = g.pan[1] * gain;
}

void rl_spatial_direction(const rl_listener3d *l, const rl_source3d *s, float *az, float *el)
{
    rl_vec3 rel;
    if (s->mode == RL_DS3DMODE_HEADRELATIVE) {
        rel = s->pos;
    } else {
        rl_vec3 front = norm(l->front, v3(0, 0, 1));
        rl_vec3 top   = norm(l->top, v3(0, 1, 0));
        rl_vec3 right = norm(cross(top, front), v3(1, 0, 0));
        rl_vec3 d = sub(s->pos, l->pos);
        rel = v3(dot(d, right), dot(d, top), dot(d, front));
    }
    float h = sqrtf(rel.x * rel.x + rel.z * rel.z);
    if (h < 1e-6f && fabsf(rel.y) < 1e-6f) { *az = 0.0f; *el = 0.0f; return; }
    *az = atan2f(rel.x, rel.z) * (180.0f / 3.14159265f);
    if (*az < 0.0f) *az += 360.0f;
    *el = atan2f(rel.y, h) * (180.0f / 3.14159265f);
}

float rl_spatial_distance(const rl_listener3d *l, const rl_source3d *s)
{
    if (s->mode == RL_DS3DMODE_HEADRELATIVE) return len(s->pos);
    return len(sub(s->pos, l->pos));
}
