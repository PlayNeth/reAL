/* reAL-Music: chord maps and the chord map and signpost tracks (dmcompos: loading, parameters, Clone, Save).
 * The addresses are XP dmcompos's. */
#include "dmcomp.h"
#include "dmsty.h"      /* dms_msort */
#include <stdlib.h>
#include <string.h>

#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define E_FAIL_ ((int32_t)0x80004005)
#define E_INVALIDARG_ ((int32_t)0x80070057)
#define E_OUTOFMEMORY_ ((int32_t)0x8007000e)
#define DMUS_E_CANNOTREAD_ ((int32_t)0x88781133)
#define DMUS_E_INVALID_CHORDMAP ((int32_t)0x88781162)
#define DMUS_E_CHUNKNOTFOUND_ ((int32_t)0x88781117)

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void get_guid(dm_guid *g, const uint8_t *p) { g->d1 = u32(p); g->d2 = u16(p + 4); g->d3 = u16(p + 6); memcpy(g->d4, p + 8, 8); }

/* a chunk reader over a list's data; `end` is the whole file's end (dmcompos's reads run past a chunk's end) */
typedef struct { const uint8_t *p, *e, *end; } rd;
typedef struct { uint32_t id, size, type; rd data; } chk;
static int next_chunk(rd *r, chk *c)
{
    if (r->e - r->p < 8) return 0;
    uint32_t sz = u32(r->p + 4);
    c->id = u32(r->p);
    c->size = sz;
    const uint8_t *d = r->p + 8;
    if ((size_t)(r->e - d) < sz) sz = (uint32_t)(r->e - d);
    c->data.p = d; c->data.e = d + sz; c->data.end = r->end;
    c->type = sz >= 4 ? u32(d) : 0;
    r->p = d + sz + (sz & 1);
    if (r->p > r->e) r->p = r->e;
    return 1;
}
static int is_list(const chk *c, uint32_t type) { return (c->id == FCC('L','I','S','T') || c->id == FCC('R','I','F','F')) && c->data.e - c->data.p >= 4 && c->type == type; }
static rd list_body(const chk *c) { rd r = { c->data.p + 4, c->data.e, c->data.end }; return r; }
/* a stream Read of n bytes at the chunk's data: it is not bounded by the chunk, only by the file (a read past the
 * file's end fails, as the loader's file stream's does); returns the count */
static size_t stream_read(const chk *c, void *dst, size_t n)
{
    size_t k = (size_t)(c->data.end - c->data.p) < n ? (size_t)(c->data.end - c->data.p) : n;
    memcpy(dst, c->data.p, k);
    return k;
}
/* a read bounded by the chunk (dmcompos's RIFF parser), returning the count */
static size_t chunk_read(const chk *c, void *dst, size_t n)
{
    size_t k = (size_t)(c->data.e - c->data.p) < n ? (size_t)(c->data.e - c->data.p) : n;
    memcpy(dst, c->data.p, k);
    return k;
}

/* ------------------------------------------------------------------ chord maps */
static void chord_clear(dmc_chord *c)          /* 772840ed */
{
    for (dmc_subnode *s = c->subs, *n; s; s = n) { n = s->next; free(s); }   /* (the subchords stay: 77281d02 only drops the pointer) */
    c->subs = NULL;
}
static void chord_free_subs(dmc_chord *c) { chord_clear(c); }

dmc_chordmap *dmc_chordmap_new(void)
{
    dmc_chordmap *m = calloc(1, sizeof(*m));
    if (m) m->refs = 1;
    return m;
}
/* 77288e22: everything loaded goes (the subchords themselves are never freed, as in dmcompos) */
static void chordmap_clear(dmc_chordmap *m)
{
    m->loaded = 0;
    memset(&m->guid, 0, sizeof(m->guid));
    for (dmc_entry *e = m->entries, *n; e; e = n) {
        n = e->next;
        chord_free_subs(&e->chord);
        for (dmc_next *x = e->nexts, *y; x; x = y) { y = x->next; free(x); }
        free(e);
    }
    m->entries = NULL;
    for (int i = 0; i < 24; i++) {
        for (dmc_palchord *c = m->palette[i], *n; c; c = n) { n = c->next; chord_free_subs(&c->c); free(c); }
        m->palette[i] = NULL;
    }
    for (dmc_signpost *s = m->signposts, *n; s; s = n) {
        n = s->next;
        chord_free_subs(&s->chord); chord_free_subs(&s->cadence[0]); chord_free_subs(&s->cadence[1]);
        free(s);
    }
    m->signposts = NULL;
}
void dmc_chordmap_addref(dmc_chordmap *m) { if (m) m->refs++; }
void dmc_chordmap_release(dmc_chordmap *m)
{
    if (!m || --m->refs > 0) return;
    chordmap_clear(m);
    free(m);
}

