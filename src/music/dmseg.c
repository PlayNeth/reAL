/* reDM: segment file (.sgt, RIFF 'DMSG') loading: the segment header and the sequence, tempo, time signature
 * and band tracks, kept in the forms the original track objects keep them (dmime CSeqTrack/CTempoTrack/CTimeSigTrack
 * Load, dmband CBandTrack/CBand Load).  Other tracks (chord, command, ...) are kept as placeholders for track order. */
#include "dmi.h"
#include "dmsty.h"
#include "dmcomp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
static const dm_guid CLSID_ChordTrack   = { 0xd2ac288b, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_CommandTrack = { 0xd2ac288c, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_PatternTrack = { 0xd2ac2897, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_MotifTrack   = { 0xd2ac288e, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_StyleTrack   = { 0xd2ac288d, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_MuteTrack    = { 0xd2ac2898, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_ChordMapTrack = { 0xd2ac2896, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_SignPostTrack = { 0xf17e8672, 0xc3b4, 0x11d1, { 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_WaveTrack    = { 0xeed36461, 0x9ea5, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };

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
        if (id == FCC('g','u','i','d') && c.e - c.p >= 16) { get_guid(&b->guid, c.p); b->hasguid = 1; }
        else if (id == FCC('L','I','S','T') && type == FCC('U','N','F','O')) {
            rd s; uint32_t sid;
            while (next_chunk(&c, &sid, &s, NULL))
                if (sid == FCC('U','N','A','M')) {
                    size_t k = 0;
                    for (const uint8_t *p = s.p; p + 1 < s.e && k + 1 < 64; p += 2) { if (!(b->name[k] = u16(p))) break; k++; }
                    b->name[k] = 0;
                }
        } else if (id == FCC('L','I','S','T') && type == FCC('l','b','i','l')) {
            rd s; uint32_t sid, st;
            while (next_chunk(&c, &sid, &s, &st))
                if (sid == FCC('L','I','S','T') && st == FCC('l','b','i','n')) load_instrument(b, s, getcoll, ctx);
        }
    }
}

dm_band *dm_band_load(const void *data, size_t size, dm_getcoll_fn getcoll, void *ctx)
{
    dm_band *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->groupbits = 0xffffffff;
    rd r = { data, (const uint8_t *)data + size };
    load_dmbd(b, r, getcoll, ctx);
    return b;
}

static void band_add(dm_bandtrack *t, dm_band *b);
void dm_mutes_free(dm_mutenode *n) { while (n) { dm_mutenode *x = n->link; free(n); n = x; } }

void dm_band_addref(dm_band *b) { if (b) __sync_add_and_fetch(&b->refs, 1); }
void dm_band_release(dm_band *b)
{
    if (!b) return;
    if (__sync_sub_and_fetch(&b->refs, 1) >= 0) return;
    for (int k = 0; k < b->ninst; k++) for (dm_dlrec *r = b->inst[k].dl, *n; r; r = n) { n = r->next; free(r); }
    free(b->inst); free(b);
}

void dm_bandtrack_add(dm_bandtrack *t, dm_band *b) { band_add(t, b); }
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

/* ------------------------------------------------------------------ SysEx track (dmime 7731b4b4)
 * Items stay in load order.  The original's chunk size bookkeeping can drop the last items of a long list (the MIDI
 * importer writes the size short by 4 bytes per item); here every item in the chunk is read. */
static dm_sysextrack *load_sysex(rd c)
{
    dm_sysextrack *t = calloc(1, sizeof(*t));
    while (c.e - c.p >= 12) {
        int32_t mt = (int32_t)u32(c.p);
        uint32_t len = u32(c.p + 8);
        c.p += 12;
        if ((uint32_t)(c.e - c.p) < len) break;
        t->items = realloc(t->items, (t->n + 1) * sizeof(*t->items));
        dm_sysexitem *it = &t->items[t->n++];
        it->mt = mt; it->len = len; it->data = malloc(len ? len : 1);
        memcpy(it->data, c.p, len);
        c.p += len;
    }
    return t;
}

/* ------------------------------------------------------------------ wave track (dmime CWavTrack Load 77328647) */
/* 773248af: an item's DMRF reference: guid, file or name must be there */
static dm_wave *load_waveref(rd r, dm_getwave_fn getwave, void *ctx)
{
    dm_guid g; int hasg = 0;
    char file[260] = "", name[128] = "";
    uint32_t id; rd c;
    while (next_chunk(&r, &id, &c, NULL)) {
        if (id == FCC('g','u','i','d') && c.e - c.p >= 16) { get_guid(&g, c.p); hasg = 1; }
        else if (id == FCC('f','i','l','e')) wcs_to_utf8(file, sizeof(file), c);
        else if (id == FCC('n','a','m','e')) wcs_to_utf8(name, sizeof(name), c);
    }
    if ((!hasg && !file[0] && !name[0]) || !getwave) return NULL;
    return getwave(ctx, hasg ? &g : NULL, file[0] ? file : NULL, name[0] ? name : NULL);
}

/* 7732532b: one item ('waih' and its wave reference) */
static dm_waveitem *load_waveitem(rd r, dm_getwave_fn getwave, void *ctx)
{
    dm_waveitem *it = calloc(1, sizeof(*it));
    if (!it) return NULL;
    it->index = -1;
    uint32_t id, type; rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('L','I','S','T') && type == FCC('D','M','R','F')) {
            dm_wave *w = load_waveref(c, getwave, ctx);
            if (w) {
                if (it->wave) dm_wave_release(it->wave);
                it->wave = w;
                it->streaming = w->streaming;           /* GetStreamingParms */
                it->nopreroll = w->nopreroll;
            }
        } else if (id == FCC('w','a','i','h')) {
            uint8_t h[0x48] = { 0 };
            memcpy(h, c.p, (size_t)(c.e - c.p) < sizeof(h) ? (size_t)(c.e - c.p) : sizeof(h));
            it->vol = (int32_t)u32(h); it->pitch = (int32_t)u32(h + 4); it->variations = u32(h + 8);
            memcpy(&it->rt, h + 0x10, 8); memcpy(&it->startoff, h + 0x18, 8); memcpy(&it->dur, h + 0x28, 8);
            it->logical = (int32_t)u32(h + 0x30); it->loopstart = u32(h + 0x34); it->loopend = u32(h + 0x38);
            it->flags = u32(h + 0x3c); it->volrange = u16(h + 0x40); it->pitchrange = u16(h + 0x42);
            if (it->loopend) it->loopend++;
        }
    }
    return it;
}

/* 77325690 / 77324e84 / 77324ec2: the original's merge sort of an item list (Init 77326edc), by logical time for a
 * music time track, by rtTime in clock time.  The split reverses both halves and the merge takes the second list's
 * item first on equal keys, which decides the order of items at the same time. */
static int wi_less(const dm_waveitem *a, const dm_waveitem *b, int clock)
{
    return clock ? a->rt < b->rt : a->logical < b->logical;
}
static dm_waveitem *wi_merge(dm_waveitem *a, dm_waveitem *b, int clock)
{
    dm_waveitem *head = NULL, **tail = &head;
    while (a && b) {
        if (wi_less(a, b, clock)) { *tail = a; a = a->next; }
        else { *tail = b; b = b->next; }
        tail = &(*tail)->next;
        *tail = NULL;
    }
    *tail = a ? a : b;
    return head;
}
static dm_waveitem *wi_sort(dm_waveitem *l, int clock)
{
    if (!l || !l->next) return l;
    dm_waveitem *a = NULL, *b = NULL;
    while (l) {
        dm_waveitem *n = l->next;
        l->next = a; a = l;
        if (!n) break;
        l = n->next;
        n->next = b; b = n;
    }
    b = wi_sort(b, clock);
    a = wi_sort(a, clock);
    return wi_merge(a, b, clock);
}

static dm_wavetrack *load_wavetrack(rd r, dm_getwave_fn getwave, void *ctx)
{
    dm_wavetrack *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->version = 1;
    uint32_t id, type; rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('w','a','t','h') && c.e - c.p >= 4) {
            t->vol = (int32_t)u32(c.p);
            if (c.e - c.p >= 8) t->flags = u32(c.p + 4);
        } else if (id == FCC('L','I','S','T') && type == FCC('w','a','v','p')) {
            dm_wavepart *p = calloc(1, sizeof(*p));
            if (!p) break;
            uint32_t sid, st; rd s;
            while (next_chunk(&c, &sid, &s, &st)) {
                if (sid == FCC('w','a','p','h')) {          /* 77327e7c: DMUS_IO_WAVE_PART_HEADER */
                    uint8_t h[0x18] = { 0 };
                    memcpy(h, s.p, (size_t)(s.e - s.p) < sizeof(h) ? (size_t)(s.e - s.p) : sizeof(h));
                    p->vol = (int32_t)u32(h); p->variations = u32(h + 4); p->pch = u32(h + 8);
                    p->lock = u32(h + 12); p->flags = u32(h + 16); p->index = u32(h + 20);
                } else if (sid == FCC('L','I','S','T') && st == FCC('w','a','v','i')) {
                    uint32_t xid, xt; rd x;
                    while (next_chunk(&s, &xid, &x, &xt))
                        if (xid == FCC('L','I','S','T') && xt == FCC('w','a','v','e')) {
                            dm_waveitem *it = load_waveitem(x, getwave, ctx);
                            if (it) { it->next = p->items; p->items = it; }     /* prepended */
                        }
                }
            }
            /* 77325009: after the parts with a lower or equal PChannel */
            dm_wavepart **pp = &t->parts;
            while (*pp && !(p->pch < (*pp)->pch)) pp = &(*pp)->next;
            p->next = *pp; *pp = p;
        }
    }
    /* 773284ae: PChannels, the persisted variation arrays, voice slots in list order */
    for (dm_wavepart *p = t->parts; p; p = p->next) t->nparts++;
    if (t->nparts) {
        t->pchs = calloc(t->nparts, sizeof(uint32_t));
        if (t->flags & 2) { t->persistvar = calloc(t->nparts, 4); t->persistused = calloc(t->nparts, 4); }
        uint32_t i = 0;
        for (dm_wavepart *p = t->parts; p; p = p->next, i++) {
            if (t->pchs) t->pchs[i] = p->pch;
            for (dm_waveitem *it = p->items; it; it = it->next) it->index = (int32_t)t->nitems++;
        }
    }
    return t;
}

void dm_wavetrack_sort(dm_wavetrack *t, int clock)     /* Init 77326edc */
{
    for (dm_wavepart *p = t->parts; p; p = p->next) if (p->items && p->items->next) p->items = wi_sort(p->items, clock);
}

static void free_wavetrack(dm_wavetrack *t)
{
    dm_mutes_free(t->mutes);
    for (dm_wavepart *p = t->parts, *pn; p; p = pn) {
        pn = p->next;
        for (dm_waveitem *it = p->items, *in; it; it = in) {
            in = it->next;
            for (dm_dlnode *d = it->dls, *dn; d; d = dn) { dn = d->next; free(d); }
            dm_wave_release(it->wave);
            free(it);
        }
        free(p);
    }
    free(t->pchs); free(t->persistvar); free(t->persistused); free(t);
}

/* ------------------------------------------------------------------ segment (77313c61, track list 7731330a) */
/* 773112d8: a new track goes before the first one with a higher position; at equal positions a SysEx track goes
 * first, then a band track (before anything but a SysEx track) */
static int track_before(const dm_segtrack *n, const dm_segtrack *e)
{
    if (n->position < e->position) return 1;
    if (n->position == e->position) {
        if (guid_eq(&n->t->clsid, &CLSID_SysExTrack)) return 1;
        if (guid_eq(&n->t->clsid, &CLSID_BandTrack) && !guid_eq(&e->t->clsid, &CLSID_SysExTrack)) return 1;
    }
    return 0;
}

dm_track *dm_track_new(int kind, const dm_guid *clsid, void *data)
{
    dm_track *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->refs = 1; t->kind = kind; t->data = data;
    if (clsid) t->clsid = *clsid;
    return t;
}
void dm_track_addref(dm_track *t) { if (t) __sync_add_and_fetch(&t->refs, 1); }
static void free_track(dm_track *t);
void dm_track_release(dm_track *t)
{
    if (t && __sync_sub_and_fetch(&t->refs, 1) <= 0) { free_track(t); free(t); }
}

int dm_segment_add_pchannels(dm_segment *s, const uint32_t *pch, uint32_t n);
/* the track's Init as it goes into a segment: its PChannels to the segment's list (sequence 77319662, band 77201983,
 * wave 77326edc); a wave track sorts its items again, by logical time or, in clock time, by time (its merge sort takes
 * the later of two equal items first, so a second sort can swap them) */
static void track_init(dm_segment *s, dm_segtrack *e)
{
    dm_track *t = e->t;
    if (!t->data) return;
    switch (t->kind) {
    case TRK_SEQ: { dm_seqtrack *q = t->data; if (q->npchs && q->pchs) dm_segment_add_pchannels(s, q->pchs, q->npchs); break; }
    case TRK_WAVE: {
        dm_wavetrack *w = t->data;
        if (w->nparts && w->pchs) dm_segment_add_pchannels(s, w->pchs, w->nparts);
        dm_wavetrack_sort(w, (e->config & 0x40) != 0);
        break;
    }
    case TRK_STYLE: case TRK_MOTIF: case TRK_PATTERN: {       /* 7741152f / 7740df34 / 774135aa: Init */
        dms_styletrack *y = t->data;
        if (dms_styletrack_pchs(y) >= 0) dm_segment_add_pchannels(s, y->pchs, y->npchs);
        break;
    }
    case TRK_BAND: {
        dm_bandtrack *b = t->data;
        uint32_t n = 0;
        for (int k = 0; k < b->nbands; k++) n += b->bands[k]->ninst;
        if (!n) break;
        uint32_t *p = malloc(n * sizeof(*p)), j = 0;
        if (!p) break;
        for (int k = 0; k < b->nbands; k++) for (int i = 0; i < b->bands[k]->ninst; i++) p[j++] = b->bands[k]->inst[i].pch;
        dm_segment_add_pchannels(s, p, n);
        free(p);
        break;
    }
    }
}

/* in segment track order (77311b2c / 773112d8) */
void dm_segment_insert_track(dm_segment *s, dm_track *t, uint32_t position, uint32_t group, uint32_t config, uint32_t priority)
{
    dm_segtrack e = { t, position, group, config, priority };
    int i = 0;
    while (i < s->ntracks && !track_before(&e, &s->tracks[i])) i++;
    dm_segtrack *n = realloc(s->tracks, (s->ntracks + 1) * sizeof(*s->tracks));
    if (!n) { dm_track_release(t); return; }
    s->tracks = n;
    memmove(s->tracks + i + 1, s->tracks + i, (s->ntracks - i) * sizeof(*s->tracks));
    s->tracks[i] = e;
    s->ntracks++;
    track_init(s, &s->tracks[i]);
    for (int k = 0; k < s->nntypes; k++) dm_track_add_notification(t, &s->ntypes[k].g);
}

typedef struct { int kind; dm_guid clsid; uint32_t position, group, config, priority; void *data; } ldtrack;
static void load_track(dm_segment *s, rd r, const dm_loadcb *cb)
{
    dm_getcoll_fn getcoll = cb->getcoll; dm_getwave_fn getwave = cb->getwave; void *ctx = cb->ctx;
    ldtrack t;
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
            else if (guid_eq(&t.clsid, &CLSID_SysExTrack) && id == FCC('s','y','e','x')) { t.kind = TRK_SYSEX; t.data = load_sysex(c); }
            else if (guid_eq(&t.clsid, &CLSID_WaveTrack) && id == FCC('L','I','S','T') && type == FCC('w','a','v','t')) {
                t.kind = TRK_WAVE; t.data = load_wavetrack(c, getwave, ctx);
                if (!t.data) t.kind = TRK_OTHER;
            }
            else if (guid_eq(&t.clsid, &CLSID_ChordTrack) && id == FCC('L','I','S','T') && type == FCC('c','o','r','d')) {
                int32_t hr;
                t.data = dms_chordtrack_load(c.p, (size_t)(c.e - c.p), &hr);
                t.kind = t.data ? TRK_CHORD : TRK_OTHER;
            } else if (guid_eq(&t.clsid, &CLSID_CommandTrack) && id == FCC('c','m','n','d')) {
                int32_t hr;
                t.data = dms_cmdtrack_load(c.p, (size_t)(c.e - c.p), &hr);
                t.kind = t.data ? TRK_COMMAND : TRK_OTHER;
            } else if (guid_eq(&t.clsid, &CLSID_StyleTrack) && id == FCC('L','I','S','T') && type == FCC('s','t','t','r')) {
                int32_t hr;
                dm_style_srand();           /* 774128f4: the track's constructor */
                t.data = dms_styletrack_load(c.p, (size_t)(c.e - c.p), cb->getstyle, ctx, &hr);
                t.kind = t.data ? TRK_STYLE : TRK_OTHER;
            } else if (guid_eq(&t.clsid, &CLSID_PatternTrack) && id == FCC('R','I','F','F') && type == FCC('D','M','P','T')) {
                int32_t hr;                 /* 7741484d: unlike the style and motif tracks, no srand */
                t.data = dms_patterntrack_load(c.p, (size_t)(c.e - c.p), getcoll, ctx, &hr);
                t.kind = t.data ? TRK_PATTERN : TRK_OTHER;
            } else if (guid_eq(&t.clsid, &CLSID_ChordMapTrack) && id == FCC('L','I','S','T') && type == FCC('p','f','t','r')) {
                int32_t hr;
                t.data = dmc_cmtrack_load(c.p, (size_t)(c.e - c.p), cb->getcmap, ctx, &hr);
                t.kind = t.data ? TRK_CHORDMAP : TRK_OTHER;
            } else if (guid_eq(&t.clsid, &CLSID_SignPostTrack) && id == FCC('s','g','n','p')) {
                int32_t hr;
                t.data = dmc_sptrack_load(c.p, (size_t)(c.e - c.p), &hr);
                t.kind = t.data ? TRK_SIGNPOST : TRK_OTHER;
            } else if (guid_eq(&t.clsid, &CLSID_MuteTrack) && id == FCC('m','u','t','e')) {
                int32_t hr;
                t.data = dms_mutetrack_load(c.p, (size_t)(c.e - c.p), &hr);
                t.kind = t.data ? TRK_MUTE : TRK_OTHER;
            }
            else t.kind = TRK_OTHER;
            dm_track *o = dm_track_new(t.kind, &t.clsid, t.data);
            if (o) dm_segment_insert_track(s, o, t.position, t.group, t.config, t.priority);
            return;
        }
    }
}

dm_segment *dm_segment_load(const void *data, size_t size, dm_getcoll_fn getcoll, void *ctx)
{
    return dm_segment_load_ex(data, size, getcoll, NULL, ctx);
}

dm_segment *dm_segment_load_ex(const void *data, size_t size, dm_getcoll_fn getcoll, dm_getwave_fn getwave, void *ctx)
{
    dm_loadcb cb = { getcoll, getwave, NULL, ctx };
    return dm_segment_load_cb(data, size, &cb);
}

dm_segment *dm_segment_load_cb(const void *data, size_t size, const dm_loadcb *cb)
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
                if (tid == FCC('R','I','F','F') && tt == FCC('D','M','T','K')) load_track(s, t, cb);
        } else if (id == FCC('L','I','S','T') && type == FCC('U','N','F','O')) {
            rd t; uint32_t tid;
            while (next_chunk(&c, &tid, &t, NULL)) if (tid == FCC('U','N','A','M')) wcs_to_utf8(s->name, sizeof(s->name), t);
        }
    }
    return s;
}

