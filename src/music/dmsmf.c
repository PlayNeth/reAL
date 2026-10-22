/* reDM: Standard MIDI Files as segments, a port of dmime's importer (77318500 and helpers).
 *
 * What dmime builds from a MIDI file (group 1 tracks):
 *  - a sequence track with every note (note-on / note-off pairs as one item with a duration), controller, pitch
 *    bend and aftertouch, PChannel = MIDI channel; controllers falling on the same time on a channel are spread one
 *    tick apart; only the last pitch bend at one time on a channel is kept; note-ons are quantised to the time
 *    signature grid with the remainder as the offset;
 *  - a band track: each program change becomes a band one tick early, with the bank from the last CC0 / CC32
 *    (bank selects are not played), on the default GM collection; a channel without a program change gets a
 *    program 0 band that is played but not downloaded; GM / GS / XG reset SysEx messages set the bands' mode;
 *  - tempo, time signature (only the first track that has any) and SysEx tracks, and a chord track (key
 *    signatures; nothing is played from it, it is kept for the track order);
 *  - the segment length: the end of the last track or note, rounded up to a whole measure.
 * Times: MIDI ticks * 768 / division.  SMPTE divisions are refused, as in the original.
 *
 * Fixed here: the original keeps its parser state (running status, the time signature switch) across loads, so a
 * second MIDI file loaded in a process lost its time signatures; and its SysEx chunk could drop the last messages. */
#include "dmi.h"
#include "dmdls.h"
#include <stdlib.h>
#include <string.h>

#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static const dm_guid CLSID_TempoTrack   = { 0xd2ac2885, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_SeqTrack     = { 0xd2ac2886, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_SysExTrack   = { 0xd2ac2887, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_TimeSigTrack = { 0xd2ac2888, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_ChordTrack   = { 0xd2ac288b, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_BandTrack    = { 0xd2ac2894, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };

/* ------------------------------------------------------------------ parser state (dmime's globals) */
typedef struct ev {
    int32_t t, dur;
    uint32_t ch;
    int16_t off;
    uint8_t st, b1, b2;
    uint32_t seq;
    struct ev *next, *pair;
} ev;

typedef struct patchitem {      /* DMUS_IO_PATCH_ITEM */
    int32_t t;
    uint8_t status, prog, msb, lsb;
    uint32_t flags;
    int notinfile;
    struct patchitem *next;
} patchitem;

typedef struct modeitem { int32_t t; int mode; struct modeitem *next; } modeitem;
typedef struct { int32_t t; double tempo; } tempoit;
typedef struct { int32_t t; uint8_t bpm, beat; uint16_t grids; } tsit;
typedef struct { int32_t t; uint32_t len; uint8_t *data; } sxit;

typedef struct {
    const uint8_t *p, *e;
    int ppq;
    uint8_t running;                    /* 7732d4d0 */
    uint32_t seqno;                     /* 7732b38c */
    uint8_t bank[16][2];                /* 7732b200: [0] CC32, [1] CC0; 0xff = none */
    int pcseen[16];                     /* 7732b460 */
    int32_t cclast[16], cccount[16];    /* 7732b280 / 7732b1b8 */
    int32_t sxzero;                     /* 7732b4a4: SysEx at time 0 go at -5, -4, ... 0 */
    int tsok;                           /* 7732afc8 */
    int32_t tstime;                     /* 7732b4a8 */
    uint8_t tsbpm, tsbeat; uint16_t tsgrids;    /* 7732b4ac */
    tempoit *tempo; int ntempo;
    tsit *ts; int nts;
    sxit *sx; int nsx;
    modeitem *modes;                    /* 7732b4cc */
    ev **pool; int npool;               /* everything allocated, freed at the end */
} smf;

static ev *ev_new(smf *s)
{
    ev *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    /* (grown by doubling from 64: a realloc an event is quadratic on big files) */
    if (!s->npool || (s->npool >= 64 && !(s->npool & (s->npool - 1)))) {
        ev **p = realloc(s->pool, (s->npool ? s->npool * 2 : 64) * sizeof(*s->pool));
        if (!p) { free(e); return NULL; }
        s->pool = p;
    }
    s->pool[s->npool++] = e;
    return e;
}