/* the subchord table of a 'chdt' chunk (in file order) */
typedef struct { dmc_sub **v; int n; } subtab;

/* 77288159: a 'chrd' list: INAM (24 bytes read whatever the chunk's size; a name without its NUL runs on into the
 * chunk header the parser keeps after the buffer) and sbcn (four subchord ids, read as 8 bytes).  S_OK only when
 * both were read. */
static int32_t load_chord(dmc_chord *ch, rd r, const subtab *tab)
{
    if (!tab || !tab->v) return E_INVALIDARG_;
    int32_t hn = E_FAIL_, hs = E_FAIL_;
    chk c;
    while (next_chunk(&r, &c)) {
        if (c.id == FCC('I','N','A','M')) {
            uint8_t b[24 + 8];
            memset(b, 0, sizeof(b));
            if (stream_read(&c, b, 24) < 24) continue;     /* (a read past the file's end fails: no name) */
            b[24] = 'I'; b[25] = 'N'; b[26] = 'A'; b[27] = 'M';
            b[28] = (uint8_t)c.size; b[29] = (uint8_t)(c.size >> 8); b[30] = (uint8_t)(c.size >> 16); b[31] = (uint8_t)(c.size >> 24);
            int k = 0;
            for (; k < DMC_NAME - 1 && k < 16 && u16(b + 2 * k); k++) ch->name[k] = u16(b + 2 * k);
            ch->name[k] = 0;
            ch->hasname = 1;
            hn = 0;
        } else if (c.id == FCC('s','b','c','n')) {
            uint8_t b[8] = { 0 };
            if (stream_read(&c, b, 8) < 8) continue;
            hs = 0;
            chord_clear(ch);
            dmc_subnode *head = NULL;
            for (int i = 3; i >= 0; i--) {
                uint16_t id = u16(b + 2 * i);
                dmc_subnode *s = calloc(1, sizeof(*s));
                if (!s) continue;
                s->sub = id < tab->n ? tab->v[id] : NULL;
                if (s->sub) s->sub->refs++;
                s->next = head;
                head = s;
            }
            ch->subs = head;
        }
    }
    return hn == 0 && hs == 0 ? 0 : E_FAIL_;
}

/* 772885e6: a 'choe' list (the entry put first); its connections to other entries resolved later */
static int32_t load_entry(dmc_chordmap *m, rd r, const subtab *tab, int16_t *maxid)
{
    dmc_entry *e = calloc(1, sizeof(*e));
    if (!e) return E_OUTOFMEMORY_;
    e->chord.hasname = 1;                       /* 7728bb4e(""): an empty name */
    e->next = m->entries;
    m->entries = e;
    int32_t hr = 0;
    chk c;
    while (next_chunk(&r, &c) && hr == 0) {
        if (c.id == FCC('L','I','S','T')) {
            if (c.type == FCC('c','h','r','d')) hr = load_chord(&e->chord, list_body(&c), tab);
        } else if (c.id == FCC('c','h','e','h')) {
            uint8_t b[8];
            if (chunk_read(&c, b, 8) != 8) { hr = E_FAIL_; break; }
            e->flags = u32(b);
            e->connid = (int16_t)u16(b + 4);
            if (*maxid < e->connid) *maxid = e->connid;
        } else if (c.id == FCC('n','c','s','q')) {
            uint8_t b2[2];
            if (chunk_read(&c, b2, 2) != 2) { hr = E_FAIL_; break; }
            uint16_t sz = u16(b2);
            int32_t left = (int32_t)c.size - 2;
            rd x = { c.data.p + 2, c.data.e, c.data.end };
            for (; left > 0; left -= sz) {
                uint8_t b[16] = { 0 };
                size_t want = sz < sizeof(b) ? sz : sizeof(b);
                if (!sz || (size_t)(x.e - x.p) < sz) { hr = E_FAIL_; break; }
                memcpy(b, x.p, want);
                x.p += sz;
                int16_t cid = (int16_t)u16(b + 10);
                if (!cid) continue;
                dmc_next *n = calloc(1, sizeof(*n));
                if (!n) { hr = E_OUTOFMEMORY_; break; }
                n->flags = u32(b); n->weight = u16(b + 4); n->minbeats = u16(b + 6); n->maxbeats = u16(b + 8);
                n->connid = cid;
                n->next = e->nexts;
                e->nexts = n;
            }
            if (hr == 0 && left != 0) hr = E_FAIL_;
        }
    }
    return hr;
}