/* dmime segment Load of a RIFF WAVE (77313d.. / 77328957 AddWave): one wave track (clock time, config 0x78) with
 * one part on PChannel 0 and one item at 0 lasting the wave's length; the segment's length is that, in reference
 * time (whole milliseconds, IDirectSoundWave's GetLength) */
/* dmstyle 77409532 (IDirectMusicStyle::GetMotif): the motif's parts sorted again, a segment of the motif's length
 * with a motif track (its repeats, resolution, start and loop from 'mtfs', else no repeats and beat resolution) and,
 * when the motif has a band, a band track with it (physical time -64) */
dm_segment *dms_motif_segment(dms_style *st, dms_pattern *p)
{
    const uint8_t *ts = p->ts[1] ? p->ts : st->ts;
    for (dms_partref *r = p->prefs; r; r = r->next) if (r->part) dms_part_resort(r->part);
    dm_style_srand();                   /* 7740ec38: the track's constructor */
    dms_styletrack *T = dms_motiftrack_new(st, p);
    dm_segment *s = calloc(1, sizeof(*s));
    if (!T || !s) { dms_styletrack_free(T); free(s); return NULL; }
    s->refs = 1;
    s->length = 1;
    int32_t len = dms_beat_clocks(ts) * p->measures * ts[0];
    if (len > 0) s->length = len;
    if (!p->hasmotif) { s->repeats = 0; s->resolution = 0x1000; }
    else {
        s->repeats = p->repeats; s->resolution = p->resolution;
        if (p->playstart >= 0 && p->playstart < s->length) s->playstart = p->playstart;
        mtime_t a = p->loopstart, b = p->loopend;
        if (!((a || b) && (a < 0 || b > s->length || a >= b))) { s->loopstart = a; s->loopend = b; }
    }
    dm_track *o = dm_track_new(TRK_MOTIF, &CLSID_MotifTrack, T);
    if (!o) { dms_styletrack_free(T); dm_segment_release(s); return NULL; }
    dm_segment_insert_track(s, o, 0, 1, 0x38, 0);
    if (p->band) {
        dm_bandtrack *bt = calloc(1, sizeof(*bt));
        if (bt) {
            p->band->logical = 0; p->band->physical = -64;
            dm_band_addref(p->band);
            band_add(bt, p->band);
            dm_track *b = dm_track_new(TRK_BAND, &CLSID_BandTrack, bt);
            if (b) dm_segment_insert_track(s, b, 0, 1, 0x38, 0);
            else { dm_track tmp = { .kind = TRK_BAND, .data = bt }; free_track(&tmp); }
        }
    }
    return s;
}