static int rd8(smf *s, uint8_t *v) { if (s->p >= s->e) return 0; *v = *s->p++; return 1; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* 773174ea: a variable length quantity; returns the bytes read */
static int vlq(smf *s, uint32_t *v)
{
    uint8_t b;
    int n = 0;
    *v = 0;
    do {
        if (!rd8(s, &b)) return n;
        n++;
        *v = (*v << 7) | (b & 0x7f);
    } while (b & 0x80);
    return n;
}

/* 7731754a */
static int32_t tick2mt(smf *s, uint32_t ticks)
{
    if (s->ppq == 768) return (int32_t)ticks;
    return (int32_t)((int64_t)ticks * 768 / s->ppq);
}

/* 77317c0e: sorted insert, after items at the same time */
static void add_mode(smf *s, int32_t t, int mode)
{
    modeitem *m = calloc(1, sizeof(*m));
    if (!m) return;
    m->t = t; m->mode = mode;
    modeitem **pp = &s->modes;
    if (*pp && t >= (*pp)->t) {
        modeitem *q = *pp;
        while (q->next && q->next->t <= t) q = q->next;
        pp = &q->next;
    }
    m->next = *pp;
    *pp = m;
}

/* 77317c6b: one event; returns the bytes consumed, 0 on error */
static int parse_event(smf *s, uint32_t ticks, ev **list, patchitem **bands)
{
    s->seqno++;
    int32_t mt = tick2mt(s, ticks);
    uint8_t st;
    if (!rd8(s, &st)) return 0;
    int used = 1;
    if (st < 0x80) { s->p--; used = 0; st = s->running; }
    if (st < 0xf0) {
        s->running = st;
        uint8_t type = st & 0xf0, ch = st & 0xf, d1 = 0, d2 = 0;
        if (type == 0xc0) {
            if (!rd8(s, &d1)) return used;
            used++;
            patchitem *b = calloc(1, sizeof(*b));
            if (b) {
                b->t = mt - 1; b->status = st; b->prog = d1;
                b->msb = s->bank[ch][1]; b->lsb = s->bank[ch][0];
                b->flags = 1;
                if (b->msb != 0xff && b->lsb != 0xff) b->flags |= 2;
                s->pcseen[ch] = 1;
                b->next = *bands;
                *bands = b;
            }
            return used;
        }
        if (type == 0xd0) {
            if (!rd8(s, &d1)) return used;
            used++;
            ev *e = ev_new(s);
            if (e) { e->t = mt; e->st = 0xd0; e->ch = ch; e->b1 = d1; e->seq = s->seqno; e->next = *list; *list = e; }
            return used;
        }
        if (!rd8(s, &d1)) return used;
        used++;
        if (!rd8(s, &d2)) return used;
        used++;
        ev *e = ev_new(s);
        if (!e) return used;
        e->t = mt; e->ch = ch; e->st = type; e->b1 = d1; e->b2 = d2; e->seq = s->seqno;
        if (type == 0x90 && d2 == 0) e->st = 0x80;
        if (type == 0xb0) {
            if (mt == s->cclast[ch]) { s->cccount[ch]++; e->t += s->cccount[ch]; }
            else { s->cccount[ch] = 0; s->cclast[ch] = mt; }
            if (d1 == 0 || d1 == 32) { s->bank[ch][d1 == 32 ? 0 : 1] = d2; return used; }
        }
        e->next = *list;
        *list = e;
        return used;
    }
    if (st == 0xf0) {
        uint32_t len;
        used += vlq(s, &len);
        if ((uint32_t)(s->e - s->p) < len) { s->p = s->e; return used + (int)len; }
        const uint8_t *d = s->p;
        s->p += len;
        int32_t t = mt;
        if (mt == 0) { t = s->sxzero++; if (t > 0) t = 0; }
        /* GM / GS / XG system on: the bands' mode from here on */
        static const uint8_t xg[8] = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00 };
        static const uint8_t gs[10] = { 0xf0, 0x41, 0x00, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41 };
        uint8_t x[11];
        uint32_t n = len + 1 < sizeof(x) ? len + 1 : sizeof(x);
        x[0] = 0xf0;
        memcpy(x + 1, d, n - 1);
        if (len >= 1 && d[0] == 0x43) {
            int i;
            for (i = 0; i < 8 && i < (int)n; i++) {
                if (i == 2) { if ((x[2] & 0xf0) != 0x10) break; }
                else if (x[i] != xg[i]) break;
            }
            if (i == 8) add_mode(s, t, 4);
        } else if (len >= 1 && d[0] == 0x41) {
            int i;
            for (i = 0; i < 10 && i < (int)n; i++) if (i != 2 && x[i] != gs[i]) break;
            if (i == 10) add_mode(s, t, 2);
        } else if (len >= 3 && d[0] == 0x7e && d[2] == 0x09) add_mode(s, t, 1);
        sxit *nsx = realloc(s->sx, (s->nsx + 1) * sizeof(*s->sx));
        uint8_t *data = malloc((size_t)len + 1);
        if (!nsx || !data) { if (nsx) s->sx = nsx; free(data); return used + (int)len; }
        s->sx = nsx;
        sxit *it = &s->sx[s->nsx++];
        it->t = t; it->len = len + 1; it->data = data;
        it->data[0] = 0xf0;
        memcpy(it->data + 1, d, len);
        return used + (int)len;
    }
    if (st == 0xf7) {
        uint32_t len;
        used += vlq(s, &len);
        s->p += len <= (uint32_t)(s->e - s->p) ? len : (uint32_t)(s->e - s->p);
        return used + (int)len;
    }
    if (st != 0xff) return used;
    uint8_t type;
    if (!rd8(s, &type)) return used;
    uint32_t len;
    used += 1 + vlq(s, &len);
    if ((uint32_t)(s->e - s->p) < len) len = (uint32_t)(s->e - s->p);
    if (type == 0x51) {
        uint32_t us = 0;
        for (uint32_t i = 0; i < len; i++) us = us << 8 | s->p[i];
        s->p += len;
        if (!us) us = 1;
        tempoit *nt = realloc(s->tempo, (s->ntempo + 1) * sizeof(*s->tempo));
        if (!nt) return used + (int)len;
        s->tempo = nt;
        s->tempo[s->ntempo].t = mt; s->tempo[s->ntempo].tempo = 60000000.0 / (double)us;
        s->ntempo++;
        return used + (int)len;
    }
    if (type == 0x58 && s->tsok && len >= 2) {
        s->tsok = 2;
        s->tsbpm = s->p[0];
        s->tsbeat = (uint8_t)(1u << (s->p[1] & 0x1f));
        if (!s->tsbeat) s->tsbeat = 4;      /* (a denominator of 2^8 or more: a 0 beat divides by 0 in quantise) */
        s->tsgrids = 4;
        s->tstime = mt;
        tsit *nts = realloc(s->ts, (s->nts + 1) * sizeof(*s->ts));
        s->p += len;
        if (!nts) return used + (int)len;
        s->ts = nts;
        s->ts[s->nts].t = mt; s->ts[s->nts].bpm = s->tsbpm; s->ts[s->nts].beat = s->tsbeat; s->ts[s->nts].grids = 4;
        s->nts++;
        return used + (int)len;
    }
    s->p += len;       /* key signatures feed the chord track, which plays nothing; other meta events are skipped */
    return used + (int)len;
}

