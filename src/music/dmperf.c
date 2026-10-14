/* reDM performance engine, a port of dmime.dll's CPerformance/CSegmentState and the sequence, tempo, time
 * signature and band tracks (dmime/dmband, DirectX 9 5.3.x).  See dmperf.h.
 *
 * Comments give the original function addresses (dmime image base 0x77300000, dmband 0x77200000) and offsets.  The
 * code keeps the original's arithmetic, orderings and quirks: event times and their order at the port depend on them. */
#include "dmperf.h"
#include <time.h>
#include "dmdls.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ objects */
typedef struct {            /* DX8 control merger: { list; sum; base } */
    struct mnode { struct mnode *next; uint32_t idx; int32_t val; } *list;
    int32_t sum, base;
} merger;
enum { MG_VOL, MG_EXPR, MG_TRANSPOSE, MG_PB, MG_PAN, MG_CC74, MG_MOD, MG_REV, MG_CHORUS, MG_N };

typedef struct {
    merger m[MG_N];
    uint32_t port, group, chan;
    int16_t transpose;
    uint8_t flags;
    int used;
} pchentry;

typedef struct pchblock { struct pchblock *next; uint32_t base; pchentry e[16]; } pchblock;

#define BUF_BYTES 0x400
typedef struct { rtime_t rt; uint32_t group; uint32_t len; uint8_t d[4]; } bufev;
typedef struct {
    dm_portops ops; void *ctx;
    rtime_t lastpacked;     /* +0x00 */
    int dirty;              /* +0x14 */
    bufev ev[BUF_BYTES / 24 + 1]; int nev; uint32_t used;
} port;

typedef struct {
    dm_track *t;
    void *state;
    int notok;              /* +0x20 */
    uint32_t vid;           /* +0x24 */
    uint32_t group;         /* +0x28 */
    uint32_t cfg;           /* +0x34 */
    uint32_t rtbits;        /* +0x38 */
} sstrack;

struct dm_segstate {
    int refs;
    dm_segstate *lnext;
    dm_perf *perf;          /* +0x28 */
    dm_segment *seg;        /* +0x2c */
    dm_audiopath *audiopath;    /* +0x30 */
    sstrack *tr; int ntr;   /* +0x38 */
    mtime_t loopstart, loopend;                 /* +0x3c +0x40 */
    uint32_t repeats;                           /* +0x44 */
    mtime_t length;                             /* +0x48 */
    uint32_t trackflags, flags, segflags;       /* +0x4c +0x50 +0x54 */
    mtime_t mtstart, mtend, mtoffset, mtlast, mtstop, segpos, startpoint, mtabort;   /* +0x58 .. +0x74 */
    rtime_t rtloopstart, rtloopend;             /* +0x88 +0x90 */
    int64_t i64start;                           /* +0x98 */
    rtime_t rtlast, rtstartpoint, rtoffset, rtend, rtsegpos, rtlength, rtd0, rtd8, rte0;
    uint32_t repeatsleft;                       /* +0xe8 */
    int started;                                /* +0xec */
    uint32_t version;
    uint32_t firstid, lastid;
    int inited;                                 /* +0xfc */
    int canstop;                                /* +0x100 */
    int inplay, retire;                         /* +0x104 +0x108 */
    int released;                               /* the application's reference is gone */
    void *wrapper;                              /* the COM object (weak) */
    void *graph;                                /* +0x34 its tool graph (the host's; a clone of the segment's) */
};

struct dm_audiopath {
    int refs;
    dm_perf *pf;
    uint32_t vid;                               /* +0x0c virtual track id */
    int active;                                 /* +0x30 */
    uint32_t *virt, *real; uint32_t n;          /* +0x38 +0x3c +0x40 */
    rtime_t volend;                             /* +0x10 end of the last volume ramp */
    dm_audiopath *next;
    void *graph;                                /* +0x48 its tool graph (the host's) */
    void *wrapper;                              /* the COM object (weak) */
};

#define NLISTS 10
struct dm_perf {
    dm_hostops ops; void *ctx;
    port ports[8]; int nports;                  /* +0x2c/+0x30 */
    pchblock *blocks;                           /* +0x34 */
    dm_segstate *lists[NLISTS];                 /* +0x3c + 8k */
    int inside_c7c8;                            /* +0x10c */
    int inplay;                                 /* +0x110 */
    dm_queue early, nearq, ontime, tmap, thist, notif, tsq;  /* +0x114 .. +0x144 */
    double lasttempo; mtime_t lasttempo_mt;     /* +0x150 +0x158 */
    rtime_t rtbase;                             /* +0x2f0 */
    rtime_t adjust;                             /* +0x2f8 */
    rtime_t lastnoteon;                         /* +0x300 */
    rtime_t latfloor;                           /* +0x308 */
    rtime_t horizon;                            /* +0x310 */
    rtime_t bumper;                             /* +0x320 */
    mtime_t prepared, target, nextscan;         /* +0x328 +0x32c +0x330 */
    uint32_t prepare_ms, bumper_ms;             /* +0x334 +0x338 */
    float master;                               /* +0x340 */
    uint32_t period;                            /* +0x354 */
    int played, stopped, tempochanged;          /* +0x360 +0x364 +0x368 */
    uint32_t dxver;                             /* +0x370 */
    dm_segstate *ssplaying; uint32_t cfgplaying;   /* +0x374 +0x378 */
    int autodl;
    dm_audiopath *paths, *defpath;
    uint32_t nextpch;                           /* PChannels allocated so far (any) */
    struct apblock *apblocks;                   /* audiopaths' PChannel blocks (7730b361) */
    int xprec, appxprec;        /* extended precision now / in application threads */
    dm_pmsg *pool;
    uint32_t fixes;             /* DM_FIX_* departures from the original */
    dm_guid *ntypes; int nntypes;               /* +0x100 AddNotificationType list */
    rtime_t notif_min;                          /* +0xf8 how long delivered notifications are kept */
    void (*notify_cb)(void *ctx); void *notify_ctx;     /* +0xf0 the notification event */
    struct wdl *wdls;                           /* wave downloads (dmime's process-wide list 7732d4f0) */
    uint32_t rnd, rndapp;                       /* rand() of the transport thread (srand(time) at its start 7730eb7f)
                                                 * and of the application thread (seed 1) */
    int apprand;                                /* running in the application thread (PlaySegment) */
    int hasgroove; int8_t groove;               /* GUID_PerfMasterGrooveLevel (a global parameter once set) */
    dm_toolops tops; void *tctx;                /* tool graphs and application tools (the host's) */
    void *graph;                                /* +0x24 the application's tool graph (SetGraph) */
    dm_pmsg *apphead, *apptail;                 /* messages waiting for an application tool's ProcessPMsg */
};

static uint32_t g_trackid;                      /* 7732b1b0 */

/* a port's wave voice, shared by the wave track's state and the wave messages (dmwavetrack.c) */
typedef struct dm_wvoice { int refs; int port; void *h; } dm_wvoice;
static void wv_release(dm_perf *pf, dm_wvoice *v);
static int wave_out(dm_perf *pf, dm_pmsg *p, rtime_t rt);

/* ------------------------------------------------------------------ small helpers */
/* _ftol: truncation.  The tempo arithmetic runs on the x87 with the calling thread's precision control: dmime's own
 * threads get 53-bit precision, an application thread whatever it set (64-bit under Wine).  A 32-bit x87 build
 * (DM_FPU_NATIVE) simply computes in long double and gets the same behaviour from the thread's control word; other
 * builds emulate it with pf->xprec, set while an application call runs. */
static int64_t ftol(long double x) { return (int64_t)x; }
#ifdef DM_FPU_NATIVE
#define FDIV(pf, a, b) ((long double)(a) / (b))
#define FMUL(pf, a, b) ((long double)(a) * (b))
#else
#define FDIV(pf, a, b) ((pf)->xprec ? (long double)(a) / (b) : (long double)((double)(a) / (b)))
#define FMUL(pf, a, b) ((pf)->xprec ? (long double)(a) * (b) : (long double)((double)(a) * (b)))
#endif

/* ------------------------------------------------------------------ pmsg allocation (77305777 / 773058a1) */
static dm_pmsg *pm_alloc(dm_perf *pf, uint32_t size)
{
    dm_pmsg *p = pf->pool;
    if (p) pf->pool = p->next;
    else if (!(p = malloc(sizeof(*p)))) return NULL;
    memset(p, 0, sizeof(*p));
    p->size = size;
    p->hflags = pf->inplay ? (HF_MAGIC | HF_INPLAY) : HF_MAGIC;
    return p;
}

static void ss_unref(dm_segstate *ss);
static int guid_same(const dm_guid *a, const dm_guid *b) { return !memcmp(a, b, sizeof(*a)); }

static int pm_free(dm_perf *pf, dm_pmsg *p)
{
    if ((p->hflags & 0xFFFF0) != HF_MAGIC || (p->hflags & HF_INQUEUE)) return DMUS_E_CANNOT_FREE;
    free(p->sx_data);
    p->sx_data = NULL;
    if (p->userref && p->user) ss_unref(p->user);       /* FreePMsg releases punkUser */
    p->user = NULL; p->userref = 0;
    if (p->w_voice) { wv_release(pf, p->w_voice); p->w_voice = NULL; }
    if (p->ext) { if (pf->tops.free_ext) pf->tops.free_ext(pf->tctx, p); p->ext = NULL; }
    p->hflags = 0xDEAD0;
    p->next = pf->pool;
    pf->pool = p;
    return DM_OK;
}

static dm_pmsg *pm_clone(dm_perf *pf, const dm_pmsg *s)
{
    dm_pmsg *p = pm_alloc(pf, s->size);
    if (!p) return NULL;
    uint32_t hf = p->hflags;
    *p = *s;
    p->next = NULL; p->hflags = hf; p->rtlast = 0;
    if (s->sx_data) { p->sx_data = malloc(s->sx_len); if (p->sx_data) memcpy(p->sx_data, s->sx_data, s->sx_len); }
    if (p->userref && p->user) ((dm_segstate *)p->user)->refs++;
    if (p->w_voice) p->w_voice->refs++;
    p->ext = NULL;
    if (s->ext && pf->tops.clone_ext) pf->tops.clone_ext(pf->tctx, p, s);
    return p;
}

/* ------------------------------------------------------------------ queues (773192a3, 77319384, 77319341, 773193a5) */
static void q_insert(dm_queue *q, dm_pmsg *h)
{
    if (!h || (h->hflags & HF_INQUEUE)) return;
    h->hflags |= HF_INQUEUE;
    if (!(h->flags & PF_REFTIME)) return;
    dm_pmsg *a = q->hint;
    if (!a || h->rt < a->rt) a = q->head;
    if (a && a->rt <= h->rt) {
        dm_pmsg *n = a->next;
        while (n && n->rt <= h->rt) { a = n; n = n->next; }
        h->next = a->next; a->next = h;
    } else {
        h->next = q->head; q->head = h;
    }
    q->hint = h;
}

static dm_pmsg *q_pop(dm_queue *q)
{
    dm_pmsg *h = q->head;
    if (h) {
        q->head = h->next;
        h->hflags &= ~HF_INQUEUE;
        h->next = NULL;
        if (q->hint == h) q->hint = q->head;
    }
    return h;
}

static dm_pmsg *q_pop_stale(dm_queue *q, rtime_t t)
{
    dm_pmsg *h = q->head;
    if (h && h->next && h->next->rt < t) {
        if (q->hint == h) q->hint = h->next;
        q->head = h->next;
        h->hflags &= ~HF_INQUEUE;
        h->next = NULL;
        return h;
    }
    return NULL;
}

static dm_pmsg *q_remove(dm_queue *q, dm_pmsg *x)
{
    if (q->head == x) return q_pop(q);
    for (dm_pmsg *a = q->head; a; a = a->next)
        if (a->next == x) {
            a->next = x->next;
            x->next = NULL;
            x->hflags &= ~HF_INQUEUE;
            if (q->hint == x) q->hint = a;
            return x;
        }
    return NULL;
}

/* 773191ad: merge sort of n entries; on equal times the merge takes the second half's entry first */
static dm_pmsg *q_msort(dm_pmsg *l, int n)
{
    if (n < 3) {
        if (l && n != 1 && l->next && l->next->rt < l->rt) {
            dm_pmsg *b = l->next;
            l->next = NULL; b->next = l;
            return b;
        }
        return l;
    }
    int h = n >> 1;
    dm_pmsg *m = l;
    for (int i = 1; i < h; i++) m = m->next;
    dm_pmsg *second = q_msort(m->next, n - h);
    m->next = NULL;
    dm_pmsg *first = q_msort(l, h);
    dm_pmsg head, *t = &head;
    while (first && second) {
        if (second->rt <= first->rt) { t->next = second; second = second->next; }
        else { t->next = first; first = first->next; }
        t = t->next;
    }
    t->next = first ? first : second;
    return head.next;
}

static void q_sort(dm_queue *q)
{
    int n = 0;
    for (dm_pmsg *p = q->head; p; p = p->next) n++;
    q->head = q_msort(q->head, n);
    q->hint = NULL;
}

/* ------------------------------------------------------------------ clocks (773041c2, 77304262, 77305605) */
static rtime_t clock_now(dm_perf *pf)
{
    rtime_t t = pf->ops.clock ? pf->ops.clock(pf->ctx) : 0;
    if (!t) t = (rtime_t)((uint64_t)pf->ops.msec(pf->ctx) * 10000);
    return t;
}

rtime_t dm_perf_latency_time(dm_perf *pf)
{
    rtime_t best = 0, t;
    for (int i = 0; i < pf->nports; i++)
        if (pf->ports[i].ops.latency && pf->ports[i].ops.latency(pf->ports[i].ctx, &t) && t > best) best = t;
    if (!best) best = clock_now(pf);
    if (best < pf->latfloor) best = pf->latfloor;
    return best;
}

rtime_t dm_perf_queue_time(dm_perf *pf)
{
    rtime_t best = 0;
    for (int i = 0; i < pf->nports; i++) if (pf->ports[i].lastpacked > best) best = pf->ports[i].lastpacked;
    rtime_t lat = dm_perf_latency_time(pf);
    if (lat > best) best = lat;
    return best;
}