/* 7728837e: a 'spst' list (appended): spsh, its chord and the cadence chords (with only DMUS_SPOSTCADENCEF_2 the
 * cadence list's chords both go to the second) */
static int32_t load_signpost(dmc_chordmap *m, rd r, const subtab *tab)
{
    dmc_signpost *s = calloc(1, sizeof(*s));
    if (!s) return E_OUTOFMEMORY_;
    dmc_signpost **pp = &m->signposts;
    while (*pp) pp = &(*pp)->next;
    *pp = s;
    int32_t hr = 0;
    chk c;
    while (next_chunk(&r, &c) && hr == 0) {
        if (c.id == FCC('L','I','S','T')) {
            if (c.type == FCC('c','h','r','d')) hr = load_chord(&s->chord, list_body(&c), tab);
            else if (c.type == FCC('c','a','d','e')) {
                rd x = list_body(&c);
                chk y;
                for (int k = 0; k < 2 && hr == 0 && next_chunk(&x, &y); k++) {
                    if (y.type != FCC('c','h','r','d') || y.data.e - y.data.p < 4) continue;
                    int idx = !(s->flags & 2) && (s->flags & 4) ? 1 : k;
                    hr = load_chord(&s->cadence[idx], list_body(&y), tab);
                }
            }
        } else if (c.id == FCC('s','p','s','h')) {
            uint8_t b[8];
            if (chunk_read(&c, b, 8) != 8) return E_FAIL_;
            s->chords = u32(b); s->flags = u32(b + 4);
        }
    }
    return hr;
}

