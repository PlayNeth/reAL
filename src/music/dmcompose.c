/* reAL-Music: the composer's chord composition (dmcompos 772873cd and below): signposts and commands into steps, a
 * chord map signpost chosen for each, and between each two a path through the chord map's connections, its chords
 * spread over the beats and aligned to measures.  The result is written as a chord track ('cord') the way dmcompos
 * writes it for dmstyle to load.  The addresses are XP dmcompos's; the shapes of its structures are kept. */
#include "dmcomp.h"
#include "dmsty.h"
#include "dmperf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* msvcrt's rand() on the application thread (the composer's constructor seeded it with time()) */
static int cm_rand(void)
{
    uint32_t *r = dm_app_rand_state();
    *r = *r * 214013u + 2531011u;
    int v = (int)((*r >> 16) & 0x7fff);
    if (dm_rand_hook) dm_rand_hook(1, v, __builtin_return_address(0));
    return v;
}

/* ------------------------------------------------------------------ chords */
/* 77281de1: two subchords alike: the same chord pattern and root (roots over 11 taken modulo 12) */
static int sub_alike(const dmc_sub *a, const dmc_sub *b)
{
    if (!a || !b) return a == b;
    int8_t ra = (int8_t)a->chordroot, rb = (int8_t)b->chordroot;
    if (ra > 11) ra = (int8_t)(ra - ((uint8_t)(ra - 12) / 12) * 12 - 12);
    if (rb > 11) rb = (int8_t)(rb - ((uint8_t)(rb - 12) / 12) * 12 - 12);
    return a->chordpat == b->chordpat && ra == rb;
}
/* 77282743: two chords alike, subchord by subchord (the same count) */
static int chord_alike(const dmc_chord *a, const dmc_chord *b)
{
    const dmc_subnode *x = a->subs, *y = b->subs;
    for (; x; x = x->next, y = y->next) if (!y || !sub_alike(x->sub, y->sub)) return 0;
    return y == NULL;
}
/* 77281e50: the name copied, the subchord list shared */
static void chord_assign(dmc_chord *d, const dmc_chord *s)
{
    d->hasname = s->hasname;
    memcpy(d->name, s->name, sizeof(d->name));
    d->subs = s->subs;
}

/* the composer's own chord copies (772840c6: the name and new subchords), freed with it (77284af9) */
typedef struct ccopy { dmc_chord c; struct ccopy *next; } ccopy;
typedef struct { ccopy *copies; } composer;
static dmc_chord *chord_copy(composer *cp, const dmc_chord *s)
{
    ccopy *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->c.hasname = s->hasname;
    memcpy(k->c.name, s->name, sizeof(k->c.name));
    dmc_subnode **pp = &k->c.subs;
    for (const dmc_subnode *x = s->subs; x; x = x->next) {     /* 77284017 */
        dmc_subnode *n = calloc(1, sizeof(*n));
        dmc_sub *u = calloc(1, sizeof(*u));
        if (!n || !u) { free(n); free(u); break; }
        if (x->sub) *u = *x->sub;
        u->refs = 1;
        n->sub = u;
        *pp = n; pp = &n->next;
    }
    k->next = cp->copies;           /* 77282d8d */
    cp->copies = k;
    return &k->c;
}
static void composer_free(composer *cp)
{
    for (ccopy *k = cp->copies, *n; k; k = n) {
        n = k->next;
        for (dmc_subnode *x = k->c.subs, *y; x; x = y) { y = x->next; free(x->sub); free(x); }
        free(k);
    }
    cp->copies = NULL;
}

/* ------------------------------------------------------------------ the composed chords (0x1c) */
typedef struct cnode {
    dmc_chord *chord;               /* +0x00 */
    dmc_next *link;                 /* +0x04 the connection that led here */
    int16_t measure, beat;          /* +0x0c +0x0e (while spreading: the beat from the step's start) */
    int16_t minb, maxb;             /* +0x10 +0x12 */
    uint8_t silent;                 /* +0x14 */
    struct cnode *next;             /* +0x18 */
} cnode;
static void clist_append(cnode **head, cnode *n)        /* 7728249b: a node or list at the end */
{
    while (*head) head = &(*head)->next;
    *head = n;
}
static void clist_free(cnode *n) { while (n) { cnode *x = n->next; free(n); n = x; } }     /* 77283f78 */
static int clist_count(const cnode *n) { int k = 0; for (; n; n = n->next) k++; return k; }
static cnode *clist_nth(cnode *n, int k) { for (; n && k; k--) n = n->next; return n; }     /* 77282510 */
static cnode *clist_before(cnode *head, const cnode *n)  /* 772824fa */
{
    cnode *p = NULL;
    for (; head && head != n; head = head->next) p = head;
    return p;
}
static void clist_remove(cnode **head, cnode *n)        /* 772824c3 */
{
    for (; *head; head = &(*head)->next) if (*head == n) { *head = n->next; n->next = NULL; return; }
}
/* 772842b0: a copy of the chord (one with subchords) at measure / beat, appended */
static cnode *add_chord(composer *cp, cnode **list, const dmc_chord *c, int16_t measure, int16_t beat)
{
    if (!c->subs) return NULL;
    cnode *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->chord = chord_copy(cp, c);
    if (!n->chord) { free(n); return NULL; }
    n->measure = measure; n->beat = beat; n->minb = 1;
    clist_append(list, n);
    return n;
}
/* 77284341: one that may last up to `beats` */
static cnode *add_chord_beats(composer *cp, cnode **list, const dmc_chord *c, int16_t beats)
{
    cnode *n = add_chord(cp, list, c, 0, 0);
    if (n) n->maxb = beats;
    return n;
}