/* ------------------------------------------------------------------ tempo map: MusicToReferenceTime 773050eb,
 * ReferenceToMusicTime 7730522a.  Entries are internal tempo pmsgs sorted by rt; tempo = (float master) * tempo. */
int dm_perf_m2r(dm_perf *pf, mtime_t mt, rtime_t *out)
{
    double tempo = 120.0;
    rtime_t base = pf->rtbase;
    mtime_t bmt = 0;
    dm_pmsg *e = pf->tmap.head;
    if (e) {
        if (mt < e->mt) {
            e = pf->thist.head;
            if (!e || mt < e->mt) e = NULL;
        }
        if (e) {
            while (e->next && e->next->mt <= mt) e = e->next;
            tempo = (double)e->master * e->tempo;
            base = e->rt;
            bmt = e->mt;
        }
    }
    int64_t x = ((int64_t)(mt - bmt) * 600000000 + 384) / 768;
    *out = ftol(FDIV(pf, x, tempo)) - pf->adjust + base;
    return DM_OK;
}

int dm_perf_r2m(dm_perf *pf, rtime_t rt, mtime_t *out)
{
    double tempo = 120.0;
    rtime_t base = pf->rtbase;
    mtime_t bmt = 0;
    dm_pmsg *e = pf->tmap.head;
    if (e) {
        if (rt < e->rt) {
            e = pf->thist.head;
            if (!e || rt < e->rt) e = NULL;
        }
        if (e) {
            while (e->next && e->next->rt <= rt) e = e->next;
            tempo = (double)e->master * e->tempo;
            base = e->rt;
            bmt = e->mt;
        }
    }
    base -= pf->adjust;
    if (rt < base) rt = base;
    int64_t x = ftol(FMUL(pf, (rt - base) * 768, tempo));
    *out = (mtime_t)((x + 300000000) / 600000000) + bmt;
    return DM_OK;
}

static rtime_t M2R(dm_perf *pf, mtime_t mt) { rtime_t r; dm_perf_m2r(pf, mt, &r); return r; }
static mtime_t R2M(dm_perf *pf, rtime_t rt) { mtime_t m; dm_perf_r2m(pf, rt, &m); return m; }

/* 77305062: re-time queued messages after mt */
static void q_retime(dm_perf *pf, dm_queue *q, mtime_t mt)
{
    for (dm_pmsg *p = q->head; p; p = p->next)
        if (mt < p->mt) {
            if (!(p->flags & PF_LOCKTOREFTIME)) p->rt = M2R(pf, p->mt);
            else p->mt = R2M(pf, p->rt);
        }
    int bad = 0;
    for (dm_pmsg *p = q->head; p && p->next; p = p->next) if (p->next->rt < p->rt) bad = 1;
    if (bad) q_sort(q);
}

/* 7730a391: add a tempo map entry */
static void tempo_add(dm_perf *pf, double tempo, mtime_t mt, rtime_t rt)
{
    dm_pmsg *e = pm_alloc(pf, 0x48);
    if (!e) return;
    e->hflags = HF_MAGIC;
    pf->lasttempo_mt = mt;
    pf->lasttempo = tempo;
    double t = tempo;
    if (!(t <= 1000.0)) t = 1000.0;         /* also NaN */
    else if (t < 1.0) t = 1.0;
    e->tempo = t; e->rt = rt; e->mt = mt; e->flags = PF_REFTIME | PF_MUSICTIME;
    e->master = pf->master;
    rtime_t now = clock_now(pf);
    dm_pmsg *x;
    while ((x = q_pop_stale(&pf->tmap, now - 10000000))) q_insert(&pf->thist, x);
    q_insert(&pf->tmap, e);
    for (x = e->next; x; x = x->next) x->master = e->master;
    now = clock_now(pf);
    while ((x = q_pop_stale(&pf->thist, now - 3000000000LL))) pm_free(pf, x);
    pf->tempochanged = 1;
}

/* ------------------------------------------------------------------ parameters (GetParam) */
typedef struct { double tempo; mtime_t t; mtime_t rel; uint8_t last; } privtempo;
typedef struct { mtime_t t; uint8_t bpm, beat; uint16_t grids; } timesig;     /* DMUS_TIMESIGNATURE */
typedef struct { mtime_t t; double tempo; } tempoparam;                       /* DMUS_TEMPO_PARAM */

/* IsParamSupported: tempo 7731bcb1, time signature 7731ce8c, band (dmband) 77201858, wave 77324502.  A tempo or time
 * signature track enabled or disabled by SetParam supports only the switch back (and, enabled, its parameter). */
static int sty_isparam(const dm_track *t, int prm);
static int sty_getparam(const dm_track *t, int prm, mtime_t mt, mtime_t *next, void *data);
static int track_isparam(const dm_track *t, int prm)
{
    switch (t->kind) {
    case TRK_CHORD: case TRK_COMMAND: case TRK_MUTE: case TRK_STYLE: case TRK_MOTIF: case TRK_PATTERN:
    case TRK_CHORDMAP: case TRK_SIGNPOST: return sty_isparam(t, prm);
    case TRK_TEMPO: {
        const dm_tempotrack *T = t->data;
        if (prm != PRM_DISABLETEMPO && prm != PRM_TEMPO && prm != PRM_PRIVTEMPO && prm != PRM_ENABLETEMPO)
            return DMUS_E_TYPE_UNSUPPORTED;
        if (!T->setbyparam) return DM_OK;
        if (!T->active) return prm == PRM_ENABLETEMPO ? DM_OK : DMUS_E_TYPE_DISABLED;
        return prm == PRM_ENABLETEMPO ? DMUS_E_TYPE_DISABLED : DM_OK;
    }
    case TRK_TIMESIG: {
        const dm_tstrack *T = t->data;
        if (prm != PRM_DISABLETIMESIG && prm != PRM_TIMESIG && prm != PRM_ENABLETIMESIG) return DMUS_E_TYPE_UNSUPPORTED;
        if (!T->setbyparam) return DM_OK;
        if (!T->active) return prm == PRM_ENABLETIMESIG ? DM_OK : DMUS_E_TYPE_DISABLED;
        return prm == PRM_ENABLETIMESIG ? DMUS_E_TYPE_DISABLED : DM_OK;
    }
    case TRK_BAND:
        switch (prm) {
        case PRM_DOWNLOAD: case PRM_UNLOAD: case PRM_DOWNLOAD_AP: case PRM_UNLOAD_AP: case PRM_ENABLE_AUTODL:
        case PRM_DISABLE_AUTODL: case PRM_CLEARBANDS: case PRM_IBAND: case PRM_BANDPARAM: case PRM_SMF: case PRM_CONNECTDLS:
            return DM_OK;
        }
        return DMUS_E_TYPE_UNSUPPORTED;
    case TRK_WAVE:
        switch (prm) {
        case PRM_DOWNLOAD: case PRM_DOWNLOAD_AP: case PRM_UNLOAD_AP: case PRM_ENABLE_AUTODL: case PRM_DISABLE_AUTODL:
        case PRM_WAVEPRIV: case PRM_UNLOAD:
            return DM_OK;
        }
        return DMUS_E_TYPE_UNSUPPORTED;
    case TRK_SEQ: case TRK_SYSEX: return DM_E_NOTIMPL;     /* 77307918 */
    }
    return DMUS_E_TYPE_UNSUPPORTED;
}
static int track_supports(const dm_track *t, int prm) { return prm == PRM_ANY || track_isparam(t, prm) == DM_OK; }

/* a track's GetParam (tempo 7731bfc8, time signature 7731d0ea); *next relative, 0 = none */
static int track_getparam(const dm_track *t, int prm, mtime_t mt, mtime_t *next, void *data)
{
    if (t->kind == TRK_TEMPO && (prm == PRM_TEMPO || prm == PRM_PRIVTEMPO)) {
        const dm_tempotrack *tt = t->data;
        if (prm == PRM_PRIVTEMPO) {
            tempoparam tp;
            mtime_t n = 0;
            int hr = track_getparam(t, PRM_TEMPO, mt, &n, &tp);
            privtempo *o = data;
            if (next) *next = n;
            if (hr == DM_OK) { o->tempo = tp.tempo; o->t = 0; o->rel = tp.t; o->last = next && *next == 0; }
            else if (hr == DMUS_E_NOT_FOUND) o->last = 1;
            return hr;
        }
        if (!tt->active) return DMUS_E_TYPE_DISABLED;
        if (!tt->n) return DMUS_E_NOT_FOUND;
        int prev = 0, i;
        for (i = 0; i < tt->n; i++) { if (mt < tt->items[i].mt) break; prev = i; }
        tempoparam *o = data;
        o->tempo = tt->items[prev].tempo;
        o->t = tt->items[prev].mt - mt;
        if (next) *next = i < tt->n ? tt->items[i].mt - mt : 0;
        return DM_OK;
    }
    if (t->kind == TRK_TIMESIG && prm == PRM_TIMESIG) {
        const dm_tstrack *ts = t->data;
        if (!ts->active) return DMUS_E_TYPE_DISABLED;
        if (!ts->n) return DMUS_E_NOT_FOUND;
        int prev = 0, i;
        for (i = 0; i < ts->n; i++) { if (mt < ts->items[i].mt) break; prev = i; }
        timesig *o = data;
        o->t = ts->items[prev].mt - mt; o->bpm = ts->items[prev].bpm; o->beat = ts->items[prev].beat;
        o->grids = ts->items[prev].grids;
        if (next) *next = i < ts->n ? ts->items[i].mt - mt : 0;
        return DM_OK;
    }
    if (t->kind >= TRK_CHORD) return sty_getparam(t, prm, mt, next, data);
    if (t->kind == TRK_SEQ || t->kind == TRK_SYSEX) return DM_E_NOTIMPL;    /* 7730790b */
    return DMUS_E_GET_UNSUPPORTED;      /* (BandParam would need band objects; the wave track's is private) */
}

/* GetParamEx with the state's data (dmime passes it to IDirectMusicTrack8 tracks) */
static int sty_getparam_state(const dm_track *t, void *state, int prm, mtime_t mt, mtime_t *next, void *data);
static int track_getparam_ex(const dm_track *t, void *state, int prm, mtime_t mt, mtime_t *next, void *data)
{
    if (state && t->kind >= TRK_CHORD) return sty_getparam_state(t, state, prm, mt, next, data);
    return track_getparam(t, prm, mt, next, data);
}

/* 77311a65: the index-th track (after `after`) in the group that supports the parameter; GetParam takes only
 * control-enabled tracks (anycfg 0), SetParam any */
static dm_segtrack *seg_findtrack(dm_segment *s, dm_segtrack *after, int prm, uint32_t group, uint32_t index, int anycfg)
{
    for (int i = after ? (int)(after - s->tracks) + 1 : 0; i < s->ntracks; i++) {
        dm_segtrack *t = &s->tracks[i];
        if (!(t->group & group) || (!anycfg && !(t->config & 8)) || !track_supports(t->t, prm)) continue;
        if (!index) return t;
        index--;
    }
    return NULL;
}

/* the segment's GetParam (7731215a): DM_SEG_ANYTRACK tries the next track while one answers NOT_FOUND;
 * IDirectMusicTrack-only tracks (time signature) get *next clamped to the segment end */
static int seg_getparam(dm_segment *s, int prm, uint32_t group, uint32_t index, mtime_t mt, mtime_t *next, void *data)
{
    int any = index == DM_SEG_ANYTRACK, hr = DMUS_E_TRACK_NOT_FOUND;
    dm_segtrack *t = seg_findtrack(s, NULL, prm, group, any ? 0 : index, 0);
    while (t) {
        hr = track_getparam(t->t, prm, mt, next, data);
        if (t->t->kind == TRK_TIMESIG && next && (*next == 0 || s->length - mt < *next)) *next = s->length - mt;
        if (!any || hr != DMUS_E_NOT_FOUND) break;
        t = seg_findtrack(s, t, prm, group, 0, 0);
    }
    return hr;
}
int dm_track_isparam(const dm_track *t, int prm) { return track_isparam(t, prm); }
int dm_track_getparam(const dm_track *t, int prm, mtime_t mt, mtime_t *next, void *data) { return track_getparam(t, prm, mt, next, data); }
int dm_segment_getparam(dm_segment *s, int prm, uint32_t group, uint32_t index, mtime_t mt, mtime_t *next, void *data)
{
    return seg_getparam(s, prm, group, index, mt, next, data);
}

/* 773156d6: performance time -> segment time (folded into the loop with the full repeat count) */
static int ss_fold(dm_segstate *ss, mtime_t *t, mtime_t *off, uint32_t *loops)
{
    mtime_t o = ss->mtstart - ss->startpoint, s = *t - ss->mtstart + ss->startpoint, len = ss->loopend - ss->loopstart;
    uint32_t n = 0;
    while (ss->loopend <= s && n < ss->repeats) { o += len; s -= len; n++; }
    *t = s; *off = o; *loops = n;
    return (s < 0 || ss->length <= s) ? 1 : 0;
}

/* 77315b54: a segment state's GetParam (tracks with CONTROL_ENABLED); *next clamped to the segment end */
static int ss_getparam(dm_segstate *ss, int prm, uint32_t group, uint32_t index, mtime_t mt, mtime_t *next, void *data)
{
    int hr = DMUS_E_TRACK_NOT_FOUND;    /* the original's initial value */
    int any = index == DM_SEG_ANYTRACK;   /* DMUS_SEG_ANYTRACK: the next track while one answers NOT_FOUND */
    if (any) index = 0;
    for (int i = 0; i < ss->ntr; i++) {
        sstrack *e = &ss->tr[i];
        if (!(e->group & group) || !(e->cfg & 8) || !track_supports(e->t, prm)) continue;
        if (index) { index--; continue; }
        hr = track_getparam_ex(e->t, e->state, prm, mt, next, data);
        if (hr >= 0) {
            if (next && (*next == 0 || ss->length - mt < *next)) *next = ss->length - mt;
            return hr;
        }
        if (!any || hr != DMUS_E_NOT_FOUND) return hr;
    }
    return hr;
}

