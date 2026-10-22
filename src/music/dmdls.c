/* reDM: DLS collections and dmusic-style instrument downloads.  See dmdls.h.
 *
 * What dmusic sends a DirectX 8 synth (from captures of dmusic.dll 5.3.x):
 *  - each wave once per port, as its own DMUS_DOWNLOADTYPE_WAVE buffer (DMUS_WAVE + WAVEFORMATEX + DMUS_WAVEDATA),
 *    with download id = the collection's id base + its pool index;
 *  - the instrument as DMUS_DOWNLOADTYPE_INSTRUMENT2: DMUS_INSTRUMENT, the regions in reverse file order, the
 *    articulations as DMUS_ARTICULATION2 chains of connection lists (DLS1 art1 and DLS2 art2 alike, copied raw);
 *  - a region's rgnh (first 12 bytes), wlnk (ulTableIndex replaced by the wave's download id) and wsmp with its first
 *    loop are copied raw; a region without wsmp gets an all-zero one (it does not inherit the wave's);
 *  - the patch is (ulBank << 8) | (ulBank & 0x80000000) | ulInstrument; GM set instruments have
 *    DMUS_INSTRUMENT_GM_INSTRUMENT.
 * Extension chunks and copyright strings, which the synth ignores, are not sent.  dmusic refuses an instrument that
 * uses the last wave of the pool (DMUS_E_INVALIDOFFSET); that bug is not reproduced. */
#include "dmdls.h"
#include <stdlib.h>
#include <string.h>

#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct { const uint8_t *p; uint32_t n; } blob;

typedef struct {
    uint8_t rgnh[12];
    uint8_t wlnk[12];
    uint8_t wsmp[20];
    uint8_t loop[16];
    blob *arts; int narts;          /* connection blocks (cbSize, cConnections, connections) */
} dls_region;

typedef struct {
    uint32_t patch;
    char name[64];
    dls_region *rgn; int nrgn;      /* file order */
    blob *arts; int narts;
} dls_instr;

typedef struct { uint32_t off; const uint8_t *fmt; uint32_t fmtlen; const uint8_t *data; uint32_t len; } dls_wave;

struct dm_collection {
    volatile long refs;                 /* references (dls_addref / dls_release) */
    uint32_t serial;                    /* unique per collection: what a port's per-collection state is keyed on */
    uint8_t *data;
    dls_instr *ins; int nins;
    uint32_t *cues; int ncues;
    dls_wave *waves; int nwaves;
    int gm;
};

typedef struct { const uint8_t *p, *e; } rd;
static int next_chunk(rd *r, uint32_t *id, rd *data, uint32_t *type)
{
    if (r->e - r->p < 8) return 0;
    uint32_t sz = u32(r->p + 4);
    *id = u32(r->p);
    const uint8_t *d = r->p + 8;
    if ((size_t)(r->e - d) < sz) sz = (uint32_t)(r->e - d);
    data->p = d; data->e = d + sz;
    r->p = d + sz + (sz & 1);
    if (r->p > r->e) r->p = r->e;
    *type = 0;
    if ((*id == FCC('R','I','F','F') || *id == FCC('L','I','S','T')) && sz >= 4) { *type = u32(d); data->p += 4; }
    return 1;
}

static void add_arts(blob **arts, int *n, rd c)
{
    uint32_t id, t; rd x;
    while (next_chunk(&c, &id, &x, &t))
        if ((id == FCC('a','r','t','1') || id == FCC('a','r','t','2')) && x.e - x.p >= 8) {
            uint32_t cb = u32(x.p), nc = u32(x.p + 4);
            size_t len = (size_t)(x.e - x.p);       /* sizes, not pointers: pointer sums wrap in a 32-bit process */
            if (cb < 8 || cb > len || nc > (len - cb) / 12) continue;
            *arts = realloc(*arts, (*n + 1) * sizeof(**arts));
            (*arts)[*n].p = x.p; (*arts)[*n].n = cb + 12 * nc;
            (*n)++;
        }
}

