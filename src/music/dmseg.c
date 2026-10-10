/* reAL-Music: segment file (.sgt, RIFF 'DMSG') loading: the segment header and the sequence, tempo, time signature
 * and band tracks, kept in the forms the original track objects keep them (dmime CSeqTrack/CTempoTrack/CTimeSigTrack
 * Load, dmband CBandTrack/CBand Load).  Other tracks (chord, command, ...) are kept as placeholders for track order. */
#include "dmi.h"
#include <stdlib.h>
#include <string.h>

#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct { const uint8_t *p, *e; } rd;    /* a chunk's data */

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* next chunk of r: id, data (and list type for RIFF/LIST in *type, data after it) */
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
    if (type) {
        *type = 0;
        if ((*id == FCC('R','I','F','F') || *id == FCC('L','I','S','T')) && sz >= 4) { *type = u32(d); data->p += 4; }
    }
    return 1;
}

static const dm_guid CLSID_TempoTrack   = { 0xd2ac2885, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_SeqTrack     = { 0xd2ac2886, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_SysExTrack   = { 0xd2ac2887, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_TimeSigTrack = { 0xd2ac2888, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_BandTrack    = { 0xd2ac2894, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };

static void get_guid(dm_guid *g, const uint8_t *p)
{
    g->d1 = u32(p); g->d2 = u16(p + 4); g->d3 = u16(p + 6); memcpy(g->d4, p + 8, 8);
}
static int guid_eq(const dm_guid *a, const dm_guid *b) { return !memcmp(a, b, sizeof(*a)); }

/* ------------------------------------------------------------------ sequence track (dmime 7731af00) */
static dm_seqpart *seq_part(dm_seqtrack *t, uint32_t pch, int *last)
{
    if (*last >= 0 && t->parts[*last].pch == pch) return &t->parts[*last];
    for (int i = 0; i < t->nparts; i++) if (t->parts[i].pch == pch) { *last = i; return &t->parts[i]; }
    t->parts = realloc(t->parts, (t->nparts + 1) * sizeof(*t->parts));
    t->pchs = realloc(t->pchs, (t->npchs + 1) * sizeof(*t->pchs));
    memset(&t->parts[t->nparts], 0, sizeof(*t->parts));
    t->parts[t->nparts].pch = pch;
    t->pchs[t->npchs++] = pch;
    *last = t->nparts;
    return &t->parts[t->nparts++];
}

static dm_seqtrack *load_seq(rd r)
{
    dm_seqtrack *t = calloc(1, sizeof(*t));
    uint32_t id; rd c;
    int last = -1;
    /* parts are kept here in creation order; the original pushes them at the list head, reversed below */
    while (next_chunk(&r, &id, &c, NULL)) {
        if (id != FCC('e','v','t','l') && id != FCC('c','u','r','l')) continue;
        if (c.e - c.p < 4) continue;
        uint32_t isz = u32(c.p);
        c.p += 4;
        if (!isz) continue;
        int curl = id == FCC('c','u','r','l');
        uint32_t full = curl ? 0x20 : 0x14, rsz = isz < full ? isz : full;
        uint8_t buf[0x20] = { 0 };
        while (c.e - c.p >= (ptrdiff_t)rsz) {
            if (curl) buf[0x1e] = buf[0x1f] = 0;        /* wMergeIndex is cleared before every read */
            memcpy(buf, c.p, rsz);
            c.p += isz;
            if (c.p > c.e) c.p = c.e;
            if (!curl) {
                dm_seqpart *p = seq_part(t, u32(buf + 8), &last);
                p->ev = realloc(p->ev, (p->nev + 1) * sizeof(*p->ev));
                dm_seqitem *e = &p->ev[p->nev++];
                e->mt = (int32_t)u32(buf); e->dur = (int32_t)u32(buf + 4); e->pch = u32(buf + 8);
                e->off = (int16_t)u16(buf + 12); e->status = buf[14]; e->b1 = buf[15]; e->b2 = buf[16];
            } else {
                dm_seqpart *p = seq_part(t, u32(buf + 12), &last);
                p->cv = realloc(p->cv, (p->ncv + 1) * sizeof(*p->cv));
                dm_curveitem *e = &p->cv[p->ncv++];
                e->start = (int32_t)u32(buf); e->dur = (int32_t)u32(buf + 4); e->resetdur = (int32_t)u32(buf + 8);
                e->pch = u32(buf + 12); e->off = (int16_t)u16(buf + 16); e->sval = (int16_t)u16(buf + 18);
                e->eval = (int16_t)u16(buf + 20); e->rval = (int16_t)u16(buf + 22); e->type = buf[24];
                e->shape = buf[25]; e->cc = buf[26]; e->flags = buf[27]; e->param = u16(buf + 28); e->merge = u16(buf + 30);
            }
        }
    }
    /* part list order: newest part first */
    for (int i = 0, j = t->nparts - 1; i < j; i++, j--) { dm_seqpart x = t->parts[i]; t->parts[i] = t->parts[j]; t->parts[j] = x; }
    return t;
}

/* ------------------------------------------------------------------ tempo track (7731c0ea) */
static dm_tempotrack *load_tempo(rd c)
{
    dm_tempotrack *t = calloc(1, sizeof(*t));
    t->active = 1;
    if (c.e - c.p < 4) return t;
    uint32_t isz = u32(c.p);
    c.p += 4;
    if (!isz) return t;
    uint32_t rsz = isz < 16 ? isz : 16;
    uint8_t buf[16] = { 0 };
    while (c.e - c.p >= (ptrdiff_t)rsz) {
        memcpy(buf, c.p, rsz);
        c.p += isz;
        if (c.p > c.e) c.p = c.e;
        t->items = realloc(t->items, (t->n + 1) * sizeof(*t->items));
        t->items[t->n].mt = (int32_t)u32(buf);
        memcpy(&t->items[t->n].tempo, buf + 8, 8);
        t->n++;
    }
    return t;
}

/* ------------------------------------------------------------------ time signature track (7731d495) */
static void load_tims(dm_tstrack *t, rd c)
{
    if (c.e - c.p < 4) return;
    uint32_t isz = u32(c.p);
    c.p += 4;
    if (!isz) return;
    uint32_t rsz = isz < 8 ? isz : 8;
    while (c.e - c.p >= (ptrdiff_t)rsz) {
        uint8_t buf[8] = { 0 };
        memcpy(buf, c.p, rsz);
        c.p += isz;
        if (c.p > c.e) c.p = c.e;
        dm_tsitem it = { (int32_t)u32(buf), buf[4], buf[5], u16(buf + 6) };
        if (!it.bpm) it.bpm = 4;
        if (!it.beat) it.beat = 4;
        if (!it.grids) it.grids = 4;
        t->items = realloc(t->items, (t->n + 2) * sizeof(*t->items));
        t->items[t->n++] = it;
    }
    if (t->n && t->items[0].mt > 0) {        /* a copy of the first signature at time 0 */
        memmove(t->items + 1, t->items, t->n * sizeof(*t->items));
        t->items[0].mt = 0;
        t->n++;
    }
}

/* ------------------------------------------------------------------ band track (dmband 77202c39 / 772052f8) */
static void wcs_to_utf8(char *out, size_t n, rd c)
{
    size_t k = 0;
    for (const uint8_t *p = c.p; p + 1 < c.e && k + 1 < n; p += 2) {
        uint16_t ch = u16(p);
        if (!ch) break;
        out[k++] = ch < 0x80 ? (char)ch : '?';
    }
    out[k] = 0;
}

static void load_instrument(dm_band *b, rd r, dm_getcoll_fn getcoll, void *ctx)
{
    dm_inst in;
    memset(&in, 0, sizeof(in));
    int have_ref = 0;
    uint32_t id, type; rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('b','i','n','s')) {
            uint8_t buf[0x2c] = { 0 };
            memcpy(buf, c.p, (size_t)(c.e - c.p) < sizeof(buf) ? (size_t)(c.e - c.p) : sizeof(buf));
            in.patch = u32(buf); in.assign = u32(buf + 4);
            for (int i = 0; i < 4; i++) in.ranges[i] = u32(buf + 8 + 4 * i);
            in.pch = u32(buf + 24); in.flags = u32(buf + 28); in.pan = buf[32]; in.vol = buf[33];
            in.transpose = (int16_t)u16(buf + 34); in.priority = u32(buf + 36); in.pbrange = (int16_t)u16(buf + 40);
            in.coll = NULL;
        } else if (id == FCC('L','I','S','T') && type == FCC('D','M','R','F') && getcoll) {
            dm_guid g; int hasg = 0;
            char file[260] = "";
            rd s; uint32_t sid;
            while (next_chunk(&c, &sid, &s, NULL)) {
                if (sid == FCC('g','u','i','d') && s.e - s.p >= 16) { get_guid(&g, s.p); hasg = 1; }
                else if (sid == FCC('f','i','l','e')) wcs_to_utf8(file, sizeof(file), s);
            }
            in.coll = getcoll(ctx, hasg ? &g : NULL, file[0] ? file : NULL);
            have_ref = 1;
        }
    }
    if (!have_ref && !in.coll && (in.flags & 0x700) && getcoll) in.coll = getcoll(ctx, NULL, NULL);
    /* the instrument list is built by prepending: reverse file order */
    b->inst = realloc(b->inst, (b->ninst + 1) * sizeof(*b->inst));
    memmove(b->inst + 1, b->inst, b->ninst * sizeof(*b->inst));
    b->inst[0] = in;
    b->ninst++;
}

static void load_dmbd(dm_band *b, rd r, dm_getcoll_fn getcoll, void *ctx)
{
    uint32_t id, type; rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('L','I','S','T') && type == FCC('l','b','i','l')) {
            rd s; uint32_t sid, st;
            while (next_chunk(&c, &sid, &s, &st))
                if (sid == FCC('L','I','S','T') && st == FCC('l','b','i','n')) load_instrument(b, s, getcoll, ctx);
        }
    }
}