/* ------------------------------------------------------------------ a path request (0x36) */
typedef struct creq {
    dmc_chord start, end;           /* +0x00 +0x0c */
    cnode *reuse;                   /* +0x18 an earlier step's chords to follow */
    dmc_entry *reuse_entry;         /* +0x1c and its first entry */
    int16_t beats;                  /* +0x20 */
    int16_t minsum, maxsum, count;  /* +0x22 +0x24 +0x26 the path so far */
    int16_t minchords, maxchords;   /* +0x28 +0x2a */
    int16_t activity;               /* +0x2c */
    int16_t f_short, f_long, f_many, f_few;    /* +0x2e +0x30 +0x32 +0x34 why paths failed */
} creq;
/* 77282f45: a copy (the names copied, the subchord lists shared) */
static void req_copy(creq *d, const creq *s)
{
    chord_assign(&d->start, &s->start);
    chord_assign(&d->end, &s->end);
    d->reuse = s->reuse; d->reuse_entry = s->reuse_entry;
    d->beats = s->beats; d->minsum = s->minsum; d->maxsum = s->maxsum; d->count = s->count;
    d->minchords = s->minchords; d->maxchords = s->maxchords; d->activity = s->activity;
    d->f_short = s->f_short; d->f_long = s->f_long; d->f_many = s->f_many; d->f_few = s->f_few;
}

/* ------------------------------------------------------------------ the path search */
static int path_from(dmc_entry *e, creq *R);
/* 772827bf: a connection not yet tried, chosen by weight */
static dmc_next *pick_next(dmc_entry *e)
{
    int sum = 0;
    for (dmc_next *x = e->nexts; x; x = x->next) if (!(x->flags & 4) && x->to) sum += x->weight;
    if (!sum) return NULL;
    int r = cm_rand() % sum;
    for (dmc_next *x = e->nexts; x; x = x->next) {
        if (x->flags & 4) continue;
        if (x->to) r -= x->weight;
        if (r < 0) return x;
    }
    return NULL;
}
/* 7728277c: along a connection (its beats and one more chord counted on the way down) */
static int path_via(dmc_next *x, creq *R)
{
    if (!x->to) return 0;
    R->minsum += x->minbeats; R->maxsum += x->maxbeats; R->count++;
    int ok = path_from(x->to, R);
    R->count--; R->minsum -= x->minbeats; R->maxsum -= x->maxbeats;
    return ok;
}
/* 77282817: from an entry to one that may end on the end chord, within the request's bounds; the path's connections
 * marked 2 (tried and failed: 4) */
static int path_from(dmc_entry *e, creq *R)
{
    if (R->maxchords < R->count) { R->f_many++; return 0; }
    for (dmc_next *x = e->nexts; x; x = x->next) x->flags &= ~6u;
    if ((e->flags & 4) && chord_alike(&e->chord, &R->end)) {
        if (R->count < R->minchords) R->f_few++;
        else if (R->maxsum < R->beats) R->f_long++;
        else if (R->beats < R->minsum) R->f_short++;
        else { e->flags |= 0x10; return 1; }
    }
    e->flags &= ~0x10u;
    if (R->reuse) R->reuse = R->reuse->next;
    if (R->reuse && R->reuse->chord && R->reuse->link) {
        dmc_next *x = R->reuse->link;
        if (path_via(x, R)) { x->flags |= 2; e->flags |= 0x10; return 1; }
        x->flags |= 4;
        R->reuse = NULL;
    }
    for (dmc_next *x; (x = pick_next(e)); ) {
        if (path_via(x, R)) { x->flags |= 2; e->flags |= 0x10; return 1; }
        x->flags |= 4;
    }
    return 0;
}
static void clear_marks(dmc_entry *entries)             /* 77282f20(2) and the entries' 0x10 */
{
    for (dmc_entry *e = entries; e; e = e->next) for (dmc_next *x = e->nexts; x; x = x->next) x->flags &= ~2u;
    for (dmc_entry *e = entries; e; e = e->next) e->flags &= ~0x10u;
}

typedef struct cstep {
    int16_t measure;                /* +0x00 */
    uint8_t command, groove, range, repeat;   /* +0x02 .. +0x05 */
    uint32_t chords;                /* +0x08 the signpost groups */
    dmc_signpost *sp;               /* +0x0c the chord map's signpost for it */
    dmc_entry *start;               /* +0x10 */
    cnode *out;                     /* +0x14 */
    creq R;                         /* +0x18 */
    struct cstep *next;             /* +0x50 */
} cstep;