/* ------------------------------------------------------------------ list processing */
/* 7731757a: of several pitch bends at one time on a channel only the first in list order (the last read) stays */
static ev *dedupe_bends(ev *list)
{
    uint32_t mask = 0;
    int32_t last = 0x7fffffff;
    for (ev *a = list; a; ) {
        ev *b = a->next;
        if (!b) break;
        if (b->t != last) { mask = 0; last = b->t; }
        if (b->st == 0xe0) {
            if (!(mask & 0xffff & (1u << b->ch))) mask |= 1u << b->ch;
            else { a->next = b->next; continue; }
        }
        a = b;
    }
    return list;
}

/* 77317743: merge; equal times: the left item first if it is a note-on and the right one is not, or if neither is
 * and its status is lower; otherwise the right one (also between items of the same kind) */
static ev *merge(ev *l, ev *r)
{
    ev head, *t = &head;
    head.next = NULL;
    while (l && r) {
        int left;
        if (r->t > l->t) left = 1;
        else if (r->t < l->t) left = 0;
        else {
            uint8_t a = l->st & 0xf0, b = r->st & 0xf0;
            left = a != 0xf0 && b != 0xf0 && a != b && (a == 0x90 || (b != 0x90 && a < b));
        }
        if (left) { t->next = l; t = l; l = l->next; }
        else { t->next = r; t = r; r = r->next; }
    }
    t->next = l ? l : r;
    return head.next;
}

