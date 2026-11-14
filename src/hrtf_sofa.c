/* reAL - an HRTF set from a SOFA file (AES69: SimpleFreeFieldHRIR and the like), hrtf=path.sofa.
 *
 * libmysofa's reader (src/sofa, BSD licence) reads the file; this turns its impulse responses into a set as
 * reAL_hrtf.bin holds one, the way tools/hrtf_build.py and tools/hrtf_kemar.py make theirs:
 * - one distance: the one most directions were measured at (a set measured at several; the farthest of a tie);
 * - each ear's onset becomes a whole-sample delay (the louder ear's first sample above 10% of its peak, less 2; the
 *   other ear's that and the time between the ears, from the two responses' cross-correlation below 1.5 kHz),
 *   counted from the earliest onset of the set, with Data.Delay added, and the 64 taps (at 48 kHz; as long a time at
 *   the file's rate) from there are kept, the last eighth faded out;
 * - one level for the whole set, the horizontal ring's mean energy at the built-in set's (0.82);
 * - rings of directions: a set measured in rings of elevation (most are) keeps its rings and its azimuths, laid out
 *   evenly (between two measured azimuths, the responses are mixed); any other layout (CIPIC's interaural grid) is
 *   sampled onto rings 10 degrees apart from its three nearest measured directions.
 * Then it loads as a reAL_hrtf.bin would (rl_hrtf_load: resampled to the output's rate). */
#include "hrtf.h"
#include "sofa/hrtf/mysofa.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARGIN 2
#define TARGET_E0 0.82
#define MAX_M 100000                /* (more directions than any real set: a damaged file) */

char *mysofa_getAttribute(struct MYSOFA_ATTRIBUTE *attr, char *name);

typedef struct { float az, el; int ok; } pos;     /* az clockwise from the front, as reAL counts it */
typedef struct { float d[2]; float *c[2]; } resp;

static int isfin(float f)                 /* (by its bits: safe under -ffast-math too) */
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x7f800000u) != 0x7f800000u;
}
static float angle(const pos *a, const pos *b)
{
    const double r = M_PI / 180.0;
    double x = sin(a->el * r) * sin(b->el * r) + cos(a->el * r) * cos(b->el * r) * cos((a->az - b->az) * r);
    return (float)(acos(x > 1 ? 1 : x < -1 ? -1 : x) / r);
}
static void unit(const pos *a, float v[3])
{
    const double r = M_PI / 180.0;
    v[0] = (float)(cos(a->el * r) * cos(a->az * r));
    v[1] = (float)(cos(a->el * r) * sin(a->az * r));
    v[2] = (float)sin(a->el * r);
}
typedef struct { float az; int ix; } azix;
static int cmp_azix(const void *a, const void *b)
{
    const azix *x = a, *y = b;
    return x->az < y->az ? -1 : x->az > y->az ? 1 : x->ix - y->ix;
}
static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    return p + 4;
}
static uint8_t *putf(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); return put32(p, u); }

/* a direction of the output set: the weighted mix of measured responses */
typedef struct { int n; int idx[3]; float w[3]; } mix;

static int fail(char *why, int n, const char *msg) { if (why && n) snprintf(why, n, "%s", msg); return 0; }