/* 77284115: a path from an entry that may begin on the start chord (the earlier step's first, when following one);
 * its chords appended to the step's (the first with no beats) */
static int find_path(dmc_entry *entries, creq *R, cstep *s)
{
    creq saved;
    memset(&saved, 0, sizeof(saved));
    req_copy(&saved, R);
    dmc_entry *e = NULL;
    if (R->reuse && R->reuse_entry) {
        e = R->reuse_entry;
        clear_marks(entries);
        if (!path_from(e, R)) e = NULL;
    }
    if (!e) {
        for (e = entries; e; e = e->next) {
            if (!(e->flags & 2) || !chord_alike(&e->chord, &R->start)) continue;
            clear_marks(entries);
            int16_t fs = R->f_short, fl = R->f_long, fm = R->f_many, ff = R->f_few;
            req_copy(R, &saved);
            R->f_short = fs; R->f_long = fl; R->f_many = fm; R->f_few = ff;
            if (path_from(e, R)) break;
        }
        if (!e) return 0;
    }
    cnode *n = calloc(1, sizeof(*n));
    int16_t k = 0;
    if (n) {
        n->chord = &e->chord;
        k = 1;
        s->start = e;
        clist_append(&s->out, n);
    }
    while (e) {
        dmc_next *x = e->nexts;
        for (; x && !(x->flags & 2); x = x->next) {}
        if (!x) break;
        cnode *m = calloc(1, sizeof(*m));
        if (m) {
            m->chord = &x->to->chord;
            m->measure = k++;
            m->minb = (int16_t)x->minbeats; m->maxb = (int16_t)x->maxbeats;
            m->link = x;
            clist_append(&s->out, m);
        }
        e = x->to;
    }
    return 1;
}

/* 77282fb6: the node's beats since `prev` within its bounds */
static int fits(const cnode *n, int prev)
{
    int d = n->beat - prev;
    return !(d < n->minb || n->maxb < d);
}
/* 77282fd8: chords off a measure line moved to one (the way chosen at random), else to the half measure, else left */
static int align_measures(cnode *n, int prev, int bpm)
{
    while (n) {
        cnode *nx = n->next;
        if (!nx) return fits(n, prev);
        int16_t s1 = n->beat;
        if (s1 % bpm == 0) {
            int ok = fits(n, prev);
            prev = s1;
            if (!ok) return 0;
        } else {
            int r = cm_rand() % 2;
            int16_t m = (int16_t)((int16_t)(s1 / bpm) * (int16_t)bpm);
            n->beat = m;
            if (r) n->beat = (int16_t)(m + bpm);
            if (fits(n, prev) && align_measures(n->next, n->beat, bpm)) return 1;
            if (!r) n->beat = (int16_t)(n->beat + bpm); else n->beat = (int16_t)(n->beat - bpm);
            if (fits(n, prev) && align_measures(n->next, n->beat, bpm)) return 1;
            if (!r) n->beat = (int16_t)(n->beat - bpm);
            n->beat = (int16_t)(n->beat + ((bpm + 1) >> 1));
            int16_t s2 = n->beat;
            if (fits(n, prev) && align_measures(n->next, s2, bpm)) return 1;
            n->beat = s1;
            if (!fits(n, prev)) return 0;
            nx = n->next;
            prev = s1;
        }
        n = nx;
    }
    return 0;
}
/* 77284362: the last chord moved by `diff`; what its bounds refuse moves the one before */
static void push_back(cnode **head, cnode *n, int diff)
{
    cnode *p = clist_before(*head, n);
    while (diff && p) {
        n->beat = (int16_t)(n->beat + diff);
        int d = n->beat - p->beat, lim = n->maxb;
        if (d <= lim && (lim = n->minb) <= d) return;
        diff = d - lim;
        cnode *pp = clist_before(*head, p);
        n = p;
        p = pp;
    }
}

/* 7728cae2 (activity 0) / 772843b8 (with activity): the chords between a step's signpost and the next's: a path
 * (else the start chord, the cadences and the start chord again), the cadences after it, the beats spread over them
 * in proportion to each one's range, aligned to measures; the last (the next step's) dropped */