static void band_add(dm_bandtrack *t, dm_band *b)
{
    int i = 0;
    while (i < t->nbands && b->logical > t->bands[i]->logical) i++;
    t->bands = realloc(t->bands, (t->nbands + 1) * sizeof(*t->bands));
    memmove(t->bands + i + 1, t->bands + i, (t->nbands - i) * sizeof(*t->bands));
    t->bands[i] = b;
    t->nbands++;
}

static dm_bandtrack *load_band(rd r, dm_getcoll_fn getcoll, void *ctx)
{
    dm_bandtrack *t = calloc(1, sizeof(*t));
    uint32_t id, type; rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('b','d','t','h') && c.e - c.p >= 4) { t->lockautodl = 1; t->autodl = u32(c.p) != 0; }
        if (id != FCC('L','I','S','T') || type != FCC('l','b','d','l')) continue;
        rd s; uint32_t sid, st;
        while (next_chunk(&c, &sid, &s, &st)) {
            if (sid != FCC('L','I','S','T') || st != FCC('l','b','n','d')) continue;
            dm_band *b = calloc(1, sizeof(*b));
            b->groupbits = 0xffffffff;
            int seen2 = 0;
            rd x; uint32_t xid, xt;
            while (next_chunk(&s, &xid, &x, &xt)) {
                if (xid == FCC('b','d','2','h') && x.e - x.p >= 8) {
                    b->logical = (int32_t)u32(x.p); b->physical = (int32_t)u32(x.p + 4); seen2 = 1;
                } else if (xid == FCC('b','d','i','h') && x.e - x.p >= 4) {
                    if (!seen2) b->logical = b->physical = (int32_t)u32(x.p);
                } else if (xid == FCC('R','I','F','F') && xt == FCC('D','M','B','D')) {
                    load_dmbd(b, x, getcoll, ctx);
                }
            }
            band_add(t, b);
        }
    }
    return t;
}

