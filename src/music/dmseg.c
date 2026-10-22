/* reDM: segment file (.sgt, RIFF 'DMSG') loading: the segment header and the sequence, tempo, time signature
 * and band tracks, kept in the forms the original track objects keep them (dmime CSeqTrack/CTempoTrack/CTimeSigTrack
 * Load, dmband CBandTrack/CBand Load).  Other tracks (chord, command, ...) are kept as placeholders for track order. */
#include "dmi.h"
#include "dmsty.h"
#include "dmcomp.h"
#include "dmdls.h"
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
static const dm_guid CLSID_MarkerTrack = { 0x55a8fd00, 0x4288, 0x11d3, { 0x9b, 0xd1, 0x8a, 0x0d, 0x61, 0xc8, 0x88, 0x35 } };
static const dm_guid CLSID_SignPostTrack = { 0xf17e8672, 0xc3b4, 0x11d1, { 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid CLSID_LyricsTrack  = { 0x995c1cf5, 0x54ff, 0x11d3, { 0x8b, 0xda, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6 } };
static const dm_guid CLSID_SegTriggerTrack = { 0xbae4d665, 0x4ea1, 0x11d3, { 0x8b, 0xda, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6 } };
static const dm_guid CLSID_ScriptTrack = { 0x4108fa85, 0x3586, 0x11d3, { 0x8b, 0xd7, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6 } };
static const dm_guid CLSID_ParamControlTrack = { 0x4be0537b, 0x5c19, 0x11d3, { 0x8b, 0xdc, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6 } };
static const dm_guid GUID_TIME_MUSIC = { 0x0574c49d, 0x5b04, 0x4b15, { 0xa5, 0x42, 0xae, 0x28, 0x20, 0x30, 0x11, 0x7b } };
static const dm_guid GUID_TIME_REFERENCE = { 0x93ad712b, 0xdaa0, 0x4ffe, { 0xbc, 0x81, 0xb0, 0xce, 0x50, 0x0f, 0xcd, 0xd9 } };
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
            c.p = (size_t)(c.e - c.p) < isz ? c.e : c.p + isz;     /* (a huge size must not wrap a 32-bit pointer) */
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
        c.p = (size_t)(c.e - c.p) < isz ? c.e : c.p + isz;         /* (a huge size must not wrap a 32-bit pointer) */
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
        c.p = (size_t)(c.e - c.p) < isz ? c.e : c.p + isz;         /* (a huge size must not wrap a 32-bit pointer) */
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
            dls_release(in.coll);       /* (a reference before the header: the header's instrument has none) */
            in.coll = NULL;
        } else if (id == FCC('L','I','S','T') && type == FCC('D','M','R','F') && getcoll) {
            dm_guid g; int hasg = 0;
            char file[260] = "";
            rd s; uint32_t sid;
            while (next_chunk(&c, &sid, &s, NULL)) {
                if (sid == FCC('g','u','i','d') && s.e - s.p >= 16) { get_guid(&g, s.p); hasg = 1; }
                else if (sid == FCC('f','i','l','e')) wcs_to_utf8(file, sizeof(file), s);
            }
            dls_release(in.coll);       /* (getcoll gives a reference, which the instrument keeps) */
            in.coll = getcoll(ctx, hasg ? &g : NULL, file[0] ? file : NULL);
            have_ref = 1;
        }
    }
    if (!have_ref && !in.coll && (in.flags & 0x700) && getcoll) in.coll = getcoll(ctx, NULL, NULL);
    /* the instrument list is built by prepending: reverse file order */
    dm_inst *ni = realloc(b->inst, (b->ninst + 1) * sizeof(*b->inst));
    if (!ni) { dls_release(in.coll); return; }
    b->inst = ni;
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
    for (int k = 0; k < b->ninst; k++) {
        for (dm_dlrec *r = b->inst[k].dl, *n; r; r = n) { n = r->next; free(r); }
        dls_release(b->inst[k].coll);
    }
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
    if (!t) return NULL;
    uint32_t id, type; rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('b','d','t','h') && c.e - c.p >= 4) { t->lockautodl = 1; t->autodl = u32(c.p) != 0; }
        if (id != FCC('L','I','S','T') || type != FCC('l','b','d','l')) continue;
        rd s; uint32_t sid, st;
        while (next_chunk(&c, &sid, &s, &st)) {
            if (sid != FCC('L','I','S','T') || st != FCC('l','b','n','d')) continue;
            dm_band *b = calloc(1, sizeof(*b));
            if (!b) break;
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

void (*dm_track_replace_hook)(dm_track *t, int after);

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


/* ------------------------------------------------------------------ lyrics and segment trigger tracks */
/* 77310932 / 773104d6: the original's merge sort of an event list by logical time (the split reverses both halves,
 * the merge takes the second list's event first on equal times) */
#define EVSORT(name, T)                                                                                       \
static T *name##_merge(T *a, T *b)                                                                           \
{                                                                                                            \
    T *head = NULL, **tail = &head;                                                                          \
    while (a && b) {                                                                                         \
        if (a->logical < b->logical) { *tail = a; a = a->next; } else { *tail = b; b = b->next; }            \
        tail = &(*tail)->next; *tail = NULL;                                                                 \
    }                                                                                                        \
    *tail = a ? a : b;                                                                                       \
    return head;                                                                                             \
}                                                                                                            \
static T *name(T *l)                                                                                         \
{                                                                                                            \
    if (!l || !l->next) return l;                                                                            \
    T *a = NULL, *b = NULL;                                                                                  \
    while (l) {                                                                                              \
        T *n = l->next; l->next = a; a = l;                                                                  \
        if (!n) break;                                                                                       \
        l = n->next; n->next = b; b = n;                                                                     \
    }                                                                                                        \
    b = name(b); a = name(a);                                                                                \
    return name##_merge(a, b);                                                                               \
}
EVSORT(lyric_sort, dm_lyric)
EVSORT(segtrig_sort, dm_segtrig)

static void lyric_free(dm_lyric *e) { if (e) { free(e->text); free(e); } }
void dm_lyricstrack_free(dm_lyricstrack *T)
{
    if (!T) return;
    for (dm_lyric *e = T->items, *n; e; e = n) { n = e->next; lyric_free(e); }
    free(T);
}
#define E_LYRICS ((int32_t)0x88781221)
/* 77321606: a 'lyre' list: 'lyrh' (DMUS_IO_LYRICSTRACK_EVENTHEADER: its timing flags may not ask for reference
 * time or a tool's queue) and 'lyrn' (the text); both must be there */
static int32_t lyric_load(dm_lyricstrack *T, rd r)
{
    dm_lyric *e = calloc(1, sizeof(*e));
    if (!e) return DM_E_OUTOFMEMORY;
    int hdr = 0;
    uint32_t id; rd c;
    while (next_chunk(&r, &id, &c, NULL)) {
        if (id == FCC('l','y','r','h')) {
            if (c.e - c.p < 16) { lyric_free(e); return E_LYRICS; }
            if (u32(c.p + 4) & 0x63) { lyric_free(e); return E_LYRICS; }
            e->flags = u32(c.p); e->timing = u32(c.p + 4); e->logical = (int32_t)u32(c.p + 8); e->physical = (int32_t)u32(c.p + 12);
            hdr = 1;
        } else if (id == FCC('l','y','r','n')) {
            size_t n = (size_t)(c.e - c.p);
            if (n & 1) { lyric_free(e); return E_LYRICS; }
            uint16_t *t = malloc(n + 2);
            if (!t) { lyric_free(e); return DM_E_OUTOFMEMORY; }
            uint32_t k = 0;
            for (; k < n / 2; k++) { t[k] = u16(c.p + 2 * k); if (!t[k]) break; }
            t[k] = 0;
            free(e->text); e->text = t; e->len = k;
        }
    }
    if (!hdr || !e->text) { lyric_free(e); return E_LYRICS; }
    e->next = T->items; T->items = e;
    return DM_OK;
}
/* 7732175a: the 'lyrt' list's 'lyrl' list of 'lyre' lists */
dm_lyricstrack *dm_lyricstrack_load(const uint8_t *p, size_t n, int32_t *hr)
{
    dm_lyricstrack *T = calloc(1, sizeof(*T));
    *hr = DM_E_OUTOFMEMORY;
    if (!T) return NULL;
    T->gen = 1;
    rd r = { p, p + n }, c;
    uint32_t id, type;
    *hr = E_LYRICS;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id != FCC('L','I','S','T') || type != FCC('l','y','r','l')) continue;
        *hr = DM_OK;
        rd x;
        while (*hr >= 0 && next_chunk(&c, &id, &x, &type))
            if (id == FCC('L','I','S','T') && type == FCC('l','y','r','e')) *hr = lyric_load(T, x);
        break;
    }
    if (*hr < 0) { dm_lyricstrack_free(T); return NULL; }
    T->items = lyric_sort(T->items);
    return T;
}
/* 77310c64: the events at logical times in [start, end), moved to 0 */
static dm_lyricstrack *lyricstrack_clone(const dm_lyricstrack *T, mtime_t start, mtime_t end)
{
    dm_lyricstrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    C->gen = 1;
    dm_lyric **tail = &C->items;
    for (const dm_lyric *e = T->items; e && e->logical < end; e = e->next) {
        if (e->logical < start) continue;
        dm_lyric *x = malloc(sizeof(*x));
        if (!x) break;
        *x = *e; x->next = NULL;
        x->logical -= start; x->physical -= start;
        x->text = malloc((e->len + 1) * 2);
        if (x->text) memcpy(x->text, e->text, (e->len + 1) * 2); else x->len = 0;
        *tail = x; tail = &x->next;
    }
    return C;
}