/* 77306acd: the performance's GetParam: control states (list 7, then 4) first, then the primary, then fallbacks */
static dm_segstate *primary_at(dm_perf *pf, mtime_t mt);
/* one segment state's answer for perf_getparam, with the loop bookkeeping the original keeps for *next */
typedef struct { mtime_t segt, loopend, length; uint32_t left; } pgctx;
static int pg_try(dm_segstate *s, int prm, uint32_t group, uint32_t index, mtime_t mt, mtime_t *next, void *data, pgctx *c, int need_inside)
{
    mtime_t t = mt, off; uint32_t loops;
    int out = ss_fold(s, &t, &off, &loops);
    c->segt = t;
    if (out && need_inside) return DMUS_E_NOT_FOUND;
    c->loopend = s->loopend; c->length = s->length; c->left = s->repeats - loops;
    return ss_getparam(s, prm, group, index, t, next, data);
}

/* 77306acd (GetParam) / 773069c2 (GetParamEx): control states (list 7, then 4) first, then the primary, then the
 * fallbacks.  For GetParamEx the asking track's state `ctx` comes first with DMUS_TRACKCONFIG_OVERRIDE_ALL (1),
 * before the primary with OVERRIDE_PRIMARY (2), after it with FALLBACK (4). */
static int perf_getparam_ex(dm_perf *pf, dm_segstate *ctx, uint32_t cflags, int prm, uint32_t group, uint32_t index,
                            mtime_t mt, mtime_t *next, void *data)
{
    int hr = DMUS_E_NOT_FOUND;
    pgctx c = { 0, 0, 0, 0 };
    if (next) *next = 0;
    if (ctx && (cflags & 1)) {
        pgctx t;
        int r = pg_try(ctx, prm, group, index, mt, next, data, &t, 1);
        if (r >= 0) { c = t; hr = r; goto done; }
    }
    for (int k = 0; k < 2; k++)
        for (dm_segstate *s = pf->lists[k ? 4 : 7]; s; s = s->lnext) {
            pgctx t;
            int r = pg_try(s, prm, group, index, mt, next, data, &t, 1);
            if (r >= 0) { c = t; hr = r; goto done; }
        }
    if (ctx && (cflags & 2)) {
        pgctx t;
        int r = pg_try(ctx, prm, group, index, mt, next, data, &t, 1);
        if (r >= 0) { c = t; hr = r; goto done; }
    }
    dm_segstate *p = primary_at(pf, mt);
    c.segt = mt;
    if (p) {
        hr = pg_try(p, prm, group, index, mt, next, data, &c, 0);
        if (hr >= 0) goto done;
    }
    if (ctx && (cflags & 4)) {
        pgctx t = c;
        int r = pg_try(ctx, prm, group, index, mt, next, data, &t, 1);
        if (r >= 0) { c = t; hr = r; goto done; }
        c.segt = t.segt;
    }
    if (next) *next = (c.left == 0 || c.loopend <= c.segt) ? c.length - c.segt : c.loopend - c.segt;
    if (prm == PRM_PRIVTEMPO || prm == PRM_TEMPO) {
        dm_pmsg *e = pf->tmap.head;
        if (e) {
            while (e->next && e->next->mt <= mt) e = e->next;
            if (prm == PRM_TEMPO) { tempoparam *o = data; o->t = e->mt - mt; o->tempo = e->tempo; }
            else { privtempo *o = data; o->t = e->mt - mt; o->tempo = e->tempo; }
            if (next) *next = 0;
            hr = DM_FALSE;
        }
    } else if (prm == PRM_TIMESIG) {
        /* 77304559: the time signature list's entry; default 4/4 */
        timesig *o = data;
        o->t = -mt; o->bpm = 4; o->beat = 4; o->grids = 4;
        hr = DM_OK;
    }
    return hr;
done:
    if (next && c.left && c.loopend - c.segt < *next && c.segt <= c.loopend) *next = c.loopend - c.segt;
    return hr;
}
static int perf_getparam(dm_perf *pf, int prm, uint32_t group, uint32_t index, mtime_t mt, mtime_t *next, void *data)
{
    return perf_getparam_ex(pf, NULL, 0, prm, group, index, mt, next, data);
}

int dm_perf_getparam(dm_perf *pf, int prm, uint32_t trackid, uint32_t group, uint32_t index, mtime_t mt, mtime_t *next, void *data)
{
    dm_segstate *ctx = NULL;
    uint32_t cf = 0;
    if (trackid)
        for (int k = 0; k < 9 && !ctx; k++)
            for (dm_segstate *s = pf->lists[k]; s && !ctx; s = s->lnext)
                for (int i = 0; i < s->ntr; i++) if (s->tr[i].vid == trackid) { ctx = s; cf = s->tr[i].cfg & 7; break; }
    if ((prm == PRM_TEMPO || prm == PRM_TIMESIG) && !data) return DM_E_POINTER;
    return perf_getparam_ex(pf, ctx, cf, prm, group, index, mt, next, data);
}

/* 7730bed9: fill the tempo map from the tempo parameter (control segments, primary) ahead of time */
typedef struct tnode { struct tnode *next; double tempo; mtime_t t, rel; uint8_t last; } tnode;
static void tempo_scan(dm_perf *pf, mtime_t mt0, int first, dm_segstate *ssf, int addall)
{
    privtempo rec = { 120.0, 0, 0, 0 };
    uint32_t idx = 0;
    mtime_t t = mt0, nextscan = mt0, mtnext = 0;
    tnode *list = NULL;
    int hr;
    for (;;) {
        rec.last = 0;
        hr = perf_getparam(pf, PRM_PRIVTEMPO, 0xffffffff, idx, t, &mtnext, &rec);
        if (hr == DM_OK) {
            rec.t = t;
            if (rec.rel > 0) {
                t += rec.rel;
                hr = perf_getparam(pf, PRM_PRIVTEMPO, 0xffffffff, idx, t, &mtnext, &rec);
                goto L53;
            }
            goto L77;
        }
L53:
        rec.t = t;
        if (hr == DM_FALSE) {
            if (first) {
                if (ssf) goto L6d;
                if (addall) tempo_add(pf, rec.tempo, t, M2R(pf, t) + pf->adjust);
                goto done;
            }
        } else {
L6d:
            if (hr == DM_OK) goto L77;
        }
        if (rec.last) { idx++; hr = DM_OK; }
        goto L22;
L77: {
            tnode *n = malloc(sizeof(*n));
            if (n) {
                n->tempo = rec.tempo; n->t = rec.t; n->rel = rec.rel; n->last = rec.last; n->next = NULL;
                if (list && list->t <= rec.t) {
                    tnode *p = list;
                    for (;;) {
                        tnode *q = p->next;
                        if (p->t == rec.t) {
                            if (p->rel < rec.rel) { tnode *keep = p->next; *p = *n; p->next = keep; }
                            free(n);
                            break;
                        }
                        if (!q || rec.t < q->t) { p->next = n; n->next = q; break; }
                        p = q;
                    }
                } else { n->next = list; list = n; }
            }
            mtime_t tn = rec.t + mtnext;
            first = 0;
            if (!rec.last && tn <= pf->prepared + 0x7800) {
                t = tn;
                if (mtnext == 0) goto done;
            } else {
                idx++;
                t = mt0;
                nextscan = tn;
            }
        }
L22:
        if (hr != DM_OK) break;
    }
done:
    if (list && mt0 < list->t) {
        tnode *n = malloc(sizeof(*n));
        if (n) { n->tempo = 120.0; n->t = mt0; n->rel = 0; n->last = 0; n->next = list; list = n; }
        else list->t = mt0;
    }
    for (tnode *n = list; n; n = n->next)
        if (addall || mt0 <= n->rel + n->t) tempo_add(pf, n->tempo, n->t, M2R(pf, n->t) + pf->adjust);
    pf->nextscan = nextscan;
    while (list) { tnode *n = list->next; free(list); list = n; }
}

static void flush_queue(dm_perf *pf, uint32_t vid, dm_queue *q, rtime_t cut, rtime_t cut2, uint32_t fl);

/* 7731318f: does the segment have a control-enabled track that supports the tempo parameter */
static int seg_has_tempo(dm_segment *s)
{
    for (int i = 0; i < s->ntracks; i++) if ((s->tracks[i].config & 8) && track_supports(s->tracks[i].t, PRM_TEMPO)) return 1;
    return 0;
}

/* 7730c3a1: rebuild the tempo map from mt after a segment start, stop, invalidation or tempo change */
static void tempo_rebuild(dm_perf *pf, dm_segstate *ss, mtime_t mt, int addall)
{
    if (mt <= 0 || (ss && !(ss->seg && seg_has_tempo(ss->seg)))) return;
    mtime_t mtp = pf->prepared;
    rtime_t rt0 = M2R(pf, mtp);
    flush_queue(pf, 0, &pf->tmap, rt0, rt0, 0);
    tempo_scan(pf, mt, 1, ss, addall);
    if (M2R(pf, mtp) != rt0) {
        q_retime(pf, &pf->tmap, mt); q_retime(pf, &pf->ontime, mt);
        q_retime(pf, &pf->nearq, mt); q_retime(pf, &pf->early, mt);
        pf->tempochanged = 1;
    }
}

/* 7730e29f: a tempo pmsg reached the output tool */
static void tempo_pmsg(dm_perf *pf, dm_pmsg *p)
{
    mtime_t mt = p->mt;
    tempo_add(pf, p->tempo, mt, p->rt);
    p->hflags = HF_MAGIC;
    q_retime(pf, &pf->tmap, mt); q_retime(pf, &pf->ontime, mt);
    q_retime(pf, &pf->nearq, mt); q_retime(pf, &pf->early, mt);
    pf->tempochanged = 1;
    tempo_rebuild(pf, NULL, mt + 1, 0);
}

/* ------------------------------------------------------------------ time resolution */
/* 77304c92: the primary state playing at mt */
static dm_segstate *primary_at(dm_perf *pf, mtime_t mt)
{
    dm_segstate *r = NULL;
    mtime_t t, o; uint32_t l;
    for (dm_segstate *s = pf->lists[6]; s; s = s->lnext) if (s->mtstart <= mt && mt <= s->mtlast) { r = s; break; }
    /* a playing primary takes precedence over a finished one that ends at mt */
    for (dm_segstate *s = pf->lists[3]; s; s = s->lnext) { t = mt; if (!ss_fold(s, &t, &o, &l)) return s; }
    if (r) return r;
    for (dm_segstate *s = pf->lists[0]; s; s = s->lnext) { t = mt; if (!ss_fold(s, &t, &o, &l)) return s; }
    return NULL;
}

/* 7730959f: quantise a music time to the grid/beat/measure (only with a primary segment, or TIMESIG_ALWAYS) */
static mtime_t resolve_mt(dm_perf *pf, mtime_t mt, uint32_t flags)
{
    if (!(flags & SEGF_TIMESIG_ALWAYS) && !primary_at(pf, mt)) return mt;
    timesig ts;
    if (perf_getparam(pf, PRM_TIMESIG, 0xffffffff, 0, mt, NULL, &ts) < 0) return mt;
    mtime_t el = -ts.t;
    uint32_t res = 1;
    if (ts.beat) {
        if (flags & SEGF_MEASURE) res = (uint32_t)(ts.bpm * 0xc00) / ts.beat;
        else if (flags & SEGF_BEAT) res = 0xc00u / ts.beat;
        else if ((flags & SEGF_GRID) && ts.grids) res = (0xc00u / ts.beat) / ts.grids;
    }
    if (!res) res = 1;
    if (el) el = ((el - 1) / (int32_t)res + 1) * (int32_t)res;
    return el + ts.t + mt;
}

/* 7730a4e3 GetResolvedTime */
static rtime_t resolved_time(dm_perf *pf, rtime_t rt, uint32_t flags)
{
    rtime_t t;
    if (rt == 0) flags |= SEGF_AFTERQUEUETIME;
    if (flags & SEGF_AFTERPREPARETIME) { t = M2R(pf, pf->prepared); if (t > rt) rt = t; }
    else if (flags & SEGF_AFTERLATENCYTIME) { t = dm_perf_latency_time(pf); if (t > rt) rt = t; }
    else if (flags & SEGF_AFTERQUEUETIME) { t = dm_perf_queue_time(pf); if (t > rt) rt = t; }
    if (flags & 0x1803800) return M2R(pf, resolve_mt(pf, R2M(pf, rt), flags));
    return rt;
}

/* ------------------------------------------------------------------ SendPMsg 77304e7e, StampPMsg */
static int send_pmsg(dm_perf *pf, dm_pmsg *p)
{
    if (p->pch == PCH_KILL_ME) { pm_free(pf, p); return DM_OK; }
    if ((p->hflags & HF_INQUEUE) || (p->hflags & 0xFFFF0) != HF_MAGIC) return DMUS_E_ALREADY_SENT;
    if (pf->dxver >= 8 && p->mt == 0 && p->rt == 0) {
        p->rt = (p->flags & PF_RESOLVE_MASK) ? dm_perf_latency_time(pf) : clock_now(pf);
        p->flags = (p->flags & ~PF_MUSICTIME) | PF_REFTIME;
    }
    uint32_t f = p->flags;
    if (!(f & PF_MUSICTIME)) {
        if (!(f & PF_REFTIME)) return DM_E_INVALIDARG;
        p->rt = resolved_time(pf, p->rt, f);
        p->flags &= 0xFE1FC3FF;
        if (p->rt == 0) p->rt = dm_perf_latency_time(pf);
        p->mt = R2M(pf, p->rt);
        p->flags |= PF_MUSICTIME;
    } else if (!(f & PF_REFTIME)) {
        p->rt = M2R(pf, p->mt);
        p->flags |= PF_REFTIME;
        rtime_t t = resolved_time(pf, p->rt, p->flags);
        p->flags &= 0xFE1FC3FF;
        if (t != p->rt) { p->rt = t; p->mt = R2M(pf, t); }
    }
    f = p->flags;
    dm_queue *q;
    if (f & PF_TOOL_QUEUE) q = &pf->nearq;
    else if (f & PF_TOOL_ATTIME) q = &pf->ontime;
    else { p->flags = f | PF_TOOL_IMMEDIATE; q = &pf->early; }
    q_insert(q, p);
    return DM_OK;
}