static void load_region(dls_instr *in, rd r)
{
    dls_region g;
    memset(&g, 0, sizeof(g));
    int have_wlnk = 0;
    uint32_t id, t; rd c;
    while (next_chunk(&r, &id, &c, &t)) {
        size_t n = (size_t)(c.e - c.p);
        if (id == FCC('r','g','n','h')) memcpy(g.rgnh, c.p, n < 12 ? n : 12);
        else if (id == FCC('w','l','n','k')) { memcpy(g.wlnk, c.p, n < 12 ? n : 12); have_wlnk = 1; }
        else if (id == FCC('w','s','m','p')) {
            memcpy(g.wsmp, c.p, n < 20 ? n : 20);
            uint32_t cb = u32(g.wsmp);      /* (from the copy: the chunk may be shorter than 4 bytes) */
            if (u32(g.wsmp + 16) && n >= 16 && cb <= n - 16) memcpy(g.loop, c.p + cb, 16);
        } else if (id == FCC('L','I','S','T') && (t == FCC('l','a','r','t') || t == FCC('l','a','r','2')))
            add_arts(&g.arts, &g.narts, c);
    }
    if (!have_wlnk) { free(g.arts); return; }
    in->rgn = realloc(in->rgn, (in->nrgn + 1) * sizeof(*in->rgn));
    in->rgn[in->nrgn++] = g;
}

static void load_instrument(dm_collection *col, rd r)
{
    dls_instr in;
    memset(&in, 0, sizeof(in));
    uint32_t id, t; rd c;
    while (next_chunk(&r, &id, &c, &t)) {
        if (id == FCC('i','n','s','h') && c.e - c.p >= 12) {
            uint32_t bank = u32(c.p + 4), prog = u32(c.p + 8);
            in.patch = (bank << 8) | (bank & 0x80000000u) | prog;
        } else if (id == FCC('L','I','S','T') && t == FCC('l','r','g','n')) {
            rd x; uint32_t xid, xt;
            while (next_chunk(&c, &xid, &x, &xt))
                if (xid == FCC('L','I','S','T') && (xt == FCC('r','g','n',' ') || xt == FCC('r','g','n','2'))) load_region(&in, x);
        } else if (id == FCC('L','I','S','T') && (t == FCC('l','a','r','t') || t == FCC('l','a','r','2')))
            add_arts(&in.arts, &in.narts, c);
        else if (id == FCC('L','I','S','T') && t == FCC('I','N','F','O')) {
            rd x; uint32_t xid, xt;
            while (next_chunk(&c, &xid, &x, &xt))
                if (xid == FCC('I','N','A','M')) {
                    size_t n = (size_t)(x.e - x.p);
                    if (n > 63) n = 63;
                    memcpy(in.name, x.p, n); in.name[n] = 0;
                }
        }
    }
    col->ins = realloc(col->ins, (col->nins + 1) * sizeof(*col->ins));
    col->ins[col->nins++] = in;
}

dm_collection *dls_load(const void *data, size_t size, int gm)
{
    void *copy = malloc(size ? size : 1);
    if (!copy) return NULL;
    memcpy(copy, data, size);
    return dls_load_owned(copy, size, gm);
}

static void coll_destroy(dm_collection *c);