int rl_hrtf_load_sofa(rl_hrtf_set *set, const void *data, uint32_t size, uint32_t out_rate, char *why, int whylen)
{
    memset(set, 0, sizeof(*set));
    int err = 0;
    struct MYSOFA_HRTF *h = mysofa_load_data(data, size, &err);
    if (!h) {
        char m[64];
        snprintf(m, sizeof(m), "not a SOFA file libmysofa reads (error %d)", err);
        return fail(why, whylen, m);
    }
    int ok = 0;
    pos *p = NULL;
    resp *rs = NULL;
    float *coefs = NULL, *rings_el = NULL, *lp = NULL, *vec = NULL;
    mix *grid = NULL;
    uint8_t *blob = NULL;
    uint32_t *ring_n = NULL;
    const char *dt = mysofa_getAttribute(h->attributes, "DataType");
    const uint32_t M = h->M, R = h->R, N = h->N;
    if (dt && strcmp(dt, "FIR")) { fail(why, whylen, "not impulse responses (DataType is not FIR)"); goto out; }
    if (R != 2) { fail(why, whylen, "not two ears (R is not 2)"); goto out; }
    if (!M || M > MAX_M || !N || N > 1u << 20 || !h->DataIR.values || h->DataIR.elements != (uint64_t)M * R * N ||
        !h->SourcePosition.values || h->SourcePosition.elements != M * 3 || !h->DataSamplingRate.values ||
        h->DataSamplingRate.elements < 1) { fail(why, whylen, "its arrays are not the sizes it declares"); goto out; }
    const float frate = h->DataSamplingRate.values[0];
    if (!isfin(frate) || frate < 8000.0f || frate > 384000.0f) {
        fail(why, whylen, "its sampling rate is out of range");
        goto out;
    }
    const uint32_t rate = (uint32_t)lrintf(frate);
    const float *delays = h->DataDelay.values;
    const uint32_t ndelay = h->DataDelay.elements;
    if (delays && ndelay != R && ndelay != M * R) delays = NULL;     /* (a size neither per set nor per direction) */

    /* the directions, in reAL's terms (SOFA's azimuth goes counterclockwise) */
    p = calloc(M, sizeof(*p));
    float *dist = malloc(M * sizeof(float));
    if (!p || !dist) { free(dist); fail(why, whylen, "out of memory"); goto out; }
    const char *type = mysofa_getAttribute(h->SourcePosition.attributes, "Type");
    const int cart = type && !strcmp(type, "cartesian");
    for (uint32_t m = 0; m < M; m++) {
        const float *v = h->SourcePosition.values + 3 * m;
        float az, el, r;
        if (cart) {
            double xy = sqrt((double)v[0] * v[0] + (double)v[1] * v[1]);
            az = (float)(atan2(v[1], v[0]) * 180.0 / M_PI);
            el = (float)(atan2(v[2], xy) * 180.0 / M_PI);
            r = (float)sqrt(xy * xy + (double)v[2] * v[2]);
        } else { az = v[0]; el = v[1]; r = v[2]; }
        dist[m] = r;
        p[m].ok = isfin(az) && isfin(el) && isfin(r) && el >= -90.5f && el <= 90.5f && fabsf(az) < 1e5f;
        if (!p[m].ok) continue;
        az = fmodf(-az, 360.0f);
        p[m].az = az < 0.0f ? az + 360.0f : az >= 360.0f ? 0.0f : az;
        p[m].el = el > 90.0f ? 90.0f : el < -90.0f ? -90.0f : el;
    }
    {   /* the distance most directions have (to the centimetre) */
        float best = 0.0f;
        uint32_t bestn = 0;
        float *sorted = malloc(M * sizeof(float));
        if (!sorted) { free(dist); fail(why, whylen, "out of memory"); goto out; }
        uint32_t k = 0;
        for (uint32_t m = 0; m < M; m++) if (p[m].ok) sorted[k++] = roundf(dist[m] * 100.0f);
        qsort(sorted, k, sizeof(float), cmp_float);
        for (uint32_t i = 0; i < k; ) {
            uint32_t j = i;
            while (j < k && sorted[j] == sorted[i]) j++;
            if (j - i >= bestn) { bestn = j - i; best = sorted[i]; }    /* (a tie: the farther one) */
            i = j;
        }
        free(sorted);
        for (uint32_t m = 0; m < M; m++) if (p[m].ok && roundf(dist[m] * 100.0f) != best) p[m].ok = 0;
    }
    free(dist);

    /* each response: its ears' onsets and taps.  The louder ear's onset is its first sample above a tenth of its
     * peak; the other ear's is that and the time between the ears, where the two responses (below 1.5 kHz)
     * correlate best within a millisecond.  (The quieter ear's own onset can come too early: a far ear's response is
     * weak, and some sets have low ripple before the sound arrives) */
    uint32_t taps = (uint32_t)ceil(64.0 * rate / 48000.0);
    if (taps > RL_HRTF_MAX_TAPS) taps = RL_HRTF_MAX_TAPS;
    if (taps > N) taps = N;
    const uint32_t fade = taps / 8;
    const int maxlag = (int)(rate / 1000);
    rs = calloc(M, sizeof(*rs));
    coefs = calloc((size_t)M * 2 * taps, sizeof(float));
    lp = malloc(2 * (size_t)N * sizeof(float));
    if (!rs || !coefs || !lp) { fail(why, whylen, "out of memory"); goto out; }
    const float a1 = expf(-2.0f * (float)M_PI * 1500.0f / (float)rate);
    float base = INFINITY;
    uint32_t nok = 0;
    for (uint32_t m = 0; m < M; m++) {
        if (!p[m].ok) continue;
        const float *ir[2];
        float peak[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < 2; ch++) {
            ir[ch] = h->DataIR.values + ((size_t)m * R + ch) * N;
            for (uint32_t k = 0; k < N; k++) {
                if (!isfin(ir[ch][k])) { peak[ch] = 0.0f; break; }
                if (fabsf(ir[ch][k]) > peak[ch]) peak[ch] = fabsf(ir[ch][k]);
            }
            if (!(peak[ch] > 0.0f)) p[m].ok = 0;        /* (silent or broken: left out) */
        }
        if (!p[m].ok) continue;
        const int loud = peak[1] > peak[0];
        uint32_t on = 0;
        while (fabsf(ir[loud][on]) <= 0.1f * peak[loud]) on++;
        /* (around the onset only: a long response's tail says nothing of it) */
        const uint32_t w0 = on > (uint32_t)maxlag ? on - maxlag : 0;
        const uint32_t w1 = on + 2u * maxlag + taps < N ? on + 2u * maxlag + taps : N;
        const int wn = (int)(w1 - w0);
        for (int ch = 0; ch < 2; ch++) {                /* (low-passed both ways: no phase shift) */
            float *y = lp + (size_t)ch * N, z = 0.0f;
            for (int k = 0; k < wn; k++) y[k] = z = ir[ch][w0 + k] + a1 * (z - ir[ch][w0 + k]);
            z = 0.0f;
            for (int k = wn; k-- > 0; ) y[k] = z = y[k] + a1 * (z - y[k]);
        }
        int best = 0;                                   /* (right ear later by this many samples) */
        double bestc = -INFINITY;
        for (int lag = -maxlag; lag <= maxlag; lag++) {
            const float *x = lp + (lag > 0 ? 0 : -lag), *y = lp + N + (lag > 0 ? lag : 0);
            const int cnt = wn - (lag > 0 ? lag : -lag);
            float c0 = 0.0f, c1 = 0.0f, c2 = 0.0f, c3 = 0.0f;   /* (four sums: the compiler can vectorise them) */
            int k = 0;
            for (; k + 4 <= cnt; k += 4) {
                c0 += x[k] * y[k]; c1 += x[k + 1] * y[k + 1]; c2 += x[k + 2] * y[k + 2]; c3 += x[k + 3] * y[k + 3];
            }
            for (; k < cnt; k++) c0 += x[k] * y[k];
            const double c = (double)c0 + c1 + c2 + c3;
            if (c > bestc) { bestc = c; best = lag; }
        }
        long onset[2];
        onset[loud] = on;
        onset[!loud] = (long)on + (loud ? -best : best);
        for (int ch = 0; ch < 2; ch++) {
            long o = onset[ch] - MARGIN;
            if (o < 0) o = 0;
            if (o > (long)N) o = N;
            float extra = delays ? delays[ndelay == R ? (uint32_t)ch : m * R + ch] : 0.0f;
            if (!isfin(extra) || extra < 0.0f || extra > 1e5f) extra = 0.0f;
            rs[m].d[ch] = (float)o + extra;
            rs[m].c[ch] = coefs + ((size_t)m * 2 + ch) * taps;
            for (uint32_t k = 0; k < taps && o + k < N; k++) {
                float f = k < taps - fade ? 1.0f :
                          0.5f + 0.5f * cosf((float)M_PI * (float)(k - (taps - fade) + 1) / fade);
                rs[m].c[ch][k] = ir[ch][o + k] * f;
            }
        }
        nok++;
        for (int ch = 0; ch < 2; ch++) if (rs[m].d[ch] < base) base = rs[m].d[ch];
    }
    if (!nok) { fail(why, whylen, "no usable responses"); goto out; }
    for (uint32_t m = 0; m < M; m++) if (p[m].ok) { rs[m].d[0] -= base; rs[m].d[1] -= base; }

    /* rings: the measured ones (elevations to half a degree), when most directions lie on rings of four or more */
    uint32_t nrings = 0, ndirs = 0;
    rings_el = malloc(RL_HRTF_MAX_RINGS * sizeof(float));
    ring_n = malloc(RL_HRTF_MAX_RINGS * sizeof(uint32_t));
    float *els = malloc(nok * sizeof(float));
    if (!rings_el || !ring_n || !els) { free(els); fail(why, whylen, "out of memory"); goto out; }
    {
        uint32_t k = 0, onrings = 0;
        for (uint32_t m = 0; m < M; m++) if (p[m].ok) els[k++] = roundf(p[m].el * 2.0f) / 2.0f;
        qsort(els, k, sizeof(float), cmp_float);
        for (uint32_t i = 0; i < k; ) {
            uint32_t j = i;
            while (j < k && els[j] == els[i]) j++;
            if (j - i >= 4 || fabsf(els[i]) >= 89.5f) {
                onrings += j - i;
                if (nrings < RL_HRTF_MAX_RINGS) { rings_el[nrings] = els[i]; ring_n[nrings] = 0; }
                nrings++;
            }
            i = j;
        }
        if (nrings > RL_HRTF_MAX_RINGS || onrings < k * 0.8) nrings = 0;      /* (not a set of rings) */
    }
    free(els);
    size_t gridcap = 0;
    if (nrings) {
        /* each ring: its measured azimuths (to half a degree), evenly laid out; a direction between two measured
         * ones mixes them by how near it is to each */
        for (uint32_t r = 0; r < nrings; r++) {
            uint32_t cnt = 0;
            for (uint32_t m = 0; m < M; m++) if (p[m].ok && roundf(p[m].el * 2.0f) / 2.0f == rings_el[r]) cnt++;
            azix *az = malloc(cnt * sizeof(*az));
            if (!az) { fail(why, whylen, "out of memory"); goto out; }
            uint32_t k = 0;
            for (uint32_t m = 0; m < M; m++)
                if (p[m].ok && roundf(p[m].el * 2.0f) / 2.0f == rings_el[r]) {
                    az[k].az = p[m].az; az[k].ix = (int)m; k++;
                }
            qsort(az, k, sizeof(*az), cmp_azix);
            uint32_t d = 0;                             /* (one of each azimuth) */
            for (uint32_t i = 0; i < k; i++)
                if (!d || roundf(az[i].az * 2.0f) != roundf(az[d - 1].az * 2.0f)) az[d++] = az[i];
            if (d > 1 && roundf(az[d - 1].az * 2.0f) == roundf((az[0].az + 360.0f) * 2.0f)) d--;
            uint32_t naz = fabsf(rings_el[r]) >= 89.5f ? 1 : d > 1024 ? 1024 : d;
            if (ndirs + naz > gridcap) {
                size_t cap = gridcap ? gridcap * 2 : 1024;
                while (cap < ndirs + naz) cap *= 2;
                mix *g = realloc(grid, cap * sizeof(*g));
                if (!g) { free(az); fail(why, whylen, "out of memory"); goto out; }
                grid = g; gridcap = cap;
            }
            for (uint32_t a = 0; a < naz; a++) {
                mix *g = &grid[ndirs + a];
                float want = 360.0f * a / naz;
                uint32_t hi = 0;
                while (hi < d && az[hi].az < want) hi++;
                uint32_t lo = hi ? hi - 1 : d - 1;
                hi %= d;
                float alo = az[lo].az, ahi = az[hi].az;
                float span = ahi - alo, at = want - alo;
                if (span <= 0.0f) span += 360.0f;
                if (at < 0.0f) at += 360.0f;
                float w = d == 1 || span <= 0.0f ? 0.0f : at / span;
                if (w < 0.0f) w = 0.0f;
                if (w > 1.0f) w = 1.0f;
                g->n = 2; g->idx[0] = az[lo].ix; g->w[0] = 1.0f - w; g->idx[1] = az[hi].ix; g->w[1] = w;
            }
            ring_n[r] = naz;
            ndirs += naz;
            free(az);
        }
    } else {
        /* rings 10 degrees apart over the measured elevations, each direction from the three nearest measured */
        float lo = 90.0f, hi = -90.0f;
        for (uint32_t m = 0; m < M; m++)
            if (p[m].ok) { lo = p[m].el < lo ? p[m].el : lo; hi = p[m].el > hi ? p[m].el : hi; }
        lo = ceilf(lo / 10.0f) * 10.0f;
        hi = floorf(hi / 10.0f) * 10.0f;
        if (hi < lo) hi = lo;
        for (float e = lo; e <= hi + 0.01f && nrings < RL_HRTF_MAX_RINGS; e += 10.0f) {
            uint32_t naz = fabsf(e) >= 89.5f ? 1 : (uint32_t)lroundf(72.0f * cosf(e * (float)M_PI / 180.0f));
            rings_el[nrings] = e; ring_n[nrings] = naz < 1 ? 1 : naz;
            ndirs += ring_n[nrings];
            nrings++;
        }
        grid = calloc(ndirs, sizeof(*grid));
        vec = malloc((size_t)M * 3 * sizeof(float));
        if (!grid || !vec) { fail(why, whylen, "out of memory"); goto out; }
        for (uint32_t m = 0; m < M; m++) if (p[m].ok) unit(&p[m], vec + 3 * m);
        uint32_t g = 0;
        for (uint32_t r = 0; r < nrings; r++)
            for (uint32_t a = 0; a < ring_n[r]; a++, g++) {
                pos want = { 360.0f * a / ring_n[r], rings_el[r], 1 };
                float wv[3], bc[3] = { -2.0f, -2.0f, -2.0f }, bd[3];
                int bi[3] = { -1, -1, -1 };
                unit(&want, wv);
                for (uint32_t m = 0; m < M; m++) {          /* (nearest: the largest cosine) */
                    if (!p[m].ok) continue;
                    const float *v = vec + 3 * m, c = v[0] * wv[0] + v[1] * wv[1] + v[2] * wv[2];
                    for (int k = 0; k < 3; k++)
                        if (c > bc[k]) {
                            for (int j = 2; j > k; j--) { bc[j] = bc[j - 1]; bi[j] = bi[j - 1]; }
                            bc[k] = c; bi[k] = (int)m;
                            break;
                        }
                }
                for (int k = 0; k < 3; k++) bd[k] = bi[k] < 0 ? INFINITY : angle(&want, &p[bi[k]]);
                mix *x = &grid[g];
                if (bd[0] < 0.5f) { x->n = 1; x->idx[0] = bi[0]; x->w[0] = 1.0f; continue; }
                float sum = 0.0f;
                x->n = 0;
                for (int k = 0; k < 3; k++)
                    if (bi[k] >= 0) { x->idx[x->n] = bi[k]; x->w[x->n] = 1.0f / bd[k]; sum += x->w[x->n]; x->n++; }
                for (int k = 0; k < x->n; k++) x->w[k] /= sum;
            }
    }

    /* the level: the ring nearest the horizon at the built-in set's mean energy */
    uint32_t r0 = 0;
    for (uint32_t r = 1; r < nrings; r++) if (fabsf(rings_el[r]) < fabsf(rings_el[r0])) r0 = r;
    double e0 = 0.0;
    {
        uint32_t first = 0;
        for (uint32_t r = 0; r < r0; r++) first += ring_n[r];
        for (uint32_t a = 0; a < ring_n[r0]; a++) {
            const mix *x = &grid[first + a];
            for (int ch = 0; ch < 2; ch++)
                for (uint32_t k = 0; k < taps; k++) {
                    double v = 0.0;
                    for (int j = 0; j < x->n; j++) v += x->w[j] * rs[x->idx[j]].c[ch][k];
                    e0 += v * v;
                }
        }
        e0 /= ring_n[r0];
    }
    const float scale = e0 > 0.0 ? (float)sqrt(TARGET_E0 / e0) : 1.0f;

    /* as a reAL_hrtf.bin, loaded as one */
    size_t bsize = 20 + 8 * (size_t)nrings + (size_t)ndirs * (8 + 8 * taps);
    blob = malloc(bsize);
    if (!blob) { fail(why, whylen, "out of memory"); goto out; }
    uint8_t *q = blob;
    memcpy(q, "RLHRTF01", 8); q += 8;
    q = put32(q, rate); q = put32(q, taps); q = put32(q, nrings);
    for (uint32_t r = 0; r < nrings; r++) { q = putf(q, rings_el[r]); q = put32(q, ring_n[r]); }
    for (uint32_t g = 0; g < ndirs; g++) {
        const mix *x = &grid[g];
        for (int ch = 0; ch < 2; ch++) {
            float d = 0.0f;
            for (int j = 0; j < x->n; j++) d += x->w[j] * rs[x->idx[j]].d[ch];
            q = putf(q, d);
        }
        for (int ch = 0; ch < 2; ch++)
            for (uint32_t k = 0; k < taps; k++) {
                float v = 0.0f;
                for (int j = 0; j < x->n; j++) v += x->w[j] * rs[x->idx[j]].c[ch][k];
                q = putf(q, v * scale);
            }
    }
    ok = rl_hrtf_load(set, blob, (uint32_t)bsize, out_rate);
    if (!ok) fail(why, whylen, "its directions don't make a set reAL can use");
out:
    free(blob); free(grid); free(vec); free(ring_n); free(rings_el); free(lp); free(coefs); free(rs); free(p);
    mysofa_free(h);
    return ok;
}