/* 77307925: the performance's StampPMsg.  Through the application's graph (SetGraph) first: a tool there takes the
 * message, which comes back here when the tool stamps it again (pGraph is the performance's graph).  After the last
 * tool, the output tool from the near queue. */
static int perf_stamp(dm_perf *pf, dm_pmsg *p)
{
    if (pf->graph && pf->tops.stamp && pf->tops.stamp(pf->tctx, pf->graph, DM_GRAPH_PERF, pf, p) == DM_OK) {
        p->tool = 2;
        return DM_OK;
    }
    if (p->ext && pf->tops.unstamp) pf->tops.unstamp(pf->tctx, p);
    p->flags = (p->flags & ~0x14u) | PF_TOOL_QUEUE;
    p->tool = 1;
    return DM_OK;
}

/* 7731ef40: the audiopath's StampPMsg: its graph first (on virtual PChannels), then virtual to performance PChannels
 * and the performance's; a broadcast to the audiopath is cloned to every PChannel of the path (the original takes
 * the first); notes on an inactive path are killed */
static int ap_stamp(dm_perf *pf, dm_audiopath *ap, dm_pmsg *p)
{
    if (!ap->active && (p->type == PT_NOTE || p->type == PT_WAVE)) { p->pch = PCH_KILL_ME; return DMUS_E_AUDIOPATH_INACTIVE; }
    if (ap->graph && pf->tops.stamp) {
        int hr = pf->tops.stamp(pf->tctx, ap->graph, DM_GRAPH_AUDIOPATH, ap, p);
        if (hr == DM_OK) { p->tool = 2; return hr; }
        if (hr >= 0 && hr != DMUS_S_LAST_TOOL) return hr;
    }
    if (p->pch == PCH_BROADCAST_AUDIOPATH) {
        for (uint32_t i = 1; i < ap->n; i++) {
            uint32_t r = ap->real[i];
            if (r >= 0xfffffffcu || (p->type == PT_TRANSPOSE && (r & 0xf) == 9)) continue;
            dm_pmsg *c = pm_clone(pf, p);
            if (!c) continue;
            c->pch = r;
            perf_stamp(pf, c);
            if (send_pmsg(pf, c) < 0) pm_free(pf, c);
        }
        p->pch = (ap->n && ap->real[0] < 0xfffffffcu) ? ap->real[0] : PCH_KILL_ME;
    } else {
        uint32_t i;
        for (i = 0; i < ap->n; i++) if (ap->virt[i] == p->pch) { p->pch = ap->real[i]; break; }
        if (i == ap->n && p->type != PT_NOTIFICATION && p->pch < 0xfffffffcu) p->pch = PCH_KILL_ME;
    }
    return perf_stamp(pf, p);
}

/* 77315fdd: the segment state's StampPMsg: its graph (a clone of the segment's), a broadcast to the segment cloned to
 * the segment's PChannels, then the audiopath's or the performance's */
static int ss_stamp(dm_perf *pf, dm_segstate *ss, dm_pmsg *p)
{
    if (ss->graph && pf->tops.stamp) {
        int hr = pf->tops.stamp(pf->tctx, ss->graph, DM_GRAPH_STATE, ss, p);
        if (hr == DM_OK) { p->tool = 2; return hr; }
        if (hr >= 0 && (pf->dxver == 0 || hr != DMUS_S_LAST_TOOL)) return hr;
    }
    if (p->pch == PCH_BROADCAST_SEGMENT && ss->seg) {
        dm_segment *g = ss->seg;
        for (int i = 1; i < g->npchs; i++) {
            uint32_t r = g->pchs[i];
            if (r >= 0xfffffffcu || (p->type == PT_TRANSPOSE && (r & 0xf) == 9)) continue;
            dm_pmsg *c = pm_clone(pf, p);
            if (!c) continue;
            c->pch = r;
            int hr = ss->audiopath ? ap_stamp(pf, ss->audiopath, c) : perf_stamp(pf, c);
            if (hr < 0 || send_pmsg(pf, c) < 0) pm_free(pf, c);
        }
        p->pch = (g->npchs && g->pchs[0] < 0xfffffffcu) ? g->pchs[0] : PCH_KILL_ME;
    }
    return ss->audiopath ? ap_stamp(pf, ss->audiopath, p) : perf_stamp(pf, p);
}

static void stamp_pmsg(dm_perf *pf, dm_segstate *ss, dm_pmsg *p)
{
    if (ss) ss_stamp(pf, ss, p);
    else perf_stamp(pf, p);
}

static void stamp_send(dm_perf *pf, dm_segstate *ss, dm_pmsg *p)
{
    stamp_pmsg(pf, ss, p);
    if (send_pmsg(pf, p) < 0) pm_free(pf, p);
}

/* ------------------------------------------------------------------ PChannels (77306594, 773065ce) */
static pchentry *pch_lookup(dm_perf *pf, uint32_t pch)
{
    for (pchblock *b = pf->blocks; b; b = b->next)
        if (b->base <= pch && pch < b->base + 16) return &b->e[pch - b->base];
    return NULL;
}

static pchentry *pch_get(dm_perf *pf, uint32_t pch)
{
    pchentry *e = pch_lookup(pf, pch);
    if (e) return e;
    pchblock *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->base = pch & ~15u;
    b->next = pf->blocks;
    pf->blocks = b;
    return &b->e[pch - b->base];
}

int dm_perf_assign_pchannel(dm_perf *pf, uint32_t pch, int port, uint32_t group, uint32_t chan)
{
    if (port < 0 || port >= pf->nports || !group || chan > 15) return DM_E_INVALIDARG;
    pchentry *e = pch_get(pf, pch);
    if (!e) return DM_E_OUTOFMEMORY;
    e->port = port; e->group = group; e->chan = chan; e->flags |= 6; e->used = 1;
    return DM_OK;
}

int dm_perf_assign_block(dm_perf *pf, uint32_t block, int port, uint32_t group)
{
    for (uint32_t i = 0; i < 16; i++) {
        int hr = dm_perf_assign_pchannel(pf, block * 16 + i, port, group, i);
        if (hr < 0) return hr;
    }
    return DM_OK;
}

/* PChannelInfo 7730672d / 773065ce: any PChannel in an assigned block (used or not) */
int dm_perf_pchannel_info(dm_perf *pf, uint32_t pch, int *port, uint32_t *group, uint32_t *chan)
{
    if (!pf->nports) return DMUS_E_NOT_INIT;
    pchentry *e = pch_lookup(pf, pch);
    if (!e) return DM_E_INVALIDARG;
    *port = (int)e->port; *group = e->group; *chan = e->chan;
    return DM_OK;
}

/* AdjustTime 773053a5: +-1 s added to the reference time of the tempo map */
int dm_perf_adjust_time(dm_perf *pf, rtime_t rt)
{
    if (rt < -10000000 || rt > 10000000) return DM_E_INVALIDARG;
    pf->adjust += rt;
    return DM_OK;
}

static int map_pch(dm_perf *pf, uint32_t pch, uint32_t *portidx, uint32_t *group, uint32_t *chan, int16_t *transpose)
{
    if (!pf->nports) return DMUS_E_NOT_INIT;
    pchentry *e = pch_lookup(pf, pch);
    if (!e || !e->used) return DMUS_E_NOT_FOUND_PCH;
    *portidx = e->port; *group = e->group; *chan = e->chan;
    if (transpose) *transpose = e->transpose;
    return DM_OK;
}

/* ------------------------------------------------------------------ mergers (77308bf3 and friends) */
static const int32_t vol2db[128] = {
    -9600,-8415,-7211,-6506,-6006,-5619,-5302,-5034,-4802,-4598,-4415,-4249,-4098,-3959,-3830,-3710,-3598,-3493,-3394,-3300,
    -3211,-3126,-3045,-2968,-2894,-2823,-2755,-2689,-2626,-2565,-2506,-2449,-2394,-2341,-2289,-2238,-2190,-2142,-2096,-2050,
    -2006,-1964,-1922,-1881,-1841,-1802,-1764,-1726,-1690,-1654,-1619,-1584,-1551,-1518,-1485,-1453,-1422,-1391,-1361,-1331,
    -1302,-1273,-1245,-1217,-1190,-1163,-1137,-1110,-1085,-1059,-1034,-1010,-985,-961,-938,-914,-891,-869,-846,-824,-802,
    -781,-759,-738,-718,-697,-677,-657,-637,-617,-598,-579,-560,-541,-522,-504,-486,-468,-450,-432,-415,-397,-380,-363,-347,
    -330,-313,-297,-281,-265,-249,-233,-218,-202,-187,-172,-157,-142,-127,-113,-98,-84,-69,-55,-41,-27,-13,0 };
static const int32_t db2vol[98] = {
    127,119,113,106,100,95,89,84,80,75,71,67,63,60,56,53,50,47,45,42,40,37,35,33,31,30,28,26,25,23,22,21,20,19,17,16,15,15,
    14,13,12,11,11,10,10,9,8,8,8,7,7,6,6,6,5,5,5,4,4,4,4,3,3,3,3,3,2,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,
    0,0,0 };

static int32_t mg_merge(merger *m, uint32_t idx, int32_t v)
{
    if (idx == 0) { m->base = v; return v + m->sum; }
    m->sum = 0;
    int found = 0;
    for (struct mnode *n = m->list; n; n = n->next) {
        if (n->idx == idx) { n->val = v; found = 1; }
        m->sum += n->val;
    }
    if (!found) {
        struct mnode *n = malloc(sizeof(*n));
        if (n) { n->idx = idx; n->val = v; n->next = m->list; m->list = n; m->sum += v; }
    }
    return m->base + m->sum;
}
static int32_t mg_get(merger *m, uint32_t idx)
{
    if (!idx) return m->base;
    for (struct mnode *n = m->list; n; n = n->next) if (n->idx == idx) return n->val;
    return 0;
}
static void mg_reset(merger *m, int32_t v)
{
    while (m->list) { struct mnode *n = m->list->next; free(m->list); m->list = n; }
    m->sum = 0; m->base = v;
}
static int32_t mg_off(merger *m, uint32_t idx, int32_t v, int32_t center, int32_t max)
{
    int32_t r = mg_merge(m, idx, v - center) + center;
    return r < 0 ? 0 : r > max ? max : r;
}
static int32_t db_to_vol(int32_t x)
{
    if (x < -9600) x = -9600;
    if (x > 0) x = 0;
    int32_t i = -x / 100, f = -x % 100;
    return (uint8_t)(db2vol[i] + (db2vol[i + 1] - db2vol[i]) * f / 100);
}
static int32_t mg_vol(merger *m, uint32_t idx, uint8_t v)
{
    int32_t r = mg_merge(m, idx, vol2db[v & 0x7f]);
    if (m->sum == 0 && idx == 0) return v;
    return db_to_vol(r);
}
static void mg_resetall(pchentry *e, int v)
{
    if (v) { mg_reset(&e->m[MG_PAN], 0); mg_reset(&e->m[MG_VOL], -415); }
    mg_reset(&e->m[MG_PB], 0); mg_reset(&e->m[MG_EXPR], 0); mg_reset(&e->m[MG_CC74], 0);
    mg_reset(&e->m[MG_REV], -87); mg_reset(&e->m[MG_CHORUS], -127); mg_reset(&e->m[MG_MOD], -127);
}

/* ------------------------------------------------------------------ port buffers (dmusic 77183231, dmime 77307142) */
static const uint8_t msglen[128] = {
    3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3, 0,2,3,2,0,0,1,0,1,0,1,1,1,0,1,1 };

static void port_play(port *po)
{
    if (!po->nev) return;
    dm_event ev[BUF_BYTES / 24 + 1];
    for (int i = 0; i < po->nev; i++) { ev[i].rt = po->ev[i].rt; ev[i].group = po->ev[i].group; ev[i].len = po->ev[i].len; ev[i].data = po->ev[i].d; }
    if (po->ops.play) po->ops.play(po->ctx, ev, po->nev);
}

static int buf_pack(port *po, rtime_t rt, uint32_t group, uint32_t msg)
{
    uint32_t len = (msg & 0x80) ? msglen[msg & 0x7f] : 0;
    if (!len) return 0;
    uint32_t sz = (len + 0x1b) & ~7u;
    if (BUF_BYTES - po->used < sz) return 0;
    int i = po->nev;
    while (i > 0 && rt < po->ev[i - 1].rt) i--;        /* after events with an equal time */
    memmove(&po->ev[i + 1], &po->ev[i], (po->nev - i) * sizeof(po->ev[0]));
    po->ev[i].rt = rt; po->ev[i].group = group; po->ev[i].len = len;
    memcpy(po->ev[i].d, &msg, 4);
    po->nev++;
    po->used += sz;
    return 1;
}

static void buf_flush(port *po) { po->nev = 0; po->used = 0; }

static int pack(port *po, uint32_t msg, rtime_t rt, uint32_t group)
{
    if (buf_pack(po, rt, group, msg)) return 1;
    port_play(po);
    buf_flush(po);
    return buf_pack(po, rt, group, msg);
}

/* 7730414b */
static void flush_ports(dm_perf *pf)
{
    for (int i = 0; i < pf->nports; i++) {
        port *po = &pf->ports[i];
        if (po->dirty) { po->dirty = 0; port_play(po); buf_flush(po); }
    }
}

/* ------------------------------------------------------------------ the output tool */
/* 7730468d */
static uint32_t curve_step(const dm_pmsg *p)
{
    uint32_t k = (p->c_type == CURVET_PB || p->c_type == CURVET_RPN || p->c_type == CURVET_NRPN) ? 100 : 3;
    int32_t d = (int32_t)p->c_end - (int32_t)p->c_start;
    if (d < 0) d = -d;
    if (!d) d = 1;
    uint32_t s = (uint32_t)(p->c_dur * (int32_t)k) / (uint32_t)d;
    if ((int32_t)s < 16) s = 16;
    return s;
}