static void segtrig_item_free(dm_segtrig *e) { if (e) { if (e->seg) dm_segment_release(e->seg); free(e); } }
void dm_segtrigtrack_free(dm_segtrigtrack *T)
{
    if (!T) return;
    for (dm_segtrig *e = T->items, *n; e; e = n) { n = e->next; segtrig_item_free(e); }
    free(T);
}
#define E_SEGTRIG ((int32_t)0x88781220)
static int wname_ieq(const uint16_t *a, const uint16_t *b)
{
    for (;; a++, b++) {
        uint16_t x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
/* a reference's guid, file and name */
static void ref_read(rd r, dm_guid *g, int *hasg, char *file, size_t nf, char *name, size_t nn)
{
    uint32_t id; rd c;
    *hasg = 0; file[0] = name[0] = 0;
    while (next_chunk(&r, &id, &c, NULL)) {
        if (id == FCC('g','u','i','d') && c.e - c.p >= 16) { get_guid(g, c.p); *hasg = 1; }
        else if (id == FCC('f','i','l','e')) wcs_to_utf8(file, nf, c);
        else if (id == FCC('n','a','m','e')) wcs_to_utf8(name, nn, c);
    }
}
/* 77323454: an 'lseg' list: 'sgih' (DMUS_IO_SEGMENT_ITEM_HEADER), then the segment's reference, or (flag 1,
 * DMUS_SEGMENTTRACKF_MOTIF) the style's reference and the motif's name ('snam'); an event whose reference is missing
 * is left out, one whose segment does not load fails the track */
static int32_t segtrig_load_item(dm_segtrigtrack *T, rd r, const dm_loadcb *cb)
{
    uint32_t id, type; rd c;
    int got = 0;
    dm_segtrig *e = calloc(1, sizeof(*e));
    if (!e) return DM_E_OUTOFMEMORY;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('s','g','i','h')) {
            if (c.e - c.p < 16) { free(e); return E_SEGTRIG; }
            e->logical = (int32_t)u32(c.p); e->physical = (int32_t)u32(c.p + 4);
            e->playflags = u32(c.p + 8); e->flags = u32(c.p + 12);
            got = 1;
            break;
        }
    }
    if (!got) { free(e); return E_SEGTRIG; }
    dm_guid g; int hasg;
    char file[260], name[128];
    if (e->flags & 1) {
        struct dms_style *st = NULL;
        uint16_t *mname = NULL;
        int32_t hr = DM_OK;
        while (hr >= 0 && next_chunk(&r, &id, &c, &type)) {
            if (id == FCC('L','I','S','T') && type == FCC('D','M','R','F')) {
                ref_read(c, &g, &hasg, file, sizeof(file), name, sizeof(name));
                struct dms_style *x = cb && cb->getstyle ? cb->getstyle(cb->ctx, hasg ? &g : NULL, file[0] ? file : NULL, name[0] ? name : NULL) : NULL;
                if (!x) hr = E_SEGTRIG;
                if (st) dms_style_release(st);
                st = x;
            } else if (id == FCC('s','n','a','m')) {
                size_t n = (size_t)(c.e - c.p);
                if (n & 1) { hr = E_SEGTRIG; break; }
                free(mname);
                if (!(mname = malloc(n + 2))) { hr = DM_E_OUTOFMEMORY; break; }
                size_t k = 0;
                for (; k < n / 2; k++) { mname[k] = u16(c.p + 2 * k); if (!mname[k]) break; }
                mname[k] = 0;
            }
        }
        if (hr >= 0 && st && mname) {
            hr = DMUS_E_NOT_FOUND;
            for (int k = 0; k < st->nmotifs; k++) {
                uint16_t mn[65];
                memcpy(mn, st->motifs[k]->name, sizeof(st->motifs[k]->name)); mn[64] = 0;
                if (!wname_ieq(mname, mn)) continue;
                e->seg = dms_motif_segment(st, st->motifs[k]);
                hr = e->seg ? DM_OK : DM_E_OUTOFMEMORY;
                break;
            }
        }
        free(mname);
        if (st) dms_style_release(st);
        if (hr < 0 || !e->seg) { free(e); return hr < 0 ? hr : DM_OK; }
    } else {
        while (next_chunk(&r, &id, &c, &type)) {
            if (id != FCC('L','I','S','T') || type != FCC('D','M','R','F')) continue;
            ref_read(c, &g, &hasg, file, sizeof(file), name, sizeof(name));
            e->seg = cb && cb->getseg ? cb->getseg(cb->ctx, hasg ? &g : NULL, file[0] ? file : NULL, name[0] ? name : NULL) : NULL;
            if (!e->seg) { free(e); return E_SEGTRIG; }
            break;
        }
        if (!e->seg) { free(e); return DM_OK; }
    }
    e->next = T->items; T->items = e;
    return DM_OK;
}
/* 77323691: the 'segt' list: 'sgth' (the track's flags) and the 'lsgl' list of 'lseg' lists */
dm_segtrigtrack *dm_segtrigtrack_load(const uint8_t *p, size_t n, const dm_loadcb *cb, int32_t *hr)
{
    dm_segtrigtrack *T = calloc(1, sizeof(*T));
    *hr = DM_E_OUTOFMEMORY;
    if (!T) return NULL;
    T->gen = 1;
    *hr = DM_OK;
    rd r = { p, p + n }, c, x;
    uint32_t id, type;
    while (*hr >= 0 && next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('s','g','t','h')) { if (c.e - c.p >= 4) T->hflags = u32(c.p); else *hr = E_SEGTRIG; }
        else if (id == FCC('L','I','S','T') && type == FCC('l','s','g','l'))
            while (*hr >= 0 && next_chunk(&c, &id, &x, &type))
                if (id == FCC('L','I','S','T') && type == FCC('l','s','e','g')) *hr = segtrig_load_item(T, x, cb);
    }
    if (*hr < 0) { dm_segtrigtrack_free(T); return NULL; }
    T->items = segtrig_sort(T->items);
    return T;
}
/* 77310b0b: the events at logical times in [start, end), moved to 0 (the track's flags not copied) */
static dm_segtrigtrack *segtrigtrack_clone(const dm_segtrigtrack *T, mtime_t start, mtime_t end)
{
    dm_segtrigtrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    C->gen = 1;
    dm_segtrig **tail = &C->items;
    for (const dm_segtrig *e = T->items; e && e->logical < end; e = e->next) {
        if (e->logical < start) continue;
        dm_segtrig *x = malloc(sizeof(*x));
        if (!x) break;
        *x = *e; x->next = NULL;
        x->logical -= start; x->physical -= start;
        if (x->seg) x->seg->refs++;
        *tail = x; tail = &x->next;
    }
    return C;
}