/* ------------------------------------------------------------------ segment (77313c61, track list 7731330a) */
/* 773112d8: a new track goes before the first one with a higher position; at equal positions a SysEx track goes
 * first, then a band track (before anything but a SysEx track) */
static int track_before(const dm_track *n, const dm_track *e)
{
    if (n->position < e->position) return 1;
    if (n->position == e->position) {
        if (guid_eq(&n->clsid, &CLSID_SysExTrack)) return 1;
        if (guid_eq(&n->clsid, &CLSID_BandTrack) && !guid_eq(&e->clsid, &CLSID_SysExTrack)) return 1;
    }
    return 0;
}

static void load_track(dm_segment *s, rd r, dm_getcoll_fn getcoll, void *ctx)
{
    dm_track t;
    memset(&t, 0, sizeof(t));
    t.config = 0x38;
    uint32_t ckid = 0, fcc = 0, id, type;
    int have_hdr = 0;
    rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('t','r','k','h') && c.e - c.p >= 32) {
            get_guid(&t.clsid, c.p);
            t.position = u32(c.p + 16); t.group = u32(c.p + 20); ckid = u32(c.p + 24); fcc = u32(c.p + 28);
            have_hdr = 1;
        } else if (id == FCC('t','r','k','x') && c.e - c.p >= 8) {
            t.config = u32(c.p); t.priority = u32(c.p + 4);
        } else if (have_hdr && (((id == FCC('R','I','F','F') || id == FCC('L','I','S','T')) && type == fcc && fcc) ||
                                (id == ckid && ckid))) {
            if (guid_eq(&t.clsid, &CLSID_SeqTrack) && id == FCC('s','e','q','t')) { t.kind = TRK_SEQ; t.data = load_seq(c); }
            else if (guid_eq(&t.clsid, &CLSID_TempoTrack) && id == FCC('t','e','t','r')) { t.kind = TRK_TEMPO; t.data = load_tempo(c); }
            else if (guid_eq(&t.clsid, &CLSID_TimeSigTrack)) {
                dm_tstrack *ts = calloc(1, sizeof(*ts));
                ts->active = 1;
                if (id == FCC('t','i','m','s')) load_tims(ts, c);
                else { rd x; uint32_t xid; while (next_chunk(&c, &xid, &x, NULL)) if (xid == FCC('t','i','m','s')) load_tims(ts, x); }
                t.kind = TRK_TIMESIG; t.data = ts;
            } else if (guid_eq(&t.clsid, &CLSID_BandTrack)) { t.kind = TRK_BAND; t.data = load_band(c, getcoll, ctx); }
            else t.kind = TRK_OTHER;
            /* insert in segment track order */
            int i = 0;
            while (i < s->ntracks && !track_before(&t, &s->tracks[i])) i++;
            s->tracks = realloc(s->tracks, (s->ntracks + 1) * sizeof(*s->tracks));
            memmove(s->tracks + i + 1, s->tracks + i, (s->ntracks - i) * sizeof(*s->tracks));
            s->tracks[i] = t;
            s->ntracks++;
            return;
        }
    }
}