dm_segment *dm_band_segment(dm_band *b)
{
    dm_segment *s = calloc(1, sizeof(*s));
    dm_bandtrack *t = calloc(1, sizeof(*t));
    if (!s || !t) { free(s); free(t); return NULL; }
    s->refs = 1;
    s->length = 1;
    t->autodl = 0; t->lockautodl = 1;
    if (b) { dm_band_addref(b); band_add(t, b); }
    dm_track *o = dm_track_new(TRK_BAND, &CLSID_BandTrack, t);
    if (!o) { dm_track tmp = { .kind = TRK_BAND, .data = t }; free_track(&tmp); free(s); return NULL; }
    dm_segment_insert_track(s, o, 0, 1, 0x38, 0);
    return s;
}

dm_segment *dm_segment_from_wave(dm_wave *w)
{
    dm_segment *s = calloc(1, sizeof(*s));
    dm_wavetrack *t = calloc(1, sizeof(*t));
    dm_wavepart *p = calloc(1, sizeof(*p));
    dm_waveitem *it = calloc(1, sizeof(*it));
    if (!s || !t || !p || !it) { free(s); free(t); free(p); free(it); return NULL; }
    s->refs = 1;
    s->length = 1;
    uint32_t ms = dm_wave_length_ms(w);
    /* 77326ba1 */
    it->wave = w; dm_wave_addref(w);
    it->dur = (int64_t)ms * 10000;
    it->variations = 0xffffffff;
    it->streaming = w->streaming; it->nopreroll = w->nopreroll;
    p->variations = 0xffffffff;         /* 7732801a */
    p->items = it;
    t->parts = p; t->nparts = 1; t->version = 1;
    t->pchs = calloc(1, sizeof(uint32_t));
    it->index = 0; t->nitems = 1;
    s->rtlength = (int64_t)ms * 10000;
    s->ioflags = 1;                     /* DMUS_SEGIOF_REFLENGTH */
    dm_track *o = dm_track_new(TRK_WAVE, &CLSID_WaveTrack, t);
    if (o) dm_segment_insert_track(s, o, 0, 1, 0x78, 0);
    snprintf(s->name, sizeof(s->name), "%s", w->name);
    return s;
}

