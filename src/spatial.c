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

void rl_spatialize(const rl_listener3d *l, const rl_source3d *s, float gains[2], float *dist_gain)
{
    if (dist_gain) *dist_gain = 1.0f;
    if (s->mode == RL_DS3DMODE_DISABLE) { gains[0] = gains[1] = 1.0f; return; }

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
    float gain = 1.0f;
    if (s->mute_at_max && dist > s->max_dist) gain = 0.0f;
    else {
        float d = dist < s->min_dist ? s->min_dist : dist;
        if (d > s->max_dist) d = s->max_dist;
        float denom = s->min_dist + l->rolloff_factor * (d - s->min_dist);
        if (denom > 1e-6f) gain = s->min_dist / denom;
        if (gain > 1.0f) gain = 1.0f;
    }

    /* sound cone */
    if (gain > 0.0f && (s->cone_inside < 360 || s->cone_outside < 360) && dist > 1e-6f) {
        rl_vec3 to_listener = (s->mode == RL_DS3DMODE_HEADRELATIVE)
            ? v3(-rel.x, -rel.y, -rel.z) : sub(l->pos, s->pos);
        rl_vec3 o = norm(s->cone_orient, v3(0, 0, 1));
        float c = dot(norm(to_listener, o), o);
        if (c > 1.0f) c = 1.0f; else if (c < -1.0f) c = -1.0f;
        float angle = acosf(c) * (360.0f / 3.14159265f); /* full cone angle equivalent */
        float in = (float)s->cone_inside, out = (float)s->cone_outside;
        float cg = 1.0f, og = rl_db100_to_gain(s->cone_outside_volume);
        if (angle >= out) cg = og;
        else if (angle > in && out > in) cg = 1.0f + (og - 1.0f) * (angle - in) / (out - in);
        gain *= cg;
    }

    if (dist_gain) *dist_gain = gain;

    /* equal power pan from azimuth */
    float x = dist > 1e-6f ? rel.x / dist : 0.0f;
    float a = (x + 1.0f) * 0.78539816f; /* 0..pi/2 */
    gains[0] = cosf(a) * 1.41421356f * gain;
    gains[1] = sinf(a) * 1.41421356f * gain;
    if (gains[0] > gain) gains[0] = gain; /* keep centre at unity, sides <= unity */
    if (gains[1] > gain) gains[1] = gain;
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