/* 773177c3: merge sort, split as the original does */
static ev *sort_list(ev *list)
{
    if (!list || !list->next) return list;
    ev *slow = list, *fast = list->next;
    while (fast->next && (fast = fast->next->next)) slow = slow->next;
    ev *second = slow->next;
    slow->next = NULL;
    second = sort_list(second);
    list = sort_list(list);
    return merge(list, second);
}

/* 773175f0: order items at one time by when they were read, then pair each note-off with the oldest open
 * note-on of its key (the note-off goes); note-ons left without an end last one tick */
static ev *pair_notes(ev *list)
{
    if (!list) return NULL;
    ev head;
    memset(&head, 0, sizeof(head));
    head.t = -1;
    head.next = list;
    ev *p3 = &head, *p5;
    do {
        p5 = p3->next;
        if (p5) {
            int swapped;
            do {
                p5 = p3->next;
                swapped = 0;
                ev *p4 = p5, *p8 = p3;
                if (!p5->next) break;
                do {
                    ev *p1 = p4->next;
                    p5 = p4;
                    if (p4->t != p1->t) break;
                    p5 = p1;
                    ev *p9 = p4;
                    if (p1->seq < p4->seq) {
                        p8->next = p1; p4->next = p1->next; p1->next = p4;
                        swapped = 1;
                        p5 = p4; p9 = p1;
                    }
                    p4 = p5; p8 = p9;
                } while (p5->next);
            } while (swapped);
        }
        p3 = p5;
    } while (p5);
    list = head.next;
    ev **open = calloc(16 * 128, sizeof(*open));    /* (not static: loads may run on several threads) */
    if (!open) return list;
    ev *prev = NULL, *cur = list;
    while (cur) {
        ev *next = cur->next;
        cur->pair = NULL;
        uint8_t type = cur->st & 0xf0;
        if (type == 0x90) {
            ev **q = &open[cur->ch * 128 + cur->b1];
            if (!*q) *q = cur;
            else { ev *x = *q; while (x->pair) x = x->pair; x->pair = cur; }
            prev = cur;
        } else if (type == 0x80) {
            ev **q = &open[cur->ch * 128 + cur->b1];
            if (*q) { (*q)->dur = cur->t - (*q)->t; *q = (*q)->pair; }
            if (prev) prev->next = next; else list = next;
        } else prev = cur;
        cur = next;
    }
    free(open);
    for (ev *e = list; e; e = e->next) if ((e->st & 0xf0) == 0x90 && e->dur == 0) e->dur = 1;
    return list;
}

/* 77317b19 */
static ev *concat(ev *a, ev *b)
{
    if (!a) return b;
    ev *t = a;
    while (t->next) t = t->next;
    t->next = b;
    return a;
}

/* the time signature track's GetParam (7731d3c3): the signature in effect at mt, its time relative to mt, the time
 * to the next one (0 = none) */