static void free_track(dm_track *t)
{
    switch (t->kind) {
    case TRK_SEQ: {
        dm_seqtrack *q = t->data;
        for (int i = 0; i < q->nparts; i++) { free(q->parts[i].ev); free(q->parts[i].cv); }
        dm_mutes_free(q->mutes);
        free(q->parts); free(q->pchs); free(q);
        break;
    }
    case TRK_TEMPO: free(((dm_tempotrack *)t->data)->items); free(t->data); break;
    case TRK_SYSEX: {
        dm_sysextrack *x = t->data;
        for (int i = 0; i < x->n; i++) free(x->items[i].data);
        free(x->items); free(x);
        break;
    }
    case TRK_TIMESIG: free(((dm_tstrack *)t->data)->items); free(t->data); break;
    case TRK_WAVE: free_wavetrack(t->data); break;
    case TRK_CHORD: dms_chordtrack_free(t->data); break;
    case TRK_COMMAND: dms_cmdtrack_free(t->data); break;
    case TRK_MUTE: dms_mutetrack_free(t->data); break;
    case TRK_STYLE: case TRK_MOTIF: case TRK_PATTERN: dms_styletrack_free(t->data); break;
    case TRK_CHORDMAP: dmc_cmtrack_free(t->data); break;
    case TRK_SIGNPOST: dmc_sptrack_free(t->data); break;
    case TRK_BAND: {
        dm_bandtrack *b = t->data;
        for (int i = 0; i < b->nbands; i++) dm_band_release(b->bands[i]);
        free(b->bands); free(b);
        break;
    }
    }
}