static const int16_t curve_sine[193] = {
    0,1,5,11,20,32,46,62,82,103,128,155,184,216,250,287,327,369,413,460,509,561,615,671,730,792,855,921,990,1060,1133,1208,
    1286,1365,1447,1531,1617,1706,1796,1889,1983,2080,2179,2279,2382,2486,2593,2701,2811,2923,3037,3153,3270,3389,3509,3632,
    3755,3881,4008,4136,4266,4397,4530,4664,4799,4936,5074,5213,5354,5495,5638,5781,5926,6071,6218,6365,6514,6663,6813,6963,
    7115,7267,7420,7573,7727,7881,8036,8191,8346,8502,8659,8815,8972,9128,9285,9442,9600,9757,9914,10071,10227,10384,10540,
    10697,10853,11008,11163,11318,11472,11626,11779,11932,12084,12236,12386,12536,12685,12834,12981,13128,13273,13418,13561,
    13704,13845,13986,14125,14263,14399,14535,14669,14802,14933,15063,15191,15318,15444,15567,15690,15810,15929,16046,16162,
    16276,16388,16498,16606,16713,16817,16920,17020,17119,17216,17310,17403,17493,17582,17668,17752,17834,17913,17991,18066,
    18139,18209,18278,18344,18407,18469,18528,18584,18638,18690,18739,18786,18830,18872,18912,18949,18983,19015,19044,19071,
    19096,19117,19137,19153,19167,19179,19188,19194,19198,19200 };
static const int16_t curve_log[193] = {
    0,381,747,1097,1432,1755,2066,2366,2655,2934,3205,3467,3721,3967,4207,4439,4666,4886,5101,5310,5515,5714,5909,6099,6285,
    6467,6645,6819,6990,7157,7321,7482,7640,7795,7947,8096,8243,8387,8529,8668,8805,8940,9073,9204,9332,9459,9584,9707,9828,
    9947,10065,10181,10295,10408,10520,10630,10738,10845,10951,11056,11159,11261,11361,11461,11559,11656,11752,11847,11941,
    12034,12126,12216,12306,12395,12483,12570,12656,12741,12826,12909,12992,13074,13155,13235,13315,13394,13472,13549,13626,
    13702,13777,13851,13925,13998,14071,14143,14214,14285,14355,14425,14494,14562,14630,14698,14764,14831,14896,14962,15026,
    15091,15154,15218,15280,15343,15405,15466,15527,15587,15647,15707,15766,15825,15883,15941,15999,16056,16113,16169,16225,
    16281,16336,16391,16446,16500,16554,16607,16661,16713,16766,16818,16870,16921,16973,17024,17074,17124,17174,17224,17273,
    17323,17371,17420,17468,17516,17564,17611,17658,17705,17752,17798,17844,17890,17936,17981,18026,18071,18116,18160,18204,
    18248,18292,18335,18379,18422,18464,18507,18549,18592,18634,18675,18717,18758,18799,18840,18881,18921,18962,19002,19042,
    19081,19121,19160,19200 };
static const int16_t curve_exp[193] = {
    0,40,79,119,158,198,238,279,319,360,401,442,483,525,566,608,651,693,736,778,821,865,908,952,996,1040,1084,1129,1174,1219,
    1264,1310,1356,1402,1448,1495,1542,1589,1636,1684,1732,1780,1829,1877,1927,1976,2026,2076,2126,2176,2227,2279,2330,2382,
    2434,2487,2539,2593,2646,2700,2754,2809,2864,2919,2975,3031,3087,3144,3201,3259,3317,3375,3434,3493,3553,3613,3673,3734,
    3795,3857,3920,3982,4046,4109,4174,4238,4304,4369,4436,4502,4570,4638,4706,4775,4845,4915,4986,5057,5129,5202,5275,5349,
    5423,5498,5574,5651,5728,5806,5885,5965,6045,6126,6208,6291,6374,6459,6544,6630,6717,6805,6894,6984,7074,7166,7259,7353,
    7448,7544,7641,7739,7839,7939,8041,8144,8249,8355,8462,8570,8680,8792,8905,9019,9135,9253,9372,9493,9616,9741,9868,9996,
    10127,10260,10395,10532,10671,10813,10957,11104,11253,11405,11560,11718,11879,12043,12210,12381,12555,12733,12915,13101,
    13291,13486,13685,13890,14099,14314,14534,14761,14993,15233,15479,15733,15995,16266,16545,16834,17134,17445,17768,18103,
    18453,18819,19200 };

static int curve_table(int shape, int i)
{
    switch (shape) {
    case CURVES_LINEAR: return 100 * i;
    case CURVES_EXP: return curve_exp[i];
    case CURVES_LOG: return curve_log[i];
    default: return curve_sine[i];
    }
}

/* 773046e6: the value to send now; moves the curve forward (rt 0 = finished) */
static int32_t curve_value(dm_pmsg *p)
{
    if (p->c_shape != CURVES_LINEAR && p->c_shape != CURVES_EXP && p->c_shape != CURVES_LOG && p->c_shape != CURVES_SINE) {
        if (p->flags & PF_TOOL_FLUSH) { p->rt = 0; return p->c_reset; }
        if ((p->c_flags & CURVEF_RESET) && p->c_resetdur > 0) {
            p->mt = p->c_origstart + p->c_resetdur; p->c_dur = 0; p->flags &= ~PF_REFTIME;
        } else p->rt = 0;
        return p->c_end;
    }
    mtime_t s = p->c_origstart, el = p->mt - s;
    if (p->flags & PF_TOOL_FLUSH) { p->rt = 0; return p->c_reset; }
    int R = p->c_flags & CURVEF_RESET;
    if (R && p->c_resetdur < 0 && p->mt >= p->c_dur + s + p->c_resetdur) { p->rt = 0; return p->c_reset; }
    if (p->c_dur != 0 && el < p->c_dur) {
        int32_t i = (int32_t)(el * 193) / p->c_dur, val;
        if (i < 0) i = 0;
        if (i < 192) val = ((int32_t)p->c_end - (int32_t)p->c_start) * curve_table(p->c_shape, i) / 19200 + p->c_start;
        else val = p->c_end;
        if (R && p->c_resetdur < 0) p->mt = p->c_dur + s + p->c_resetdur;
        else {
            if (p->c_measure == 0) p->c_measure = (uint16_t)curve_step(p);
            p->mt = (mtime_t)((uint32_t)p->c_measure + (uint32_t)p->mt);
        }
        if (p->mt > s + p->c_dur) p->mt = s + p->c_dur;
        p->flags &= ~PF_REFTIME;
        return val;
    }
    if (R) { p->mt = p->c_resetdur + p->c_dur + s; p->flags &= ~PF_REFTIME; }
    else p->rt = 0;
    return p->c_end;
}

/* 7730488b: the curve's value at elapsed time el (for invalidation) */
static int32_t curve_value_at(const dm_pmsg *p, mtime_t el)
{
    if (p->c_shape != CURVES_LINEAR && p->c_shape != CURVES_EXP && p->c_shape != CURVES_LOG && p->c_shape != CURVES_SINE)
        return p->c_end;
    if (p->c_dur == 0 || p->c_dur <= el) return p->c_end;
    int32_t i = (el * 193) / p->c_dur;
    if (i < 0) i = 0;
    if (i < 192) return ((int32_t)p->c_end - p->c_start) * curve_table(p->c_shape, i) / 19200 + p->c_start;
    return p->c_end;
}

/* 77307197 */
static int note_out(dm_perf *pf, dm_pmsg *p, rtime_t rt)
{
    uint32_t pi, group, chan;
    int16_t transpose;
    if (map_pch(pf, p->pch, &pi, &group, &chan, &transpose) < 0) return DMUS_S_FREE;
    if (pi > (uint32_t)pf->nports || pi >= (uint32_t)pf->nports) return DMUS_S_FREE;
    port *po = &pf->ports[pi];
    int on = p->n_flags & NOTEF_NOTEON;
    uint32_t msg;
    if (!on) {
        if (rt < p->rtlast) rt = p->rtlast + 10000;
        msg = (uint32_t)p->n_midival << 8 | chan | 0x80;
    } else {
        int16_t n = (int16_t)(p->n_midival + transpose);
        if (n > 127 || n < 0 || p->n_dur < 1) return DMUS_S_FREE;
        p->n_midival = (uint8_t)n;
        msg = (uint32_t)p->n_vel << 16 | (uint32_t)n << 8 | chan | 0x90;
    }
    if (!pack(po, msg, rt - 2, group)) return DMUS_S_FREE;
    po->dirty = 1;
    rtime_t t = rt;
    if (p->n_offset != 0 && on) { rtime_t t2 = M2R(pf, p->mt - p->n_offset + 1); if (t2 > t) t = t2; }
    if (t > po->lastpacked) po->lastpacked = t;
    if (!on) return DMUS_S_FREE;
    p->rtlast = rt;
    pf->lastnoteon = rt;
    if (!(p->flags & PF_LOCKTOREFTIME)) {
        p->mt += p->n_dur;
        if (p->n_dur > 1) p->mt -= 1;
        rtime_t r = M2R(pf, p->mt);
        if (r < p->rt + 2) r = p->rt + 2;
        p->rt = r - 1;
    } else {
        rtime_t old = p->rt;
        p->rt += (int64_t)(int32_t)(p->n_dur * 10000);
        if (p->n_dur > 1) p->rt -= 10000;
        if (p->rt < old + 1) p->rt = old + 1;
        p->flags &= ~PF_MUSICTIME;
    }
    p->n_flags &= ~NOTEF_NOTEON;
    return DMUS_S_REQUEUE;
}

/* 7730bb58 */
static int midi_out(dm_perf *pf, dm_pmsg *p, rtime_t rt)
{
    pchentry *e = pch_lookup(pf, p->pch);
    if (!e || !e->used) return DMUS_S_FREE;
    port *po = e->port < (uint32_t)pf->nports ? &pf->ports[e->port] : NULL;
    p->m_status &= 0xf0;
    if (p->m_status == 0xb0) {
        switch (p->m_b1) {
        case 7: p->m_b2 = (uint8_t)mg_vol(&e->m[MG_VOL], 0, p->m_b2); break;
        case 11: p->m_b2 = (uint8_t)mg_vol(&e->m[MG_EXPR], 0, p->m_b2); break;
        case 10: p->m_b2 = (uint8_t)mg_off(&e->m[MG_PAN], 0, p->m_b2, 0x40, 0x7f); break;
        case 74: p->m_b2 = (uint8_t)mg_off(&e->m[MG_CC74], 0, p->m_b2, 0x40, 0x7f); break;
        case 1: p->m_b2 = (uint8_t)mg_off(&e->m[MG_MOD], 0, p->m_b2, 0x7f, 0x7f); break;
        case 91: p->m_b2 = (uint8_t)mg_off(&e->m[MG_REV], 0, p->m_b2, 0x7f, 0x7f); break;
        case 93: p->m_b2 = (uint8_t)mg_off(&e->m[MG_CHORUS], 0, p->m_b2, 0x7f, 0x7f); break;
        case 121: mg_resetall(e, p->m_b2); break;
        }
    } else if (p->m_status == 0xe0) {
        int32_t v = mg_off(&e->m[MG_PB], 0, (p->m_b2 << 7) | p->m_b1, 0x2000, 0x3fff);
        p->m_b1 = v & 0x7f; p->m_b2 = (v >> 7) & 0x7f;
    }
    uint32_t msg = ((uint32_t)p->m_b1 << 8 | (uint32_t)p->m_b2 << 16) | e->chan | p->m_status;
    if (po && pack(po, msg, rt, e->group)) { po->dirty = 1; po->lastpacked = rt; }
    return DMUS_S_FREE;
}

/* 773074ab */
static int sysex_out(dm_perf *pf, dm_pmsg *p, rtime_t rt)
{
    uint32_t pi, group, chan;
    if (map_pch(pf, p->pch, &pi, &group, &chan, NULL) < 0 || pi >= (uint32_t)pf->nports) return DMUS_S_FREE;
    port *po = &pf->ports[pi];
    dm_event ev = { rt - 4, group, p->sx_len, p->sx_data };
    if (po->ops.play && p->sx_data) po->ops.play(po->ctx, &ev, 1);
    return DMUS_S_FREE;
}

/* 773075e8 */
static int patch_out(dm_perf *pf, dm_pmsg *p, rtime_t rt)
{
    uint32_t pi, group, chan;
    if (map_pch(pf, p->pch, &pi, &group, &chan, NULL) < 0 || pi >= (uint32_t)pf->nports) return DMUS_S_FREE;
    port *po = &pf->ports[pi];
    pack(po, 0xb0 | chan | 0x20 << 8 | (uint32_t)p->m_b2 << 16, rt - 12, group);
    pack(po, 0xb0 | chan | (uint32_t)p->m_b1 << 16, rt - 11, group);
    if (pack(po, 0xc0 | chan | (uint32_t)p->m_status << 8, rt - 10, group)) { po->dirty = 1; po->lastpacked = rt - 10; }
    return DMUS_S_FREE;
}

