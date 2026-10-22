/* reDM: the composer's chord composition (dmcompos 772873cd and below): signposts and commands into steps, a
 * chord map signpost chosen for each, and between each two a path through the chord map's connections, its chords
 * spread over the beats and aligned to measures.  The result is written as a chord track ('cord') the way dmcompos
 * writes it for dmstyle to load.  The addresses are XP dmcompos's; the shapes of its structures are kept. */
#include "dmcomp.h"
#include "dmsty.h"
#include "dmperf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* msvcrt's rand() on the application thread (the composer's constructor seeded it with time()); recomposing from
 * a track's Play uses the thread that plays (g_crnd) */
static uint32_t *g_crnd;
static int g_capp = 1;
static int g_fixes = 1;
void dmc_set_fixes(int on) { g_fixes = on; }
static int cm_rand(void)
{
    uint32_t *r = g_crnd ? g_crnd : dm_app_rand_state();
    *r = *r * 214013u + 2531011u;
    int v = (int)((*r >> 16) & 0x7fff);
    if (dm_rand_hook) dm_rand_hook(g_crnd ? g_capp : 1, v, __builtin_return_address(0));
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
    dmc_next **path;                /* (fixes) the connection taken at each count, kept while searching */
    int pathcap, pathend;           /* (fixes) its size; the count the path ended at */
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
/* (fixes) the connection taken from the entry at the path's current count */
static void path_took(creq *R, dmc_next *x) { if (R->path && R->count >= 0 && R->count < R->pathcap) R->path[R->count] = x; }
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
        else { e->flags |= 0x10; R->pathend = R->count; return 1; }
    }
    e->flags &= ~0x10u;
    if (R->reuse) R->reuse = R->reuse->next;
    if (R->reuse && R->reuse->chord && R->reuse->link) {
        dmc_next *x = R->reuse->link;
        if (path_via(x, R)) { x->flags |= 2; e->flags |= 0x10; path_took(R, x); return 1; }
        x->flags |= 4;
        R->reuse = NULL;
    }
    for (dmc_next *x; (x = pick_next(e)); ) {
        if (path_via(x, R)) { x->flags |= 2; e->flags |= 0x10; path_took(R, x); return 1; }
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
    /* (fixes) the connections taken, kept as the path is found */
    int base = R->count;
    R->path = NULL; R->pathcap = 0; R->pathend = -1;
    if (g_fixes && R->maxchords >= 0) {
        R->pathcap = R->maxchords + 1;
        R->path = calloc((size_t)R->pathcap, sizeof(*R->path));
    }
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
        if (!e) { free(R->path); R->path = NULL; return 0; }
    }
    dmc_next **path = R->path;
    int pathend = R->pathend;
    R->path = NULL; R->pathcap = 0;
    cnode *n = calloc(1, sizeof(*n));
    int16_t k = 0;
    if (n) {
        n->chord = &e->chord;
        k = 1;
        s->start = e;
        clist_append(&s->out, n);
    }
    /* the path is read back along the marked connections, the first marked one of each entry; when it came back to
     * an entry, the marks can lead round a cycle for ever (Microsoft hangs there).  Fixed: such a path is followed
     * as it was found instead */
    int found = 0;
    if (path) {
        int ne = 0, steps = 0;
        for (dmc_entry *q = entries; q; q = q->next) ne++;
        for (dmc_entry *q = e; q; ) {
            dmc_next *x = q->nexts;
            for (; x && !(x->flags & 2); x = x->next) {}
            if (!x) break;
            if (++steps > ne) { found = 1; break; }
            q = x->to;
        }
    }
    for (int i = base; e; i++) {
        dmc_next *x;
        if (found) {
            if (i >= pathend || !(x = path[i])) break;
        } else {
            for (x = e->nexts; x && !(x->flags & 2); x = x->next) {}
            if (!x) break;
        }
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
    free(path);
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
    int16_t bpm = ts[0] ? ts[0] : 4;    /* (a style's 0 beats a measure: align_measures divides by it) */
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
static const dm_guid CLSID_CommandTrack_ = { 0xd2ac288c, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_MarkerTrack_ = { 0x55a8fd00, 0x4288, 0x11d3, { 0x9b, 0xd1, 0x8a, 0x0d, 0x61, 0xc8, 0x88, 0x35 } };
static const dm_guid CLSID_SignPostTrack_ = { 0xf17e8672, 0xc3b4, 0x11d1, { 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };

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
            }
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
            }
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

/* ------------------------------------------------------------------ Segment::Compose (dmime 77314b64) */
/* 7728b532: the signpost track's Compose: the segment's style (at 0, in the track's group), chord map track,
 * command track and length composed (activity mode off) into a new chord track.  The chord track's times are
 * written for 4/4 whatever the style's signature (dmcompos passes a fixed one). */
static dm_track *signpost_compose(dm_segment *seg, dm_track *spt, uint32_t group, int *hr)
{
    dms_style *style = NULL;
    *hr = dm_segment_getparam(seg, PRM_STYLE, group, 0, 0, NULL, &style);
    if (*hr < 0) return NULL;
    dm_segtrack *cmt = seg_track(seg, TRK_CHORDMAP, group, 0), *cmdt = seg_track(seg, TRK_COMMAND, group, 0);
    if (!cmt || !cmdt) { dms_style_release(style); *hr = DMUS_E_NOT_FOUND; return NULL; }
    composer cp = { 0 };
    dm_style_srand();                   /* the composer it makes (77282d2f) */
    uint8_t key = 0;
    uint32_t scale = 0;
    cnode *chords = compose_chords(&cp, style->ts, NULL, cmt->t->data, spt->data, cmdt->t->data, seg->length, 0, 0,
                                   &key, &scale, hr);
    dm_track *out = NULL;
    static const uint8_t ts44[2] = { 4, 4 };
    size_t n;
    uint8_t *b = chords_cord(chords, key, scale, ts44, &n);
    int32_t h = DM_E_OUTOFMEMORY;
    dms_chordtrack *ct = b ? dms_chordtrack_load(b, n, &h) : NULL;
    free(b);
    if (ct) out = dm_track_new(TRK_CHORD, &CLSID_ChordTrack_, ct);
    *hr = out ? h : DM_E_OUTOFMEMORY;
    clist_free(chords);
    composer_free(&cp);
    dms_style_release(style);
    return out;
}

/* 77314b64: the composing tracks (DMUS_TRACKCONFIG_COMPOSING), highest priority first, each composed into a new
 * track that replaces the segment's of its class in the group (the signpost track's: a chord track) */
int dmc_segment_compose(dm_segment *s)
{
    int n = 0, hr = DM_OK;
    dm_segtrack *list = malloc((s->ntracks ? s->ntracks : 1) * sizeof(*list));
    if (!list) return DM_E_OUTOFMEMORY;
    for (int i = 0; i < s->ntracks; i++) {
        if (!(s->tracks[i].config & 0x200)) continue;
        int k = 0;
        while (k < n && !(list[k].priority < s->tracks[i].priority)) k++;
        memmove(list + k + 1, list + k, (n - k) * sizeof(*list));
        list[k] = s->tracks[i];
        dm_track_addref(list[k].t);
        n++;
    }
    for (int i = 0; i < n; i++) {
        dm_track *t = NULL;
        if (list[i].t->kind == TRK_SIGNPOST) t = signpost_compose(s, list[i].t, list[i].group, &hr);
        else hr = DM_E_NOTIMPL;                 /* (the other tracks' Compose) */
        if (hr < 0 || !t) { if (t) dm_track_release(t); if (hr >= 0) hr = DM_E_FAIL; break; }
        for (int k = 0; k < s->ntracks; k++)
            if (s->tracks[k].t->kind == t->kind && !memcmp(&s->tracks[k].t->clsid, &t->clsid, sizeof(t->clsid)) &&
                (s->tracks[k].group & list[i].group)) { seg_remove(s, &s->tracks[k]); break; }
        dm_segment_insert_track(s, t, 0, list[i].group, 0x38, 0);
    }
    for (int i = 0; i < n; i++) dm_track_release(list[i].t);
    free(list);
    return hr;
}

/* ------------------------------------------------------------------ shapes (ComposeTemplateFromShape 77286629,
 * ComposeSegmentFromShape 77287754 / 77286edd / 77283810).  A shape's template (0x18: its measures at +0x10, its
 * list at +0x14) holds a node a measure (0x10, as cmdsp): a signpost (its groups) or a command and groove level. */
typedef struct { int16_t measures; cmdsp *list; } tshape;

static const int16_t SEC_LEN[10] = { 8, 8, 8, 8, 6, 12, 8, 8, 10, 6 };     /* 7728e214 / 7728e260 */
static const uint16_t SEC_SHAPE[7] = { 1, 3, 4, 6, 6, 6, 6 };               /* 7728e22a / 7728e276 */
static const uint8_t SEC_FILL[7] = { 3, 1, 3, 1, 1, 3, 1 };                 /* 7728e1ed / 7728e239: 1 fill, 3 break */
static const int SEC_TRANS[7] = { -1, 0, 0, 1, -1, 2, -2 };                 /* 7728e1f8 / 7728e244 */
static const uint8_t GROOVES[4] = { 12, 37, 62, 87 };                       /* 7728e284 */
static const uint32_t SP_GROUPS[7] = { 0x100, 1, 4, 0x10, 2, 8, 0x20 };      /* 7728e288 */

static void tlist_free(cmdsp *n) { while (n) { cmdsp *x = n->next; free(n); n = x; } }
static void tshape_free(tshape *T) { if (T) { tlist_free(T->list); free(T); } }
static int tnode_cmp(const void *a, const void *b) { return ((const cmdsp *)a)->measure < ((const cmdsp *)b)->measure; }   /* 77281d9c */
static int cnode_cmp(const void *a, const void *b)      /* 77281db4 */
{
    const cnode *x = a, *y = b;
    return x->measure < y->measure || (x->measure == y->measure && x->beat < y->beat);
}

/* 772890f2: 0..n-1, the lower more likely (n - the bit length of 1 + rand() % (2^n - 1)) */
static int rand_bits(int n)
{
    if (n < 1 || n > 15) return 0;
    unsigned r = (unsigned)(cm_rand() % ((1 << n) - 1)) + 1;
    int k = n;
    for (; r; r >>= 1) k--;
    return k;
}
static void t_append(tshape *T, cmdsp *n)                /* 77283f64 */
{
    cmdsp **pp = &T->list;
    while (*pp) pp = &(*pp)->next;
    *pp = n;
}
/* 77289bec: into the list by measure; on a measure that has a node, a signpost (mode 0) replaces its command and a
 * command (mode 1) its signpost */
static void t_insert(tshape *T, cmdsp *n, int mode)
{
    n->next = NULL;
    for (cmdsp **pp = &T->list; *pp; pp = &(*pp)->next) {
        cmdsp *p = *pp;
        if (p->measure == n->measure) {
            if (!mode) { p->chords = n->chords; p->command = p->groove = p->range = p->repeat = 0; }
            else { p->chords = 0; p->command = n->command; p->groove = n->groove; p->range = n->range; p->repeat = n->repeat; }
            free(n);
            return;
        }
        if (n->measure < p->measure) { n->next = p; *pp = n; return; }
    }
    t_append(T, n);
}
static void t_add_sp(tshape *T, int16_t m, uint32_t chords)            /* 77289d27 */
{
    cmdsp *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->measure = m; n->chords = chords;
    t_insert(T, n, 0);
}
/* 77289c6c: a command by its DMUS_SHAPET-like type: 1 fill, 2 intro, 0x20 break, 0x40 end, else a groove level */
static void t_add_cmd(tshape *T, int16_t m, unsigned type)
{
    cmdsp *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->measure = m;
    switch (type) {
    case 1: n->command = 1; break;
    case 2: n->command = 2; break;
    case 0x20: n->command = 3; break;
    case 0x40: n->command = 4; break;
    case 0x80: n->groove = 0x0c; break;
    case 0x100: n->groove = 0x25; break;
    case 0x400: n->groove = 0x57; break;
    default: n->groove = 0x3e; break;
    }
    t_insert(T, n, 1);
}

/* 77289d71: the signposts: up to seven of the groups (the first may be a cadence one), placed every 2 or 4 measures
 * (each group its own step), the last on the last measure */
static void t_signposts(tshape *T)
{
    T->list = NULL;
    int16_t m = T->measures;
    int lv = -2;
    if (m) do { lv++; m = (int16_t)(m >> 1); } while (m);
    if (lv <= 0) lv = 1;
    if (lv > 7) lv = 7;
    uint32_t pool[7], chosen[7] = { 0 };
    int steps[7];
    for (int i = 0; i < 7; i++) { pool[i] = SP_GROUPS[i]; steps[i] = cm_rand() % 3 ? 4 : 2; }
    if (cm_rand() % 2) steps[0] = 4;
    for (int c = 0; c < lv; c++) {
        int k = rand_bits(c ? 7 - c : 3);
        for (int i = 0; i < 7; i++) {
            if (!pool[i]) continue;
            if (!k) {
                chosen[c] = pool[i];
                if (cm_rand() % 2) chosen[c] |= 0x8000;
                pool[i] = 0;
                break;
            }
            k--;
        }
    }
    t_add_sp(T, 0, chosen[0]);
    uint32_t prev = chosen[0];
    int pos = 0, idx = 0;
    while (pos < T->measures) {
        pos += steps[idx];
        if (T->measures - 1 <= pos) {
            if (cm_rand() % 3) chosen[0] = 0x100;
            t_add_sp(T, (int16_t)(T->measures - 1), chosen[0]);
            return;
        }
        idx = rand_bits(lv);
        uint32_t v = chosen[idx];
        if (v == prev) { idx = rand_bits(lv); v = chosen[idx]; }
        t_add_sp(T, (int16_t)pos, v);
        prev = v;
    }
}

/* 77289211: the signposts' groove levels over [lo, hi): from level `from` toward `to` (at each node's middle), or a
 * random walk; a level kept for over six measures moves */
static void t_grooves(tshape *T, int lo, int hi, int from, int to, int rnd)
{
    int diff = to - from, span = hi - lo, last = -1, lastm = 0;
    uint8_t cur = 0;
    for (cmdsp *n = T->list; n; n = n->next) {
        if (!n->command && n->groove) { cur = n->groove; lastm = n->measure; }
        int m = n->measure;
        if (m < lo) continue;
        if (m >= hi) return;
        int pos = n->next ? ((n->next->measure + m) >> 1) - lo : m - lo;
        int idx;
        if (rnd) {
            idx = cm_rand() % 3 - 1 + last;
            if (idx > 3) idx = 2;
            if (idx < 0) idx = 1;
        } else {
            idx = (pos * diff + (span >> 1)) / span + from;
            if (idx < 0 || idx >= 4) continue;
        }
        uint8_t c;
        if (GROOVES[idx] == cur) {
            if (n->measure <= lastm + 6) continue;
            idx = idx - 1 + cm_rand() % 3;
            if (idx < 0) idx += 2;
            if (idx > 3) idx -= 2;
            if (!diff && idx < last && idx <= from && cm_rand() % 2) idx++;
            c = GROOVES[idx];
            if (c == cur) continue;
        } else {
            if (last >= 0) { if (idx < last) idx = last - 1; else if (idx > last) idx = last + 1; }
            c = GROOVES[idx];
        }
        n->range = 0; n->groove = c; cur = c; lastm = n->measure; last = idx;
    }
}

/* 77289efe: a shape's groove levels, then fills before (and breaks) at level changes; the rising and level shapes add
 * more now and then */
static void t_shape(tshape *T, unsigned shape, int fill, int brk)
{
    int from, to, more = 0, rnd = 0, lo = 0, hi = T->measures - 1;
    switch (shape) {
    case 0: from = 3; to = 0; break;
    case 3: from = to = 3; more = 1; break;
    case 4: from = 0; to = 1; more = 1; break;
    case 5: t_grooves(T, 0, T->measures >> 1, 0, 3, 0); lo = T->measures >> 1; from = 3; to = 0; break;
    case 6: from = to = 0; rnd = 1; break;
    case 7: from = 0; to = 3; break;
    default: from = to = 2; more = 1; break;
    }
    t_grooves(T, lo, hi, from, to, rnd);
    uint8_t prevg = 0;
    int lastm = 0;
    for (cmdsp *n = T->list; n; n = n->next) {
        if (n->command) continue;
        uint8_t g = n->groove;
        if (g && g != prevg && cm_rand() % 2) {
            int m = n->measure;
            if (m >= fill && m - fill > lastm && (g > 0x32 || prevg > 0x4b)) t_add_cmd(T, (int16_t)(m - fill), 1);
            else if (m >= brk && m - brk > lastm && cm_rand() % 3) t_add_cmd(T, (int16_t)(m - brk), 0x20);
        }
        prevg = g;
        if (n->groove) lastm = n->measure;
    }
    if (!more) return;
    int cur = 0;
    lastm = 0;
    for (cmdsp *n = T->list; n; n = n->next) {
        cmdsp *nx = n->next;
        int m = n->measure;
        if (n->command) { cur = m; continue; }
        uint8_t g = n->groove;
        if (cur + fill + 4 + brk < m && cm_rand() % 3 == 0) {
            cur = m;
            if (g > 0x32 && m >= fill && m - fill > lastm && cm_rand() % 2) t_add_cmd(T, (int16_t)(m - fill), 1);
            else if (nx && m + brk <= nx->measure) {
                int t = m + brk, at = m;
                do {
                    t_add_cmd(T, (int16_t)at, 0x20);
                    t += brk; at += brk;
                    if (cm_rand() % 2) break;
                } while (t <= nx->measure);
            }
        }
        if (g) lastm = n->measure;
    }
}

/* 77289b56: the end command `n` measures before the end (a signpost there kept only on the first two measures) */
static void t_end(tshape *T, int n)
{
    cmdsp *f = NULL;
    for (cmdsp *p = T->list; p; p = p->next) if (T->measures - n == p->measure) { f = p; break; }
    cmdsp *x = f ? f : calloc(1, sizeof(*x));
    if (!x) return;
    x->measure = (int16_t)(T->measures - n);
    if (!f || f->measure > 1) x->chords = 0;
    x->command = 4; x->groove = x->range = x->repeat = 0;
    if (!f) t_append(T, x);
}
/* 7728999a (`chords` NULL; `keep`: the intro takes the first measure in place) / 772898bc (with the composed chords,
 * which move by `n`): the intro command at 0, the rest moved by `n`; the first signpost goes to the intro unless the
 * node had no groove level (it becomes the intro itself) */
static void t_intro(tshape *T, int keep, int n, cnode *chords, int seg)
{
    cmdsp *x = calloc(1, sizeof(*x));
    if (!x) return;
    x->command = 2;
    int merged = 0;
    for (cnode *c = chords; c; c = c->next) c->measure = (int16_t)(c->measure + n);
    for (cmdsp *p = T->list; p; p = p->next) {
        int early = seg ? p->measure < n : p->measure < 1;
        if (early && p->chords) {
            int move = seg ? (p->groove || p->command == 4) : (!keep && (p->groove || p->command));
            if (move) { x->chords = p->chords; p->measure = (int16_t)(p->measure + n); p->chords = 0; }
            else { p->command = 2; p->groove = p->range = p->repeat = 0; merged = 1; }
        } else p->measure = (int16_t)(p->measure + n);
    }
    if (seg || !keep) T->measures = (int16_t)(T->measures + n);
    if (merged) free(x);
    else { x->next = T->list; T->list = x; }
}
/* 77281f9d: a command and groove level moved a level up (n > 0) or down (n < 0): a fill up, a break down */
static void groove_shift(cmdsp *d, const cmdsp *s, int n)
{
    uint8_t g = s->groove, c = s->command;
    if (n > 0) {
        if (g == 12) g = 37; else if (g == 37) g = 62; else if (g == 62 || g == 87) g = 87;
        if (c == 1 || c == 3) c = 1;
    } else if (n < 0) {
        if (g == 12 || g == 37) g = 12; else if (g == 62) g = 37; else if (g == 87) g = 62;
        if (c == 1 || c == 3) c = 3;
    }
    d->groove = g; d->command = c;
}
/* 77289a6e / 7728471d: a section's nodes at `pos` (in front, reversed), its levels shifted; its chords too */
static void t_copy(cmdsp **list, int pos, const cmdsp *s, int trans, cnode **out, const cnode *chords)
{
    for (; s; s = s->next) {
        cmdsp *x = calloc(1, sizeof(*x));
        if (!x) continue;
        x->measure = (int16_t)(s->measure + pos);
        groove_shift(x, s, trans);
        x->chords = s->chords;
        x->next = *list; *list = x;
    }
    for (; out && chords; chords = chords->next) {
        cnode *c = malloc(sizeof(*c));
        if (!c) continue;
        *c = *chords;
        c->measure = (int16_t)(c->measure + pos);
        c->next = *out; *out = c;
    }
}
/* 772847f9: a fill (1) or break (3) `len` measures before a section, unless a command or level falls inside */
static void t_fillbreak(cmdsp **list, int pos, int len, uint8_t cmd)
{
    int t = pos - len;
    cmdsp *f = NULL;
    for (cmdsp *p = *list; p; p = p->next) {
        if (p->measure == t) f = p;
        else if ((p->command || p->groove) && p->measure > t && p->measure < len + t) return;
    }
    if (f) { f->command = cmd; return; }
    cmdsp *x = calloc(1, sizeof(*x));
    if (!x) return;
    x->measure = (int16_t)t; x->command = cmd;
    x->next = *list; *list = x;
}

/* the song shape (8, over 24 measures): two to seven sections of 6 to 12 measures, each with its own shape (and, from
 * a style, chords), strung together at random (the shorter last), shifted a level now and then */
static int t_song(composer *cp, tshape *T, int n, int fill, int brk, const uint8_t *ts, dmc_chordmap *cm, int act,
                  cnode **chords)
{
    tshape *S[8] = { 0 };
    cnode *CL[8] = { 0 };
    int L[8] = { 0 }, TR[8] = { 0 }, hr = DM_OK;
    uint8_t B[8] = { 0 };
    int minlen = 12, cnt = 2;
    for (unsigned v = (unsigned)n >> 5; v; ) { cnt++; v >>= 1; if (cnt >= 8) break; }
    for (int c = 0; c + 1 < cnt; c++) {
        int16_t len = SEC_LEN[cm_rand() % 10];
        tshape *s = S[c + 1] = calloc(1, sizeof(*s));
        if (!s) { hr = DM_E_OUTOFMEMORY; goto done; }
        s->measures = len; L[c + 1] = len;
        if (len < minlen) minlen = len;
        t_signposts(s);
        t_shape(s, SEC_SHAPE[c], fill, brk);
        if (chords) CL[c + 1] = compose_steps(cp, ts, cm, s->list, 1, act);
        TR[c + 1] = SEC_TRANS[c];
        B[c + 1] = cm_rand() % 4 ? SEC_FILL[c] : 0;
    }
    T->measures = (int16_t)n;
    int cur = 1, prev = 0, pos = 0;
    while (pos < n) {
        if (n - pos <= minlen) {
            cur = 0;
            tshape *s = calloc(1, sizeof(*s));
            if (!s) { hr = DM_E_OUTOFMEMORY; goto done; }
            tshape_free(S[0]); clist_free(CL[0]);
            S[0] = s; CL[0] = NULL;
            s->measures = (int16_t)(n - pos); L[0] = n - pos;
            t_signposts(s);
            t_shape(s, 0, fill, brk);
            if (chords) CL[0] = compose_steps(cp, ts, cm, s->list, 1, act);
            TR[0] = 0;
            B[0] = cm_rand() % 4 != 0;
        }
        t_copy(&T->list, pos, S[cur]->list, TR[cur], chords, CL[cur]);
        if (B[cur] == 1 && pos >= fill) t_fillbreak(&T->list, pos, fill, 1);
        else if (B[cur] == 3 && pos >= brk) t_fillbreak(&T->list, pos, brk, 3);
        if (TR[cur] < 0) TR[cur]++;
        else if (cm_rand() % 3) { if (cm_rand() % 2) TR[cur]++; else TR[cur]--; }
        pos += L[cur];
        int nx = rand_bits(cnt - 1) + 1;
        if (nx == cur) {
            if (nx == prev) nx = rand_bits(cnt - 1) + 1;
            if (nx == cur) nx = rand_bits(cnt - 1) + 1;
        }
        if (n - pos > minlen) while (n - pos < L[nx]) nx = rand_bits(cnt - 1) + 1;
        prev = cur; cur = nx;
    }
    T->list = dms_msort(T->list, offsetof(cmdsp, next), tnode_cmp);
    if (chords) *chords = dms_msort(*chords, offsetof(cnode, next), cnode_cmp);
done:
    for (int k = 0; k < 8; k++) { tshape_free(S[k]); clist_free(CL[k]); }
    return hr;
}

/* the command track ('cmnd': DMUS_IO_COMMAND for each node with a command or level) and signpost track ('sgnp':
 * DMUS_IO_SIGNPOST for each node, or none) of a template's nodes, a measure being `mlen` */
static dm_track *t_cmdtrack(const cmdsp *list, int32_t mlen)
{
    buf b = { 0 };
    put32(&b, 0xc);
    for (const cmdsp *n = list; n; n = n->next) {
        if (!n->command && !n->groove) continue;
        put32(&b, (uint32_t)(mlen * n->measure));
        put16(&b, (uint16_t)n->measure);
        uint8_t r[6] = { 0, n->command, n->groove, n->range, n->repeat, 0 };
        put(&b, r, 6);
    }
    int32_t h;
    dms_cmdtrack *T = b.p ? dms_cmdtrack_load(b.p, b.n, &h) : NULL;
    free(b.p);
    dm_track *t = T ? dm_track_new(TRK_COMMAND, &CLSID_CommandTrack_, T) : NULL;
    return t;
}
static dm_track *t_sptrack(const cmdsp *list, int32_t mlen)
{
    buf b = { 0 };
    put32(&b, 0xc);
    for (const cmdsp *n = list; n; n = n->next) {
        put32(&b, (uint32_t)(mlen * n->measure));
        put32(&b, n->chords);
        put32(&b, (uint16_t)n->measure);
    }
    int32_t h;
    dmc_sptrack *T = b.p ? dmc_sptrack_load(b.p, b.n, &h) : NULL;
    free(b.p);
    dm_track *t = T ? dm_track_new(TRK_SIGNPOST, &CLSID_SignPostTrack_, T) : NULL;
    return t;
}
static dm_segment *new_seg(void)                         /* CoCreateInstance(CLSID_DirectMusicSegment) */
{
    dm_segment *s = calloc(1, sizeof(*s));
    if (s) { s->refs = 1; s->length = 1; }
    return s;
}

/* 77286629 (from ComposeTemplateFromShape with intro, fill and break lengths 1): a template's commands and signposts */
dm_segment *dmc_template_from_shape(unsigned measures, unsigned shape, int intro, int end, int introlen, int fill, int brk,
                                    int endlen, int *hr)
{
    uint16_t m16 = (uint16_t)measures;
    *hr = DM_E_INVALIDARG;
    if (!m16 || (end && !endlen) || (intro && !introlen)) return NULL;
    if (end && ((intro && introlen + endlen > m16) || endlen > m16)) return NULL;
    if (intro && introlen > m16) return NULL;
    int n = m16 & 0x8000 ? 1 : m16;
    if (end && endlen <= 0) endlen = 1;
    shape = (uint16_t)shape;
    if (shape > 8) shape = 6;
    int endm = 0, keep = 0;
    if (intro) { n -= introlen; if (n < introlen) { n = introlen; keep = 1; } }
    if (end) { endm = n; n += 1 - endlen; if (n < 1) n = 1; }
    tshape *T = calloc(1, sizeof(*T));
    if (!T) { *hr = DM_E_OUTOFMEMORY; return NULL; }
    if (shape == 8 && n > 24) {
        int r = t_song(NULL, T, n, fill, brk, NULL, NULL, 0, NULL);
        if (r < 0) { tshape_free(T); *hr = r; return NULL; }
    } else {
        if (shape == 2) n++;
        T->measures = (int16_t)n;
        t_signposts(T);
        t_shape(T, shape, fill, brk);
        if (shape == 2) { n--; T->measures = (int16_t)n; }
    }
    if (end) { T->measures = (int16_t)endm; t_end(T, endlen > endm ? endm : endlen); }
    if (intro) t_intro(T, keep, introlen, NULL, 0);
    /* the template object's segment (77289598): the command and signpost tracks in group 1 */
    dm_segment *s = new_seg();
    if (!s) { tshape_free(T); *hr = DM_E_OUTOFMEMORY; return NULL; }
    s->length = (mtime_t)T->measures * 0xc00;
    dm_track *ct = t_cmdtrack(T->list, 0xc00), *st = t_sptrack(T->list, 0xc00);
    if (ct) dm_segment_insert_track(s, ct, 0, 1, 0x38, 0);
    if (st) dm_segment_insert_track(s, st, 0, 1, 0x38, 0);
    tshape_free(T);
    *hr = DM_OK;
    return s;
}

/* 7740b2ae (dmstyle's private interface): the n-th time all the style's patterns for the command and groove range
 * (lo/hi the level less/plus half the range) can start at, by their start markers; 1 (S_FALSE) when there is none */
static int style_valid_start(const dms_style *st, unsigned n, const cmdsp *c, mtime_t *out)
{
    int lo = (int)c->groove - (c->range >> 1), hi = (int)(c->range >> 1) + c->groove;
    if (lo < 0) lo = 0;
    if (hi > 100) hi = 100;
    const dms_mtime *ptr[64];
    int k = 0;
    for (int i = st->npatterns - 1; i >= 0 && k < 64; i--) {     /* (gathered in front, so last first) */
        const dms_pattern *p = st->patterns[i];
        if (!(p->groovelo <= (uint8_t)hi && (uint8_t)lo <= p->groovehi)) continue;
        uint8_t cmd = c->command, eh = (uint8_t)(p->embellish >> 8);
        int ok = (cmd == 1 && p->embellish == 1) || (cmd == 2 && p->embellish == 4) || (cmd == 3 && p->embellish == 2) ||
                 (cmd == 4 && p->embellish == 8) || (cmd == 0 && p->embellish == 0) || (eh && eh == cmd);
        if (ok && p->markers) ptr[k++] = p->markers;
    }
    if (!k) return 1;
    mtime_t cur = 0;
    for (unsigned j = 0; ; j++) {
        if (n < j) break;
        mtime_t mn = ptr[0]->t, mx = mn;
        for (int i = 0; i < k; i++) { if (ptr[i]->t < mn) mn = ptr[i]->t; if (mx < ptr[i]->t) mx = ptr[i]->t; }
        for (;;) {
            cur = mx;
            if (mx == mn) break;
            mtime_t nm = mx;
            for (int i = 0; i < k; i++) {
                if (ptr[i]->t >= cur) continue;
                if (!(ptr[i] = ptr[i]->next)) return 1;
                mtime_t v = ptr[i]->t;
                if (v < nm) nm = v;
                if (cur < v) cur = v;
            }
            mn = nm; mx = cur;
        }
        if (j < n) for (int i = 0; i < k; i++) if (!(ptr[i] = ptr[i]->next)) return 1;
    }
    *out = cur;
    return 0;
}
/* 77283810 / 77281e6b: the marker track ('MARK' with 'vals') of the valid starts for the first command (empty and E_FAIL
 * without one) */
static dm_track *marker_track(const dms_style *st, const cmdsp *c, int *hr)
{
    buf b = { 0 };
    put(&b, "vals", 4); put32(&b, 0); put32(&b, 4);
    int n = 0;
    mtime_t t;
    while (!style_valid_start(st, (unsigned)n, c, &t)) { put32(&b, (uint32_t)t); n++; }
    if (!n || !b.p) {                   /* E_FAIL: the track stays empty (it is inserted all the same) */
        free(b.p);
        *hr = DM_E_FAIL;
        dm_markertrack *E = calloc(1, sizeof(*E));
        dm_track *tr = E ? dm_track_new(TRK_MARKER, &CLSID_MarkerTrack_, E) : NULL;
        return tr;
    }
    uint32_t sz = (uint32_t)b.n - 8;
    b.p[4] = (uint8_t)sz; b.p[5] = (uint8_t)(sz >> 8); b.p[6] = (uint8_t)(sz >> 16); b.p[7] = (uint8_t)(sz >> 24);
    int32_t h;
    dm_markertrack *T = dm_markertrack_load(b.p, b.n, &h);
    free(b.p);
    dm_track *tr = T ? dm_track_new(TRK_MARKER, &CLSID_MarkerTrack_, T) : NULL;
    *hr = tr ? DM_OK : DM_E_OUTOFMEMORY;
    return tr;
}
/* 77283810: a composed segment: the chord track (when there are chords), the command track, with `align` (and a
 * dmstyle style) a marker track of the first command's valid starts, the style, chord map, tempo (given, else the
 * style's) and band (given, else the style's default) tracks.  With no band the band's SetParam fails: the segment
 * has only its tempo track and the error is returned with it. */
static dm_segment *build_segment(const cmdsp *cmds, const cnode *chords, dms_style *style, dmc_chordmap *cm, int measures,
                                 uint8_t key, uint32_t scale, const double *tempo, dm_band *band, int align, int *hr)
{
    uint8_t ts[2] = { style->ts[0], style->ts[1] };
    int32_t mlen = ts[1] ? 0xc00 / ts[1] * ts[0] : 0;
    dm_segment *s = new_seg();
    if (!s) { *hr = DM_E_OUTOFMEMORY; return NULL; }
    dm_track *chordt = NULL, *cmdt = NULL, *mark = NULL;
    if (chords) {
        size_t len;
        uint8_t *b = chords_cord(chords, key, scale, ts, &len);
        int32_t h;
        dms_chordtrack *ct = b ? dms_chordtrack_load(b, len, &h) : NULL;
        free(b);
        if (ct) chordt = dm_track_new(TRK_CHORD, &CLSID_ChordTrack_, ct);
    }
    cmdt = t_cmdtrack(cmds, mlen);
    if (align && cmds) {
        int h;
        mark = marker_track(style, cmds, &h);
    }
    dm_track *stt = NULL, *cmt = NULL, *tt = NULL, *bt = NULL;
    dm_style_srand();                   /* (the new style track's constructor) */
    dms_styletrack *ST = calloc(1, sizeof(*ST));
    if (ST) stt = dm_track_new(TRK_STYLE, &CLSID_StyleTrack_, ST);
    if (stt) dm_track_setparam(stt, PRM_STYLE, 0, style);
    dmc_cmtrack *CT = calloc(1, sizeof(*CT));
    if (CT) cmt = dm_track_new(TRK_CHORDMAP, &CLSID_ChordMapTrack_, CT);
    if (cmt && cm) dmc_cmtrack_setparam(cmt->data, 0, cm);
    dm_tempotrack *TT = calloc(1, sizeof(*TT));
    if (TT && (TT->items = calloc(1, sizeof(*TT->items)))) {
        TT->active = 1; TT->n = 1; TT->items[0].mt = 0; TT->items[0].tempo = tempo ? *tempo : style->tempo;
        if ((tt = dm_track_new(TRK_TEMPO, &CLSID_TempoTrack_, TT))) dm_segment_insert_track(s, tt, 0, 1, 0x38, 0);
    } else free(TT);
    dm_bandtrack *BT = calloc(1, sizeof(*BT));
    if (BT) bt = dm_track_new(TRK_BAND, &CLSID_BandTrack_, BT);
    int r = DM_E_OUTOFMEMORY;
    if (bt) {
        dm_bandparam bp = { -64, band ? band : style->defband };
        r = dm_track_setparam(bt, PRM_BANDPARAM, 0, &bp);
    }
    dm_track *all[6] = { bt, stt, cmt, cmdt, mark, chordt };
    if (r < 0) {
        for (int i = 0; i < 6; i++) if (all[i]) dm_track_release(all[i]);
    } else {
        s->repeats = 0;
        if ((mtime_t)mlen * measures > 0) s->length = (mtime_t)mlen * measures;     /* (SetLength refuses 0) */
        for (int i = 0; i < 6; i++) if (all[i]) dm_segment_insert_track(s, all[i], 0, 1, 0x38, 0);
    }
    *hr = r;
    return s;
}

/* 77286edd / 77283810: ComposeSegmentFromShape after the style's embellishment lengths (77287754 adjusts the
 * measures): the shape's commands with chords composed from the chord map, as a segment with the tempo, band, style,
 * chord map, command and chord tracks.  With no default band the band's SetParam fails: the segment has only its
 * tempo track and the error is returned with it. */
dm_segment *dmc_segment_from_shape(dms_style *style, dmc_chordmap *cm, unsigned measures, unsigned shape, int activity,
                                   int intro, int end, int introlen, int fill, int brk, int endlen, int *hr)
{
    composer cp = { 0 };
    *hr = DM_OK;
    int n = (int)measures, endm = 0;
    if (intro) { n -= introlen; if (n < 1) n = 1; }
    if (end) { endm = n; n += 1 - endlen; if (n < 1) n = 1; }
    uint8_t ts[2] = { style->ts[0], style->ts[1] };
    tshape T = { 0 };
    cnode *chords = NULL;
    if (shape == 8 && n > 24) {
        int r = t_song(&cp, &T, n, fill, brk, ts, cm, activity, &chords);
        if (r < 0) { *hr = r; goto out; }
    } else {
        if (shape == 2) n++;
        T.measures = (int16_t)n;
        t_signposts(&T);
        t_shape(&T, shape, fill, brk);
        if (shape == 2) { n--; T.measures = (int16_t)n; }
        chords = compose_steps(&cp, ts, cm, T.list, 1, activity);
    }
    if (end) { T.measures = (int16_t)endm; t_end(&T, endlen > endm ? endm : endlen); }
    if (intro) t_intro(&T, 0, introlen, chords, 1);
    cmdsp *cmds = NULL, **pp = &cmds;
    for (const cmdsp *p = T.list; p; p = p->next) {
        if (!p->command && !p->groove) continue;
        cmdsp *x = malloc(sizeof(*x));
        if (!x) { *hr = DM_E_OUTOFMEMORY; break; }
        *x = *p; x->next = NULL;
        *pp = x; pp = &x->next;
    }
    dm_segment *s = NULL;
    if (*hr >= 0) s = build_segment(cmds, chords, style, cm, (int)measures, (uint8_t)(cm->scale >> 24), cm->scale & 0xffffff,
                                    NULL, NULL, 0, hr);
    tlist_free(cmds);
    clist_free(chords);
    tlist_free(T.list);
    composer_free(&cp);
    return s;
out:
    clist_free(chords);
    tlist_free(T.list);
    composer_free(&cp);
    return NULL;
}

/* ------------------------------------------------------------------ ChangeChordMap (77284b30) */
/* 77282302: the scale's tonic (its pattern among the twelve major scales) from the key */
static uint8_t scale_tonic(uint32_t scale, uint8_t key)
{
    switch (scale & 0xfff) {
    case 0xad5: key += 7; break;
    case 0x56b: key += 1; break;
    case 0x5ab: key += 8; break;
    case 0x5ad: key += 3; break;
    case 0x6ad: key += 10; break;
    case 0x6b5: key += 5; break;
    case 0xad6: key += 2; break;
    case 0xb56: key += 9; break;
    case 0xb5a: key += 4; break;
    case 0xd5a: key += 11; break;
    case 0xd6a: key += 6; break;
    }
    return (uint8_t)(key % 12);
}

/* the segment's first chord track, as dmstyle saves it and dmcompos reads it back (77284959: the measure, beat and
 * silence of each chord, its name and subchords; sorted by measure and beat), each chord replaced by the chord map's
 * palette chord on its root's degree in the old key (moved by the change of tonic when `trackscale`); the track
 * reloaded with the chord map's key and scale (dmstyle's Save writes no times: they come from the style's measure) */

int dmc_change_chordmap(dm_segment *seg, int trackscale, dmc_chordmap *cm)
{
    dm_segtrack *st = seg_track(seg, TRK_CHORD, 0xffffffff, 0);
    if (!st) return DMUS_E_NOT_FOUND;
    dms_chordtrack *T = st->t->data;
    /* dmstyle's chord track Save (7740475a) assigns its zeroed record buffer to each chord's name before writing it:
     * the track's names are gone, the change failing or not (fixed: kept) */
    if (!g_fixes) for (dms_chord *c = T->chords; c; c = c->next) memset(c->name, 0, sizeof(c->name));
    composer cp = { 0 };
    uint8_t oldkey = (uint8_t)(T->root % 24);
    uint32_t oldscale = T->scale;
    cnode *list = NULL;
    int hr = DM_OK;
    for (const dms_chord *c = T->chords; c; c = c->next) {
        cnode *n = calloc(1, sizeof(*n));
        ccopy *k = calloc(1, sizeof(*k));
        if (!n || !k) { free(n); free(k); hr = DM_E_OUTOFMEMORY; break; }
        k->c.hasname = 1;               /* (the name read back is the empty one Save wrote) */
        dmc_subnode **pp = &k->c.subs;
        for (uint32_t s = 0; s < c->nsub; s++) {
            dmc_subnode *x = calloc(1, sizeof(*x));
            dmc_sub *u = calloc(1, sizeof(*u));
            if (!x || !u) { free(x); free(u); break; }
            u->chordpat = c->sub[s].chord; u->scalepat = c->sub[s].scale; u->invpat = c->sub[s].inversion;
            u->levels = c->sub[s].levels; u->chordroot = c->sub[s].chordroot; u->scaleroot = c->sub[s].scaleroot;
            u->refs = 1;
            x->sub = u;
            *pp = x; pp = &x->next;
        }
        k->next = cp.copies; cp.copies = k;
        n->chord = &k->c;
        n->measure = (int16_t)c->measure; n->beat = c->beat; n->silent = c->silent;
        clist_append(&list, n);
    }
    if (list && list->next) list = dms_msort(list, offsetof(cnode, next), cnode_cmp);
    uint8_t newkey = (uint8_t)(cm->scale >> 24);
    uint32_t newscale = cm->scale & 0xffffff;
    int8_t shift;
    if (!(cm->flags & 1)) shift = (int8_t)(scale_tonic(newscale, newkey) - scale_tonic(oldscale, oldkey));
    else {
        if (newkey < oldkey % 12) newkey = (uint8_t)(newkey + 12);
        shift = (int8_t)(((int)newkey - oldkey % 12) % 12);
    }
    for (cnode *n = list; n && hr >= 0; n = n->next) {
        int8_t d = (int8_t)(n->chord->subs && n->chord->subs->sub ? n->chord->subs->sub->chordroot : 0);
        if (trackscale) d = (int8_t)(d + shift);
        d = (int8_t)(d - (int8_t)oldkey);
        if (d < 0) d = (int8_t)(d + ((uint8_t)(-d - 1) / 12) * 12 + 12);
        if (d > 23) d = (int8_t)(d - ((uint8_t)(d - 24) / 12) * 12 - 12);
        const dmc_palchord *p = cm->palette[(int)d];
        if (!p) { hr = DM_E_POINTER; break; }
        dmc_chord *c = chord_copy(&cp, &p->c);
        if (!c) { hr = DM_E_OUTOFMEMORY; break; }
        n->chord = c;
    }
    if (hr >= 0) {
        dms_style *style = NULL;
        hr = dm_segment_getparam(seg, PRM_STYLE, 0xffffffff, 0, 0, NULL, &style);
        if (hr >= 0) {
            size_t len;
            uint8_t *b = chords_cord(list, newkey, newscale, style->ts, &len);
            int32_t h = DM_E_OUTOFMEMORY;
            dms_chordtrack *N = b ? dms_chordtrack_load(b, len, &h) : NULL;
            free(b);
            hr = h;
            if (N) {
                dms_chord *old = T->chords;
                T->chords = N->chords; T->root = N->root; T->scale = N->scale;
                N->chords = old;
                dms_chordtrack_free(N);
            }
            dms_style_release(style);
        }
    }
    clist_free(list);
    composer_free(&cp);
    return hr;
}

/* ------------------------------------------------------------------ ComposeTransition (772853bb) */
/* 7728264a: a DMUS_CHORD_KEY as a chord (the name, then its subchords) */
static void chord_from_key(composer *cp, dmc_chord *d, const dm_chord_key *k)
{
    memset(d, 0, sizeof(*d));
    d->hasname = 1;
    for (int i = 0; i < 16 && k->name[i]; i++) d->name[i] = k->name[i];
    dmc_subnode **pp = &d->subs;
    for (int i = 0; i < k->nsub && i < 8; i++) {
        dmc_subnode *x = calloc(1, sizeof(*x));
        dmc_sub *u = calloc(1, sizeof(*u));
        if (!x || !u) { free(x); free(u); break; }
        u->chordpat = k->sub[i].chord; u->scalepat = k->sub[i].scale; u->invpat = k->sub[i].inversion;
        u->levels = k->sub[i].levels; u->chordroot = k->sub[i].chordroot; u->scaleroot = k->sub[i].scaleroot;
        u->refs = 1;
        x->sub = u;
        *pp = x; pp = &x->next;
    }
    (void)cp;
}
static void chord_free_subs_(dmc_chord *c) { for (dmc_subnode *x = c->subs, *y; x; x = y) { y = x->next; free(x->sub); free(x); } c->subs = NULL; }

/* 774085da: the longest embellishment of a type (as the command numbers it) and groove level; S_FALSE without */
static int style_emblength(const dms_style *st, unsigned type, unsigned level, uint32_t *mx)
{
    uint32_t mn = 0;
    *mx = 0;
    if (!st || !st->npatterns) return 1;
    for (int k = 0; k < st->npatterns; k++) {
        const dms_pattern *p = st->patterns[k];
        int ok;
        switch (type) {
        case 0: ok = p->embellish == 0 && p->groovelo <= (uint8_t)level && (uint8_t)level <= p->groovehi; break;
        case 1: ok = p->embellish == 1; break;
        case 2: ok = p->embellish == 4; break;
        case 3: ok = p->embellish == 2; break;
        case 4: ok = p->embellish == 8; break;
        default: ok = (p->embellish >> 8) && (uint16_t)(p->embellish >> 8) == (uint16_t)type; break;
        }
        if (!ok) continue;
        if (*mx < p->measures) *mx = p->measures;
        if (p->measures < mn || mn == 0) mn = p->measures;
    }
    return mn && *mx ? 0 : 1;
}
/* 7728219d: the segment's first tempo track's tempo at 0 */
static int seg_tempo(dm_segment *s, double *t)
{
    dm_segtrack *tt = seg_track(s, TRK_TEMPO, 0xffffffff, 0);
    if (!tt) return DMUS_E_NOT_FOUND;
    struct { mtime_t t; double tempo; } tp;
    int r = dm_track_getparam(tt->t, PRM_TEMPO, 0, NULL, &tp);
    if (r >= 0) *t = tp.tempo;
    return r;
}

/* 772853bb: the transition from what plays (its style `fs`, band, tempo, command and chord) into `to` (its first
 * chord `tk`): the chords (the from chord, or with DMUS_COMPOSEF_MODULATE a chord map signpost's cadences into the
 * to chord), the command's embellishment (and with DMUS_COMPOSEF_LONG a groove measure, and an intro from the to
 * style) as commands, and the segment built from them.  *side: whose graph and audiopath configuration it takes
 * (1 from, 2 to, 0 none).  NULL with S_OK when there is no style to compose with. */
dm_segment *dmc_compose_transition(dms_style *fs, dm_band *band, const double *tempo_from, const dmc_cmdparam *cmdp,
                                   const dm_chord_key *fk, const dm_chord_key *tk, dm_segment *to, unsigned cmd,
                                   uint32_t flags, dmc_chordmap *cm, int *side, int *hr)
{
    composer cp = { 0 };
    uint8_t key = fk->key;
    uint32_t scale = fk->scale;
    dmc_chord fromc, toc;
    chord_from_key(&cp, &fromc, fk);
    chord_from_key(&cp, &toc, tk);
    cmd = (uint16_t)cmd;
    int islong = (flags >> 13) & 1, modulate = (flags >> 12) & 1;
    int isend = cmd == 4 || cmd == 5, isintro = cmd == 2 || cmd == 5;
    int entire = (flags & 0x4000) || (isend && !(flags & 0x8000));
    int addition = (flags >> 16) & 1, align = flags & 1;
    dms_style *ts_ = NULL;
    dm_segment *seg = NULL;
    *hr = DM_OK;
    *side = 0;
    if (to) {
        *hr = dm_segment_getparam(to, PRM_STYLE, 0xffffffff, 0, 0, NULL, &ts_);
        if (*hr < 0) ts_ = NULL;
    }
    if (!ts_ && !fs) { *hr = DM_OK; goto done; }
    double tfrom = 120.0, tto = 120.0;
    const double *tempo = tempo_from;
    if (tempo_from) tfrom = *tempo_from;
    if (!to || seg_tempo(to, &tto) < 0) {
        if (tempo_from) tto = tfrom;
        else { tfrom = tto; tempo = &tfrom; }
    } else if (!tempo_from) { tfrom = tto; tempo = &tfrom; }
    uint8_t fbpm = 4, fbeat = 4, tbpm = 4, tbeat = 4;
    if (fs) { fbpm = fs->ts[0]; fbeat = fs->ts[1]; }
    if (ts_) { tbpm = ts_->ts[0]; tbeat = ts_->ts[1]; }
    if (!tbpm) tbpm = 4;
    if (!fbeat) fbeat = 4;              /* (a style's 0 beat would divide by 0 below) */
    if (!tbeat) tbeat = 4;
    int16_t half = (int16_t)((tbpm >> 1) + ((tbpm >> 1) * 2 != tbpm));
    cnode *chords = NULL;
    cmdsp *head = calloc(1, sizeof(*head)), *tail = head;
    if (!head) { *hr = DM_E_OUTOFMEMORY; goto done; }
    if (cmd != 2) {
        dmc_signpost *sp = modulate ? pick_signpost(cm, &toc, isend, scale, (int8_t)key) : NULL;
        int cad1 = sp && (sp->flags & 2), cad2 = sp && (sp->flags & 4);
        if (!cad1 && !cad2) add_chord(&cp, &chords, &fromc, 0, 0);
        else {
            const dmc_chord *first = cad1 ? ((!islong && isend && cad2) ? &sp->cadence[1] : &sp->cadence[0]) : &sp->cadence[1];
            add_chord(&cp, &chords, first, 0, 0);
            int bottom;
            if (!cad1 || !cad2) bottom = isend;
            else {
                if (islong == isend) add_chord(&cp, &chords, &sp->cadence[1], 0, half);
                if (!islong) bottom = isend;
                else if (!isend) { add_chord(&cp, &chords, &sp->cadence[1], 1, 0); bottom = 0; }
                else bottom = 1;
            }
            if (bottom) add_chord(&cp, &chords, &sp->chord, (int16_t)(islong ? 1 : 0), (int16_t)(islong ? 0 : half));
        }
    }
    head->command = cmdp->cmd; head->groove = cmdp->groove; head->range = cmdp->range; head->repeat = cmdp->repeat;
    int total = 0, introat = 0;
    uint32_t len = 1;
    if (fs) {
        if (cmd != 2) {
            unsigned type = isend ? 4 : cmd;
            if (!entire) total = 1;
            else { if (style_emblength(fs, type, cmdp->groove, &len)) len = 1; total = (int)len; }
        }
        if (!islong) { if (cmd != 2) head->command = (uint8_t)(cmd == 5 ? 4 : cmd); }
        else {
            if (!addition || style_emblength(fs, 0, cmdp->groove, &len)) len = 1;
            total += (int)len;
            if (cmd != 2) {
                if (cmd == 0) head->command = 0;            /* (a dmstyle style: else a groove command after it) */
                else {
                    cmdsp *n = calloc(1, sizeof(*n));
                    if (n) { n->measure = (int16_t)len; n->command = (uint8_t)(cmd == 5 ? 4 : cmd); tail->next = n; tail = n; }
                }
            }
        }
    }
    int before = total;
    if (ts_ && isintro) {
        introat = total;
        uint32_t il;
        if (!entire) total += 1;
        else total += style_emblength(ts_, 2, cmdp->groove, &il) ? 1 : (int)il;
        add_chord(&cp, &chords, &toc, (int16_t)before, 0);
        if (before < 1) head->command = 2;
        else {
            if (cmd == 2) add_chord(&cp, &chords, &fromc, 0, 0);
            cmdsp *n = calloc(1, sizeof(*n));
            if (n) { n->measure = (int16_t)before; n->command = 2; tail->next = n; tail = n; }
        }
    }
    if (!isintro || isend || !ts_) { if (fs) *side = 1; }
    else *side = 2;
    if (!isintro) {
        if (fs) seg = build_segment(head, chords, fs, cm, total, key, scale, tempo, band, align, hr);
        else *hr = DM_OK;
    } else if (!fs) {
        if (ts_) seg = build_segment(head, chords, ts_, cm, total, key, scale, &tto, band, align, hr);
    } else {
        seg = build_segment(head, chords, fs, cm, total, key, scale, tempo, band, align, hr);
        if (*hr >= 0 && ts_) {
            mtime_t at = (mtime_t)(0xc00 / fbeat) * fbpm * introat;
            dm_segtrack *st = seg_track(seg, TRK_STYLE, 0xffffffff, 0);
            if (st) dm_track_setparam(st->t, PRM_STYLE, at, ts_);
            dm_segtrack *tt = seg_track(seg, TRK_TEMPO, 0xffffffff, 0);
            *hr = tt ? DM_OK : DMUS_E_NOT_FOUND;
            if (tt) {
                struct { mtime_t t; double tempo; } tp = { at, tto };
                dm_track_setparam(tt->t, PRM_TEMPO, at, &tp);
            }
            if (at == 0) dm_segment_setparam(seg, PRM_CLEARBANDS, 0xffffffff, 0, 0, NULL);
            dm_bandparam bp;
            if (to && dm_segment_getparam(to, PRM_BANDPARAM, 0xffffffff, DM_SEG_ANYTRACK, 0, NULL, &bp) >= 0) {
                bp.physical = at;
                dm_segment_setparam(seg, PRM_BANDPARAM, 0xffffffff, 0, at, &bp);
                dm_band_release(bp.band);
            }
            if (introat < total && (mtime_t)(0xc00 / tbeat) * (total - introat) * tbpm + at > 0)
                seg->length = (mtime_t)(0xc00 / tbeat) * (total - introat) * tbpm + at;
        }
    }
    tlist_free(head);
    clist_free(chords);
done:
    if (ts_) dms_style_release(ts_);
    chord_free_subs_(&fromc);
    chord_free_subs_(&toc);
    composer_free(&cp);
    return seg;
}

/* ------------------------------------------------------------------ Segment::Compose with from and to segments (dmime 77314ccc) */
static dm_segtrack *seg_track_clsid(dm_segment *s, const dm_guid *clsid, uint32_t group)   /* GetTrack(clsid, group, 0) */
{
    if (!s) return NULL;
    for (int i = 0; i < s->ntracks; i++)
        if (!memcmp(&s->tracks[i].t->clsid, clsid, sizeof(*clsid)) && (s->tracks[i].group & group)) return &s->tracks[i];
    return NULL;
}
static void seg_replace(dm_segtrack *st, dm_track *t) { dm_track *o = st->t; st->t = t; dm_track_release(o); }

/* Each track is composed with the end of `from` (the measure before mt) joined in front and the start of `to` (a
 * measure) joined behind, the segment's length grown to match; afterwards the joined tracks are cut back.  The
 * composed tracks themselves (the signpost track's new chord track) keep the joined span, from part and all.  A
 * track's DMUS_TRACKCONFIG_TRANS1_* flags first replace it by the from or to segment's track of its class.  A from
 * or to segment without a track of a class the segment has fails the call there (the tracks so far stay joined
 * and cut back). */
int dmc_segment_compose_ex(dm_segment *s, mtime_t mt, dm_segment *from, dm_segment *to)
{
    uint8_t bpm = 4, beat = 4;
    if (from) {
        struct { mtime_t t; uint8_t bpm, beat; uint16_t grids; } ts;
        if (dm_segment_getparam(from, PRM_TIMESIG, 0xffffffff, 0, mt, NULL, &ts) >= 0) { bpm = ts.bpm; beat = ts.beat; }
        else bpm = 4;
        if (!beat) beat = 4;
    }
    mtime_t measure = (mtime_t)((uint32_t)bpm * 0xc00 / beat);
    mtime_t fromlen = from ? (mt <= measure ? mt : measure) : 0;
    mtime_t tolen = to ? (to->length <= measure ? to->length : measure) : 0;
    int n = s->ntracks, hr = DM_OK, joined = 0;
    dm_track **jt = calloc(n ? n : 1, sizeof(*jt));
    uint8_t *fl = calloc(n ? n : 1, 1);
    if (!jt || !fl) { free(jt); free(fl); return DM_E_OUTOFMEMORY; }
    for (int i = 0; i < n && hr >= 0; i++) {
        dm_segtrack *st = &s->tracks[i];
        uint32_t group = st->group, cfg = st->config;
        dm_guid clsid = st->t->clsid;
        dm_track *cur = NULL;
        if (cfg & 0x1c00) {
            mtime_t off = (cfg & 0x800) ? mt : 0;
            dm_segment *src = (cfg & 0xc00) ? from : (cfg & 0x1000) ? to : NULL;
            if (src) {
                dm_segtrack *x = seg_track_clsid(src, &clsid, group);
                if (!x) hr = DMUS_E_NOT_FOUND;
                else cur = dm_track_clone(x->t, off, s->length + off, &hr);
            }
        }
        if (!cur) { cur = st->t; dm_track_addref(cur); }
        dm_track *fc = NULL, *tc = NULL;
        if (from && fromlen) {
            dm_segtrack *x = seg_track_clsid(from, &clsid, group);
            if (!x) hr = DMUS_E_NOT_FOUND;
            else { hr = DM_OK; fc = dm_track_clone(x->t, mt - fromlen, mt, &hr); }
        }
        if (to && tolen) {
            dm_segtrack *x = seg_track_clsid(to, &clsid, group);
            if (!x) hr = DMUS_E_NOT_FOUND;
            else { hr = DM_OK; tc = dm_track_clone(x->t, 0, tolen, &hr); }
        }
        int t8 = cur->kind != TRK_TIMESIG && cur->kind != TRK_MARKER;    /* (IDirectMusicTrack8 tracks) */
        if (tc) {
            if (t8 && dm_track_join(cur, tc, s->length, s, group, NULL) >= 0) { fl[i] |= 2; joined = 1; }
            dm_track_release(tc);
        }
        if (hr < 0) { if (fc) dm_track_release(fc); }
        else if (fc && !t8) { hr = (int)0x80004002; dm_track_release(fc);   /* (E_NOINTERFACE) */ }
        else if (fc) {
            if (dm_track_join(fc, cur, fromlen, s, group, NULL) < 0) dm_track_release(fc);
            else { fl[i] |= 1; joined = 1; dm_track_release(cur); cur = fc; }
        }
        seg_replace(st, cur);
        jt[i] = cur;
    }
    mtime_t len = s->length;
    if (joined) s->length = tolen + len + fromlen;
    if (hr >= 0) hr = dmc_segment_compose(s);
    if (joined) {
        for (int k = 0; k < s->ntracks; k++) {
            int i;
            for (i = 0; i < n && jt[i] != s->tracks[k].t; i++) {}
            if (i == n || !(fl[i] & 3)) continue;
            int h;
            dm_track *c = (fl[i] & 1) ? dm_track_clone(s->tracks[k].t, fromlen, fromlen + len, &h)
                                      : dm_track_clone(s->tracks[k].t, 0, len, &h);
            if (c) seg_replace(&s->tracks[k], c);
        }
        s->length = len;
    }
    free(jt); free(fl);
    return hr;
}

/* ------------------------------------------------------------------ recomposing (77287974 with flags 0, from the
 * signpost track's Play 7728aeaf on DMUS_TRACKF_RECOMPOSE) */
/* The segment's chords composed again (activity mode off) from its style, chord map track, signpost track and
 * command track (the signpost track's group, else all), the chord track of that group loaded with them (one made
 * and inserted when there is none).  S_FALSE when no chord comes out; E_INVALIDARG without a style or chord map. */
int dmc_recompose(dm_segment *seg, uint32_t *rnd, int app)
{
    composer cp = { 0 };
    dms_style *style = NULL;
    if (dm_segment_getparam(seg, PRM_STYLE, 0xffffffff, 0, 0, NULL, &style) < 0) return DM_E_INVALIDARG;
    dmc_chordmap *m = NULL;
    dm_segtrack *mt0 = seg_track(seg, TRK_CHORDMAP, 0xffffffff, 0);
    if (!mt0 || dmc_cmtrack_getparam(mt0->t->data, 0, NULL, &m) < 0) { dms_style_release(style); return DM_E_INVALIDARG; }
    dmc_chordmap_release(m);
    dm_segtrack *spt = seg_track(seg, TRK_SIGNPOST, 0xffffffff, 0);
    uint32_t group = spt ? spt->group : 0xffffffff;
    dms_style_release(style);
    style = NULL;
    if (dm_segment_getparam(seg, PRM_STYLE, group, 0, 0, NULL, &style) < 0) return DM_E_INVALIDARG;
    dm_segtrack *cmt = seg_track(seg, TRK_CHORDMAP, group, 0);
    if (!cmt) { dms_style_release(style); return DM_E_INVALIDARG; }
    dm_segtrack *cmdt = seg_track(seg, TRK_COMMAND, group, 0);
    uint8_t key = 0;
    uint32_t scale = 0;
    int hr;
    g_crnd = rnd; g_capp = app;
    cnode *chords = compose_chords(&cp, style->ts, NULL, cmt->t->data, spt ? spt->t->data : NULL,
                                   cmdt ? cmdt->t->data : NULL, seg->length, 0, 0, &key, &scale, &hr);
    g_crnd = NULL; g_capp = 1;
    if (hr >= 0) {
        if (chords) {               /* 772832d6 */
            dm_segtrack *ct = seg_track(seg, TRK_CHORD, group, 0);
            dm_track *t = ct ? ct->t : NULL;
            if (!t) {
                dms_chordtrack *E = calloc(1, sizeof(*E));
                if (E && (t = dm_track_new(TRK_CHORD, &CLSID_ChordTrack_, E)) && !dm_segment_insert_track(seg, t, 0, group, 0x38, 0))
                    t = NULL;           /* (released by the insert) */
            }
            size_t len;
            uint8_t ts[2] = { style->ts[0], style->ts[1] };
            uint8_t *b = t ? chords_cord(chords, key, scale, ts, &len) : NULL;
            int32_t h = DM_E_OUTOFMEMORY;
            dms_chordtrack *N = b ? dms_chordtrack_load(b, len, &h) : NULL;
            free(b);
            hr = h;
            if (N) {
                dms_chordtrack *T = t->data;
                dms_chord *old = T->chords;
                T->chords = N->chords; T->root = N->root; T->scale = N->scale;
                N->chords = old;
                dms_chordtrack_free(N);
            }
        } else hr = 1;                  /* (S_FALSE) */
    }
    clist_free(chords);
    composer_free(&cp);
    dms_style_release(style);
    return hr;
}