/* ------------------------------------------------------------------ Clone (IDirectMusicTrack::Clone, Segment::Clone) */
static int g_clonefix;
void dm_set_clone_fix(int on) { g_clonefix = on; }

/* the item in effect at the start of a clone: the last one before start, none when one is exactly at start
 * (tempo 7731c619, time signature 7731d82d) */
#define CARRY_PREV(arr, n, field, start, end, out, nout, T) do { \
    int prev_ = -1, i_; \
    for (i_ = 0; i_ < (n); i_++) { \
        if ((arr)[i_].field < (start)) { if (prev_ != -2) prev_ = i_; continue; } \
        if ((arr)[i_].field >= (end)) break; \
        if ((arr)[i_].field == (start)) prev_ = -2; \
    } \
    if (prev_ >= 0) { (out)[(nout)] = (arr)[prev_]; (out)[(nout)].field = 0; (nout)++; } \
    for (i_ = 0; i_ < (n); i_++) { \
        if ((arr)[i_].field < (start)) continue; \
        if ((arr)[i_].field >= (end)) break; \
        (out)[(nout)] = (arr)[i_]; (out)[(nout)].field -= (start); (nout)++; \
    } \
} while (0)

static void *clone_tempo(const dm_tempotrack *t, mtime_t start, mtime_t end)
{
    dm_tempotrack *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->active = t->active; c->setbyparam = t->setbyparam;
    if (t->n) {
        c->items = malloc((t->n + 1) * sizeof(*c->items));
        if (!c->items) { free(c); return NULL; }
        CARRY_PREV(t->items, t->n, mt, start, end, c->items, c->n, dm_tempoitem);
    }
    return c;
}
static void *clone_timesig(const dm_tstrack *t, mtime_t start, mtime_t end)
{
    dm_tstrack *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->active = t->active; c->setbyparam = t->setbyparam;
    if (t->n) {
        c->items = malloc((t->n + 1) * sizeof(*c->items));
        if (!c->items) { free(c); return NULL; }
        CARRY_PREV(t->items, t->n, mt, start, end, c->items, c->n, dm_tsitem);
    }
    return c;
}
/* 7731b7c4: the items from start until the first at or after end */
static void *clone_sysex(const dm_sysextrack *t, mtime_t start, mtime_t end)
{
    dm_sysextrack *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (t->n && !(c->items = calloc(t->n, sizeof(*c->items)))) { free(c); return NULL; }
    for (int i = 0; i < t->n; i++) {
        const dm_sysexitem *it = &t->items[i];
        if (it->mt < start) continue;
        if (it->mt >= end) break;
        if (!it->len) continue;
        dm_sysexitem *o = &c->items[c->n];
        if (!(o->data = malloc(it->len))) continue;
        memcpy(o->data, it->data, it->len);
        o->len = it->len; o->mt = it->mt - start;
        c->n++;
    }
    return c;
}
/* 7731aa7b: every part, with its events and curves from start until the first at or after end; the part list ends up
 * in the original's order: each new part goes to the front and the whole list is reversed after each one */