/* 7730b5b8 */
static int curve_out(dm_perf *pf, dm_pmsg *p, rtime_t rt)
{
    for (;;) {
        int fromcur = 0, ret = DMUS_S_FREE;
        if (p->c_origstart == 0) {
            if (p->flags & PF_TOOL_FLUSH) return DMUS_S_FREE;
            if (p->flags & PF_LOCKTOREFTIME) {
                mtime_t x = R2M(pf, p->rt + (int64_t)(int32_t)(p->c_dur * 10000));
                p->c_dur = x - p->mt;
                x = R2M(pf, p->rt + (int64_t)(int32_t)(p->c_resetdur * 10000));
                p->flags &= ~PF_LOCKTOREFTIME;
                p->c_resetdur = x - p->mt;
            }
            p->c_origstart = p->mt;
            if (p->c_shape != CURVES_INSTANT) {
                mtime_t mtlat = R2M(pf, dm_perf_latency_time(pf));
                if (p->mt < mtlat) p->mt = (p->mt + p->c_dur < mtlat) ? p->mt + p->c_dur : mtlat;
                if (p->c_flags & CURVEF_START_FROM_CURRENT) fromcur = 1;
                else p->c_measure = (uint16_t)curve_step(p);
            }
        }
        if ((p->c_flags & CURVEF_RESET) && p->c_resetdur >= 0 &&
            p->mt == p->c_dur + p->c_resetdur + p->c_origstart && !(p->flags & PF_TOOL_FLUSH)) {
            if (!(p->hflags & HF_ARMED)) return DMUS_S_FREE;
            p->hflags &= ~HF_ARMED;
            p->flags |= PF_TOOL_FLUSH;
            if (rt <= p->rtlast) rt = p->rtlast + 10000;
            continue;
        }
        pchentry *e = pch_lookup(pf, p->pch);
        if (!e || !e->used || e->port >= (uint32_t)pf->nports) return DMUS_S_FREE;
        port *po = &pf->ports[e->port];
        uint32_t idx = (p->flags & PF_DX8) ? p->c_merge : 0, msg = 0;
        int32_t v;
        switch (p->c_type) {
        case CURVET_PB:
            if (fromcur) p->c_start = (int16_t)(mg_get(&e->m[MG_PB], idx) + 0x2000);
            v = mg_off(&e->m[MG_PB], idx, curve_value(p), 0x2000, 0x3fff);
            msg = 0xe0 | (v & 0x7f) << 8 | (v & 0x3f80) << 9;
            break;
        case CURVET_CC: {
            int mi = -1, center = 0x7f;
            switch (p->c_cc) {
            case 74: mi = MG_CC74; center = 0x40; break;
            case 1: mi = MG_MOD; break;
            case 10: mi = MG_PAN; center = 0x40; break;
            case 91: mi = MG_REV; break;
            case 93: mi = MG_CHORUS; break;
            }
            if (mi >= 0) {
                if (fromcur) p->c_start = (int16_t)(mg_get(&e->m[mi], idx) + center);
                v = mg_off(&e->m[mi], idx, curve_value(p), center, 0x7f);
            } else if (p->c_cc == 7 || p->c_cc == 11) {
                merger *m = &e->m[p->c_cc == 7 ? MG_VOL : MG_EXPR];
                if (fromcur) p->c_start = (int16_t)(uint16_t)db_to_vol(mg_get(m, idx));
                v = mg_vol(m, idx, (uint8_t)curve_value(p)) & 0xff;
            } else if (p->c_cc == 121) {
                v = curve_value(p);
                mg_resetall(e, p->c_end);
            } else v = curve_value(p);
            msg = (((uint32_t)v << 8 | p->c_cc) << 8) | 0xb0;
            break;
        }
        case CURVET_MAT: msg = (uint32_t)curve_value(p) << 8 | 0xd0; break;
        case CURVET_PAT: msg = (((uint32_t)curve_value(p) << 8 | p->c_cc) << 8) | 0xa0; break;
        case CURVET_RPN: case CURVET_NRPN: {
            if (!(p->flags & PF_DX8)) return DMUS_S_FREE;
            v = curve_value(p);
            uint32_t msb = p->c_type == CURVET_RPN ? 101 : 99, lsb = p->c_type == CURVET_RPN ? 100 : 98, c = e->chan;
            pack(po, 0xb0 | c | msb << 8 | ((p->c_param >> 7) & 0x7f) << 16, rt - 3, e->group);
            pack(po, 0xb0 | c | lsb << 8 | (p->c_param & 0x7f) << 16, rt - 2, e->group);
            pack(po, 0xb0 | c | 6 << 8 | ((v >> 7) & 0x7f) << 16, rt - 1, e->group);
            msg = 0xb0 | 38 << 8 | (v & 0x7f) << 16;
            break;
        }
        default: return DMUS_S_FREE;
        }
        if (msg != 0 && pack(po, msg | e->chan, rt, e->group)) {
            po->dirty = 1;
            po->lastpacked = rt;
            if (p->rt != 0) {
                if (fromcur) p->c_measure = (uint16_t)curve_step(p);
                ret = DMUS_S_REQUEUE;
            }
        }
        return ret;
    }
}

/* 7730e5b7: the performance's tool ProcessPMsg */
static int process_pmsg(dm_perf *pf, dm_pmsg *p)
{
    rtime_t rt = p->rt;
    if (pf->horizon > rt + 50000000) {
        switch (p->type) {
        case PT_NOTIFICATION:   /* late: only MUSICSTOPPED and SEGEND of these two kinds still go out */
            if (guid_same(&p->nf_guid, &GUID_NOTIFICATION_PERFORMANCE_) && p->nf_option != NOTIF_MUSICSTOPPED) return DMUS_S_FREE;
            if (guid_same(&p->nf_guid, &GUID_NOTIFICATION_SEGMENT_) && p->nf_option != NOTIF_SEGEND) return DMUS_S_FREE;
            break;
        case PT_NOTE: if (p->n_flags & NOTEF_NOTEON) return DMUS_S_FREE; break;
        case PT_WAVE: if (!(p->w_flags & WAVEF_OFF)) return DMUS_S_FREE; break;
        default: return DMUS_S_FREE;
        }
    }
    if (p->type == PT_TEMPO) {
        if (!(p->hflags & HF_INPLAY)) tempo_pmsg(pf, p);
        return DMUS_S_FREE;
    }
    if (p->pch == PCH_BROADCAST_GROUPS || p->pch == PCH_BROADCAST_PERFORMANCE) {
        int n = p->pch == PCH_BROADCAST_GROUPS ? 1 : 16;
        for (pchblock *b = pf->blocks; b; b = b->next)
            for (int i = 0; i < n; i++) {
                pchentry *e = &b->e[i];
                if (e->group && (e->flags & 6) && !(p->type == PT_TRANSPOSE && ((b->base + i) & 0xf) == 9)) {
                    dm_pmsg *c = pm_clone(pf, p);
                    if (c) { c->pch = b->base + i; if (send_pmsg(pf, c) < 0) pm_free(pf, c); }
                }
            }
        return DMUS_S_FREE;
    }
    if (p->type == PT_TRANSPOSE) {
        if (p->flags & PF_TOOL_QUEUE) {
            pchentry *e = pch_lookup(pf, p->pch);
            if (e) e->transpose = (int16_t)mg_merge(&e->m[MG_TRANSPOSE], (p->flags & PF_DX8) ? p->t_merge : 0, p->t_transpose);
            return DMUS_S_FREE;
        }
        p->flags = (p->flags & ~0x14u) | PF_TOOL_QUEUE;
        return DMUS_S_REQUEUE;
    }
    if (p->type == PT_NOTIFICATION && guid_same(&p->nf_guid, &GUID_NOTIFICATION_PRIVATE_CHORD_)) {
        /* 77309f00: a chord changed: queued notes of the state's track regenerated (not yet: the style tracks
         * convert their notes when they make them) */
        return DMUS_S_FREE;
    }
    if (p->type == PT_NOTIFICATION) {
        if (!(p->flags & PF_TOOL_ATTIME)) { p->flags = (p->flags & ~0xcu) | PF_TOOL_ATTIME; return DMUS_S_REQUEUE; }
        p->tool = 0;
        q_insert(&pf->notif, p);
        if (pf->notify_cb) pf->notify_cb(pf->notify_ctx);
        return DM_OK;
    }
    if (p->type == PT_TIMESIG) {
        p->tool = 0;
        if (p->ts_grids == 0 || p->ts_bpm == 0 || p->ts_beat == 0 || (p->ts_beat % 2) != 0) return DMUS_S_FREE;
        rtime_t t = clock_now(pf) - 10000000;
        dm_pmsg *x;
        while ((x = q_pop_stale(&pf->tsq, t))) pm_free(pf, x);
        q_insert(&pf->tsq, p);
        return DM_OK;
    }
    if (!(p->flags & PF_TOOL_IMMEDIATE)) {
        switch (p->type) {
        case PT_MIDI: return midi_out(pf, p, rt);
        case PT_NOTE: return note_out(pf, p, rt);
        case PT_SYSEX: return sysex_out(pf, p, rt);
        case PT_CURVE: return curve_out(pf, p, rt);
        case PT_PATCH: return patch_out(pf, p, rt);
        case PT_WAVE: return wave_out(pf, p, rt);
        case PT_CHANNEL_PRIORITY: {
            uint32_t pi, group, chan;
            if (map_pch(pf, p->pch, &pi, &group, &chan, NULL) >= 0 && pi < (uint32_t)pf->nports && pf->ports[pi].ops.priority)
                pf->ports[pi].ops.priority(pf->ports[pi].ctx, group, chan, p->priority);
            return DMUS_S_FREE;
        }
        default: return DMUS_S_FREE;
        }
    }
    if (p->type == PT_STOP) {
        dm_segstate *s = p->user;
        if (s && p->mt > pf->prepared) s->mtstop = p->mt;
    }
    p->flags = (p->flags & ~0x14u) | PF_TOOL_QUEUE;
    return DMUS_S_REQUEUE;
}

/* 7730bd7b: the tool's Flush */
static void tool_flush(dm_perf *pf, dm_pmsg *p, rtime_t rt)
{
    if (p->tool == 2) { if (pf->tops.flush) pf->tops.flush(pf->tctx, p, rt); return; }
    if (p->type == PT_NOTE) { if (!(p->n_flags & NOTEF_NOTEON)) note_out(pf, p, rt); }
    else if (p->type == PT_CURVE) { if (p->c_flags & CURVEF_RESET) curve_out(pf, p, rt); }
    else if (p->type == PT_WAVE) { if (p->w_flags & WAVEF_OFF) wave_out(pf, p, rt); }
}

/* 7730463b */
static int invalidatable(const dm_pmsg *p)
{
    if (p->type == PT_NOTE) return !(p->n_flags & NOTEF_NOINVALIDATE);
    if (p->type == PT_WAVE) return !(p->w_flags & WAVEF_NOINVALIDATE);
    return 1;
}

/* 7730974c: flush (invalidate) a queue from rt cut on: later messages are dropped (pending note-offs and curve resets
 * are sent through the tool's Flush), notes and curves that run past the cut are shortened */
static void flush_queue(dm_perf *pf, uint32_t vid, dm_queue *q, rtime_t cut, rtime_t cut2, uint32_t fl)
{
    (void)dm_perf_queue_time(pf);
    dm_pmsg *x, *nx;
    for (x = q->head; x; x = nx) {
        nx = x->next;
        x->hflags &= ~(HF_REQUEUE | HF_MARK);
        if ((vid && x->vpch != vid) || ((fl & 4) && (x->flags & PF_LOCKTOREFTIME))) continue;
        rtime_t r = x->rt;
        mtime_t s;
        if (x->type == PT_NOTE) {
            if (!(x->n_flags & NOTEF_NOTEON)) {
                if (fl & 1) x->n_flags |= NOTEF_NOINVALIDATE;
            } else r = M2R(pf, x->mt - x->n_offset);
        } else if (x->type == PT_CURVE) {
            if (fl & 1) { r = 0; goto test; }
            s = x->c_origstart ? x->c_origstart : x->mt;
            r = M2R(pf, s - x->c_offset);
            if (r < cut) {
                if (!(x->c_flags & CURVEF_RESET) || x->c_origstart == 0 || cut2 < cut) { r = 0; goto test; }
                mtime_t e = x->c_dur + s;
                r = M2R(pf, e);
                if (cut <= r && !(x->hflags & HF_ARMED)) {
                    mtime_t m = R2M(pf, cut);
                    x->c_resetdur = 1;
                    x->c_dur = m - s - 1;
                    goto test;
                }
                r = M2R(pf, x->c_resetdur + e);
                if (cut <= r) {
                    if (!(x->hflags & HF_ARMED)) x->c_resetdur = R2M(pf, cut) - s - x->c_dur;
                    goto test;
                }
                goto keep;
            }
            goto drop;
        } else if (x->type == PT_WAVE) {       /* 773097ec */
            if ((x->w_flags & WAVEF_IGNORELOOPS) && (fl & 2)) continue;
            if (!(x->w_flags & WAVEF_OFF)) { if (!(x->flags & PF_LOCKTOREFTIME)) r = M2R(pf, x->mt); }
            else if (fl & 1) x->w_flags |= WAVEF_NOINVALIDATE;
        }
test:
        if (r < cut) {
keep:
            if (x->type == PT_NOTE && !(fl & 1)) {
                if ((x->n_flags & NOTEF_NOTEON) && invalidatable(x)) {
                    mtime_t m = x->mt - 2 + x->n_dur;
                    if (cut <= M2R(pf, m)) {
                        mtime_t d = R2M(pf, cut) - x->mt;
                        x->n_dur = d < 1 ? 0 : d;
                    }
                }
                continue;
            }
            if (x->type == PT_CURVE && !(fl & 1)) {
                s = x->c_origstart ? x->c_origstart : x->mt;
                mtime_t d = (x->c_flags & CURVEF_RESET) ? x->c_resetdur + x->c_dur : x->c_dur;
                if (cut <= M2R(pf, d + s)) {
                    mtime_t m = R2M(pf, cut), el = m - s;
                    x->c_end = (int16_t)curve_value_at(x, el);
                    int32_t u = el - 2;
                    if (u > 0) {
                        if (!(x->c_flags & CURVEF_RESET)) x->hflags |= HF_MARK;
                        else {
                            int32_t nd;
                            if (x->c_dur < u) { x->hflags |= HF_ARMED; nd = x->c_dur; x->c_resetdur = m - nd - s; }
                            else { x->c_resetdur = 1; nd = el - 3 < 1 ? 1 : el - 3; x->hflags |= HF_ARMED; }
                            x->rt = cut - 10000;
                            x->mt = m - 2;
                            x->hflags |= HF_REQUEUE;
                            x->c_dur = nd;
                        }
                    }
                }
            }
            continue;
        }
drop:
        if (x->hflags & HF_ARMED) continue;
        if (!x->tool) { x->hflags |= HF_MARK; continue; }
        if (!invalidatable(x)) continue;
        x->hflags |= HF_MARK;
        x->flags |= PF_TOOL_FLUSH;
        tool_flush(pf, x, x->rtlast < cut ? cut : x->rtlast + 10000);   /* hdr+0x10: the time it was last processed */
    }
    for (x = q->head; x; x = nx) {
        nx = x->next;
        if (x->hflags & (HF_REQUEUE | HF_MARK)) {
            x->hflags &= ~HF_MARK;
            if (q_remove(q, x)) {
                if (!(x->hflags & HF_REQUEUE)) pm_free(pf, x);
                else { x->hflags &= ~HF_REQUEUE; q_insert(q, x); }
            }
        }
    }
    flush_ports(pf);
}