static void compose_span(composer *cp, dmc_entry *entries, cstep *s, creq *R, int16_t bpm, const dmc_chord *cad1,
                         const dmc_chord *cad2, int actmode)
{
    int16_t beats = R->beats;
    creq copy;
    memset(&copy, 0, sizeof(copy));
    R->minsum = R->maxsum = R->count = 0;
    R->f_short = R->f_long = R->f_many = R->f_few = 0;
    if (!actmode) {
        if (cad1 || cad2) {
            R->minsum = 1; R->maxsum = bpm; R->count = 1;
            if (cad1 && cad2) { R->minsum = 2; R->count = 2; }
        }
    } else {
        if (cad1) { R->minsum = 1; R->maxsum = (int16_t)((bpm + 1) >> 1); R->count = 1; }
        if (cad2) { R->minsum++; R->maxsum = (int16_t)(R->maxsum + ((bpm + 1) >> 1)); R->count++; }
    }
    req_copy(&copy, R);
    if (!actmode) {
        s->out = NULL;
        find_path(entries, R, s);
        req_copy(R, &copy);
    } else {
        for (int tries = 0; tries < 4; tries++) {
            s->out = NULL;
            find_path(entries, R, s);
            if (s->out) break;
            if (R->f_long < R->f_short) copy.beats >>= 1;
            else if (R->f_short < R->f_long) copy.beats = (int16_t)(copy.beats << 1);
            else if (R->f_few <= R->f_many) break;
            else copy.minchords >>= 1;
            req_copy(R, &copy);
        }
    }
    R->beats = beats;
    int minsum = 0, maxsum = 0;
    for (cnode *n = s->out; n; n = n->next) { minsum += n->minb; maxsum += n->maxb; }
    if (!s->out) {
        int b = beats;
        cnode *n = add_chord_beats(cp, &s->out, &R->start, 0);
        if (n) n->minb = 0;
        if (cad1) { add_chord_beats(cp, &s->out, cad1, (int16_t)b); maxsum += b; minsum++; b = bpm + 1; }
        if (cad2) { add_chord_beats(cp, &s->out, cad2, (int16_t)b); maxsum += b; minsum++; b = bpm + 1; }
        add_chord_beats(cp, &s->out, &R->start, (int16_t)b);
        minsum++; maxsum += b;
    } else {
        int k = clist_count(s->out);
        if (k > 1) k--;
        int per = maxsum / k;
        if (per < 1) per = 1;
        if (cad1 || cad2) {
            if (cad1 && cad2) { add_chord_beats(cp, &s->out, cad2, (int16_t)per); maxsum += per; minsum++; }
            add_chord_beats(cp, &s->out, &R->end, (int16_t)per);
            maxsum += per; minsum++;
        }
    }
    /* each chord's share of the free beats, as a running total */
    int range = maxsum - minsum, freeb = R->beats - minsum;
    if (range < 1) range = 1;
    int acc = 0;
    for (cnode *n = s->out; n; n = n->next) {
        int v = ((n->maxb - n->minb) * freeb + (range >> 1)) / range;
        if (v < n->minb) v = n->minb;
        if (v > n->maxb) v = n->maxb;
        acc += v;
        n->beat = (int16_t)acc;
    }
    /* scaled to the span's beats */
    int last = acc;
    if (!actmode) {
        int prev = 0;
        for (cnode *n = s->out; n; n = n->next) {
            int v = acc ? (n->beat * R->beats - 1 + acc) / acc : n->beat;     /* (dmcompos divides by 0 there) */
            if (v - prev < n->minb) v = prev + n->minb;
            if (v - prev > n->maxb) v = prev + n->maxb;
            n->beat = (int16_t)v;
            prev = v;
            last = (int16_t)v;
        }
    } else {
        if (!acc) acc = 1;
        for (cnode *n = s->out; n; n = n->next) {
            n->beat = (int16_t)((n->beat * R->beats - 1 + acc) / acc);
            last = n->beat;
        }
    }
    cnode *ln = clist_nth(s->out, clist_count(s->out) - 1);
    if (ln && (!actmode || last <= R->beats)) push_back(&s->out, ln, R->beats - last);
    align_measures(s->out, 0, bpm);
    for (cnode *n = s->out; n; ) {
        if (!n->next) { clist_remove(&s->out, n); free(n); break; }
        n->measure = (int16_t)(n->beat / bpm + s->measure);
        n->beat = (int16_t)(n->beat % bpm);
        n = n->next;
    }
}

/* 77282dbb: each step's signpost groups (within mask) given a chord map signpost: one already given the same group,
 * else one at random among those that have it (unused ones only, unless `reuse`) */
static void assign_signposts(dmc_signpost *sps, cstep *steps, uint32_t mask, int reuse)
{
    for (cstep *s = steps; s; s = s->next) {
        uint32_t g = s->chords;
        if (!(mask & g)) continue;
        dmc_signpost *found = NULL;
        int count = 0, k;
        for (dmc_signpost *p = sps; p; p = p->next) {
            if (mask & p->used & g) { found = p; break; }
            if ((p->chords & g & mask) && (!p->used || reuse)) count++;
        }
        if (!found) {
            k = (int16_t)count ? cm_rand() % (int16_t)count : 0;
            k++;
            for (dmc_signpost *p = sps; p; p = p->next)
                if ((p->chords & s->chords & mask) && (!p->used || reuse) && !(int16_t)--k) { found = p; break; }
        }
        if (found && !s->sp) { found->used |= g; s->sp = found; }
    }
}

/* 77282e97: an earlier step on the same chord map signpost going to the same chord: its path is followed (else the
 * last earlier one on that signpost) */
static void find_reuse(cstep *steps, cstep *s, creq *R)
{
    cstep *last = NULL;
    R->reuse = NULL; R->reuse_entry = NULL;
    for (cstep *p = steps; p && p != s; p = p->next) {
        if (p->sp != s->sp) continue;
        last = p;
        if (chord_alike(&p->R.end, &s->R.end)) { R->reuse = p->out; R->reuse_entry = p->start; return; }
    }
    if (last) { R->reuse = last->out; R->reuse_entry = last->start; }
}