static void *clone_seq(const dm_seqtrack *t, mtime_t start, mtime_t end)
{
    dm_seqtrack *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (t->npchs) {
        c->pchs = malloc(t->npchs * sizeof(*c->pchs));
        if (c->pchs) { memcpy(c->pchs, t->pchs, t->npchs * sizeof(*c->pchs)); c->npchs = t->npchs; }
    }
    if (!t->nparts) return c;
    c->parts = calloc(t->nparts, sizeof(*c->parts));
    if (!c->parts) return c;
    for (int k = 0; k < t->nparts; k++) {
        const dm_seqpart *sp = &t->parts[k];
        dm_seqpart np = { sp->pch, NULL, 0, NULL, 0 };
        if (sp->nev) np.ev = malloc(sp->nev * sizeof(*np.ev));
        for (int i = 0; np.ev && i < sp->nev; i++) {
            if (sp->ev[i].mt < start) continue;
            if (sp->ev[i].mt >= end) break;
            np.ev[np.nev] = sp->ev[i]; np.ev[np.nev].mt -= start; np.nev++;
        }
        if (sp->ncv) np.cv = malloc(sp->ncv * sizeof(*np.cv));
        for (int i = 0; np.cv && i < sp->ncv; i++) {
            if (sp->cv[i].start < start) continue;
            if (sp->cv[i].start >= end) break;
            np.cv[np.ncv] = sp->cv[i]; np.cv[np.ncv].start -= start; np.ncv++;
        }
        /* push front, then reverse the list */
        memmove(c->parts + 1, c->parts, c->nparts * sizeof(*c->parts));
        c->parts[0] = np; c->nparts++;
        for (int i = 0, j = c->nparts - 1; i < j; i++, j--) { dm_seqpart x = c->parts[i]; c->parts[i] = c->parts[j]; c->parts[j] = x; }
    }
    return c;
}
/* 77328a7d / 7732887c / 7732819c: every part; its items starting in [start, end) (end 0: no end), moved by start.
 * The original leaves out an item's random volume and pitch ranges and its no-preroll flag (kept with the clone fix)
 * and keeps its logical time unmoved. */
static void *clone_wave(const dm_wavetrack *t, mtime_t start, mtime_t end)
{
    dm_wavetrack *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->vol = t->vol; c->flags = t->flags;
    dm_wavepart **pt = &c->parts;
    for (const dm_wavepart *sp = t->parts; sp; sp = sp->next) {
        dm_wavepart *np = calloc(1, sizeof(*np));
        if (!np) break;
        np->pch = sp->pch; np->index = sp->index; np->vol = sp->vol; np->variations = sp->variations;
        np->lock = sp->lock; np->flags = sp->flags;
        dm_waveitem **pi = &np->items;
        for (const dm_waveitem *it = sp->items; it; it = it->next) {
            int32_t lo = (int32_t)it->rt;
            if (!(start <= lo && (end == 0 || lo < end))) continue;
            dm_waveitem *ni = calloc(1, sizeof(*ni));
            if (!ni) break;
            ni->rt = it->rt - start;
            ni->vol = it->vol; ni->pitch = it->pitch; ni->variations = it->variations; ni->startoff = it->startoff;
            ni->dur = it->dur; ni->logical = it->logical; ni->flags = it->flags; ni->wave = it->wave; dm_wave_addref(ni->wave);
            ni->streaming = it->streaming; ni->loopstart = it->loopstart; ni->loopend = it->loopend;
            if (g_clonefix) { ni->volrange = it->volrange; ni->pitchrange = it->pitchrange; ni->nopreroll = it->nopreroll; }
            dm_dlnode **pd = &ni->dls;      /* 77325719: the same downloads (the list comes out reversed) */
            for (const dm_dlnode *d = it->dls; d; d = d->next) {
                dm_dlnode *nd = calloc(1, sizeof(*nd));
                if (!nd) break;
                nd->dl = d->dl; nd->next = *pd; *pd = nd;
            }
            *pi = ni; pi = &ni->next;
        }
        *pt = np; pt = &np->next;
    }
    for (dm_wavepart *p = c->parts; p; p = p->next) c->nparts++;
    if (c->nparts) {
        c->pchs = calloc(c->nparts, sizeof(uint32_t));
        if (c->flags & 2) { c->persistvar = calloc(c->nparts, 4); c->persistused = calloc(c->nparts, 4); }
        uint32_t i = 0;
        for (dm_wavepart *p = c->parts; p; p = p->next, i++) {
            if (c->pchs) c->pchs[i] = p->pch;
            for (dm_waveitem *it = p->items; it; it = it->next) it->index = (int32_t)c->nitems++;
        }
    }
    return c;
}
/* dmband 772038cd: an instrument added to a band goes to the front of its list; the copy leaves out the priority,
 * pitch bend range and the MIDI-file state (kept with the clone fix) */
static void inst_copy(dm_inst *d, const dm_inst *s)
{
    memset(d, 0, sizeof(*d));
    d->patch = s->patch; d->assign = s->assign; d->pan = s->pan; d->vol = s->vol; d->transpose = s->transpose;
    d->pch = s->pch; d->flags = s->flags; memcpy(d->ranges, s->ranges, sizeof(d->ranges)); d->coll = s->coll;
    if (g_clonefix) { d->priority = s->priority; d->pbrange = s->pbrange; d->notinfile = s->notinfile; d->smf = s->smf; d->sendpatch = s->sendpatch; }
}
static dm_band *band_copy(const dm_band *b)        /* 77202d94's copy of a band: instruments re-added, so reversed */
{
    dm_band *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    *n = *b;
    n->refs = 0;
    n->inst = NULL; n->ninst = 0;
    if (b->ninst) {
        if (!(n->inst = malloc(b->ninst * sizeof(*n->inst)))) { free(n); return NULL; }
        for (int k = 0; k < b->ninst; k++) inst_copy(&n->inst[b->ninst - 1 - k], &b->inst[k]);
        n->ninst = b->ninst;
    }
    return n;
}
/* dmband 77202d94: from start > 0, one band at 0 with each PChannel's latest instrument before start (made only when
 * a band follows at or after start: without one the original drops them; kept with the clone fix), then the bands
 * in [start, end) moved by start */