/* 7730d1fd: flush the pmsg queues (of one track, or all) from mt on */
static void flush_from(dm_perf *pf, uint32_t vid, mtime_t mt, mtime_t mt2, uint32_t fl)
{
    rtime_t cut = mt ? M2R(pf, mt) : 0, cut2 = (!mt2 || mt2 == mt) ? cut : M2R(pf, mt2);
    flush_queue(pf, vid, &pf->ontime, cut, cut2, fl);
    flush_queue(pf, vid, &pf->nearq, cut, cut2, fl);
    flush_queue(pf, vid, &pf->early, cut, cut2, fl);
    if (vid == 0) {
        mtime_t m = R2M(pf, cut);
        flush_queue(pf, 0, &pf->tmap, cut, cut2, fl);
        tempo_rebuild(pf, NULL, m, 1);
    }
}

/* ------------------------------------------------------------------ the realtime thread (7730ad0b, PickDue 7730928d) */
static dm_pmsg *pick_due(dm_perf *pf)
{
    dm_pmsg *h = pf->ontime.head;
    if (h && h->rt <= clock_now(pf)) { dm_pmsg *r = q_pop(&pf->ontime); if (r) return r; }
    h = pf->nearq.head;
    if (h && h->rt < pf->horizon + (pf->bumper >> 1)) { dm_pmsg *r = q_pop(&pf->nearq); if (r) return r; }
    if (!pf->inside_c7c8 && pf->early.head) return q_pop(&pf->early);
    return NULL;
}

/* a message for an application tool waits for dm_perf_run_app_tools (the tool runs without the engine's lock) */
static void app_defer(dm_perf *pf, dm_pmsg *p)
{
    p->rtlast = p->rt;
    p->next = NULL;
    if (pf->apptail) pf->apptail->next = p; else pf->apphead = p;
    pf->apptail = p;
}

static void run_pmsg(dm_perf *pf, dm_pmsg *p)
{
    if (!p->tool) p->tool = 1;
    if (p->tool == 2) { app_defer(pf, p); return; }
    p->rtlast = p->rt;
    int hr = process_pmsg(pf, p);
    if (hr != DM_OK) {
        if (hr != DMUS_S_REQUEUE || send_pmsg(pf, p) < 0) pm_free(pf, p);
    }
}

/* the realtime and transport threads' ProcessPMsg of an application's tool (77309396 ff.): S_OK, the tool keeps the
 * message; DMUS_S_REQUEUE, it goes back to the queues; anything else frees it */
void dm_perf_run_app_tools(dm_perf *pf)
{
    dm_pmsg *p;
    while ((p = pf->apphead)) {
        pf->apphead = p->next;
        if (!pf->apphead) pf->apptail = NULL;
        p->next = NULL;
        int hr = pf->tops.process ? pf->tops.process(pf->tctx, p) : DMUS_S_FREE;
        if (hr != DM_OK && (hr != DMUS_S_REQUEUE || send_pmsg(pf, p) < 0)) pm_free(pf, p);
    }
}

uint32_t dm_perf_realtime(dm_perf *pf)
{
    pf->xprec = 0;
    uint32_t wake = pf->ops.msec(pf->ctx), nflush = 0;
    rtime_t h = dm_perf_latency_time(pf) + pf->bumper;
    if (h > pf->horizon) pf->horizon = h;
    uint32_t tflush = wake;
    for (;;) {
        uint32_t now = pf->ops.msec(pf->ctx);
        if (now - tflush > pf->period) {
            if (++nflush > 10) break;
            flush_ports(pf);
            tflush = now;
        }
        dm_pmsg *p = pick_due(pf);
        if (!p) break;
        run_pmsg(pf, p);
    }
    flush_ports(pf);
    uint32_t el = pf->ops.msec(pf->ctx) - wake;
    return el + 2 < pf->period ? pf->period - el : 2;
}

/* 7730935d: hand the early queue to the tools (transport thread, after every piece) */
static void run_early(dm_perf *pf)
{
    dm_pmsg *p;
    while ((p = q_pop(&pf->early))) {
        if (!p->tool) {
            p->tool = 1;
            p->flags = (p->flags & ~0x14u) | PF_TOOL_QUEUE;
            if (send_pmsg(pf, p) < 0) pm_free(pf, p);
        } else if (p->tool == 2) {
            app_defer(pf, p);
        } else {
            p->rtlast = p->rt;
            int hr = process_pmsg(pf, p);
            if (hr != DM_OK && (hr != DMUS_S_REQUEUE || send_pmsg(pf, p) < 0)) pm_free(pf, p);
        }
    }
}

/* ------------------------------------------------------------------ notifications */
static int perf_has_ntype(dm_perf *pf, const dm_guid *g)    /* 77305d75 */
{
    for (int i = 0; i < pf->nntypes; i++) if (guid_same(&pf->ntypes[i], g)) return 1;
    return 0;
}

/* 7730a5d3 AddNotificationType: also to the segment of every state (77305ca3) */
int dm_perf_add_notification_type(dm_perf *pf, const dm_guid *g)
{
    if (perf_has_ntype(pf, g)) return DM_FALSE;
    dm_guid *n = realloc(pf->ntypes, (pf->nntypes + 1) * sizeof(*n));
    if (!n) return DM_E_OUTOFMEMORY;
    pf->ntypes = n;
    pf->ntypes[pf->nntypes++] = *g;
    for (int k = 0; k < 9; k++)
        for (dm_segstate *s = pf->lists[k]; s; s = s->lnext) dm_segment_add_notification(s->seg, g, 1);
    return DM_OK;
}

/* 77305da6 RemoveNotificationType (GUID_NULL: all); the segments lose the type too (77305d0c) */
int dm_perf_remove_notification_type(dm_perf *pf, const dm_guid *g)
{
    static const dm_guid null = { 0 };
    if (guid_same(g, &null)) {
        while (pf->nntypes) {
            dm_guid t = pf->ntypes[0];
            memmove(pf->ntypes, pf->ntypes + 1, (pf->nntypes - 1) * sizeof(*pf->ntypes));
            pf->nntypes--;
            for (int k = 0; k < 9; k++)
                for (dm_segstate *s = pf->lists[k]; s; s = s->lnext) dm_segment_remove_notification(s->seg, &t, 1);
        }
        return DM_OK;
    }
    int i;
    for (i = 0; i < pf->nntypes && !guid_same(&pf->ntypes[i], g); i++) ;
    if (i == pf->nntypes) return DM_FALSE;
    for (int k = 0; k < 9; k++)
        for (dm_segstate *s = pf->lists[k]; s; s = s->lnext) dm_segment_remove_notification(s->seg, g, 1);
    memmove(&pf->ntypes[i], &pf->ntypes[i + 1], (pf->nntypes - i - 1) * sizeof(*pf->ntypes));
    pf->nntypes--;
    return DM_OK;
}

/* 77305bf6 SetNotificationHandle: cb is called (SetEvent) whenever a notification is delivered; rtmin 0 keeps the
 * current retention (default 2 s) */
void dm_perf_set_notification_handle(dm_perf *pf, void (*cb)(void *ctx), void *ctx, rtime_t rtmin)
{
    pf->notify_cb = cb; pf->notify_ctx = ctx;
    if (rtmin) pf->notif_min = rtmin;
}

/* 77305c42 GetNotificationPMsg: the oldest delivered notification (the caller frees it), NULL if none */
dm_pmsg *dm_perf_get_notification(dm_perf *pf) { return q_pop(&pf->notif); }
dm_pmsg *dm_perf_alloc_pmsg(dm_perf *pf, uint32_t size) { return pm_alloc(pf, size); }
int dm_perf_send_pmsg(dm_perf *pf, dm_pmsg *p)
{
    int hr = send_pmsg(pf, p);
    if (hr < 0) pm_free(pf, p);
    return hr;
}

int dm_perf_free_pmsg(dm_perf *pf, dm_pmsg *p) { return p ? pm_free(pf, p) : DM_E_POINTER; }
int dm_perf_send_pmsg_keep(dm_perf *pf, dm_pmsg *p) { return send_pmsg(pf, p); }
dm_pmsg *dm_perf_clone_pmsg(dm_perf *pf, const dm_pmsg *p) { return pm_clone(pf, p); }

/* ------------------------------------------------------------------ tool graphs */
void dm_perf_set_toolops(dm_perf *pf, const dm_toolops *ops, void *ctx)
{
    if (ops) pf->tops = *ops; else memset(&pf->tops, 0, sizeof(pf->tops));
    pf->tctx = ctx;
}
static void graph_drop(dm_perf *pf, void *g) { if (g && pf->tops.graph_release) pf->tops.graph_release(pf->tctx, g); }
void dm_perf_set_graph(dm_perf *pf, void *graph) { void *o = pf->graph; pf->graph = graph; graph_drop(pf, o); }
void *dm_perf_get_graph(dm_perf *pf) { return pf->graph; }
void dm_segstate_set_graph(dm_segstate *ss, void *graph) { void *o = ss->graph; ss->graph = graph; graph_drop(ss->perf, o); }
void *dm_segstate_get_graph(dm_segstate *ss) { return ss->graph; }
dm_audiopath *dm_segstate_audiopath(dm_segstate *ss) { return ss->audiopath; }
void dm_audiopath_set_graph(dm_audiopath *ap, void *graph) { void *o = ap->graph; ap->graph = graph; graph_drop(ap->pf, o); }
void *dm_audiopath_get_graph(dm_audiopath *ap) { return ap->graph; }
void **dm_audiopath_wrapper(dm_audiopath *ap) { return &ap->wrapper; }
/* CloseDown: the messages the application has seen go now (they hold its objects) */
void dm_perf_drop_app_messages(dm_perf *pf)
{
    dm_queue *qs[] = { &pf->early, &pf->nearq, &pf->ontime, &pf->notif };
    for (int k = 0; k < 4; k++)
        for (dm_pmsg *p = qs[k]->head, *nx; p; p = nx) {
            nx = p->next;
            if (p->ext && q_remove(qs[k], p)) pm_free(pf, p);
        }
    while (pf->apphead) { dm_pmsg *p = pf->apphead; pf->apphead = p->next; pm_free(pf, p); }
    pf->apptail = NULL;
}
int dm_perf_stamp(dm_perf *pf, dm_pmsg *p) { return perf_stamp(pf, p); }
int dm_segstate_stamp(dm_segstate *ss, dm_pmsg *p) { return ss_stamp(ss->perf, ss, p); }
int dm_audiopath_stamp(dm_audiopath *ap, dm_pmsg *p) { return ap_stamp(ap->pf, ap, p); }
int dm_perf_output_process(dm_perf *pf, dm_pmsg *p) { p->rtlast = p->rt; return process_pmsg(pf, p); }
void dm_perf_output_flush(dm_perf *pf, dm_pmsg *p, rtime_t rt) { int t = p->tool; p->tool = 1; tool_flush(pf, p, rt); p->tool = t; }

/* 77309199: a GUID_NOTIFICATION_PERFORMANCE message at mt (through the performance's graph) */
static void perf_notify(dm_perf *pf, uint32_t option, mtime_t mt, dm_segstate *ss)
{
    if (!perf_has_ntype(pf, &GUID_NOTIFICATION_PERFORMANCE_)) return;
    dm_pmsg *m = pm_alloc(pf, 0x58);
    if (!m) return;
    m->type = PT_NOTIFICATION; m->mt = mt; m->flags = PF_MUSICTIME | PF_TOOL_ATTIME; m->group = 0xffffffff;
    m->nf_guid = GUID_NOTIFICATION_PERFORMANCE_; m->nf_option = option;
    if (ss) { m->user = ss; m->userref = 1; ss->refs++; }
    perf_stamp(pf, m);
    if (send_pmsg(pf, m) < 0) pm_free(pf, m);
}

/* 77315748: a GUID_NOTIFICATION_SEGMENT message at mt, if the state's segment has the type (through its graph) */
static void ss_notify(dm_perf *pf, dm_segstate *ss, uint32_t option, mtime_t mt)
{
    if (!dm_segment_has_notification(ss->seg, &GUID_NOTIFICATION_SEGMENT_)) return;
    dm_pmsg *m = pm_alloc(pf, 0x58);
    if (!m) return;
    m->type = PT_NOTIFICATION; m->mt = mt; m->flags = PF_MUSICTIME | PF_TOOL_ATTIME; m->group = 0xffffffff;
    m->nf_guid = GUID_NOTIFICATION_SEGMENT_; m->nf_option = option;
    m->user = ss; m->userref = 1; ss->refs++;
    stamp_send(pf, ss, m);
}

/* 77315831: a DMUS_PMSGT_DIRTY message at mt through the state's graph (what tools hold is out of date), after the
 * start, the end and an abort; not from a secondary segment that is not a control one */
static void ss_dirty(dm_perf *pf, dm_segstate *ss, mtime_t mt)
{
    if ((ss->flags & SEGF_SECONDARY) && !(ss->flags & SEGF_CONTROL)) return;
    dm_pmsg *m = pm_alloc(pf, 0x38);
    if (!m) return;
    m->type = PT_DIRTY; m->mt = mt; m->flags = PF_MUSICTIME | PF_TOOL_IMMEDIATE; m->group = 0xffffffff;
    stamp_send(pf, ss, m);
}

/* 77309fcc: a pending MUSICSTOPPED goes when a segment is played */
static void drop_musicstopped(dm_perf *pf)
{
    for (dm_pmsg *p = pf->ontime.head, *nx; p; p = nx) {
        nx = p->next;
        if (p->type == PT_NOTIFICATION && guid_same(&p->nf_guid, &GUID_NOTIFICATION_PERFORMANCE_) && p->nf_option == NOTIF_MUSICSTOPPED) {
            if (q_remove(&pf->ontime, p)) pm_free(pf, p);
            pf->stopped = 0;
        }
    }
}

