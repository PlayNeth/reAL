/* The timeline of a waveOut stream (wavecore.h) */
#include "wavecore.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

int wc_init(wc_stream *s, uint32_t rate, uint32_t block, uint8_t silence, uint32_t ring, uint32_t period,
            uint32_t delta, uint32_t guard, double t)
{
    wc_seg *seg = malloc(64 * sizeof(*seg));
    if (!seg) return 0;
    void (*put)(void *, uint32_t, const uint8_t *, uint32_t) = s->put;
    void (*done)(void *, void *) = s->done;
    void *ctx = s->ctx;
    memset(s, 0, sizeof(*s));
    s->put = put; s->done = done; s->ctx = ctx;
    s->seg = seg; s->cap = 64;
    s->rate = rate; s->block = block; s->silence = silence;
    s->ring = ring; s->period = period; s->delta = delta; s->guard = guard;
    s->lead = 2 * delta + period;
    s->ia = delta + period / 2.0; s->ta = t; s->slope = 1.0; s->running = 1;
    return 1;
}
void wc_free(wc_stream *s)
{
    free(s->seg); s->seg = NULL; s->head = s->n = s->cap = 0;
    free(s->later); s->later = NULL; s->nlater = s->caplater = 0;
}

double wc_ideal(const wc_stream *s, double t)
{
    return s->running ? s->ia + (t - s->ta) * s->rate * s->slope : s->ia;
}

void wc_mixer_at(wc_stream *s, uint32_t ringpos)
{
    uint32_t r = (uint32_t)(s->R % s->ring);
    s->R += (ringpos + s->ring - r) % s->ring;
}

/* frames [at, at + n) of the timeline to the ring, from src (NULL: silence) */
static void put_range(wc_stream *s, int64_t at, const uint8_t *src, int64_t n)
{
    while (n > 0) {
        uint32_t p = (uint32_t)(at % s->ring), k = s->ring - p;
        if (k > n) k = (uint32_t)n;
        s->put(s->ctx, p, src, k);
        if (src) src += (size_t)k * s->block;
        at += k; n -= k;
    }
}
/* the ring up to `upto` of the timeline (never so far that it overwrites what the mixer has still to read) */
static void fill(wc_stream *s, int64_t upto)
{
    if (s->W < s->R + s->guard) s->W = s->R + s->guard;   /* (fell behind the mixer: what it passed is gone) */
    if (upto > s->R + s->ring - s->guard) upto = s->R + s->ring - s->guard;
    int64_t x = s->W;
    if (upto <= x) return;
    for (int i = s->head; i < s->n && x < upto; i++) {
        const wc_seg *g = &s->seg[i];
        int64_t e = g->start + g->frames;
        if (e <= x) continue;
        if (g->start >= upto) break;
        if (g->start > x) { put_range(s, x, NULL, g->start - x); x = g->start; }
        int64_t to = e < upto ? e : upto;
        put_range(s, x, g->data + (size_t)(x - g->start) * s->block, to - x);
        x = to;
    }
    if (x < upto) put_range(s, x, NULL, upto - x);
    s->W = upto;
}
static void topup(wc_stream *s, double I)
{
    int64_t want = s->R + s->lead, ideal = (int64_t)ceil(I) + s->guard;
    fill(s, want > ideal ? want : ideal);
}
/* the pieces the ideal device has played (and the ring holds) go; a buffer's last one makes it done */
static void retire(wc_stream *s, double I)
{
    while (s->head < s->n) {
        const wc_seg *g = &s->seg[s->head];
        int64_t e = g->start + g->frames;
        if ((double)e > I || e > s->W) break;
        void *h = g->last ? g->hdr : NULL;
        if (s->nloop && !s->loop_open && g->hdr == s->loop[s->nloop - 1].hdr && g->pass + 1 == s->passes &&
            s->next_pass == s->passes)
            s->nloop = 0, s->passes = s->next_pass = 0;          /* (the loop's very end) */
        s->pos_done += g->frames;
        s->head++;
        if (h) s->done(s->ctx, h);
    }
    if (s->head == s->n) s->head = s->n = 0;
    else if (s->head > s->cap / 2) {
        memmove(s->seg, s->seg + s->head, (size_t)(s->n - s->head) * sizeof(*s->seg));
        s->n -= s->head; s->head = 0;
    }
}