/* the steps' signposts and spans (7728cdb8 / 77286c89) */
typedef struct cmdsp {              /* 77284f12: a measure's command and signpost groups (0x10) */
    int16_t measure;
    uint8_t command, groove, range, repeat;
    uint32_t chords;
    struct cmdsp *next;
} cmdsp;

static cnode *compose_steps(composer *cp, const uint8_t *ts, dmc_chordmap *m, const cmdsp *items, int actmode, int act)
{
    int16_t bpm = ts[0];
    if (actmode && (uint16_t)act > 3) act = 3;
    for (dmc_signpost *p = m->signposts; p; p = p->next) p->used = 0;
    cstep *steps = NULL, **pp = &steps;
    for (const cmdsp *c = items; c; c = c->next) {
        cstep *s = calloc(1, sizeof(*s));
        if (!s) continue;
        s->measure = c->measure; s->command = c->command; s->groove = c->groove; s->range = c->range;
        s->repeat = c->repeat; s->chords = c->chords;
        *pp = s; pp = &s->next;
    }
    assign_signposts(m->signposts, steps, 0x7f00, 0);
    assign_signposts(m->signposts, steps, 0x3f, 0);
    assign_signposts(m->signposts, steps, 0x7f00, 1);
    assign_signposts(m->signposts, steps, 0x3f, 1);
    for (cstep *s = steps; s; s = s->next) {
        if (!s->chords || !s->sp) continue;
        cstep *nx = s->next;
        for (; nx && (!nx->chords || !nx->sp); nx = nx->next) {}      /* 77282e77 */
        if (!nx) { add_chord(cp, &s->out, &s->sp->chord, s->measure, 0); continue; }
        const dmc_chord *cad1 = NULL, *cad2 = NULL;
        chord_assign(&s->R.start, &s->sp->chord);
        dmc_signpost *q = nx->sp;
        if (!(nx->chords & 0x8000)) chord_assign(&s->R.end, &q->chord);
        else if (q->flags & 2) {
            chord_assign(&s->R.end, &q->cadence[0]);
            cad1 = &q->cadence[0];
            if (q->flags & 4) cad2 = &q->cadence[1];
        } else if (q->flags & 4) { chord_assign(&s->R.end, &q->cadence[1]); cad2 = &q->cadence[1]; }
        else chord_assign(&s->R.end, &q->chord);
        int16_t beats = (int16_t)((nx->measure - s->measure) * bpm);
        s->R.beats = beats;
        if (!actmode) { s->R.minchords = 0; s->R.maxchords = beats; }
        else { s->R.activity = (int16_t)act; s->R.maxchords = (int16_t)(beats >> act); s->R.minchords = (int16_t)(beats >> (act + 1)); }
        find_reuse(steps, s, &s->R);
        compose_span(cp, m->entries, s, &s->R, bpm, cad1, cad2, actmode);
    }
    cnode *out = NULL;
    for (cstep *s = steps; s; s = s->next) { clist_append(&out, s->out); s->out = NULL; }
    /* 772846b6: fills, intros and breaks keep only the chord on their measure's first beat */
    for (cstep *s = steps; s; s = s->next) {
        if (s->command != 2 && s->command != 3 && s->command != 4) continue;
        for (;;) {
            cnode *n = out;
            for (; n && n->measure <= s->measure; n = n->next) if (n->measure == s->measure && n->beat) break;
            if (!n || n->measure > s->measure) break;
            clist_remove(&out, n);
            free(n);
        }
    }
    for (cstep *s = steps, *n; s; s = n) { n = s->next; clist_free(s->out); free(s); }
    return out;
}

/* 77283d96: how well a signpost suits a chord after a key change: 0 when its chord is the chord (25 when it is not but
 * the signpost is a "1" one, else 50), plus its cadence chord's notes outside the old scale (30 without a cadence) */
static int notes_outside(uint32_t scale, int shift, uint32_t pattern)   /* 7728209f */
{
    uint32_t s = (scale & 0xfff) | (scale & 0xfff) << 12;
    int8_t c = (int8_t)shift;
    if (c < 0) c = (int8_t)(c + ((uint8_t)(-c - 1) / 12) * 12 + 12);
    if (c > 11) c = (int8_t)(c - ((uint8_t)(c - 12) / 12) * 12 - 12);
    pattern <<= (c & 31);
    int k = 0;
    for (int i = 0; i < 24; i++, pattern >>= 1, s >>= 1) if ((pattern & 1) && !(s & 1)) k++;
    return k;
}
static int16_t signpost_score(const dmc_signpost *p, int8_t key, uint32_t scale, const dmc_chord *c)
{
    int8_t k = (int8_t)(key % 12);
    int16_t v = 50;
    if (chord_alike(c, &p->chord)) v = 0;
    else if (p->chords & 0x100) v = 25;
    const dmc_chord *cad;
    if (p->flags & 2) cad = &p->cadence[0];
    else if (p->flags & 4) cad = &p->cadence[1];
    else return (int16_t)(v + 30);
    uint8_t root = cad->subs && cad->subs->sub ? cad->subs->sub->chordroot : 0;
    uint32_t pat = cad->subs && cad->subs->sub ? cad->subs->sub->chordpat : 0;
    if (root < (uint8_t)k) root = (uint8_t)(root + 12);
    int shift = ((int)root - (int)(uint8_t)k) % 12;
    return (int16_t)(v + notes_outside(scale, shift, pat));
}
/* 77283e14: the chord map's best signpost for a chord (ties at random), kept only when its chord is that chord; or,
 * `first`, one of its "1" signposts at random */