/* as dls_load, taking over a malloc'd buffer (freed with the collection, or now on failure) */
dm_collection *dls_load_owned(void *data, size_t size, int gm)
{
    static volatile long s_serial;
    dm_collection *col = calloc(1, sizeof(*col));
    if (!col) { free(data); return NULL; }
    col->refs = 1;
    col->serial = (uint32_t)__sync_add_and_fetch(&s_serial, 1);
    col->data = data;
    col->gm = gm;
    rd f = { col->data, col->data + size }, r;
    uint32_t id, t;
    if (!next_chunk(&f, &id, &r, &t) || id != FCC('R','I','F','F') || t != FCC('D','L','S',' ')) { coll_destroy(col); return NULL; }
    rd c;
    while (next_chunk(&r, &id, &c, &t)) {
        if (id == FCC('L','I','S','T') && t == FCC('l','i','n','s')) {
            rd x; uint32_t xid, xt;
            while (next_chunk(&c, &xid, &x, &xt)) if (xid == FCC('L','I','S','T') && xt == FCC('i','n','s',' ')) load_instrument(col, x);
        } else if (id == FCC('p','t','b','l') && c.e - c.p >= 8) {
            uint32_t cb = u32(c.p), n = u32(c.p + 4);
            size_t len = (size_t)(c.e - c.p);       /* sizes, not pointers (see add_arts) */
            if (cb <= len && n <= (len - cb) / 4) {
                free(col->cues);
                col->ncues = 0;
                col->cues = malloc((n ? n : 1) * sizeof(uint32_t));
                if (col->cues) {
                    for (uint32_t i = 0; i < n; i++) col->cues[i] = u32(c.p + cb + 4 * i);
                    col->ncues = (int)n;
                }
            }
        } else if (id == FCC('L','I','S','T') && t == FCC('w','v','p','l')) {
            const uint8_t *base = c.p;
            rd x; uint32_t xid, xt;
            while (1) {
                const uint8_t *at = c.p;
                if (!next_chunk(&c, &xid, &x, &xt)) break;
                if (xid != FCC('L','I','S','T') || xt != FCC('w','a','v','e')) continue;
                dls_wave w = { (uint32_t)(at - base), NULL, 0, NULL, 0 };
                rd y; uint32_t yid, yt;
                while (next_chunk(&x, &yid, &y, &yt)) {
                    if (yid == FCC('f','m','t',' ')) { w.fmt = y.p; w.fmtlen = (uint32_t)(y.e - y.p); }
                    else if (yid == FCC('d','a','t','a')) { w.data = y.p; w.len = (uint32_t)(y.e - y.p); }
                }
                col->waves = realloc(col->waves, (col->nwaves + 1) * sizeof(*col->waves));
                col->waves[col->nwaves++] = w;
            }
        }
    }
    return col;
}

static void coll_destroy(dm_collection *c)
{
    if (!c) return;
    for (int i = 0; i < c->nins; i++) {
        for (int k = 0; k < c->ins[i].nrgn; k++) free(c->ins[i].rgn[k].arts);
        free(c->ins[i].rgn); free(c->ins[i].arts);
    }
    free(c->ins); free(c->cues); free(c->waves); free(c->data); free(c);
}

void dls_addref(dm_collection *c) { if (c) __sync_add_and_fetch(&c->refs, 1); }
void dls_release(dm_collection *c)
{
    if (c && __sync_sub_and_fetch(&c->refs, 1) == 0) coll_destroy(c);
}

int dls_count(const dm_collection *c) { return c->nins; }
int dls_enum(const dm_collection *c, int i, uint32_t *patch, const char **name)
{
    if (i < 0 || i >= c->nins) return -1;
    if (patch) *patch = c->ins[i].patch;
    if (name) *name = c->ins[i].name;
    return 0;
}
int dls_find(const dm_collection *c, uint32_t patch)
{
    for (int i = 0; i < c->nins; i++) if (c->ins[i].patch == patch) return i;
    return -1;
}

/* the wave a table index refers to */
static int wave_index(const dm_collection *c, uint32_t table)
{
    if (table >= (uint32_t)c->ncues) return -1;     /* unsigned: a file's index can be 2^31 or more */
    for (int i = 0; i < c->nwaves; i++) if (c->waves[i].off == c->cues[table]) return i;
    return -1;
}

/* ------------------------------------------------------------------ buffer building */
typedef struct { uint8_t *b; uint32_t n, cap; uint32_t *ofs; uint32_t nofs; } bld;