/* 7728883c: the form's chunks.  The result is the status of the last palette chord, guid, perh or chdt read. */
static int32_t chordmap_parse(dmc_chordmap *m, rd r)
{
    int32_t hr = 0;
    subtab tab = { NULL, 0 };
    chk c;
    while (next_chunk(&r, &c)) {
        if (c.id == FCC('L','I','S','T')) {
            if (c.type == FCC('c','h','p','l')) {
                rd x = list_body(&c);
                chk y;
                for (int i = 0; i < 24 && next_chunk(&x, &y); i++) {
                    if (!is_list(&y, FCC('c','h','r','d'))) continue;
                    dmc_palchord *pc = calloc(1, sizeof(*pc));
                    if (!pc) continue;
                    pc->next = m->palette[i];
                    m->palette[i] = pc;
                    hr = load_chord(&pc->c, list_body(&y), &tab);
                }
            } else if (c.type == FCC('c','m','a','p')) {
                rd x = list_body(&c);
                chk y;
                int16_t maxid = 0;
                while (next_chunk(&x, &y))
                    if (is_list(&y, FCC('c','h','o','e'))) load_entry(m, list_body(&y), &tab, &maxid);
                dmc_entry **byid = calloc((size_t)maxid + 1, sizeof(*byid));
                if (!byid) { hr = E_OUTOFMEMORY_; break; }
                int bad = 0;
                for (dmc_entry *e = m->entries; e && !bad; e = e->next) {
                    if (e->connid < 0 || maxid < e->connid) bad = 1;
                    else byid[e->connid] = e;
                }
                for (dmc_entry *e = m->entries; e && !bad; e = e->next)
                    for (dmc_next *n = e->nexts; n; n = n->next) {
                        if (n->connid < 0 || maxid < n->connid) { bad = 1; break; }
                        n->to = byid[n->connid];       /* (an id no entry has is a stale pointer in dmcompos) */
                    }
                free(byid);
                if (bad) { hr = DMUS_E_INVALID_CHORDMAP; break; }
            } else if (c.type == FCC('s','p','s','q')) {
                rd x = list_body(&c);
                chk y;
                while (next_chunk(&x, &y))
                    if (is_list(&y, FCC('s','p','s','t'))) load_signpost(m, list_body(&y), &tab);
            }
        } else if (c.id == FCC('g','u','i','d')) {
            uint32_t want = c.size > 15 ? 16 : c.size;
            uint8_t b[16] = { 0 };
            if (chunk_read(&c, b, want) != want) { hr = DMUS_E_CANNOTREAD_; break; }
            /* (a short guid fills only the start) */
            uint8_t cur[16];
            cur[0] = (uint8_t)m->guid.d1; cur[1] = (uint8_t)(m->guid.d1 >> 8); cur[2] = (uint8_t)(m->guid.d1 >> 16); cur[3] = (uint8_t)(m->guid.d1 >> 24);
            cur[4] = (uint8_t)m->guid.d2; cur[5] = (uint8_t)(m->guid.d2 >> 8); cur[6] = (uint8_t)m->guid.d3; cur[7] = (uint8_t)(m->guid.d3 >> 8);
            memcpy(cur + 8, m->guid.d4, 8);
            memcpy(cur, b, want);
            get_guid(&m->guid, cur);
            hr = 0;
        } else if (c.id == FCC('p','e','r','h')) {
            uint8_t b[0x30 + 8];
            memset(b, 0, sizeof(b));
            b[40] = b[41] = b[42] = b[43] = 0xff;         /* the scale is -1 when the chunk is short */
            uint32_t want = c.size > 0x2f ? 0x30 : c.size;
            if (chunk_read(&c, b, want) != want) { hr = DMUS_E_CANNOTREAD_; break; }
            uint32_t scale = u32(b + 40), flags = u32(b + 44);
            if (flags >> 16) flags = 0;
            if (!(flags & 1) && (scale >> 24)) { hr = DMUS_E_INVALID_CHORDMAP; break; }
            /* the name: wcslen over the 20 characters, then the scale and flags that follow on the stack */
            b[44] = (uint8_t)flags; b[45] = (uint8_t)(flags >> 8); b[46] = (uint8_t)(flags >> 16); b[47] = (uint8_t)(flags >> 24);
            int k = 0;
            for (; k < 24 && u16(b + 2 * k); k++) m->name[k] = u16(b + 2 * k);
            if (k > 23) k = 23;
            m->name[k] = 0;
            m->hasname = 1;
            m->scale = scale;
            m->flags = flags;
            hr = 0;
        } else if (c.id == FCC('c','h','d','t')) {
            uint8_t b2[2];
            if (chunk_read(&c, b2, 2) != 2) { hr = DMUS_E_CANNOTREAD_; break; }
            uint16_t sz = u16(b2);
            int32_t left = (int32_t)c.size - 2;
            rd x = { c.data.p + 2, c.data.e, c.data.end };
            dmc_sub **v = NULL;
            int n = 0;
            for (; left > 0; left -= sz) {
                uint8_t b[20] = { 0 };
                if (!sz || (size_t)(x.e - x.p) < sz) { hr = DMUS_E_CANNOTREAD_; break; }
                memcpy(b, x.p, sz < 20 ? sz : 20);
                x.p += sz;
                dmc_sub *s = calloc(1, sizeof(*s));
                dmc_sub **nv = realloc(v, (n + 1) * sizeof(*nv));
                if (!s || !nv) { free(s); hr = E_OUTOFMEMORY_; break; }
                v = nv;
                s->chordpat = u32(b); s->scalepat = u32(b + 4); s->invpat = u32(b + 8);
                s->chordroot = b[12]; s->scaleroot = b[13]; s->cflags = u16(b + 14); s->levels = u32(b + 16);
                v[n++] = s;
            }
            if (hr < 0) { free(v); break; }
            if (left != 0) { free(v); hr = E_FAIL_; break; }
            free(tab.v);
            tab.v = v; tab.n = n;
            hr = 0;
        }
    }
    free(tab.v);
    return hr;
}