static dmc_signpost *pick_signpost(const dmc_chordmap *m, const dmc_chord *c, int first, uint32_t scale, int8_t key)
{
    if ((!first && !c) || !m) return NULL;
    dmc_signpost *p = m->signposts, *best = p;
    int bestv = 100, count = 0;
    if (first) {
        for (; p && !(p->chords & 0x100); p = p->next) {}
        if (p) { best = p; count = 0; for (; p; p = p->next) if (p->chords & 0x100) count++; }
    } else {
        for (; p; p = p->next) {
            int v = signpost_score(p, key, scale, c);
            if (v == bestv) count++;
            else if (v < bestv) { bestv = v; best = p; count = 1; }
        }
    }
    if (count) count = cm_rand() % count;
    p = best;
    int k = 0;
    while (count >= 0 && p && k <= count) {
        if (first ? (p->chords & 0x100) != 0 : signpost_score(p, key, scale, c) == bestv) { k++; best = p; }
        p = p->next;
    }
    if (!first && best && !chord_alike(c, &best->chord)) best = NULL;
    return best;
}

/* ------------------------------------------------------------------ the chord track */
typedef struct { uint8_t *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 256;
        while (c < b->n + n) c *= 2;
        uint8_t *q = realloc(b->p, c);
        if (!q) return;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put32(buf *b, uint32_t v) { uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; put(b, x, 4); }
static void put16(buf *b, uint16_t v) { uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(b, x, 2); }
/* 77283123 / 772828f2: the 'cord' list's data: crdh (the scale, the key in its top byte), then a crdb per chord with
 * subchords (DMUS_IO_CHORD, DMUS_IO_SUBCHORD) */
static uint8_t *chords_cord(const cnode *list, uint8_t key, uint32_t scale, const uint8_t *ts, size_t *n)
{
    buf b = { 0 };
    put(&b, "crdh", 4); put32(&b, 4); put32(&b, scale | (uint32_t)key << 24);
    int32_t beatlen = ts[1] ? 0xc00 / ts[1] : 0;
    for (const cnode *c = list; c; c = c->next) {
        if (!c->chord || !c->chord->subs) continue;
        uint32_t ns = 0;
        for (const dmc_subnode *x = c->chord->subs; x; x = x->next) ns++;
        put(&b, "crdb", 4); put32(&b, 4 + 0x28 + 8 + 0x14 * ns);
        put32(&b, 0x28);
        uint16_t name[16] = { 0 };
        if (c->chord->hasname) for (int k = 0; k < 16 && c->chord->name[k]; k++) name[k] = c->chord->name[k];
        name[15] = 0;
        for (int k = 0; k < 16; k++) put16(&b, name[k]);
        put32(&b, (uint32_t)((int32_t)c->measure * beatlen * ts[0] + (int32_t)(uint16_t)beatlen * c->beat));
        put16(&b, (uint16_t)c->measure);
        uint8_t bb[2] = { (uint8_t)c->beat, c->silent ? 1 : 0 };
        put(&b, bb, 2);
        put32(&b, ns & 0xffff);
        put32(&b, 0x14);
        for (const dmc_subnode *x = c->chord->subs; x; x = x->next) {
            const dmc_sub *u = x->sub;
            dmc_sub z = { 0 };
            if (!u) u = &z;
            put32(&b, u->chordpat); put32(&b, u->scalepat); put32(&b, u->invpat); put32(&b, u->levels);
            uint8_t r[4] = { u->chordroot, u->scaleroot, 0, 0 };
            put(&b, r, 4);
        }
    }
    *n = b.n;
    return b.p;
}

/* ------------------------------------------------------------------ segments */
static dm_segtrack *seg_track(dm_segment *s, int kind, uint32_t group, int index)
{
    for (int i = 0; i < s->ntracks; i++) {
        dm_segtrack *t = &s->tracks[i];
        if (t->t->kind != kind || !(t->group & group)) continue;
        if (!index--) return t;
    }
    return NULL;
}
static void seg_remove(dm_segment *s, dm_segtrack *t)
{
    int k = (int)(t - s->tracks);
    dm_track *x = t->t;
    memmove(s->tracks + k, s->tracks + k + 1, (s->ntracks - k - 1) * sizeof(*s->tracks));
    s->ntracks--;
    dm_track_release(x);
}

/* 7728525c / 77284f12: the signposts (by measure) merged into the commands (by measure); a signpost's measure without
 * a command gets one with the groove level before it (62 at first) */
static cmdsp *commands_and_signposts(const dmc_sptrack *sp, const dms_cmdtrack *cmd)
{
    cmdsp *list = NULL, **pp = &list;
    if (cmd)
        for (const dms_command *c = cmd->cmds; c; c = c->next) {
            cmdsp *n = calloc(1, sizeof(*n));
            if (!n) break;
            n->measure = (int16_t)c->measure; n->command = c->command; n->groove = c->groove; n->range = c->range;
            n->repeat = c->repeat;
            *pp = n; pp = &n->next;
        }
    uint8_t groove = 62;
    for (const dmc_sp *x = sp ? sp->items : NULL; x; x = x->next) {
        cmdsp *prev = NULL, *c = list;
        int done = 0;
        for (; c; prev = c, c = c->next) {
            if ((int)x->measure == c->measure) { c->chords = x->chords; done = 1; break; }
            if ((int)x->measure < c->measure) break;
            if (c->groove) groove = c->groove;
        }
        if (done) continue;
        cmdsp *n = calloc(1, sizeof(*n));
        if (!n) continue;
        n->measure = (int16_t)x->measure; n->groove = groove; n->chords = x->chords;
        if (!prev) { n->next = list; list = n; }
        else { prev->next = n; n->next = c; }
    }
    return list;
}

/* 772873cd: the chords for [0, length): one chord map throughout, or the chord map track's in turn (where it changes,
 * the new map's cadence for the chord there ends the old part) */
static cnode *compose_chords(composer *cp, const uint8_t *ts0, dmc_chordmap *cm, dmc_cmtrack *cmt, const dmc_sptrack *sp,
                             const dms_cmdtrack *cmd, mtime_t length, int actmode, int act, uint8_t *key, uint32_t *scale, int *hr)
{
    uint8_t ts[2] = { ts0[0] ? ts0[0] : 4, ts0[1] ? ts0[1] : 4 };
    int32_t mlen = (int32_t)((uint32_t)ts[0] * 0xc00 / ts[1]);
    int16_t half = (int16_t)((ts[0] >> 1) + ((ts[0] >> 1) * 2 != ts[0]));
    cmdsp *items = commands_and_signposts(sp, cmd);
    cnode *out = NULL;
    dmc_chordmap *prev = NULL;
    mtime_t at = 0;
    int r = 0;
    *hr = 0;
    while (*hr >= 0 && at < length && r >= 0) {
        mtime_t start = at;
        dmc_chordmap *m = cm;
        if (m) { dmc_chordmap_addref(m); at = length; }
        else if (!cmt) { r = *hr = DM_E_POINTER; break; }
        else {
            mtime_t next = 0;
            r = dmc_cmtrack_getparam(cmt, start, &next, &m);
            if (r < 0) break;
            at = next ? start + next : length;
        }
        if (!start) { *key = (uint8_t)(m->scale >> 24); *scale = m->scale & 0xffffff; }
        cmdsp *part = NULL, **pp = &part;
        for (const cmdsp *c = items; c; c = c->next) {
            if (start <= c->measure * mlen && c->measure * mlen < at) {
                cmdsp *n = malloc(sizeof(*n));
                if (!n) { *hr = DM_E_OUTOFMEMORY; break; }
                *n = *c; n->next = NULL;
                *pp = n; pp = &n->next;
            }
        }
        if (*hr >= 0) {
            cnode *list = compose_steps(cp, ts, m, part, actmode, act);
            int16_t meas = (int16_t)(start / mlen);
            if (out && meas && prev) {
                int8_t pkey = (int8_t)(prev->scale >> 24);
                uint32_t psc = prev->scale & 0xffffff;
                dmc_signpost *q = list ? pick_signpost(m, list->chord, 0, psc, pkey) : pick_signpost(m, NULL, 1, psc, pkey);
                if (q) {
                    if (q->flags & 2) add_chord(cp, &list, &q->cadence[0], (int16_t)(meas - 1), 0);
                    if (q->flags & 4) add_chord(cp, &list, &q->cadence[1], (int16_t)(meas - 1), (q->flags & 2) ? half : 0);
                }
            }
            if (getenv("DMC_DEBUG")) {
                FILE *f = fopen(getenv("DMC_DEBUG"), "a");
                if (f) {
                    fprintf(f, "range %ld..%ld\n", (long)start, (long)at);
                    for (cnode *n = list; n; n = n->next) {
                        int copy = 0;
                        for (ccopy *k = cp->copies; k; k = k->next) if (&k->c == n->chord) copy = 1;
                        fprintf(f, "  m %d b %d name '%c' %s\n", n->measure, n->beat, n->chord->name[0] ? n->chord->name[0] : ' ', copy ? "copy" : "entry");
                    }
                    fclose(f);
                }
            }
            clist_append(&out, list);
        }
        for (cmdsp *c = part, *n; c; c = n) { n = c->next; free(c); }
        dmc_chordmap_release(prev);
        prev = m;
    }
    dmc_chordmap_release(prev);
    for (cmdsp *c = items, *n; c; c = n) { n = c->next; free(c); }
    return out;
}

/* ------------------------------------------------------------------ ComposeSegmentFromTemplate (77287974 / 772833fa) */
static const dm_guid CLSID_ChordTrack_   = { 0xd2ac288b, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_StyleTrack_   = { 0xd2ac288d, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_ChordMapTrack_ = { 0xd2ac2896, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_TempoTrack_   = { 0xd2ac2885, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_BandTrack_    = { 0xd2ac2894, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };

dm_segment *dmc_compose_template(dm_segment *tmpl, dms_style *style, dmc_chordmap *cm, int activity, int *hr)
{
    composer cp = { 0 };
    *hr = 0;
    int ownstyle = 0, owncm = 0;
    if (!style) {
        mtime_t nx = 0;
        int r = dm_segment_getparam(tmpl, PRM_STYLE, 0xffffffff, 0, 0, &nx, &style);
        if (r < 0) { *hr = DM_E_INVALIDARG; return NULL; }
        ownstyle = 1;
    }
    dm_segtrack *spt = seg_track(tmpl, TRK_SIGNPOST, 0xffffffff, 0);
    uint32_t group = spt ? spt->group : 0xffffffff, grp = group;
    dmc_cmtrack *cmt = NULL;
    if (!cm) {
        dm_segtrack *t = seg_track(tmpl, TRK_CHORDMAP, grp, 0);
        if (!t) { if (ownstyle) dms_style_release(style); *hr = DM_E_INVALIDARG; return NULL; }
        cmt = t->t->data;
        owncm = 1;
    }
    dm_segtrack *cmdt = seg_track(tmpl, TRK_COMMAND, grp, 0);
    uint8_t key = 0;
    uint32_t scale = 0;
    cnode *chords = compose_chords(&cp, style->ts, cm, cmt, spt ? spt->t->data : NULL, cmdt ? cmdt->t->data : NULL,
                                   tmpl->length, 1, activity, &key, &scale, hr);
    dm_segment *out = NULL;
    if (*hr >= 0) {
        int r;
        out = dm_segment_clone(tmpl, 0, tmpl->length, &r);
        uint32_t low = 1;
        for (int i = 0; i < 32; i++) if (group & (1u << i)) { low = 1u << i; break; }
        if (out && chords) {
            size_t n;
            uint8_t ts4[2] = { style->ts[0], style->ts[1] };
            uint8_t *b = chords_cord(chords, key, scale, ts4, &n);
            int32_t h;
            dms_chordtrack *ct = b ? dms_chordtrack_load(b, n, &h) : NULL;
            free(b);
            dm_track *t = ct ? dm_track_new(TRK_CHORD, &CLSID_ChordTrack_, ct) : NULL;
            if (t) dm_segment_insert_track(out, t, 0, group, 0x38, 0);
        }
        if (out && !ownstyle) {
            for (dm_segtrack *t; (t = seg_track(out, TRK_STYLE, 0xffffffff, 0)); ) seg_remove(out, t);
            dm_style_srand();               /* (the new style track's constructor) */
            dms_styletrack *T = calloc(1, sizeof(*T));
            dm_track *t = T ? dm_track_new(TRK_STYLE, &CLSID_StyleTrack_, T) : NULL;
            if (t) {
                dm_track_setparam(t, PRM_STYLE, 0, style);
                dm_segment_insert_track(out, t, 0, low, 0x38, 0);
            } else free(T);
        }
        if (out && !owncm) {
            for (dm_segtrack *t; (t = seg_track(out, TRK_CHORDMAP, 0xffffffff, 0)); ) seg_remove(out, t);
            dmc_cmtrack *T = calloc(1, sizeof(*T));
            if (T) {
                dmc_cmtrack_setparam(T, 0, cm);
                dm_track *t = dm_track_new(TRK_CHORDMAP, &CLSID_ChordMapTrack_, T);
                if (t) dm_segment_insert_track(out, t, 0, group, 0x38, 0);
            }
        }
        if (out && !seg_track(tmpl, TRK_TEMPO, 0xffffffff, 0)) {
            dm_tempotrack *T = calloc(1, sizeof(*T));
            if (T && (T->items = calloc(1, sizeof(*T->items)))) {
                T->active = 1; T->n = 1; T->items[0].mt = 0; T->items[0].tempo = style->tempo;
                dm_track *t = dm_track_new(TRK_TEMPO, &CLSID_TempoTrack_, T);
                if (t) dm_segment_insert_track(out, t, 0, group, 0x38, 0);
            } else free(T);
        }
        if (out && !seg_track(tmpl, TRK_BAND, 0xffffffff, 0) && style->defband) {
            dm_bandtrack *T = calloc(1, sizeof(*T));
            dm_track *t = T ? dm_track_new(TRK_BAND, &CLSID_BandTrack_, T) : NULL;
            if (t) {
                dm_bandparam bp = { -64, style->defband };
                dm_track_setparam(t, PRM_BANDPARAM, 0, &bp);
                dm_segment_insert_track(out, t, 0, group, 0x38, 0);
            } else free(T);
        }
        if (out) {
            out->repeats = tmpl->repeats;
            out->length = tmpl->length;
            out->loopstart = tmpl->loopstart; out->loopend = tmpl->loopend;
        }
    }
    clist_free(chords);
    composer_free(&cp);
    if (ownstyle) dms_style_release(style);
    return out;
}