static int ts_getparam(const tsit *ts, int n, int32_t mt, int32_t *next, int32_t *rel, uint8_t *beat, uint16_t *grids)
{
    if (!n) return -1;
    int prev = 0, i;
    for (i = 0; i < n; i++) { if (mt < ts[i].t) break; prev = i; }
    *rel = ts[prev].t - mt; *beat = ts[prev].beat; *grids = ts[prev].grids;
    *next = i < n ? ts[i].t - mt : 0;
    return 0;
}

/* 7731781a: note-ons onto the grid, the rest into the offset (nearest grid line) */
static void quantise(smf *s, ev *list, const tsit *ts, int nts, int have_track)
{
    uint8_t beat = s->tsbeat ? s->tsbeat : 4;
    uint16_t grids = s->tsgrids ? s->tsgrids : 4;
    int32_t next = 0, rel = 0;
    if (have_track && ts_getparam(ts, nts, 0, &next, &rel, &beat, &grids) < 0) next = 0;
    int32_t grid = (int32_t)(0xc00u / beat / grids);
    for (ev *e = list; e; e = e->next) {
        if ((e->st & 0xf0) != 0x90) continue;
        if (next && have_track && next < e->t) {
            if (ts_getparam(ts, nts, next, &next, &rel, &beat, &grids) < 0) next = 0;
            else grid = (int32_t)(0xc00u / beat / grids);
        }
        int16_t g = (int16_t)grid;
        if (!g) g = 1;
        int16_t o = (int16_t)((e->t - rel) % (int32_t)g);
        e->off = o;
        e->t -= o;
        if ((int32_t)((uint32_t)(int32_t)g / 2) < o) { e->t += g; e->off = (int16_t)(o - g); }
    }
}