int dmc_chordmap_load(dmc_chordmap *m, const uint8_t *p, size_t n)
{
    chordmap_clear(m);
    rd r = { p, p + n, p + n };
    int32_t hr = chordmap_parse(m, r);
    if (hr >= 0) m->loaded = 1;
    return hr;
}

/* 77287eea: the guid (DMUS_OBJ_OBJECT) and the perh name (DMUS_OBJ_NAME, cut at 16 characters) */
uint32_t dmc_chordmap_parse(const uint8_t *p, size_t n, dm_guid *guid, uint16_t name[17])
{
    uint32_t valid = 0;
    rd r = { p, p + n, p + n };
    chk c;
    name[0] = 0;
    while (next_chunk(&r, &c)) {
        if (c.id == FCC('g','u','i','d')) {
            if (c.size < 16 || c.data.e - c.data.p < 16) break;
            get_guid(guid, c.data.p);
            valid |= 1;
        } else if (c.id == FCC('p','e','r','h')) {
            uint32_t want = c.size > 0x2f ? 0x30 : c.size;
            uint8_t b[0x30 + 2];
            memset(b, 0, sizeof(b));
            if (chunk_read(&c, b, want) != want) break;
            int k = 0;
            for (; k < 16 && u16(b + 2 * k); k++) name[k] = u16(b + 2 * k);
            name[k] = 0;
            if (name[0]) valid |= 4;
        }
    }
    return valid;
}

/* ------------------------------------------------------------------ the chord map track */
void dmc_cmtrack_free(dmc_cmtrack *T)
{
    if (!T) return;
    for (dmc_cmnode *x = T->items, *n; x; x = n) { n = x->next; dmc_chordmap_release(x->map); free(x); }
    free(T);
}

static void wstr_utf8(char *out, size_t max, rd c)
{
    size_t k = 0;
    for (const uint8_t *p = c.p; p + 1 < c.e && k + 4 < max; p += 2) {
        uint32_t ch = u16(p);
        if (!ch) break;
        if (ch < 0x80) out[k++] = (char)ch;
        else if (ch < 0x800) { out[k++] = (char)(0xc0 | ch >> 6); out[k++] = (char)(0x80 | (ch & 0x3f)); }
        else { out[k++] = (char)(0xe0 | ch >> 12); out[k++] = (char)(0x80 | ((ch >> 6) & 0x3f)); out[k++] = (char)(0x80 | (ch & 0x3f)); }
    }
    out[k] = 0;
}

static int cmnode_cmp(const void *a, const void *b) { return ((const dmc_cmnode *)a)->t < ((const dmc_cmnode *)b)->t; }

/* 7728c9e9 / 7728c897 / 7728c5df: 'pfrf' lists ('stmp', a DMRF the loader resolves), appended, then sorted by time;
 * an item whose reference fails is dropped (and fails the Load) */
dmc_cmtrack *dmc_cmtrack_load(const uint8_t *p, size_t n, dm_getchordmap_fn get, void *ctx, int32_t *hr)
{
    dmc_cmtrack *T = calloc(1, sizeof(*T));
    *hr = 0;
    if (!T) { *hr = E_OUTOFMEMORY_; return NULL; }
    rd r = { p, p + n, p + n };
    chk c;
    while (next_chunk(&r, &c)) {
        if (!is_list(&c, FCC('p','f','r','f'))) continue;
        dmc_cmnode *x = calloc(1, sizeof(*x));
        if (!x) { *hr = E_OUTOFMEMORY_; break; }
        rd y = list_body(&c);
        chk d;
        int32_t h = 0;
        while (next_chunk(&y, &d)) {
            if (is_list(&d, FCC('D','M','R','F'))) {
                dm_guid g; int hasg = 0;
                char file[520] = "", name[260] = "";
                rd z = list_body(&d);
                chk e;
                while (next_chunk(&z, &e)) {
                    if (e.id == FCC('g','u','i','d') && e.data.e - e.data.p >= 16) { get_guid(&g, e.data.p); hasg = 1; }
                    else if (e.id == FCC('f','i','l','e')) wstr_utf8(file, sizeof(file), e.data);
                    else if (e.id == FCC('n','a','m','e')) wstr_utf8(name, sizeof(name), e.data);
                }
                dmc_chordmap *m = get ? get(ctx, hasg ? &g : NULL, file[0] ? file : NULL, name[0] ? name : NULL) : NULL;
                if (!m) h = DMUS_E_NOT_FOUND;   /* (the loader's failure) */
                else { h = 0; dmc_chordmap_release(x->map); x->map = m; }
            } else if (d.id == FCC('s','t','m','p')) {
                if (d.data.e - d.data.p < 4) { h = E_FAIL_; break; }
                x->t = (int32_t)u32(d.data.p);
            }
        }
        *hr = h;
        if (h < 0) { dmc_chordmap_release(x->map); free(x); continue; }
        dmc_cmnode **pp = &T->items;
        while (*pp) pp = &(*pp)->next;
        *pp = x;
    }
    T->items = dms_msort(T->items, offsetof(dmc_cmnode, next), cmnode_cmp);
    return T;
}