dm_segment *dm_segment_load(const void *data, size_t size, dm_getcoll_fn getcoll, void *ctx)
{
    rd f = { data, (const uint8_t *)data + size }, r;
    uint32_t id, type;
    if (!next_chunk(&f, &id, &r, &type) || id != FCC('R','I','F','F') || type != FCC('D','M','S','G')) return NULL;
    dm_segment *s = calloc(1, sizeof(*s));
    s->refs = 1;
    rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('s','e','g','h')) {
            uint8_t h[0x40] = { 0 };
            memcpy(h, c.p, (size_t)(c.e - c.p) < sizeof(h) ? (size_t)(c.e - c.p) : sizeof(h));
            s->repeats = u32(h); s->length = (int32_t)u32(h + 4); s->playstart = (int32_t)u32(h + 8);
            s->loopstart = (int32_t)u32(h + 12); s->loopend = (int32_t)u32(h + 16); s->resolution = u32(h + 20);
            memcpy(&s->rtlength, h + 24, 8); s->ioflags = u32(h + 32);
            memcpy(&s->rtloopstart, h + 40, 8); memcpy(&s->rtloopend, h + 48, 8); memcpy(&s->rtplaystart, h + 56, 8);
        } else if (id == FCC('L','I','S','T') && type == FCC('t','r','k','l')) {
            rd t; uint32_t tid, tt;
            while (next_chunk(&c, &tid, &t, &tt))
                if (tid == FCC('R','I','F','F') && tt == FCC('D','M','T','K')) load_track(s, t, getcoll, ctx);
        } else if (id == FCC('L','I','S','T') && type == FCC('U','N','F','O')) {
            rd t; uint32_t tid;
            while (next_chunk(&c, &tid, &t, NULL)) if (tid == FCC('U','N','A','M')) wcs_to_utf8(s->name, sizeof(s->name), t);
        }
    }
    return s;
}

static void free_track(dm_track *t)
{
    switch (t->kind) {
    case TRK_SEQ: {
        dm_seqtrack *q = t->data;
        for (int i = 0; i < q->nparts; i++) { free(q->parts[i].ev); free(q->parts[i].cv); }
        free(q->parts); free(q->pchs); free(q);
        break;
    }
    case TRK_TEMPO: free(((dm_tempotrack *)t->data)->items); free(t->data); break;
    case TRK_TIMESIG: free(((dm_tstrack *)t->data)->items); free(t->data); break;
    case TRK_BAND: {
        dm_bandtrack *b = t->data;
        for (int i = 0; i < b->nbands; i++) { free(b->bands[i]->inst); free(b->bands[i]); }
        free(b->bands); free(b);
        break;
    }
    }
}

void dm_segment_release(dm_segment *s)
{
    if (!s || --s->refs > 0) return;
    for (int i = 0; i < s->ntracks; i++) free_track(&s->tracks[i]);
    free(s->tracks); free(s->pchs); free(s);
}