/* ------------------------------------------------------------------ the script track (dmscript) */
void (*dm_unk_ref)(void *unk, int add);
EVSORT(scriptev_sort, dm_scriptev)
static void scriptev_free(dm_scriptev *e)
{
    if (!e) return;
    if (e->script && dm_unk_ref) dm_unk_ref(e->script, 0);
    free(e->routine); free(e);
}
void dm_scripttrack_free(dm_scripttrack *T)
{
    if (!T) return;
    for (dm_scriptev *e = T->items, *n; e; e = n) { n = e->next; scriptev_free(e); }
    free(T);
}
#define E_SCRIPTTRACK ((int32_t)0x88781219)
/* 00410249: a 'scre' list: 'scrh' (DMUS_IO_SCRIPTTRACK_EVENTHEADER, as much of it as there is), the script's
 * reference ('DMRF': the loader's GetObject; its failure is the Load's) and 'scrn' (the routine); all three must be
 * there.  The event goes in front (the list is sorted after) */
static int32_t scriptev_load(dm_scripttrack *T, rd r, const dm_loadcb *cb)
{
    dm_scriptev *e = calloc(1, sizeof(*e));
    if (!e) return DM_E_OUTOFMEMORY;
    int hdr = 0;
    uint32_t id, type; rd c;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id == FCC('L','I','S','T') && type == FCC('D','M','R','F')) {
            dm_guid g; int hasg; char file[260], name[128];
            ref_read(c, &g, &hasg, file, sizeof(file), name, sizeof(name));
            int32_t hr = DM_E_FAIL;
            void *x = cb && cb->getscript ? cb->getscript(cb->ctx, hasg ? &g : NULL, file[0] ? file : NULL, name[0] ? name : NULL, &hr) : NULL;
            if (!x) { scriptev_free(e); return hr < 0 ? hr : DM_E_FAIL; }
            if (e->script && dm_unk_ref) dm_unk_ref(e->script, 0);
            e->script = x;
        } else if (id == FCC('s','c','r','h')) {
            uint8_t h[12] = { 0 };
            memcpy(h, c.p, (size_t)(c.e - c.p) < 12 ? (size_t)(c.e - c.p) : 12);
            e->flags = u32(h); e->logical = (int32_t)u32(h + 4); e->physical = (int32_t)u32(h + 8);
            hdr = 1;
        } else if (id == FCC('s','c','r','n')) {
            size_t n = (size_t)(c.e - c.p);
            if (n & 1) { scriptev_free(e); return E_SCRIPTTRACK; }
            uint16_t *t = malloc(n + 2);
            if (!t) { scriptev_free(e); return DM_E_OUTOFMEMORY; }
            size_t k = 0;
            for (; k < n / 2; k++) { t[k] = u16(c.p + 2 * k); if (!t[k]) break; }
            t[k] = 0;
            free(e->routine); e->routine = t;       /* (the original's last one leaks the one before) */
        }
    }
    if (!hdr || !e->script || !e->routine) { scriptev_free(e); return E_SCRIPTTRACK; }
    e->next = T->items; T->items = e;
    return DM_OK;
}
/* 0041057a: the 'scrt' list's first 'scrl' list of 'scre' lists */
dm_scripttrack *dm_scripttrack_load(const uint8_t *p, size_t n, const dm_loadcb *cb, int32_t *hr)
{
    dm_scripttrack *T = calloc(1, sizeof(*T));
    *hr = DM_E_OUTOFMEMORY;
    if (!T) return NULL;
    T->gen = 1;
    rd r = { p, p + n }, c;
    uint32_t id, type;
    *hr = E_SCRIPTTRACK;
    while (next_chunk(&r, &id, &c, &type)) {
        if (id != FCC('L','I','S','T') || type != FCC('s','c','r','l')) continue;
        *hr = DM_OK;
        rd x;
        while (*hr >= 0 && next_chunk(&c, &id, &x, &type))
            if (id == FCC('L','I','S','T') && type == FCC('s','c','r','e')) *hr = scriptev_load(T, x, cb);
        break;
    }
    if (*hr < 0) { dm_scripttrack_free(T); return NULL; }
    T->items = scriptev_sort(T->items);
    return T;
}
/* the events at logical times in [start, end), moved to 0 (0040f797's copies) */
static dm_scripttrack *scripttrack_clone(const dm_scripttrack *T, mtime_t start, mtime_t end)
{
    dm_scripttrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    C->gen = 1;
    dm_scriptev **tail = &C->items;
    for (const dm_scriptev *e = T->items; e && e->logical < end; e = e->next) {
        if (e->logical < start) continue;
        dm_scriptev *x = malloc(sizeof(*x));
        if (!x) break;
        *x = *e; x->next = NULL;
        x->logical -= start; x->physical -= start;
        size_t k = 0; while (e->routine[k]) k++;
        if (!(x->routine = malloc((k + 1) * 2))) { free(x); break; }
        memcpy(x->routine, e->routine, (k + 1) * 2);
        if (x->script && dm_unk_ref) dm_unk_ref(x->script, 1);
        *tail = x; tail = &x->next;
    }
    return C;
}