static void extend(wc_stream *s, double I);
void wc_tick(wc_stream *s, double t)
{
    if (s->running) {
        /* the mixer's position moves a mixing period at a time: on average it is half a period behind */
        double I = wc_ideal(s, t), target = (double)s->R + s->delta + s->period / 2.0;
        if (I < (double)(s->R + s->guard)) {        /* (the mixer caught up: start the clock again ahead of it) */
            s->ia = target; s->slope = 1.0; s->err = 0;
        } else {
            /* the output's clock and the real-time one may differ a little: I keeps to real time (any other rate
             * would move the game's sound at each reset) until it is a mixing period or more off the mixer's, then
             * goes a little faster or slower until it is back */
            s->err += (target - I - s->err) * 0.02;
            double band = s->period / 2.0 + s->rate / 200.0;
            if (fabs(s->err) > band) {
                double c = s->err / (s->rate * 8.0);
                s->slope = 1.0 + (c > 5e-4 ? 5e-4 : c < -5e-4 ? -5e-4 : c);
            } else if (fabs(s->err) < band / 4) s->slope = 1.0;
            s->ia = I;
        }
        s->ta = t;
    }
    double I = wc_ideal(s, t);
    extend(s, I);
    topup(s, I);
    retire(s, I);
}

static int append(wc_stream *s, void *hdr, const uint8_t *data, uint32_t frames, int64_t start, int last, uint32_t pass)
{
    if (s->n == s->cap) {
        if (s->head) {
            memmove(s->seg, s->seg + s->head, (size_t)(s->n - s->head) * sizeof(*s->seg));
            s->n -= s->head; s->head = 0;
        } else {
            wc_seg *g = realloc(s->seg, (size_t)s->cap * 2 * sizeof(*g));
            if (!g) return 0;
            s->seg = g; s->cap *= 2;
        }
    }
    wc_seg *g = &s->seg[s->n++];
    g->hdr = hdr; g->data = data; g->frames = frames; g->start = start; g->last = (uint8_t)last; g->pass = pass;
    s->tail = start + frames;
    return 1;
}
/* the loop ends with `pass`: its buffers' pieces of that pass are their last ones, or (played already) they are
 * done now */
static void loop_final(wc_stream *s, uint32_t pass)
{
    int last_found = 0;
    for (int k = 0; k < s->nloop; k++) {
        int found = 0;
        for (int i = s->head; i < s->n; i++)
            if (s->seg[i].hdr == s->loop[k].hdr && s->seg[i].pass == pass) { s->seg[i].last = 1; found = 1; }
        if (!found) s->done(s->ctx, s->loop[k].hdr);
        last_found = found;
    }
    s->passes = s->next_pass = pass + 1;
    if (!last_found) s->nloop = 0, s->passes = s->next_pass = 0;     /* (over already) */
}
static int write_at(wc_stream *s, double I, void *hdr, const uint8_t *data, uint32_t frames, uint32_t flags,
                    uint32_t loops);
/* the loop's next passes, up to two seconds past the ideal device; the buffers that waited for its end after it */
static void extend(wc_stream *s, double I)
{
    while (s->next_pass < s->passes && (double)s->tail < I + 2.0 * s->rate) {
        uint32_t pass = s->next_pass;
        int last = pass + 1 == s->passes;
        s->next_pass++;
        for (int k = 0; k < s->nloop; k++)
            if (!append(s, s->loop[k].hdr, s->loop[k].data, s->loop[k].frames, s->tail, last, pass)) {
                loop_final(s, pass);        /* (out of memory: it ends with this pass, as far as it got) */
                break;
            }
    }
    while (s->next_pass == s->passes && s->nlater) {    /* (until one of them starts another loop to queue) */
        struct wc_later l = s->later[0];
        memmove(s->later, s->later + 1, (size_t)(--s->nlater) * sizeof(*s->later));
        write_at(s, I, l.hdr, l.data, l.frames, l.flags, l.loops);
    }
}
static int write_at(wc_stream *s, double I, void *hdr, const uint8_t *data, uint32_t frames, uint32_t flags,
                    uint32_t loops)
{
    if (s->next_pass < s->passes) {        /* (a loop being queued: after it) */
        if (s->nlater == s->caplater) {
            int cap = s->caplater ? s->caplater * 2 : 16;
            struct wc_later *l = realloc(s->later, (size_t)cap * sizeof(*l));
            if (!l) return 0;
            s->later = l; s->caplater = cap;
        }
        s->later[s->nlater++] = (struct wc_later){ hdr, data, frames, flags, loops };
        return 1;
    }
    int64_t at = s->tail, now = llround(I);
    if (at < now) at = now;
    if (at < s->R + s->guard) at = s->R + s->guard;
    if ((flags & WC_BEGINLOOP) && !s->loop_open) {
        s->loop_open = 1; s->nloop = 0; s->broken = 0;
        s->passes = 0;
        s->next_pass = loops ? loops : 1;   /* (kept here until the loop's end comes) */
    }
    int inloop = s->loop_open && s->nloop < WC_MAXLOOP;    /* (more buffers than that: the rest played once) */
    if (inloop) {
        s->loop[s->nloop].hdr = hdr; s->loop[s->nloop].data = data; s->loop[s->nloop].frames = frames;
        s->nloop++;
    }
    if (!append(s, hdr, data, frames, at, !inloop, 0)) {
        if (inloop) s->nloop--;
        return 0;
    }
    if (at < s->W) s->W = at;               /* (written into what the ring has as silence) */
    if (s->loop_open && (flags & WC_ENDLOOP)) {
        uint64_t total = 0;
        for (int k = 0; k < s->nloop; k++) total += s->loop[k].frames;
        uint32_t p = s->broken || !total ? 1 : s->next_pass;       /* (nothing to repeat: once) */
        s->loop_open = 0;
        if (p == 1) loop_final(s, 0);
        else { s->passes = p; s->next_pass = 1; extend(s, I); }
    }
    return 1;
}