/* 77203c0e: is the patch in the GS sound set (bank select MSB / program; drum kits on channel 11) */
static int gs_patch(const patchitem *p)
{
    uint8_t msb = 0, prog = 0;
    if (p->flags & 2) {
        if (p->lsb != 0) return 0;
        msb = p->msb;
    }
    if (p->flags & 1) prog = p->prog & 0x7f;
    switch (msb) {
    case 0:
        if ((p->status & 0xf) != 10) return 1;
        return prog == 0 || prog == 8 || prog == 0x10 || prog == 0x18 || prog == 0x19 || prog == 0x20 || prog == 0x28 ||
               prog == 0x30 || prog == 0x38;
    case 1:
        if (prog == 0x26 || prog == 0x39 || prog == 0x3c || prog == 0x50 || prog == 0x51 || prog == 0x62 || prog == 0x66 || prog == 0x68) return 1;
        return !(prog <= 0x77) && prog < 0x80;
    case 2:
        if (prog == 0x66 || prog == 0x78) return 1;
        return !(prog <= 0x79) && prog < 0x80;
    case 3:
        return !(prog <= 0x79) && prog < 0x80;
    case 4: case 5:
        if (prog == 0x7a) return 1;
        if (prog < 0x7c) return 0;
        return prog < 0x7f;
    case 6: case 7:
        return prog == 0x7d;
    case 8:
        if (prog < 7 || prog == 0x7d) return 1;
        if (prog > 0x3f && prog < 0x50) return 0;
        if (prog >= 0x50 && prog <= 0x71) return prog == 0x50 || prog == 0x51 || prog == 0x6b;
        if (prog >= 0x20 && prog <= 0x3f) return (prog > 0x25 && prog < 0x29) || prog > 0x3c || prog == 0x30 || prog == 0x32;
        if (prog > 10 && prog < 0x12 && prog != 0xd) return prog != 0xf;
        if (prog < 0x10 || prog > 0x1f) return 0;
        return prog > 0x17 || prog == 0x13 || prog == 0x15;
    case 9:
        return prog == 0xe || prog == 0x76 || prog == 0x7d;
    case 0x10:
        return prog == 0x18 || prog == 0 || prog == 4 || prog == 5 || prog == 6 || prog == 0x10 || prog == 0x13 ||
               prog == 0x19 || prog == 0x1c || prog == 0x27 || prog == 0x3e || prog == 0x3f;
    case 0x18:
        return prog == 4 || prog == 6;
    case 0x20:
        return prog == 0x10 || prog == 0x11 || prog == 0x18 || prog == 0x34;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ chunk writer */
typedef struct { uint8_t *b; size_t n, cap; } buf;
static void put(buf *o, const void *p, size_t n)
{
    if (o->n + n > o->cap) { size_t c = o->cap ? o->cap * 2 : 4096; while (c < o->n + n) c *= 2; o->b = realloc(o->b, c); o->cap = c; }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}
static void p32(buf *o, uint32_t v) { put(o, &v, 4); }
static size_t open_ck(buf *o, uint32_t id) { p32(o, id); size_t at = o->n; p32(o, 0); return at; }
static void close_ck(buf *o, size_t at)
{
    uint32_t sz = (uint32_t)(o->n - at - 4);
    memcpy(o->b + at, &sz, 4);
    if (sz & 1) { uint8_t z = 0; put(o, &z, 1); }
}
static size_t open_list(buf *o, uint32_t id, uint32_t type) { size_t at = open_ck(o, id); p32(o, type); return at; }

static void track(buf *o, const dm_guid *clsid, uint32_t ckid, const void *data, size_t n)
{
    size_t t = open_list(o, FCC('R','I','F','F'), FCC('D','M','T','K'));
    size_t h = open_ck(o, FCC('t','r','k','h'));
    put(o, clsid, 16); p32(o, 0); p32(o, 1); p32(o, ckid); p32(o, 0);
    close_ck(o, h);
    size_t c = open_ck(o, ckid);
    if (n) put(o, data, n);
    close_ck(o, c);
    close_ck(o, t);
}

/* ------------------------------------------------------------------ import */
int dm_is_midi(const void *data, size_t size)
{
    const uint8_t *p = data;
    if (size >= 4 && !memcmp(p, "MThd", 4)) return 1;
    return size >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "RMID", 4);
}

dm_segment *dm_segment_from_midi(const void *data, size_t size, dm_getcoll_fn getcoll, void *ctx)
{
    smf S, *s = &S;
    memset(s, 0, sizeof(*s));
    s->p = data; s->e = s->p + size;
    memset(s->bank, 0xff, sizeof(s->bank));
    for (int i = 0; i < 16; i++) s->cclast[i] = -1;
    s->sxzero = -5;
    s->tsok = 1;
    /* RMID: the MIDI file is the 'data' chunk */
    if (size >= 12 && !memcmp(s->p, "RIFF", 4) && !memcmp(s->p + 8, "RMID", 4)) {
        const uint8_t *q = s->p + 12;
        /* (sizes checked against what is left: a huge one must not wrap the pointer, as it does on 32 bits) */
        while (s->e - q >= 8 && memcmp(q, "data", 4)) {
            uint32_t n; memcpy(&n, q + 4, 4);
            if ((size_t)(s->e - q - 8) < (size_t)n + (n & 1)) return NULL;
            q += 8 + (size_t)n + (n & 1);
        }
        if (s->e - q < 8) return NULL;
        s->p = q + 8;
    }
    if (s->e - s->p < 14 || memcmp(s->p, "MThd", 4)) return NULL;
    uint32_t hlen = be32(s->p + 4);
    int16_t div = (int16_t)(s->p[12] << 8 | s->p[13]);
    if (div < 0) return NULL;                       /* SMPTE time */
    s->ppq = div ? div : 768;
    if (hlen < 6) hlen = 6;
    s->p = (size_t)(s->e - s->p - 8) < hlen ? s->e : s->p + 8 + (size_t)hlen;   /* (past the end: no tracks) */
    ev *all = NULL;
    patchitem *bands = NULL;
    uint32_t maxticks = 0;
    while (s->e - s->p >= 8 && !memcmp(s->p, "MTrk", 4)) {
        uint32_t len = be32(s->p + 4);
        s->p += 8;
        const uint8_t *end = len <= (size_t)(s->e - s->p) ? s->p + len : s->e;    /* (no pointer wrap) */
        const uint8_t *save_e = s->e;
        s->e = end;
        ev *list = NULL;
        uint32_t ticks = 0;
        int32_t remaining = (int32_t)(end - s->p);
        while (remaining > 0) {
            uint32_t d;
            remaining -= vlq(s, &d);
            ticks += d;
            if (remaining < 1) break;
            int n = parse_event(s, ticks, &list, &bands);
            if (!n) { remaining = -1; break; }
            remaining -= n;
        }
        s->p = end;
        s->e = save_e;
        if (s->tsok > 1) s->tsok = 0;
        if (maxticks < ticks) maxticks = ticks;
        list = dedupe_bends(list);
        list = sort_list(list);
        list = pair_notes(list);
        all = concat(all, list);
    }
    int32_t length = tick2mt(s, maxticks);
    all = sort_list(all);

    /* default bands for channels without a program change: program 0, not downloaded */
    for (int ch = 0; ch < 16; ch++) {
        if (s->pcseen[ch]) continue;
        patchitem *b = calloc(1, sizeof(*b));
        if (!b) continue;
        b->t = 0; b->status = (uint8_t)(0xc0 | ch); b->flags = 1; b->notinfile = 1;
        b->msb = b->lsb = 0;
        b->next = bands;
        bands = b;
    }

    /* the tracks, written as a segment file and loaded in the order dmime's InsertTrack gives them */
    buf o = { 0 };
    size_t riff = open_list(&o, FCC('R','I','F','F'), FCC('D','M','S','G'));
    size_t trkl = open_list(&o, FCC('L','I','S','T'), FCC('t','r','k','l'));
    track(&o, &CLSID_ChordTrack, FCC('c','r','d','t'), NULL, 0);
    if (s->ntempo) {
        buf t = { 0 };
        p32(&t, 16);
        for (int i = 0; i < s->ntempo; i++) { p32(&t, (uint32_t)s->tempo[i].t); p32(&t, 0); put(&t, &s->tempo[i].tempo, 8); }
        track(&o, &CLSID_TempoTrack, FCC('t','e','t','r'), t.b, t.n);
        free(t.b);
    }
    /* the time signature track comes before the band track is added to the list here, but InsertTrack puts the
     * band track ahead of it anyway */
    tsit *ts = NULL;
    int nts = 0;
    if (s->nts) {
        buf t = { 0 };
        p32(&t, 8);
        for (int i = 0; i < s->nts; i++) { p32(&t, (uint32_t)s->ts[i].t); put(&t, &s->ts[i].bpm, 1); put(&t, &s->ts[i].beat, 1); put(&t, &s->ts[i].grids, 2); }
        track(&o, &CLSID_TimeSigTrack, FCC('t','i','m','s'), t.b, t.n);
        free(t.b);
        /* as loaded: a copy of the first signature at 0 */
        ts = malloc((s->nts + 1) * sizeof(*ts));
        nts = 0;
        if (ts) {
            if (s->ts[0].t > 0) { ts[0] = s->ts[0]; ts[0].t = 0; nts = 1; }
            memcpy(ts + nts, s->ts, s->nts * sizeof(*ts));
            nts += s->nts;
        }
    }
    quantise(s, all, ts, nts, s->nts != 0);
    all = sort_list(all);
    {
        buf t = { 0 };
        size_t e = open_ck(&t, FCC('e','v','t','l'));
        p32(&t, 0x14);
        for (ev *x = all; x; x = x->next) {
            if (length < x->t + x->dur) length = x->t + x->dur;
            p32(&t, (uint32_t)x->t); p32(&t, (uint32_t)x->dur); p32(&t, x->ch);
            uint16_t off = (uint16_t)x->off;
            put(&t, &off, 2); put(&t, &x->st, 1); put(&t, &x->b1, 1); put(&t, &x->b2, 1);
            uint8_t pad[3] = { 0 };
            put(&t, pad, 3);
        }
        close_ck(&t, e);
        track(&o, &CLSID_SeqTrack, FCC('s','e','q','t'), t.b, t.n);
        free(t.b);
    }
    if (s->nsx) {
        buf t = { 0 };
        for (int i = 0; i < s->nsx; i++) { p32(&t, (uint32_t)s->sx[i].t); p32(&t, 0); p32(&t, s->sx[i].len); put(&t, s->sx[i].data, s->sx[i].len); }
        track(&o, &CLSID_SysExTrack, FCC('s','y','e','x'), t.b, t.n);
        free(t.b);
    }
    close_ck(&o, trkl);
    close_ck(&o, riff);
    dm_segment *seg = dm_segment_load(o.b, o.n, getcoll, ctx);
    free(o.b);

    if (seg) {
        /* the band track: a band per patch item (AddBand), then the GM / GS / XG modes */
        dm_bandtrack *bt = calloc(1, sizeof(*bt));
        dm_collection *gm = getcoll ? getcoll(ctx, NULL, NULL) : NULL;
        for (patchitem *p = bt ? bands : NULL; p; p = p->next) {
            dm_band *b = calloc(1, sizeof(*b));
            dm_inst *bi = b ? calloc(1, sizeof(*bi)) : NULL;
            dm_band **nb = bi ? realloc(bt->bands, (bt->nbands + 1) * sizeof(*bt->bands)) : NULL;
            if (!nb) { free(bi); free(b); continue; }
            bt->bands = nb;
            b->groupbits = 0xffffffff;
            b->logical = b->physical = p->t;
            b->inst = bi;
            b->ninst = 1;
            dm_inst *in = b->inst;
            in->flags = p->flags;
            if (in->flags & 1) in->patch |= p->prog & 0x7f;
            if (in->flags & 2) in->patch |= (uint32_t)(p->lsb & 0x7f) << 8 | (uint32_t)(p->msb & 0x7f) << 16;
            if (gs_patch(p)) {
                in->flags |= 0x200;
                if (p->lsb == 0 && p->msb == 0) in->flags |= 0x100;
            }
            in->pch = p->status & 0xf;
            in->coll = gm;
            dls_addref(gm);
            in->notinfile = p->notinfile;
            /* sorted by time, a new band before others at its time (7720271c) */
            int i = 0;
            while (i < bt->nbands && b->logical > bt->bands[i]->logical) i++;
            memmove(bt->bands + i + 1, bt->bands + i, (bt->nbands - i) * sizeof(*bt->bands));
            bt->bands[i] = b;
            bt->nbands++;
        }
        dls_release(gm);                /* (each band instrument took its own reference) */
        modeitem def = { -1, 1, NULL };
        for (modeitem *m = s->modes ? s->modes : &def; bt && m; m = m->next)
            for (int i = 0; i < bt->nbands; i++)
                if (m->t <= bt->bands[i]->logical && (!m->next || bt->bands[i]->logical < m->next->t)) bt->bands[i]->mode = m->mode;
        dm_track *t = bt ? dm_track_new(TRK_BAND, &CLSID_BandTrack, bt) : NULL;
        if (t) dm_segment_insert_track(seg, t, 0, 1, 0x38, 0);
        /* the length: whole measures of the last signature */
        uint8_t beat = s->tsbeat ? s->tsbeat : 4, bpm = s->tsbpm ? s->tsbpm : 4;
        int32_t m = s->tstime;
        if (m < length) {
            int32_t meas = (int32_t)(0xc00 / beat) * bpm;
            if (meas <= 0) meas = 0xc00;
            do m += meas; while (m < length);
        }
        seg->length = m;
    }
    for (patchitem *p = bands, *n; p; p = n) { n = p->next; free(p); }
    for (modeitem *m = s->modes, *n; m; m = n) { n = m->next; free(m); }
    for (int i = 0; i < s->npool; i++) free(s->pool[i]);
    for (int i = 0; i < s->nsx; i++) free(s->sx[i].data);
    free(s->pool); free(s->tempo); free(s->ts); free(s->sx); free(ts);
    return seg;
}