/* ------------------------------------------------------------------ the parameter control track */
static void pcobj_free(dm_pcobj *o)
{
    if (!o) return;
    for (dm_pcparam *q = o->params, *n; q; q = n) { n = q->next; free(q->c); free(q); }
    free(o);
}
void dm_paramtrack_free(dm_paramtrack *T)
{
    if (!T) return;
    for (dm_pcobj *o = T->objs, *n; o; o = n) { n = o->next; pcobj_free(o); }
    free(T);
}
#define E_PARAMCTL ((int32_t)0x88781222)
/* the next chunk of the kind and id, the ones before it skipped (7732397a) */
static int find_chunk(rd *r, int list, uint32_t want, rd *c)
{
    uint32_t id, type;
    while (next_chunk(r, &id, c, &type)) {
        if (list ? (id == FCC('L','I','S','T') && type == want) : id == want) return 1;
    }
    return 0;
}
/* 77322602: a 'prpl' list: 'prph' (DMUS_IO_PARAMCONTROLTRACK_PARAMHEADER) and 'prcc' (the curves, after the size of
 * one); without 'prcc' the parameter is left out */
static int32_t pcparam_load(dm_paramtrack *T, dm_pcobj *o, rd r)
{
    rd c;
    if (!find_chunk(&r, 0, FCC('p','r','p','h'), &c)) return E_PARAMCTL;
    uint8_t h[8] = { 0 };
    memcpy(h, c.p, (size_t)(c.e - c.p) < 8 ? (size_t)(c.e - c.p) : 8);
    if (!find_chunk(&r, 0, FCC('p','r','c','c'), &c)) return DM_OK;
    size_t n = (size_t)(c.e - c.p);
    if (n < 4) return DM_E_FAIL;
    uint32_t isz = u32(c.p);
    n -= 4;
    if (!isz || n % isz) return DM_E_FAIL;
    dm_pcparam *q = calloc(1, sizeof(*q));
    if (!q) return DM_E_OUTOFMEMORY;
    q->flags = u32(h); q->index = u32(h + 4);
    q->n = (int)(n / isz);
    q->c = calloc(q->n ? q->n : 1, sizeof(dm_pccurve));
    if (!q->c) { free(q); return DM_E_OUTOFMEMORY; }
    for (int k = 0; k < q->n; k++) {
        uint8_t b[24] = { 0 };
        memcpy(b, c.p + 4 + k * isz, isz < 24 ? isz : 24);
        dm_pccurve *x = &q->c[k];
        x->start = (int32_t)u32(b); x->end = (int32_t)u32(b + 4);
        memcpy(&x->v0, b + 8, 4); memcpy(&x->v1, b + 12, 4);
        x->curve = u32(b + 16); x->flags = u32(b + 20);
    }
    q->next = o->params; o->params = q;
    T->nparam++;
    return DM_OK;
}
/* 773229d3: a 'prol' list: 'proh' (its time format music or reference time), then 'prpl' lists */
static int32_t pcobj_load(dm_paramtrack *T, rd r)
{
    rd c;
    if (!find_chunk(&r, 0, FCC('p','r','o','h'), &c)) return E_PARAMCTL;
    uint8_t h[0x34] = { 0 };
    memcpy(h, c.p, (size_t)(c.e - c.p) < sizeof(h) ? (size_t)(c.e - c.p) : sizeof(h));
    dm_pcobj *o = calloc(1, sizeof(*o));
    if (!o) return DM_E_OUTOFMEMORY;
    o->flags = u32(h); get_guid(&o->fmt, h + 4);
    o->pch = u32(h + 0x14); o->stage = u32(h + 0x18); o->buffer = u32(h + 0x1c);
    get_guid(&o->obj, h + 0x20); o->index = u32(h + 0x30);
    if (!guid_eq(&o->fmt, &GUID_TIME_MUSIC) && !guid_eq(&o->fmt, &GUID_TIME_REFERENCE)) { pcobj_free(o); return E_PARAMCTL; }
    while (find_chunk(&r, 1, FCC('p','r','p','l'), &c)) {
        int32_t hr = pcparam_load(T, o, c);
        if (hr < 0) { pcobj_free(o); return hr; }
    }
    o->next = T->objs; T->objs = o;
    T->nobj++;
    return DM_OK;
}
/* 77322c8f: the 'prmt' list's 'prol' lists */
dm_paramtrack *dm_paramtrack_load(const uint8_t *p, size_t n, int32_t *hr)
{
    dm_paramtrack *T = calloc(1, sizeof(*T));
    *hr = DM_E_OUTOFMEMORY;
    if (!T) return NULL;
    T->gen = 1;
    *hr = DM_OK;
    rd r = { p, p + n }, c;
    while (*hr >= 0 && find_chunk(&r, 1, FCC('p','r','o','l'), &c)) *hr = pcobj_load(T, c);
    if (*hr < 0) { dm_paramtrack_free(T); return NULL; }
    return T;
}
/* 773227bf: the curves from the first not over before start to the last starting before end, moved to 0; the
 * lists in the same order */