static void *clone_band(const dm_bandtrack *t, mtime_t start, mtime_t end)
{
    dm_bandtrack *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (start > 0) {
        const dm_inst **latest = NULL; int nl = 0, cap = 0, mode = 0, k;
        for (k = 0; k < t->nbands; k++) {
            const dm_band *b = t->bands[k];
            if (start <= b->logical) break;
            for (int j = 0; j < b->ninst; j++) {
                int f;
                for (f = 0; f < nl; f++) if (latest[f]->pch == b->inst[j].pch) { latest[f] = &b->inst[j]; break; }
                if (f == nl) {
                    if (nl == cap) { cap = cap ? cap * 2 : 16; const dm_inst **x = realloc(latest, cap * sizeof(*x)); if (!x) break; latest = x; }
                    memmove(latest + 1, latest, nl * sizeof(*latest));     /* new PChannels go to the front */
                    latest[0] = &b->inst[j]; nl++;
                    mode = b->mode;
                }
            }
        }
        if (nl && (k < t->nbands || g_clonefix)) {
            dm_band *m = calloc(1, sizeof(*m));
            if (m && (m->inst = malloc(nl * sizeof(*m->inst)))) {
                for (int j = 0; j < nl; j++) inst_copy(&m->inst[nl - 1 - j], latest[j]);
                m->ninst = nl;
                m->logical = 0;
                m->physical = (k < t->nbands && t->bands[k]->physical <= start) ? t->bands[k]->physical - start - 1 : -1;
                m->groupbits = 0xffffffff; m->mode = mode;
                band_add(c, m);
            } else free(m);
        }
        free(latest);
    }
    for (int k = 0; k < t->nbands; k++) {
        const dm_band *b = t->bands[k];
        if (!((start == 0 || start <= b->logical) && b->logical < end)) continue;
        dm_band *n = band_copy(b);
        if (!n) continue;
        n->logical -= start; n->physical -= start;
        band_add(c, n);
    }
    return c;
}

dm_track *dm_track_clone(const dm_track *t, mtime_t start, mtime_t end, int *hr)
{
    *hr = DM_OK;
    if (start < 0 || end < start) { *hr = DM_E_INVALIDARG; return NULL; }
    void *d = NULL;
    switch (t->kind) {
    case TRK_TEMPO: d = clone_tempo(t->data, start, end); break;
    case TRK_TIMESIG: d = clone_timesig(t->data, start, end); break;
    case TRK_SYSEX: d = clone_sysex(t->data, start, end); break;
    case TRK_SEQ: d = clone_seq(t->data, start, end); break;
    case TRK_WAVE: d = clone_wave(t->data, start, end); break;
    case TRK_BAND: d = clone_band(t->data, start, end); break;
    case TRK_CHORD: d = dms_chordtrack_clone(t->data, start, end); break;
    case TRK_COMMAND: d = dms_cmdtrack_clone(t->data, start, end); break;
    case TRK_MUTE: d = dms_mutetrack_clone(t->data, start, end); break;
    case TRK_STYLE: case TRK_MOTIF: dm_style_srand(); d = dms_styletrack_clone(t->data, start, end); break;    /* 77411f7d / 7740e877 */
    case TRK_PATTERN: d = dms_styletrack_clone(t->data, start, end); break;      /* 7740d4bf: no srand */
    case TRK_CHORDMAP: d = dmc_cmtrack_clone(t->data, start, end); break;      /* 7728c49d */
    case TRK_SIGNPOST: d = dmc_sptrack_clone(t->data, start, end); break;      /* 7728b155 */
    default: break;                 /* a track kind the engine does not play: a copy without data */
    }
    if (!d && t->kind != TRK_OTHER) { *hr = DM_E_OUTOFMEMORY; return NULL; }
    dm_track *c = dm_track_new(t->kind, &t->clsid, d);
    if (c && (t->kind == TRK_CHORD || t->kind == TRK_COMMAND || t->kind == TRK_STYLE || t->kind == TRK_MOTIF || t->kind == TRK_PATTERN)) c->notify = t->notify;    /* (copied by their Clone) */
    if (!c) { dm_track tmp = { .kind = t->kind, .data = d }; free_track(&tmp); *hr = DM_E_OUTOFMEMORY; }
    return c;
}

/* 77313a27: the range clamped as the original does (an end before the start or past the length becomes the length,
 * as does an end of 0 from 0; a start outside the segment becomes 0); the segment's settings copied, length
 * end - start, each track cloned into the same entry (a track that fails: S_FALSE) */
dm_segment *dm_segment_clone(const dm_segment *s, mtime_t start, mtime_t end, int *hr)
{
    *hr = DM_OK;
    if (end < start || s->length < end) end = s->length;
    if (end == 0 && start == 0) end = s->length;
    if (start < 0 || s->length < start) start = 0;
    dm_segment *c = calloc(1, sizeof(*c));
    if (!c) { *hr = DM_E_OUTOFMEMORY; return NULL; }
    c->refs = 1;
    c->repeats = s->repeats; c->resolution = s->resolution; c->ioflags = s->ioflags;
    c->rtlength = s->rtlength; c->rtloopstart = s->rtloopstart; c->rtloopend = s->rtloopend; c->rtplaystart = s->rtplaystart;
    c->length = end - start; c->playstart = s->playstart; c->loopstart = s->loopstart; c->loopend = s->loopend;
    memcpy(c->name, s->name, sizeof(c->name));
    if (s->npchs && (c->pchs = malloc(s->npchs * sizeof(*c->pchs)))) { memcpy(c->pchs, s->pchs, s->npchs * sizeof(*c->pchs)); c->npchs = s->npchs; }
    for (int i = 0; i < s->ntracks; i++) {
        const dm_segtrack *e = &s->tracks[i];
        int r;
        dm_track *t = dm_track_clone(e->t, start, end, &r);
        if (!t) { *hr = DM_FALSE; continue; }
        dm_segment_insert_track(c, t, e->position, e->group, e->config, e->priority);
    }
    return c;
}