#include "dmtrack.c"
#include "dmstate.c"


/* ------------------------------------------------------------------ audiopaths */
/* the audiopaths' PChannels (7730b361 / 7730b2e2): blocks of 16 on port 0, each a channel group; a channel taken in
 * the order of a table (all, all but 10 when drums are on 10, or 10 itself); a new block above every block there is */
typedef struct apblock { struct apblock *next; uint32_t base, group; uint16_t used; } apblock;
static const uint8_t s_order_all[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static const uint8_t s_order_nodrum[15] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15 };
static const uint8_t s_order_drum[1] = { 9 };
static int ap_alloc(dm_perf *pf, int drum, uint32_t *real)
{
    const uint8_t *t = drum & 1 ? (drum & 2 ? s_order_drum : s_order_nodrum) : s_order_all;
    int nt = drum & 1 ? (drum & 2 ? 1 : 15) : 16;
    apblock *b;
    for (b = pf->apblocks; b; b = b->next)
        for (int i = 0; i < nt; i++)
            if (!(b->used & (1u << t[i]))) { b->used |= (uint16_t)(1u << t[i]); *real = b->base + t[i]; goto got; }
    b = calloc(1, sizeof(*b));
    if (!b) return DM_E_OUTOFMEMORY;
    {
        int any = 0;
        uint32_t top = 0;
        for (apblock *o = pf->apblocks; o; o = o->next) { if (!any || o->base > top) top = o->base; any = 1; }
        for (pchblock *o = pf->blocks; o; o = o->next) { if (!any || o->base > top) top = o->base; any = 1; }
        b->base = any ? top + 16 : 0;
    }
    b->group = b->base / 16 + 1;
    {
        apblock **q = &pf->apblocks;      /* in the order they were made (7730262a appends) */
        while (*q) q = &(*q)->next;
        *q = b;
    }
    b->used = (uint16_t)(1u << t[0]);
    *real = b->base + t[0];
got:
    pf->nextpch++;
    dm_perf_assign_pchannel(pf, *real, 0, *real / 16 + 1, *real % 16);
    return DM_OK;
}
static void ap_free(dm_perf *pf, uint32_t real)
{
    for (apblock *b = pf->apblocks; b; b = b->next)
        if (b->base <= real && real < b->base + 16) { b->used &= (uint16_t)~(1u << (real - b->base)); return; }
}

/* an audiopath: n virtual PChannels, each on a new performance PChannel of port 0 (drum[]: 1 drums on 10, 3 this is
 * the drum channel), its channel on the buses given (nbus[i] of them, buses + 32 * i; none: not assigned) */
dm_audiopath *dm_perf_create_audiopath_ex(dm_perf *pf, int activate, uint32_t n, const uint32_t *virt, const int *drum,
                                          const uint32_t *nbus, const uint32_t *buses)
{
    if (!pf->nports) return NULL;
    dm_audiopath *ap = calloc(1, sizeof(*ap));
    if (!ap) return NULL;
    ap->refs = 1; ap->pf = pf; ap->vid = ++g_trackid; ap->active = activate;
    ap->virt = malloc((n + 1) * sizeof(uint32_t)); ap->real = malloc((n + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r;
        if (ap_alloc(pf, drum ? drum[i] : 0, &r) < 0) break;
        ap->virt[ap->n] = virt[i]; ap->real[ap->n] = r; ap->n++;
        if (nbus && nbus[i] && pf->ports[0].ops.assign) pf->ports[0].ops.assign(pf->ports[0].ctx, r / 16 + 1, r % 16, buses + 32 * i, nbus[i]);
    }
    ap->next = pf->paths; pf->paths = ap;
    return ap;
}

/* a standard audiopath: npch virtual PChannels 0..npch-1, all on the same buses */
dm_audiopath *dm_perf_create_audiopath(dm_perf *pf, uint32_t npch, int activate, const uint32_t *buses, uint32_t nb)
{
    uint32_t *virt = malloc((npch + 1) * sizeof(uint32_t)), *nbus = malloc((npch + 1) * sizeof(uint32_t));
    uint32_t *bl = calloc(npch + 1, 32 * sizeof(uint32_t));
    dm_audiopath *ap = NULL;
    if (virt && nbus && bl) {
        for (uint32_t i = 0; i < npch; i++) {
            virt[i] = i;
            nbus[i] = buses ? (nb > 32 ? 32 : nb) : 0;
            if (buses) memcpy(bl + 32 * i, buses, nbus[i] * sizeof(uint32_t));
        }
        ap = dm_perf_create_audiopath_ex(pf, activate, npch, virt, NULL, nbus, bl);
    }
    free(virt); free(nbus); free(bl);
    return ap;
}

void dm_audiopath_addref(dm_audiopath *ap) { ap->refs++; }
void dm_audiopath_release(dm_audiopath *ap)
{
    if (--ap->refs > 0) return;
    dm_perf *pf = ap->pf;
    for (dm_audiopath **pp = &pf->paths; *pp; pp = &(*pp)->next) if (*pp == ap) { *pp = ap->next; break; }
    if (pf->defpath == ap) pf->defpath = NULL;
    for (uint32_t i = 0; i < ap->n; i++) ap_free(pf, ap->real[i]);     /* 77320eba */
    graph_drop(pf, ap->graph);
    free(ap->virt); free(ap->real); free(ap);
}

void dm_perf_set_default_audiopath(dm_perf *pf, dm_audiopath *ap) { pf->defpath = ap; }
dm_audiopath *dm_perf_get_default_audiopath(dm_perf *pf) { return pf->defpath; }
uint32_t dm_perf_pchannels(dm_perf *pf) { return pf->nextpch; }
/* the channel groups port 0 has: the highest one a PChannel is on */
uint32_t dm_perf_port_groups(dm_perf *pf)
{
    uint32_t g = 0;
    for (pchblock *b = pf->blocks; b; b = b->next)
        for (int i = 0; i < 16; i++) if (b->e[i].used && b->e[i].port == 0 && b->e[i].group > g) g = b->e[i].group;
    return g;
}

int dm_audiopath_convert(dm_audiopath *ap, uint32_t pch, uint32_t *out)
{
    for (uint32_t i = 0; i < ap->n; i++) if (ap->virt[i] == pch) { *out = ap->real[i]; return DM_OK; }
    return DM_E_INVALIDARG;
}

/* 7731ee26 */
int dm_audiopath_activate(dm_audiopath *ap, int on)
{
    if (!!on == ap->active) return DM_FALSE;
    ap->active = !!on;
    if (!on) {
        dm_perf *pf = ap->pf;
        pf->xprec = pf->appxprec;
        for (int k = 0; k < 9; k++)
            for (dm_segstate *s = pf->lists[k], *nx; s; s = nx) {
                nx = s->lnext;
                if (s->audiopath == ap && s->canstop) dm_perf_stop(pf, NULL, s, 0, 0);
            }
    }
    return DM_OK;
}

/* 7731ec?? AudioPath::SetVolume: a CC7 curve to every PChannel of the path, merge index 0xffff */
int dm_audiopath_set_volume(dm_audiopath *ap, int32_t vol, uint32_t dur)
{
    if (vol < -10000 || vol > 0) return DM_E_INVALIDARG;
    dm_perf *pf = ap->pf;
    pf->xprec = pf->appxprec;
    int32_t v = vol < -9600 ? -9600 : vol;
    int32_t i = -v / 100, t0 = db2vol[i], t1 = db2vol[i + 1];
    uint8_t mv = (uint8_t)((int8_t)(((t1 - t0) * (-v % 100)) / 100) + (int8_t)t0);
    if (dur > 3600000) dur = 3600000;
    dm_pmsg *p = pm_alloc(pf, 0x58);
    if (!p) return DM_E_OUTOFMEMORY;
    rtime_t q = dm_perf_queue_time(pf);
    mtime_t mq = R2M(pf, q);
    if (q < ap->volend) flush_from(pf, ap->vid, mq, mq, 0);
    rtime_t e = (rtime_t)dur * 10000 + q;
    if (ap->volend < e) ap->volend = e;
    p->vpch = ap->vid; p->rt = q; p->mt = mq; p->flags = 0xc3; p->pch = PCH_BROADCAST_AUDIOPATH;
    p->type = PT_CURVE; p->group = 0xffffffff; p->c_dur = (mtime_t)dur; p->c_start = 0x7f; p->c_end = mv;
    p->c_type = CURVET_CC; p->c_shape = dur == 0 ? CURVES_INSTANT : CURVES_LINEAR; p->c_cc = 7;
    p->c_flags = CURVEF_START_FROM_CURRENT; p->c_merge = 0xffff;
    ap_stamp(pf, ap, p);
    int hr = send_pmsg(pf, p);
    if (hr < 0) pm_free(pf, p);
    return hr;
}

/* ------------------------------------------------------------------ construction */
dm_perf *dm_perf_new(const dm_hostops *ops, void *ctx)
{
    dm_perf *pf = calloc(1, sizeof(*pf));
    if (pf) pf->rnd = pf->rndapp = 1;
    if (!pf) return NULL;
    pf->ops = *ops; pf->ctx = ctx;
    pf->bumper = 500000; pf->bumper_ms = 50; pf->prepare_ms = 1000; pf->period = 10;
    pf->master = 1.0f; pf->dxver = 8; pf->stopped = 1;
    pf->notif_min = 20000000;
    pf->rtbase = clock_now(pf);
    pf->horizon = pf->rtbase;
    return pf;
}

int dm_perf_add_port(dm_perf *pf, const dm_portops *ops, void *ctx)
{
    if (pf->nports >= 8) return DM_E_OUTOFMEMORY;
    port *po = &pf->ports[pf->nports];
    memset(po, 0, sizeof(*po));
    po->ops = *ops; po->ctx = ctx;
    return pf->nports++;
}

void dm_perf_set_auto_download(dm_perf *pf, int on) { pf->autodl = on; }
int dm_perf_get_auto_download(dm_perf *pf) { return pf->autodl; }
void dm_perf_set_prepare_time(dm_perf *pf, uint32_t ms) { pf->prepare_ms = ms; }
uint32_t dm_perf_get_prepare_time(dm_perf *pf) { return pf->prepare_ms; }
void dm_perf_set_bumper_length(dm_perf *pf, uint32_t ms) { pf->bumper_ms = ms; pf->bumper = (rtime_t)ms * 10000; }
uint32_t dm_perf_get_bumper_length(dm_perf *pf) { return pf->bumper_ms; }
void dm_perf_set_app_precision(dm_perf *pf, int extended) { pf->appxprec = extended; }
void dm_perf_set_fixes(dm_perf *pf, uint32_t fixes) { pf->fixes = fixes; }
void dm_perf_set_seed(dm_perf *pf, uint32_t seed) { pf->rnd = seed; }
void dm_perf_set_groove(dm_perf *pf, int8_t g) { pf->groove = g; pf->hasgroove = 1; }
int dm_perf_get_groove(dm_perf *pf, int8_t *g) { if (!pf->hasgroove) return 0; *g = pf->groove; return 1; }
/* the application thread's msvcrt rand() state: one for the process (a style track's creation seeds it with
 * time(NULL), 77411f7d / 77412800) */
static uint32_t g_apprnd = 1;
static long (*g_timefn)(void);
uint32_t *dm_app_rand_state(void) { return &g_apprnd; }
void dm_perf_set_app_seed(dm_perf *pf, uint32_t seed) { (void)pf; g_apprnd = seed; }
void dm_set_time_source(long (*fn)(void)) { g_timefn = fn; }
void dm_style_srand(void) { g_apprnd = g_timefn ? (uint32_t)g_timefn() : (uint32_t)time(NULL); }
void (*dm_rand_hook)(int app, int value, void *caller);


static void free_queue(dm_perf *pf, dm_queue *q)
{
    dm_pmsg *p;
    while ((p = q_pop(q))) pm_free(pf, p);
}

void dm_perf_free(dm_perf *pf)
{
    if (!pf) return;
    /* the states still playing go, whatever holds them; held while the messages naming them go first (a
     * notification's state reference) */
    int nss = 0;
    for (int k = 0; k < NLISTS; k++) for (dm_segstate *s = pf->lists[k]; s; s = s->lnext) nss++;
    dm_segstate **ssv = nss ? malloc(nss * sizeof(*ssv)) : NULL;
    nss = 0;
    for (int k = 0; k < NLISTS; k++)
        while (pf->lists[k]) {
            dm_segstate *s = pf->lists[k]; pf->lists[k] = s->lnext;
            if (ssv) { s->refs++; ssv[nss++] = s; } else ss_destroy(s);
        }
    free_queue(pf, &pf->early); free_queue(pf, &pf->nearq); free_queue(pf, &pf->ontime); free_queue(pf, &pf->tmap);
    free_queue(pf, &pf->thist); free_queue(pf, &pf->notif); free_queue(pf, &pf->tsq);
    while (pf->apphead) { dm_pmsg *p = pf->apphead; pf->apphead = p->next; pm_free(pf, p); }
    for (int i = 0; i < nss; i++) ss_destroy(ssv[i]);
    free(ssv);
    while (pf->apblocks) { apblock *b = pf->apblocks; pf->apblocks = b->next; free(b); }
    while (pf->paths) { dm_audiopath *ap = pf->paths; pf->paths = ap->next; graph_drop(pf, ap->graph); free(ap->virt); free(ap->real); free(ap); }
    graph_drop(pf, pf->graph); pf->graph = NULL;
    while (pf->pool) { dm_pmsg *p = pf->pool; pf->pool = p->next; free(p); }
    while (pf->blocks) {
        pchblock *b = pf->blocks; pf->blocks = b->next;
        for (int i = 0; i < 16; i++) for (int k = 0; k < MG_N; k++) mg_reset(&b->e[i].m[k], 0);
        free(b);
    }
    free(pf);
}

/* debugging: the tempo map */
void dm_perf_debug_tmap(dm_perf *pf)
{
    for (dm_pmsg *e = pf->tmap.head; e; e = e->next)
        fprintf(stderr, "tmap mt %d rt %lld tempo %.17g master %.9g adjust %lld\n", e->mt, (long long)e->rt, e->tempo, (double)e->master, (long long)pf->adjust);
}