/* 7728c49d: the items in [start, end) moved by -start; the last one before start (unless one is at start) put first
 * at 0 */
dmc_cmtrack *dmc_cmtrack_clone(const dmc_cmtrack *T, mtime_t start, mtime_t end)
{
    dmc_cmtrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    const dmc_cmnode *last = NULL;
    dmc_cmnode **pp = &C->items;
    for (const dmc_cmnode *x = T->items; x; x = x->next) {
        if (x->t < start) { last = x; continue; }
        if (end <= x->t) break;
        if (x->t == start) last = NULL;
        dmc_cmnode *y = calloc(1, sizeof(*y));
        if (!y) continue;
        y->t = x->t - start; y->map = x->map; dmc_chordmap_addref(y->map);
        *pp = y; pp = &y->next;
    }
    if (last) {
        dmc_cmnode *y = calloc(1, sizeof(*y));
        if (y) { y->map = last->map; dmc_chordmap_addref(y->map); y->next = C->items; C->items = y; }
    }
    return C;
}

/* 7728c1d1: the first item's map whatever its time, then the latest at or before mt (items without a map are
 * passed over); *next to the first later item with a map */
int dmc_cmtrack_getparam(const dmc_cmtrack *T, mtime_t mt, mtime_t *next, dmc_chordmap **out)
{
    if (!T->items) return DMUS_E_NOT_FOUND;
    dmc_chordmap *m = T->items->map;
    const dmc_cmnode *x = T->items->next;
    for (; x && (x->t <= mt || !x->map); x = x->next) if (x->map) m = x->map;
    if (!m) return DM_E_POINTER;
    dmc_chordmap_addref(m);
    *out = m;
    if (next) *next = x ? x->t - mt : 0;
    return DM_OK;
}

/* 7728c327: inserted before the first item at or after mt; one at the same time is replaced */
int dmc_cmtrack_setparam(dmc_cmtrack *T, mtime_t mt, dmc_chordmap *m)
{
    dmc_cmnode *y = calloc(1, sizeof(*y));
    if (!y) return DM_E_OUTOFMEMORY;
    y->t = mt; y->map = m; dmc_chordmap_addref(m);
    dmc_cmnode *prev = NULL, *cur = T->items;
    while (cur && cur->t < mt) { prev = cur; cur = cur->next; }
    if (prev) prev->next = y; else T->items = y;
    y->next = cur;
    if (cur && cur->t == mt) {
        y->next = cur->next;
        dmc_chordmap_release(cur->map);
        free(cur);
    }
    return DM_OK;
}

/* ------------------------------------------------------------------ the signpost track */
void dmc_sptrack_free(dmc_sptrack *T)
{
    if (!T) return;
    for (dmc_sp *x = T->items, *n; x; x = n) { n = x->next; free(x); }
    free(T);
}
static int sp_cmp(const void *a, const void *b) { return ((const dmc_sp *)a)->t < ((const dmc_sp *)b)->t; }

/* 7728b2be: 'sgnp': the record size, then DMUS_IO_SIGNPOST records (appended, then sorted by time).  A record larger
 * than 12 bytes skips the rest of the chunk less 12, not of the record (as dmcompos does). */