void dm_segment_release(dm_segment *s)
{
    if (!s || --s->refs > 0) return;
    for (int i = 0; i < s->ntracks; i++) dm_track_release(s->tracks[i].t);
    free(s->tracks); free(s->pchs); free(s->ntypes); free(s);
}

/* ------------------------------------------------------------------ notification types (dmime 77311fd1, 7731322f,
 * 7731204d); tracks take the ones they generate (77311fff / 77312026: Track::AddNotificationType) */
static const dm_guid GUID_Null = { 0 };

int dm_track_add_notification(dm_track *t, const dm_guid *g)
{
    if (t->kind == TRK_TIMESIG && guid_eq(g, &GUID_NOTIFICATION_MEASUREANDBEAT_)) { t->notify = 1; return DM_OK; }   /* 7731d267 */
    if (t->kind == TRK_CHORD && guid_eq(g, &GUID_NOTIFICATION_CHORD_)) { ((dms_chordtrack *)t->data)->notify = t->notify = 1; return DM_OK; }   /* 77401e52 */
    if (t->kind == TRK_COMMAND && guid_eq(g, &GUID_NOTIFICATION_COMMAND_)) { ((dms_cmdtrack *)t->data)->notify = t->notify = 1; return DM_OK; }    /* 77403518 */
    if (t->kind == TRK_MUTE || t->kind == TRK_CHORDMAP) return DM_E_NOTIMPL;      /* (7728acae) */
    if (t->kind == TRK_SIGNPOST && guid_eq(g, &GUID_NOTIFICATION_RECOMPOSE_)) { ((dmc_sptrack *)t->data)->notify = t->notify = 1; return DM_OK; }   /* 7728accd */
    if ((t->kind == TRK_STYLE || t->kind == TRK_MOTIF || t->kind == TRK_PATTERN) && guid_eq(g, &GUID_NOTIFICATION_MEASUREANDBEAT_)) { ((dms_styletrack *)t->data)->notify = t->notify = 1; return DM_OK; }    /* 7740fe93 */
    return DM_FALSE;
}
int dm_track_remove_notification(dm_track *t, const dm_guid *g)
{
    if (t->kind == TRK_TIMESIG && guid_eq(g, &GUID_NOTIFICATION_MEASUREANDBEAT_)) { t->notify = 0; return DM_OK; }   /* 7731d2a9 */
    if (t->kind == TRK_CHORD && guid_eq(g, &GUID_NOTIFICATION_CHORD_)) { ((dms_chordtrack *)t->data)->notify = t->notify = 0; return DM_OK; }   /* 77401e91 */
    if (t->kind == TRK_COMMAND && guid_eq(g, &GUID_NOTIFICATION_COMMAND_)) { ((dms_cmdtrack *)t->data)->notify = t->notify = 0; return DM_OK; }    /* 77403557 */
    if (t->kind == TRK_MUTE || t->kind == TRK_CHORDMAP) return DM_E_NOTIMPL;
    if (t->kind == TRK_SIGNPOST && guid_eq(g, &GUID_NOTIFICATION_RECOMPOSE_)) { ((dmc_sptrack *)t->data)->notify = t->notify = 0; return DM_OK; }   /* 7728ad0c */
    if ((t->kind == TRK_STYLE || t->kind == TRK_MOTIF || t->kind == TRK_PATTERN) && guid_eq(g, &GUID_NOTIFICATION_MEASUREANDBEAT_)) { ((dms_styletrack *)t->data)->notify = t->notify = 0; return DM_OK; }    /* 7740febe */
    return DM_FALSE;
}

static int ntype_find(const dm_segment *s, const dm_guid *g)
{
    for (int i = 0; i < s->nntypes; i++) if (guid_eq(&s->ntypes[i].g, g)) return i;
    return -1;
}

int dm_segment_has_notification(const dm_segment *s, const dm_guid *g) { return ntype_find(s, g) >= 0; }

int dm_segment_add_notification(dm_segment *s, const dm_guid *g, int fromperf)
{
    int i = ntype_find(s, g);
    if (i >= 0) {
        if (s->ntypes[i].fromperf == fromperf) return DM_FALSE;
        s->ntypes[i].fromperf = 0;          /* asked for by both: the application's stays */
        return DM_OK;
    }
    dm_ntype *n = realloc(s->ntypes, (s->nntypes + 1) * sizeof(*n));
    if (!n) return DM_E_OUTOFMEMORY;
    s->ntypes = n;
    s->ntypes[s->nntypes].g = *g; s->ntypes[s->nntypes].fromperf = fromperf;
    s->nntypes++;
    for (int t = 0; t < s->ntracks; t++) dm_track_add_notification(s->tracks[t].t, g);
    return DM_OK;
}

/* GUID_NULL: every type (with fromperf, only the performance's: the application's own stay) */
int dm_segment_remove_notification(dm_segment *s, const dm_guid *g, int fromperf)
{
    if (guid_eq(g, &GUID_Null)) {
        int k = 0;
        for (int i = 0; i < s->nntypes; i++) {
            if (s->ntypes[i].fromperf == 0 && fromperf) { s->ntypes[k++] = s->ntypes[i]; continue; }
            for (int t = 0; t < s->ntracks; t++) dm_track_remove_notification(s->tracks[t].t, &s->ntypes[i].g);
        }
        s->nntypes = k;
        return DM_OK;
    }
    int i = ntype_find(s, g);
    if (i < 0) return DM_FALSE;
    memmove(&s->ntypes[i], &s->ntypes[i + 1], (s->nntypes - i - 1) * sizeof(*s->ntypes));
    s->nntypes--;
    for (int t = 0; t < s->ntracks; t++) dm_track_remove_notification(s->tracks[t].t, g);
    return DM_OK;
}