int wc_write(wc_stream *s, double t, void *hdr, const uint8_t *data, uint32_t frames, uint32_t flags, uint32_t loops)
{
    double I = wc_ideal(s, t);
    if (!write_at(s, I, hdr, data, frames, flags, loops)) return 0;
    topup(s, I);
    return 1;
}

void wc_reset(wc_stream *s, double t)
{
    double I = wc_ideal(s, t);
    int64_t x = llround(I);
    if (x < s->R + s->guard) x = s->R + s->guard;
    fill(s, x);                         /* what the ideal device has played stays (the buffers go back now) */
    s->head = s->n = 0;
    s->loop_open = 0; s->nloop = 0; s->passes = s->next_pass = 0; s->nlater = 0;
    s->tail = x;
    if (s->W > x) s->W = x;
    s->pos_done = 0;
    topup(s, I);
}

void wc_pause(wc_stream *s, double t)
{
    if (!s->running) return;
    s->ia = wc_ideal(s, t);
    s->running = 0;
}
void wc_restart(wc_stream *s, double t)
{
    if (s->running) return;
    s->ta = t;
    s->running = 1;
}

/* the loop ends with the pass the ideal device is playing: the passes after it go, and what follows moves up */
void wc_breakloop(wc_stream *s, double t)
{
    if (s->loop_open) { s->broken = 1; return; }
    if (!s->passes) return;                 /* (no loop) */
    double I = wc_ideal(s, t);
    int64_t from = (int64_t)ceil(I);
    if (from < s->R + s->guard) from = s->R + s->guard;
    /* the pass being played: that of the first piece not over by then (before the loop: it plays once) */
    int i = s->head;
    while (i < s->n && s->seg[i].start + s->seg[i].frames <= from) i++;
    uint32_t cur = i < s->n ? s->seg[i].pass : s->next_pass - 1;
    int cut = i;
    while (cut < s->n && s->seg[cut].pass <= cur) cut++;
    int end = cut;
    while (end < s->n && s->seg[end].pass > cur) end++;     /* (the passes after it; what follows the loop is pass 0) */
    if (end > cut) {
        int64_t at = s->seg[cut].start, gap = s->seg[end - 1].start + s->seg[end - 1].frames - at;
        memmove(s->seg + cut, s->seg + end, (size_t)(s->n - end) * sizeof(*s->seg));
        s->n -= end - cut;
        for (int k = cut; k < s->n; k++) s->seg[k].start -= gap;
        s->tail -= gap;
        if (s->W > at) s->W = at;
    }
    loop_final(s, cur < s->next_pass ? cur : s->next_pass - 1);
    extend(s, I);
    topup(s, I);
}

int64_t wc_position(const wc_stream *s, double t)
{
    double I = wc_ideal(s, t);
    int64_t p = s->pos_done;
    for (int i = s->head; i < s->n; i++) {
        const wc_seg *g = &s->seg[i];
        if ((double)g->start >= I) break;
        double e = (double)(g->start + g->frames);
        p += (int64_t)((e < I ? e : I) - (double)g->start);
    }
    return p;
}