static uint32_t bld_add(bld *x, const void *data, uint32_t len)
{
    uint32_t at = (x->n + 7) & ~7u;
    if (at + len > x->cap) {
        while (at + len > x->cap) x->cap = x->cap ? x->cap * 2 : 256;
        x->b = realloc(x->b, x->cap);
    }
    memset(x->b + x->n, 0, at - x->n);
    if (data) memcpy(x->b + at, data, len); else memset(x->b + at, 0, len);
    x->n = at + len;
    x->ofs = realloc(x->ofs, (x->nofs + 1) * sizeof(uint32_t));
    x->ofs[x->nofs] = at;
    return x->nofs++;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* DMUS_DOWNLOADINFO + offset table + the items */
static uint8_t *bld_finish(bld *x, uint32_t type, uint32_t id, uint32_t *size)
{
    uint32_t hdr = 16 + 4 * x->nofs, total = hdr + x->n;
    uint8_t *out = calloc(1, total);
    put32(out, type); put32(out + 4, id); put32(out + 8, x->nofs); put32(out + 12, total);
    for (uint32_t i = 0; i < x->nofs; i++) put32(out + 16 + 4 * i, x->ofs[i] + hdr);
    memcpy(out + hdr, x->b, x->n);
    free(x->b); free(x->ofs);
    *size = total;
    return out;
}

static uint8_t *build_wave(const dls_wave *w, uint32_t id, uint32_t *size)
{
    bld x = { 0 };
    uint8_t dw[32] = { 0 };     /* DMUS_WAVE: ulFirstExtCkIdx, ulCopyrightIdx, ulWaveDataIdx, WAVEFORMATEX */
    memcpy(dw + 12, w->fmt, w->fmtlen < 18 ? w->fmtlen : 16);
    put32(dw + 8, 1);
    bld_add(&x, dw, 30);
    uint32_t i = bld_add(&x, NULL, 4 + w->len);
    put32(x.b + x.ofs[i], w->len);
    memcpy(x.b + x.ofs[i] + 4, w->data, w->len);
    return bld_finish(&x, 2, id, size);
}

/* DMUS_ARTICULATION2 chain over connection blocks; returns the first index or 0 */
static uint32_t add_artchain(bld *x, const blob *arts, int n)
{
    if (!n) return 0;
    uint32_t *hi = malloc(n * sizeof(uint32_t)), first;
    for (int i = 0; i < n; i++) {
        uint32_t h = bld_add(x, NULL, 12);
        uint32_t l = bld_add(x, arts[i].p, arts[i].n);
        put32(x->b + x->ofs[h], l);
        hi[i] = h;
    }
    for (int i = 0; i + 1 < n; i++) put32(x->b + x->ofs[hi[i]] + 8, hi[i + 1]);
    first = hi[0];
    free(hi);
    return first;
}

static int in_ranges(const uint8_t *rgnh, const uint32_t *ranges, int n)
{
    if (!ranges || !n) return 1;
    uint32_t lo = rgnh[0] | rgnh[1] << 8, hi = rgnh[2] | rgnh[3] << 8;
    for (int i = 0; i < n; i++) if (ranges[2 * i] <= hi && lo <= ranges[2 * i + 1]) return 1;
    return 0;
}

static uint8_t *build_instrument(const dm_collection *c, const dls_instr *in, uint32_t patch, uint32_t id,
                                 const uint32_t *wavid, const uint32_t *ranges, int nranges, uint32_t *size)
{
    bld x = { 0 };
    uint8_t hdr[24] = { 0 };    /* DMUS_INSTRUMENT */
    put32(hdr, patch);
    put32(hdr + 20, c->gm ? 1 : 0);
    bld_add(&x, hdr, 24);
    uint32_t prev = 0;
    for (int k = 0; k < in->nrgn; k++) {           /* each new region heads the list: reverse file order */
        const dls_region *g = &in->rgn[k];
        int wi = wave_index(c, u32(g->wlnk + 8));
        if (wi < 0 || !in_ranges(g->rgnh, ranges, nranges)) continue;
        uint8_t r[72] = { 0 };
        memcpy(r, g->rgnh, 12);
        memcpy(r + 0x18, g->wlnk, 12);
        put32(r + 0x20, wavid[wi]);
        memcpy(r + 0x24, g->wsmp, 20);
        memcpy(r + 0x38, g->loop, 16);
        uint32_t ri = bld_add(&x, r, 72);
        put32(x.b + x.ofs[ri] + 0x10, prev);
        prev = ri;
        uint32_t ai = add_artchain(&x, g->arts, g->narts);
        put32(x.b + x.ofs[ri] + 0xc, ai);
    }
    put32(x.b + x.ofs[0] + 4, prev);
    /* add_artchain can move the buffer: get the index first, then the address */
    uint32_t gi = add_artchain(&x, in->arts, in->narts);
    put32(x.b + x.ofs[0] + 8, gi);
    return bld_finish(&x, 3, id, size);
}

/* ------------------------------------------------------------------ per port state */
/* a collection's ids and wave downloads on a port.  Keyed on the collection's serial, not its address (a freed
 * collection's address can come back as another one); while it has downloads it holds a reference to the collection,
 * and once the last is unloaded it lets go and keeps only the ids dmusic reserved for it */
typedef struct { uint32_t serial; dm_collection *c; uint32_t base; int nwaves; void **wh; int *wref; int ninst; } portcoll;
typedef struct dlinst { uint32_t serial; void *h; int *waves; int nwaves; struct dlinst *next, *prev; } dlinst;

struct dls_port {
    dls_sink sink; void *ctx;
    uint32_t nextid;
    portcoll *pc; int npc;
    dlinst *live;                       /* reAL: the downloads not unloaded (freed with the port) */
};

dls_port *dls_port_new(const dls_sink *sink, void *ctx)
{
    dls_port *p = calloc(1, sizeof(*p));
    if (p) { p->sink = *sink; p->ctx = ctx; }
    return p;
}

void dls_port_free(dls_port *p)
{
    if (!p) return;
    /* (reAL: downloads still out go with the port; their synth goes with it, so nothing is unloaded) */
    for (dlinst *d = p->live, *n; d; d = n) { n = d->next; free(d->waves); free(d); }
    for (int i = 0; i < p->npc; i++) { free(p->pc[i].wh); free(p->pc[i].wref); dls_release(p->pc[i].c); }
    free(p->pc); free(p);
}

static portcoll *port_find(dls_port *p, uint32_t serial)
{
    for (int i = 0; i < p->npc; i++) if (p->pc[i].serial == serial) return &p->pc[i];
    return NULL;
}

/* the collection's state for a download (made on its first; the reference and wave tables taken again if it had let
 * go of them); NULL out of memory */
static portcoll *port_coll(dls_port *p, dm_collection *c)
{
    portcoll *q = port_find(p, c->serial);
    if (!q) {
        portcoll *n = realloc(p->pc, (p->npc + 1) * sizeof(*p->pc));
        if (!n) return NULL;
        p->pc = n;
        q = &p->pc[p->npc++];
        memset(q, 0, sizeof(*q));
        q->serial = c->serial;
        q->base = p->nextid + c->nins;  /* dmusic reserves an id per instrument, then one per wave */
        q->nwaves = c->nwaves;
        p->nextid += c->nins + c->nwaves;
    }
    if (!q->wh) {
        q->wh = calloc(c->nwaves + 1, sizeof(void *));
        q->wref = calloc(c->nwaves + 1, sizeof(int));
        if (!q->wh || !q->wref) { free(q->wh); free(q->wref); q->wh = NULL; q->wref = NULL; return NULL; }
    }
    if (!q->c) { dls_addref(c); q->c = c; }
    return q;
}

/* no downloads left: the collection may go (its wave tables are all zero now) */
static void port_letgo(portcoll *q)
{
    free(q->wh); free(q->wref);
    q->wh = NULL; q->wref = NULL;
    dls_release(q->c);
    q->c = NULL;
}

void *dls_port_download(dls_port *p, dm_collection *c, uint32_t patch, int setpatch, uint32_t newpatch,
                        const uint32_t *ranges, int nranges, int *hr)
{
    int ii = dls_find(c, patch);
    if (ii < 0) { *hr = DMUS_E_NOT_FOUND; return NULL; }
    const dls_instr *in = &c->ins[ii];
    portcoll *q = port_coll(p, c);
    dlinst *d = q ? calloc(1, sizeof(*d)) : NULL;
    uint32_t *wavid = d ? malloc((c->nwaves + 1) * sizeof(uint32_t)) : NULL;
    if (d) d->waves = malloc((in->nrgn + 1) * sizeof(int));
    if (!d || !wavid || !d->waves) {
        if (d) { free(d->waves); free(d); }
        free(wavid);
        if (q && !q->ninst) port_letgo(q);
        *hr = (int32_t)0x8007000E;      /* E_OUTOFMEMORY */
        return NULL;
    }
    d->serial = c->serial;
    d->next = p->live; if (p->live) p->live->prev = d; p->live = d;
    q->ninst++;
    for (int i = 0; i < c->nwaves; i++) wavid[i] = q->base + i;
    /* waves first, in region list order (reverse file order), each once */
    for (int k = in->nrgn - 1; k >= 0; k--) {
        int wi = wave_index(c, u32(in->rgn[k].wlnk + 8));
        if (wi < 0 || !in_ranges(in->rgn[k].rgnh, ranges, nranges)) continue;
        int dup = 0;
        for (int j = 0; j < d->nwaves; j++) if (d->waves[j] == wi) dup = 1;
        if (dup) continue;
        if (!q->wref[wi]) {
            if (!c->waves[wi].fmt || !c->waves[wi].data) continue;
            uint32_t sz;
            uint8_t *b = build_wave(&c->waves[wi], wavid[wi], &sz);
            void *h = NULL;
            int r = p->sink.download(p->ctx, b, sz, &h);
            free(b);
            if (r < 0) continue;
            q->wh[wi] = h;
        }
        q->wref[wi]++;
        d->waves[d->nwaves++] = wi;
    }
    uint32_t sz;
    uint8_t *b = build_instrument(c, in, setpatch ? newpatch : patch, (uint32_t)ii, wavid, ranges, nranges, &sz);
    int r = p->sink.download(p->ctx, b, sz, &d->h);
    free(b); free(wavid);
    if (r < 0) {
        d->h = NULL;
        dls_port_unload(p, d);
        /* dmsynth's codes: -3 = regions without articulation and no instrument articulation */
        *hr = r == -3 ? (int32_t)0x88781129 /* DMUS_E_NOARTICULATION */ : DM_E_FAIL;
        return NULL;
    }
    *hr = DM_OK;
    return d;
}

void dls_port_unload(dls_port *p, void *handle)
{
    dlinst *d = handle;
    if (!d) return;
    if (d->h) p->sink.unload(p->ctx, d->h);
    portcoll *q = port_find(p, d->serial);
    if (q) {
        for (int j = 0; j < d->nwaves; j++) {
            int wi = d->waves[j];
            if (--q->wref[wi] == 0 && q->wh[wi]) { p->sink.unload(p->ctx, q->wh[wi]); q->wh[wi] = NULL; }
        }
        if (--q->ninst == 0) port_letgo(q);
    }
    if (d->prev) d->prev->next = d->next; else if (p->live == d) p->live = d->next;
    if (d->next) d->next->prev = d->prev;
    free(d->waves); free(d);
}