dmc_sptrack *dmc_sptrack_load(const uint8_t *p, size_t n, int32_t *hr)
{
    dmc_sptrack *T = calloc(1, sizeof(*T));
    *hr = E_FAIL_;
    if (!T) { *hr = E_OUTOFMEMORY_; return NULL; }
    if (n < 4) return T;
    uint32_t sz = u32(p);
    int32_t left = (int32_t)n - 4;
    const uint8_t *q = p + 4, *e = p + n;
    if (!sz || (uint32_t)left % sz) return T;
    dmc_sp **pp = &T->items;
    while (left > 0) {
        dmc_sp *x = calloc(1, sizeof(*x));
        if (!x) break;
        uint8_t b[12] = { 0 };
        if (sz < 13) {
            memcpy(b, q, (size_t)(e - q) < sz ? (size_t)(e - q) : sz);
            q += sz;
        } else {
            memcpy(b, q, (size_t)(e - q) < 12 ? (size_t)(e - q) : 12);
            q += 12;
            uint32_t sk = (uint32_t)left < 12 ? 0 : (uint32_t)left - 12;
            q = (size_t)(e - q) < sk ? e : q + sk;
        }
        x->t = (int32_t)u32(b); x->chords = u32(b + 4); x->measure = u16(b + 8);
        *pp = x; pp = &x->next;
        left -= (int32_t)sz;
    }
    if (left == 0) {
        *hr = 0;
        T->items = dms_msort(T->items, offsetof(dmc_sp, next), sp_cmp);
    }
    return T;
}

/* 7728b155: the items in [start, end) moved by -start, their measures counted from the first one's; the last one
 * before start (unless one is at start) put first at 0, measure 0 */
dmc_sptrack *dmc_sptrack_clone(const dmc_sptrack *T, mtime_t start, mtime_t end)
{
    dmc_sptrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    const dmc_sp *last = NULL;
    int first = 1;
    uint16_t m0 = 0;
    dmc_sp **pp = &C->items;
    for (const dmc_sp *x = T->items; x; x = x->next) {
        if (x->t < start) { last = x; continue; }
        if (end <= x->t) break;
        if (x->t == start) last = NULL;
        if (first) { m0 = x->measure; first = 0; }
        dmc_sp *y = calloc(1, sizeof(*y));
        if (!y) continue;
        y->t = x->t - start; y->measure = (uint16_t)(x->measure - m0); y->chords = x->chords;
        *pp = y; pp = &y->next;
    }
    if (last) {
        dmc_sp *y = calloc(1, sizeof(*y));
        if (y) { y->chords = last->chords; y->next = C->items; C->items = y; }
    }
    return C;
}

uint8_t *dmc_sptrack_save(const dmc_sptrack *T, size_t *n)
{
    size_t k = 0;
    for (const dmc_sp *x = T->items; x; x = x->next) k++;
    uint8_t *b = malloc(8 + 4 + 12 * k);
    if (!b) return NULL;
    uint32_t sz = (uint32_t)(4 + 12 * k);
    memcpy(b, "sgnp", 4);
    b[4] = (uint8_t)sz; b[5] = (uint8_t)(sz >> 8); b[6] = (uint8_t)(sz >> 16); b[7] = (uint8_t)(sz >> 24);
    b[8] = 12; b[9] = b[10] = b[11] = 0;
    uint8_t *q = b + 12;
    for (const dmc_sp *x = T->items; x; x = x->next, q += 12) {
        uint32_t t = (uint32_t)x->t;
        q[0] = (uint8_t)t; q[1] = (uint8_t)(t >> 8); q[2] = (uint8_t)(t >> 16); q[3] = (uint8_t)(t >> 24);
        q[4] = (uint8_t)x->chords; q[5] = (uint8_t)(x->chords >> 8); q[6] = (uint8_t)(x->chords >> 16); q[7] = (uint8_t)(x->chords >> 24);
        q[8] = (uint8_t)x->measure; q[9] = (uint8_t)(x->measure >> 8); q[10] = q[11] = 0;
    }
    *n = 8 + sz;
    return b;
}