static dm_paramtrack *paramtrack_clone(const dm_paramtrack *T, mtime_t start, mtime_t end)
{
    dm_paramtrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    C->gen = 2;
    dm_pcobj **ot = &C->objs;
    for (const dm_pcobj *o = T->objs; o; o = o->next) {
        dm_pcobj *x = malloc(sizeof(*x));
        if (!x) break;
        *x = *o; x->next = NULL; x->params = NULL;
        dm_pcparam **qt = &x->params;
        for (const dm_pcparam *q = o->params; q; q = q->next) {
            int a = 0, b;
            while (a < q->n && q->c[a].end < start) a++;
            for (b = a; b < q->n && q->c[b].start < end; b++) {}
            dm_pcparam *y = calloc(1, sizeof(*y));
            if (!y) break;
            y->flags = q->flags; y->index = q->index; y->n = b - a;
            y->c = malloc((y->n ? y->n : 1) * sizeof(dm_pccurve));
            if (!y->c) { free(y); break; }
            for (int k = 0; k < y->n; k++) { y->c[k] = q->c[a + k]; y->c[k].start -= start; y->c[k].end -= start; }
            *qt = y; qt = &y->next;
        }
        *ot = x; ot = &x->next;
    }
    C->nobj = T->nobj; C->nparam = T->nparam;
    return C;
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
                if (!ts) return;
                ts->active = 1;
                if (id == FCC('t','i','m','s')) load_tims(ts, c);
                else { rd x; uint32_t xid; while (next_chunk(&c, &xid, &x, NULL)) if (xid == FCC('t','i','m','s')) load_tims(ts, x); }
                t.kind = TRK_TIMESIG; t.data = ts;
            } else if (guid_eq(&t.clsid, &CLSID_BandTrack)) { t.data = load_band(c, getcoll, ctx); t.kind = t.data ? TRK_BAND : TRK_OTHER; }
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
            } else if (guid_eq(&t.clsid, &CLSID_MarkerTrack) && id == FCC('L','I','S','T') && type == FCC('M','A','R','K')) {
                int32_t hr;
                t.data = dm_markertrack_load(c.p, (size_t)(c.e - c.p), &hr);
                t.kind = t.data ? TRK_MARKER : TRK_OTHER;
            } else if (guid_eq(&t.clsid, &CLSID_LyricsTrack) && id == FCC('L','I','S','T') && type == FCC('l','y','r','t')) {
                int32_t hr;
                t.data = dm_lyricstrack_load(c.p, (size_t)(c.e - c.p), &hr);
                if (!t.data) { s->partial = 1; return; }    /* (a track whose Load fails is left out) */
                t.kind = TRK_LYRICS;
            } else if (guid_eq(&t.clsid, &CLSID_SegTriggerTrack) && id == FCC('L','I','S','T') && type == FCC('s','e','g','t')) {
                int32_t hr;
                t.data = dm_segtrigtrack_load(c.p, (size_t)(c.e - c.p), cb, &hr);
                if (!t.data) { s->partial = 1; return; }
                t.kind = TRK_SEGTRIG;
            } else if (guid_eq(&t.clsid, &CLSID_ParamControlTrack) && id == FCC('L','I','S','T') && type == FCC('p','r','m','t')) {
                int32_t hr;
                t.data = dm_paramtrack_load(c.p, (size_t)(c.e - c.p), &hr);
                if (!t.data) { s->partial = 1; return; }
                t.kind = TRK_PARAMCTL;
            } else if (guid_eq(&t.clsid, &CLSID_ScriptTrack) && id == FCC('L','I','S','T') && type == FCC('s','c','r','t')) {
                int32_t hr;
                t.data = dm_scripttrack_load(c.p, (size_t)(c.e - c.p), cb, &hr);
                if (!t.data) { s->partial = 1; return; }
                t.kind = TRK_SCRIPT;
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

/* ------------------------------------------------------------------ tracks on their own (CoCreateInstance of a track
 * class, then IPersistStream::Load of its chunk) */
static const struct { const dm_guid *g; int kind; } s_kinds[] = {
    { &CLSID_SeqTrack, TRK_SEQ }, { &CLSID_TempoTrack, TRK_TEMPO }, { &CLSID_TimeSigTrack, TRK_TIMESIG },
    { &CLSID_BandTrack, TRK_BAND }, { &CLSID_SysExTrack, TRK_SYSEX }, { &CLSID_WaveTrack, TRK_WAVE },
    { &CLSID_ChordTrack, TRK_CHORD }, { &CLSID_CommandTrack, TRK_COMMAND }, { &CLSID_StyleTrack, TRK_STYLE },
    { &CLSID_MotifTrack, TRK_MOTIF }, { &CLSID_PatternTrack, TRK_PATTERN }, { &CLSID_ChordMapTrack, TRK_CHORDMAP },
    { &CLSID_SignPostTrack, TRK_SIGNPOST }, { &CLSID_MarkerTrack, TRK_MARKER }, { &CLSID_LyricsTrack, TRK_LYRICS },
    { &CLSID_SegTriggerTrack, TRK_SEGTRIG }, { &CLSID_ParamControlTrack, TRK_PARAMCTL }, { &CLSID_MuteTrack, TRK_MUTE },
    { &CLSID_ScriptTrack, TRK_SCRIPT },
};
int dm_track_class_kind(const dm_guid *clsid)
{
    for (size_t k = 0; k < sizeof(s_kinds) / sizeof(*s_kinds); k++) if (guid_eq(clsid, s_kinds[k].g)) return s_kinds[k].kind;
    return -1;
}
/* a track as its class's constructor leaves it: no items */
dm_track *dm_track_create(const dm_guid *clsid)
{
    int kind = dm_track_class_kind(clsid);
    if (kind < 0) return NULL;
    static const uint8_t none[4];
    rd e = { none, none };
    int32_t hr;
    void *d = NULL;
    switch (kind) {
    case TRK_SEQ: d = load_seq(e); break;
    case TRK_TEMPO: d = load_tempo(e); break;
    case TRK_TIMESIG: { dm_tstrack *ts = calloc(1, sizeof(*ts)); if (ts) ts->active = 1; d = ts; break; }
    case TRK_BAND: d = load_band(e, NULL, NULL); break;
    case TRK_SYSEX: d = load_sysex(e); break;
    case TRK_WAVE: d = load_wavetrack(e, NULL, NULL); break;
    case TRK_CHORD: d = calloc(1, sizeof(dms_chordtrack)); break;
    case TRK_COMMAND: d = calloc(1, sizeof(dms_cmdtrack)); break;
    case TRK_MUTE: d = calloc(1, sizeof(dms_mutetrack)); break;
    case TRK_STYLE: case TRK_MOTIF: dm_style_srand(); d = dms_styletrack_load(none, 0, NULL, NULL, &hr); break;
    case TRK_PATTERN: {                 /* (no pattern: no style either) */
        dms_styletrack *y = calloc(1, sizeof(*y));
        if (y) { y->kind = TRK_PATTERN; y->au_byguid = y->au_all = 1; }
        d = y;
        break;
    }
    case TRK_CHORDMAP: d = dmc_cmtrack_load(none, 0, NULL, NULL, &hr); break;
    case TRK_SIGNPOST: d = dmc_sptrack_load(none, 0, &hr); break;
    case TRK_MARKER: d = dm_markertrack_load(none, 0, &hr); break;
    case TRK_LYRICS: { dm_lyricstrack *y = calloc(1, sizeof(*y)); if (y) y->gen = 1; d = y; break; }
    case TRK_SEGTRIG: d = dm_segtrigtrack_load(none, 0, NULL, &hr); break;
    case TRK_PARAMCTL: d = dm_paramtrack_load(none, 0, &hr); break;
    case TRK_SCRIPT: { dm_scripttrack *y = calloc(1, sizeof(*y)); if (y) y->gen = 1; d = y; break; }
    }
    if (!d) return NULL;
    dm_track *t = dm_track_new(kind, clsid, d);
    if (!t) { dm_track tmp = { .kind = kind, .data = d }; free_track(&tmp); }
    return t;
}
/* whether a track can start playing: a style or pattern track without a style cannot (E_POINTER), nor a motif track
 * without its motif (E_FAIL) */
int dm_track_can_play(const dm_track *t)
{
    const dms_styletrack *y = t->data;
    if (t->kind == TRK_STYLE || t->kind == TRK_PATTERN)
        if (!y || !y->styles || (t->kind == TRK_PATTERN && !y->motif)) return DM_E_POINTER;
    if (t->kind == TRK_MOTIF && (!y || !y->motif)) return DM_E_FAIL;
    return DM_OK;
}
/* Load of the track's own chunk (its header included: 'seqt', LIST 'lyrt', RIFF 'DMPT', ...); what it had before is
 * let go of once the new data is there */
int dm_track_load_chunk(dm_track *t, const uint8_t *p, size_t n, const dm_loadcb *cb)
{
    void *d = NULL;
    int hr = dm_track_parse_chunk(t, p, n, cb, &d);
    if (d) dm_track_set_data(t, d);
    return hr;
}
/* the parse alone (no lock needed: what it reads of shared objects is counted atomically, and a style's events
 * are sorted again under dms_resort_hook); *out = the new data, or NULL with the error */
int dm_track_parse_chunk(const dm_track *t, const uint8_t *p, size_t n, const dm_loadcb *cb, void **out)
{
    *out = NULL;
    if (n < 8) return DMUS_E_CHUNKNOTFOUND_;
    uint32_t id = u32(p), size = u32(p + 4), type = 0;
    rd c = { p + 8, p + 8 + (size < n - 8 ? size : n - 8) };
    if ((id == FCC('R','I','F','F') || id == FCC('L','I','S','T')) && c.e - c.p >= 4) { type = u32(c.p); c.p += 4; }
    dm_getcoll_fn getcoll = cb ? cb->getcoll : NULL;
    void *ctx = cb ? cb->ctx : NULL;
    int32_t hr = DM_OK;
    void *d = NULL;
    switch (t->kind) {
    case TRK_SEQ: if (id == FCC('s','e','q','t')) d = load_seq(c); break;
    case TRK_TEMPO: if (id == FCC('t','e','t','r')) d = load_tempo(c); break;
    case TRK_TIMESIG: {
        dm_tstrack *ts = calloc(1, sizeof(*ts));
        if (!ts) return DM_E_OUTOFMEMORY;
        ts->active = 1;
        if (id == FCC('t','i','m','s')) load_tims(ts, c);
        else { rd x; uint32_t xid; while (next_chunk(&c, &xid, &x, NULL)) if (xid == FCC('t','i','m','s')) load_tims(ts, x); }
        d = ts;
        break;
    }
    case TRK_BAND: d = load_band(c, getcoll, ctx); break;
    case TRK_SYSEX: if (id == FCC('s','y','e','x')) d = load_sysex(c); break;
    case TRK_WAVE: if (type == FCC('w','a','v','t')) d = load_wavetrack(c, cb ? cb->getwave : NULL, ctx); break;
    case TRK_CHORD: if (type == FCC('c','o','r','d')) d = dms_chordtrack_load(c.p, (size_t)(c.e - c.p), &hr); break;
    case TRK_COMMAND: if (id == FCC('c','m','n','d')) d = dms_cmdtrack_load(c.p, (size_t)(c.e - c.p), &hr); break;
    case TRK_STYLE:
        if (type == FCC('s','t','t','r')) { d = dms_styletrack_load(c.p, (size_t)(c.e - c.p), cb ? cb->getstyle : NULL, ctx, &hr); }
        break;
    case TRK_PATTERN: if (type == FCC('D','M','P','T')) d = dms_patterntrack_load(c.p, (size_t)(c.e - c.p), getcoll, ctx, &hr); break;
    case TRK_CHORDMAP: if (type == FCC('p','f','t','r')) d = dmc_cmtrack_load(c.p, (size_t)(c.e - c.p), cb ? cb->getcmap : NULL, ctx, &hr); break;
    case TRK_SIGNPOST: if (id == FCC('s','g','n','p')) d = dmc_sptrack_load(c.p, (size_t)(c.e - c.p), &hr); break;
    case TRK_MARKER: if (type == FCC('M','A','R','K')) d = dm_markertrack_load(c.p, (size_t)(c.e - c.p), &hr); break;
    case TRK_LYRICS: if (type == FCC('l','y','r','t')) d = dm_lyricstrack_load(c.p, (size_t)(c.e - c.p), &hr); break;
    case TRK_SEGTRIG: if (type == FCC('s','e','g','t')) d = dm_segtrigtrack_load(c.p, (size_t)(c.e - c.p), cb, &hr); break;
    case TRK_PARAMCTL: if (type == FCC('p','r','m','t')) d = dm_paramtrack_load(c.p, (size_t)(c.e - c.p), &hr); break;
    case TRK_SCRIPT: if (type == FCC('s','c','r','t')) d = dm_scripttrack_load(c.p, (size_t)(c.e - c.p), cb, &hr); break;
    case TRK_MUTE: if (id == FCC('m','u','t','e')) d = dms_mutetrack_load(c.p, (size_t)(c.e - c.p), &hr); break;
    default: return DM_E_FAIL;
    }
    if (!d) return hr < 0 ? hr : DMUS_E_CHUNKNOTFOUND_;
    *out = d;
    return hr;
}
/* the new data in place of the old (under the lock the states playing the track are guarded by): those states end on
 * the old data and start again on the new (dmime's tracks re-seek when their validate count changes); freeing the
 * old data under them was a use-after-free */
void dm_track_set_data(dm_track *t, void *d)
{
    if (dm_track_replace_hook) dm_track_replace_hook(t, 0);
    if (t->data) free_track(t);
    t->data = d;
    if (dm_track_replace_hook) dm_track_replace_hook(t, 1);
}

/* a segment as CoCreateInstance makes it (dmime's CSegment constructor): no tracks, a length of 1 */
dm_segment *dm_segment_new(void)
{
    dm_segment *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->refs = 1;
    s->length = 1;
    return s;
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
    if (!s) return NULL;
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
    case TRK_MARKER: dm_markertrack_free(t->data); break;
    case TRK_LYRICS: dm_lyricstrack_free(t->data); break;
    case TRK_SEGTRIG: dm_segtrigtrack_free(t->data); break;
    case TRK_PARAMCTL: dm_paramtrack_free(t->data); break;
    case TRK_SCRIPT: dm_scripttrack_free(t->data); break;
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
    dls_addref(d->coll);
    if (g_clonefix) { d->priority = s->priority; d->pbrange = s->pbrange; d->notinfile = s->notinfile; d->smf = s->smf; d->sendpatch = s->sendpatch; }
}
dm_band *dm_band_copy(const dm_band *b);
static dm_band *band_copy(const dm_band *b) { return dm_band_copy(b); }
dm_band *dm_band_copy(const dm_band *b)        /* 77202d94's copy of a band: instruments re-added, so reversed */
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
/* ------------------------------------------------------------------ the marker track (dmime CMarkerTrack) */
static void markers_free(dm_marker *m) { while (m) { dm_marker *n = m->next; free(m); m = n; } }
void dm_markertrack_free(dm_markertrack *T)
{
    if (!T) return;
    markers_free(T->starts); markers_free(T->plays);
    free(T);
}
static void markers_append(dm_marker **l, dm_marker *m) { while (*l) l = &(*l)->next; *l = m; }
/* 77321a28 / 77321958: the record size, then a time a record (a short record fills the time's first bytes) */
static int32_t markers_load(dm_marker **l, rd c)
{
    if (c.e - c.p < 4) return (int32_t)0x88781133;
    uint32_t rs = u32(c.p);
    c.p += 4;
    int32_t left = (int32_t)(c.e - c.p), hr = DM_OK;
    if (!rs) return (int32_t)0x88781133;
    uint32_t take = rs < 5 ? rs : 4, skip = rs < 5 ? 0 : rs - 4;
    while (left > 0) {
        dm_marker *m = calloc(1, sizeof(*m));
        if (!m) { hr = DM_E_OUTOFMEMORY; break; }
        if ((uint32_t)(c.e - c.p) < take) { free(m); hr = (int32_t)0x88781133; break; }
        uint8_t b[4] = { 0 };
        memcpy(b, c.p, take);
        c.p += take;
        m->t = (int32_t)u32(b);
        markers_append(l, m);           /* (prepended, then the list reversed: file order) */
        left -= (int32_t)take;
        if (skip) {
            if ((uint32_t)(c.e - c.p) < skip) { hr = (int32_t)0x88781115; break; }
            c.p += skip;
            left -= (int32_t)skip;
        }
    }
    return hr;
}
/* 77321d13: the 'vals' and 'play' chunks */
dm_markertrack *dm_markertrack_load(const uint8_t *p, size_t n, int32_t *hr)
{
    dm_markertrack *T = calloc(1, sizeof(*T));
    *hr = DM_E_OUTOFMEMORY;
    if (!T) return NULL;
    *hr = DM_OK;
    rd r = { p, p + n }, c;
    uint32_t id;
    while (next_chunk(&r, &id, &c, NULL)) {
        if (id == FCC('v','a','l','s')) *hr = markers_load(&T->starts, c);
        else if (id == FCC('p','l','a','y')) *hr = markers_load(&T->plays, c);
    }
    return T;
}
/* 77321e28: the markers in [start, end) moved to 0, in order; the last one before `start` (none at it) put first, at 0 */
static dm_marker *markers_clone(const dm_marker *m, mtime_t start, mtime_t end)
{
    dm_marker *out = NULL;
    const dm_marker *before = NULL;
    for (; m; m = m->next) {
        const dm_marker *keep = m;
        if (start <= m->t) {
            if (end <= m->t) break;
            if (m->t == start) before = NULL;
            dm_marker *x = calloc(1, sizeof(*x));
            if (x) { x->t = m->t - start; markers_append(&out, x); }
            keep = before;
        }
        before = keep;
    }
    if (before) {
        dm_marker *x = calloc(1, sizeof(*x));
        if (x) { x->next = out; out = x; }
    }
    return out;
}
static void *markertrack_clone(const dm_markertrack *T, mtime_t start, mtime_t end)
{
    dm_markertrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    C->starts = markers_clone(T->starts, start, end);
    C->plays = markers_clone(T->plays, start, end);
    return C;
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
    /* (the lyrics, segment trigger, parameter control and script tracks' Clone takes any range) */
    if ((start < 0 || end < start) && t->kind != TRK_LYRICS && t->kind != TRK_SEGTRIG &&
        t->kind != TRK_PARAMCTL && t->kind != TRK_SCRIPT) { *hr = DM_E_INVALIDARG; return NULL; }
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
    case TRK_MARKER: d = markertrack_clone(t->data, start, end); break;      /* 77321e28 */
    case TRK_LYRICS: d = lyricstrack_clone(t->data, start, end); break;
    case TRK_SEGTRIG: d = segtrigtrack_clone(t->data, start, end); break;
    case TRK_PARAMCTL: d = paramtrack_clone(t->data, start, end); break;
    case TRK_SCRIPT: d = scripttrack_clone(t->data, start, end); break;
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

/* ------------------------------------------------------------------ IDirectMusicTrack8::Join */
#include "dmperf.h"
/* the measures before mtJoin by the context segment's time signatures (dmstyle 77402803 / dmcompos 7728b802).  The
 * carry from one time signature to the next is the step modulo the measure count, as the originals compute it. */
static int32_t join_measures(dm_segment *ctx, uint32_t group, mtime_t mtjoin)
{
    int32_t total = 0, rem = 0;
    if (!ctx) return 0;
    for (mtime_t c = 0; c < mtjoin; ) {
        struct { mtime_t t; uint8_t bpm, beat; uint16_t grids; } ts;
        mtime_t next = 0;
        if (dm_segment_getparam(ctx, PRM_TIMESIG, group, 0, c, &next, &ts) < 0) break;
        mtime_t step = next ? next : mtjoin - c;
        uint32_t ml = (ts.beat ? 0xc00u / ts.beat : 0) * ts.bpm;
        uint32_t n = ml ? (uint32_t)(step + rem) / ml : 0;
        rem = (n & 0xffff) ? (int32_t)step % (int32_t)(n & 0xffff) : 0;
        total += (int32_t)n;
        c += step;
    }
    return total;
}
/* Join: `other`'s data appended at mtjoin (into a clone of [0, mtjoin) when result is asked for).  Tempo 7731caa4,
 * chord 77402abe, command 77404040, style 7741305c, band 772027cc, chord map 7728c90c, signpost 7728b802; the
 * others answer E_NOTIMPL.  In place, the command and signpost tracks drop what they have from mtjoin on, the others
 * keep it (their lists then run out of order).  The chord, command and signpost tracks need the segment (their
 * measure numbers move by its measures before mtjoin). */
int dm_track_join(dm_track *t, const dm_track *other, mtime_t mtjoin, dm_segment *ctx, uint32_t group, dm_track **result)
{
    switch (t->kind) {
    case TRK_TEMPO: case TRK_CHORD: case TRK_COMMAND: case TRK_STYLE: case TRK_BAND: case TRK_CHORDMAP: case TRK_SIGNPOST: break;
    default: return DM_E_NOTIMPL;
    }
    if (!other || other->kind != t->kind) return DM_E_POINTER;     /* (the originals read the other as their own class) */
    if ((t->kind == TRK_CHORD || t->kind == TRK_COMMAND || t->kind == TRK_SIGNPOST) && !ctx) return DM_E_POINTER;
    dm_track *d = t;
    int hr = DM_OK;
    if (result) {
        *result = dm_track_clone(t, 0, mtjoin, &hr);
        if (!*result) return hr < 0 ? hr : DM_E_OUTOFMEMORY;
        d = *result;
    }
    switch (t->kind) {
    case TRK_TEMPO: {                                   /* 7731ca05: appended */
        dm_tempotrack *T = d->data;
        const dm_tempotrack *O = other->data;
        if (!O->n) break;
        dm_tempoitem *x = realloc(T->items, (size_t)(T->n + O->n) * sizeof(*x));
        if (!x) { hr = DM_E_OUTOFMEMORY; goto fail; }
        T->items = x;
        for (int i = 0; i < O->n; i++) { x[T->n].mt = O->items[i].mt + mtjoin; x[T->n].tempo = O->items[i].tempo; T->n++; }
        break;
    }
    case TRK_CHORD: {                                   /* 77402803: appended; the chords' silence is not copied */
        dms_chordtrack *T = d->data;
        int32_t m = join_measures(ctx, group, mtjoin);
        dms_chord **pp = &T->chords;
        while (*pp) pp = &(*pp)->next;
        for (const dms_chord *c = ((const dms_chordtrack *)other->data)->chords; c; c = c->next) {
            dms_chord *n = calloc(1, sizeof(*n));
            if (!n) { hr = DM_E_OUTOFMEMORY; goto fail; }
            memcpy(n->name, c->name, sizeof(n->name));
            n->t = c->t + mtjoin;
            n->measure = (uint16_t)(c->measure + m);
            n->beat = c->beat; n->key = c->key; n->scale = c->scale;
            if (c->nsub && (n->sub = malloc(c->nsub * sizeof(*n->sub)))) { memcpy(n->sub, c->sub, c->nsub * sizeof(*n->sub)); n->nsub = c->nsub; }
            *pp = n; pp = &n->next;
        }
        break;
    }
    case TRK_COMMAND: {                                 /* 77404040: what is before mtjoin, then the other's */
        dms_cmdtrack *T = d->data;
        if (!result) {
            dms_command **pp = &T->cmds;
            while (*pp && (*pp)->t < mtjoin) pp = &(*pp)->next;
            for (dms_command *c = *pp, *n; c; c = n) { n = c->next; free(c); }
            *pp = NULL;
        }
        int32_t m = join_measures(ctx, group, mtjoin);
        dms_command **pp = &T->cmds;
        while (*pp) pp = &(*pp)->next;
        for (const dms_command *c = ((const dms_cmdtrack *)other->data)->cmds; c; c = c->next) {
            dms_command *n = malloc(sizeof(*n));
            if (!n) { hr = DM_E_OUTOFMEMORY; goto fail; }
            *n = *c; n->next = NULL;
            n->t += mtjoin; n->measure = (uint16_t)(n->measure + m);
            *pp = n; pp = &n->next;
        }
        break;
    }
    case TRK_STYLE: {                                   /* 77412d3a: appended (the PChannel list is not updated) */
        dms_styletrack *T = d->data;
        dms_stynode **pp = &T->styles;
        while (*pp) pp = &(*pp)->next;
        for (const dms_stynode *s = ((const dms_styletrack *)other->data)->styles; s; s = s->next) {
            dms_stynode *n = calloc(1, sizeof(*n));
            if (!n) { hr = DM_E_OUTOFMEMORY; goto fail; }
            n->t = s->t + mtjoin; n->style = s->style;
            if (n->style) dms_style_addref(n->style);
            *pp = n; pp = &n->next;
        }
        break;
    }
    case TRK_BAND: {                                    /* 7720202e: a copy of each band, added at its time + mtjoin */
        const dm_bandtrack *O = other->data;
        for (int i = 0; i < O->nbands && hr >= 0; i++) {
            dm_band *b = dm_band_copy(O->bands[i]);
            if (!b) { hr = DM_E_OUTOFMEMORY; goto fail; }
            memset(b->name, 0, sizeof(b->name)); b->hasguid = 0;
            b->logical += mtjoin; b->physical += mtjoin;
            dm_bandtrack_add(d->data, b);
        }
        break;
    }
    case TRK_CHORDMAP: {                                /* 7728c7f5: appended */
        dmc_cmtrack *T = d->data;
        dmc_cmnode **pp = &T->items;
        while (*pp) pp = &(*pp)->next;
        for (const dmc_cmnode *c = ((const dmc_cmtrack *)other->data)->items; c; c = c->next) {
            dmc_cmnode *n = calloc(1, sizeof(*n));
            if (!n) { hr = DM_E_OUTOFMEMORY; goto fail; }
            n->t = c->t + mtjoin; n->map = c->map;
            if (n->map) dmc_chordmap_addref(n->map);
            *pp = n; pp = &n->next;
        }
        break;
    }
    case TRK_SIGNPOST: {                                /* 7728b802: what is before mtjoin, then the other's */
        dmc_sptrack *T = d->data;
        if (!result) {
            dmc_sp **pp = &T->items;
            while (*pp && (*pp)->t < mtjoin) pp = &(*pp)->next;
            for (dmc_sp *c = *pp, *n; c; c = n) { n = c->next; free(c); }
            *pp = NULL;
        }
        int32_t m = join_measures(ctx, group, mtjoin);
        dmc_sp **pp = &T->items;
        while (*pp) pp = &(*pp)->next;
        for (const dmc_sp *c = ((const dmc_sptrack *)other->data)->items; c; c = c->next) {
            dmc_sp *n = malloc(sizeof(*n));
            if (!n) { hr = DM_E_OUTOFMEMORY; goto fail; }
            *n = *c; n->next = NULL;
            n->t += mtjoin; n->measure = (uint16_t)(n->measure + m);
            *pp = n; pp = &n->next;
        }
        break;
    }
    }
    return hr;
fail:                                   /* (a clone made for the result is let go of) */
    if (result) { dm_track_release(*result); *result = NULL; }
    return hr;
}
