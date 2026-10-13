/* reAL-Music: the chord, command and mute tracks of dmstyle (loading, parameters, Clone).  Notes in
 * docs/dmstyle_notes.md; the addresses are XP dmstyle's. */
#include "dmsty.h"
#include <stdlib.h>
#include <string.h>

#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define E_FAIL_ ((int32_t)0x80004005)
#define DMUS_E_CHUNKNOTFOUND_ ((int32_t)0x88781117)
#define DMUS_E_NOT_FOUND_ ((int32_t)0x88781161)

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ------------------------------------------------------------------ the merge sort */
#define NEXT(e) (*(void **)((char *)(e) + off))
/* 77402021: the list dealt alternately into two, each reversed */
static void msplit(void *e, size_t off, void **a, void **b)
{
    void *pa = NULL, *pb = NULL;
    for (;;) {
        *a = e;
        void *n = NEXT(e);
        NEXT(e) = pa;
        pa = e;
        if (!n) return;
        *b = n;
        e = NEXT(n);
        NEXT(n) = pb;
        pb = n;
        if (!e) return;
    }
}
/* 77402056: merged, taking b's element on a tie */
static void *mmerge(void *a, void *b, size_t off, int (*cmp)(const void *, const void *))
{
    if (!b) return a;
    void *res = NULL, *tail = NULL;
    while (a) {
        if (!b) break;
        void *take;
        if (!cmp(a, b)) { take = b; b = NEXT(b); }
        else { take = a; a = NEXT(a); }
        NEXT(take) = NULL;
        if (tail) NEXT(tail) = take; else res = take;
        tail = take;
    }
    if (a) { NEXT(tail) = a; return res; }
    NEXT(tail) = b;
    return res;
}
void *dms_msort(void *head, size_t off, int (*cmp)(const void *, const void *))
{
    if (!head || !NEXT(head)) return head;
    void *a = head, *b = head;
    msplit(head, off, &a, &b);
    void *sb = dms_msort(b, off, cmp);
    void *sa = dms_msort(a, off, cmp);
    return mmerge(sa, sb, off, cmp);
}
#undef NEXT

/* a list appended to (dmstyle's AddTail) */
#define APPEND(head, e) do { if (!(head)) (head) = (e); else { __typeof__(e) _p = (head); while (_p->next) _p = _p->next; \
                             _p->next = (e); } } while (0)

/* a chunk reader over a list's data */
typedef struct { const uint8_t *p, *e; } rd;
static int next_chunk(rd *r, uint32_t *id, rd *data, uint32_t *size)
{
    if (r->e - r->p < 8) return 0;
    uint32_t sz = u32(r->p + 4);
    *id = u32(r->p);
    *size = sz;
    const uint8_t *d = r->p + 8;
    if ((size_t)(r->e - d) < sz) sz = (uint32_t)(r->e - d);
    data->p = d; data->e = d + sz;
    r->p = d + sz + (sz & 1);
    if (r->p > r->e) r->p = r->e;
    return 1;
}
/* a read of n bytes from a chunk (short reads leave the rest of dst as it was) */
static size_t take(rd *r, void *dst, size_t n)
{
    size_t k = (size_t)(r->e - r->p) < n ? (size_t)(r->e - r->p) : n;
    memcpy(dst, r->p, k);
    r->p += k;
    return k;
}
static void skip(rd *r, size_t n) { r->p = (size_t)(r->e - r->p) < n ? r->e : r->p + n; }

/* ------------------------------------------------------------------ chord track */
static void chord_free(dms_chord *c) { if (c) { free(c->sub); free(c); } }
void dms_chordtrack_free(dms_chordtrack *T)
{
    if (!T) return;
    for (dms_chord *c = T->chords, *n; c; c = n) { n = c->next; chord_free(c); }
    free(T);
}
static void chord_addsub(dms_chord *c, const dm_subchord *s)
{
    dm_subchord *n = realloc(c->sub, (c->nsub + 1) * sizeof(*n));
    if (!n) return;
    c->sub = n;
    c->sub[c->nsub++] = *s;
}
/* 77401e21: by measure, then beat */
static int chord_cmp(const void *a, const void *b)
{
    const dms_chord *x = a, *y = b;
    if (x->measure < y->measure) return 1;
    if (x->measure > y->measure) return 0;
    return x->beat < y->beat;
}
/* 77402603: a 'crdb' chunk */
static dms_chord *load_crdb(const dms_chordtrack *T, rd r)
{
    uint8_t buf[40];
    uint32_t sz;
    memset(buf, 0, sizeof(buf));
    if (take(&r, buf, 4) != 4) return NULL;
    sz = u32(buf);
    memset(buf, 0, sizeof(buf));
    if (sz < 0x29) take(&r, buf, sz);
    else { take(&r, buf, 0x28); skip(&r, sz - 0x28); }
    dms_chord *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    for (int i = 0; i < 20; i++) { c->name[i] = u16(buf + 2 * i); if (!c->name[i]) break; }
    c->t = (int32_t)u32(buf + 0x20);
    c->measure = u16(buf + 0x24);
    c->beat = buf[0x26];
    c->key = T->root;
    c->scale = T->scale;
    c->silent = buf[0x27] & 1;
    uint8_t n4[4], s4[4];
    if (take(&r, n4, 4) != 4 || take(&r, s4, 4) != 4) return c;
    uint32_t n = u32(n4), ssz = u32(s4);
    uint8_t sb[0x14];               /* (cleared once per chord: a short record keeps the previous one's rest) */
    memset(sb, 0, sizeof(sb));
    for (; n; n--) {
        if (ssz < 0x15) take(&r, sb, ssz);
        else { take(&r, sb, 0x14); skip(&r, ssz - 0x14); }
        dm_subchord s;
        memset(&s, 0, sizeof(s));
        s.chord = u32(sb); s.scale = u32(sb + 4); s.inversion = u32(sb + 8); s.levels = u32(sb + 12);
        s.chordroot = sb[16]; s.scaleroot = sb[17];
        chord_addsub(c, &s);
    }
    return c;
}
/* 77402e42: the 'cord' list: 'crdh' first, then the 'crdb' chunks */
dms_chordtrack *dms_chordtrack_load(const uint8_t *p, size_t n, int32_t *hr)
{
    rd r = { p, p + n }, c;
    uint32_t id, size;
    *hr = DMUS_E_CHUNKNOTFOUND_;
    if (!next_chunk(&r, &id, &c, &size) || id != FCC('c','r','d','h')) return NULL;
    if (c.e - c.p < 4) { *hr = E_FAIL_; return NULL; }
    dms_chordtrack *T = calloc(1, sizeof(*T));
    if (!T) return NULL;
    uint32_t h = u32(c.p);
    T->root = (uint8_t)(h >> 24);
    if (T->root > 23) T->root %= 24;
    T->scale = h & 0xffffff;
    int32_t left = (int32_t)n - 8 - (int32_t)size;     /* (the list size counts its type: size - 12 - crdh) */
    while (left > 0 && next_chunk(&r, &id, &c, &size)) {
        if (id == FCC('c','r','d','b')) {
            dms_chord *ch = load_crdb(T, c);
            if (!ch) break;
            APPEND(T->chords, ch);
        }
        left -= 8 + (int32_t)size;
    }
    if (left != 0) { *hr = E_FAIL_; dms_chordtrack_free(T); return NULL; }
    T->chords = dms_msort(T->chords, offsetof(dms_chord, next), chord_cmp);
    *hr = 0;
    return T;
}

/* 774045b5: the chord as DMUS_CHORD_KEY.  The original builds it on the stack: the name copied with wcscpy, every
 * subchord at its place in the list (a ninth lands on dwScale and bKey; the untouched rest is the stack's) */
void dms_chord_key(const dms_chord *c, dm_chord_key *k)
{
    uint8_t buf[0x24 + 9 * 0x14];
    memset(buf, 0, sizeof(buf));
    int i = 0;
    for (; c->name[i] && i < 20; i++) { buf[2 * i] = (uint8_t)c->name[i]; buf[2 * i + 1] = (uint8_t)(c->name[i] >> 8); }
    buf[0x20] = (uint8_t)c->measure; buf[0x21] = (uint8_t)(c->measure >> 8);
    buf[0x22] = c->beat;
    buf[0xc8] = c->key;
    buf[0xc9] = c->silent ? 1 : 0;
    buf[0xc4] = (uint8_t)c->scale; buf[0xc5] = (uint8_t)(c->scale >> 8); buf[0xc6] = (uint8_t)(c->scale >> 16);
    buf[0xc7] = (uint8_t)(c->scale >> 24);
    uint32_t n = c->nsub < 9 ? c->nsub : 9;
    for (uint32_t s = 0; s < n; s++) memcpy(buf + 0x24 + 0x14 * s, &c->sub[s], 0x14);
    buf[0x23] = (uint8_t)c->nsub;
    memcpy(k, buf, sizeof(*k));
}

/* 774022ac / 7740230c */
int dms_chord_getparam(const dms_chordtrack *T, int rhythm, mtime_t t, mtime_t *next, void *data)
{
    if (!T->chords) return DMUS_E_NOT_FOUND_;
    if (!rhythm) {
        const dms_chord *c = T->chords, *n = c->next;
        for (; n && n->t <= t; n = n->next) c = n;
        dms_chord_key(c, data);
        if (next) *next = n ? n->t - t : 0;
        return 0;
    }
    dms_rhythmparam *rp = data;
    uint32_t ml = rp->beat ? (uint32_t)(0xc00 / rp->beat) * rp->bpm : 0;
    uint16_t m = (uint16_t)(ml ? (uint32_t)t / ml : 0);
    uint32_t bits = 0;
    const dms_chord *c = T->chords, *after = NULL;
    while (c) {
        after = c->next;
        if (m < c->measure) break;
        if (m == c->measure && !c->silent) bits |= 1u << (c->beat & 31);
        c = after;
    }
    rp->rhythm = bits;
    if (next) *next = after ? after->t - t : 0;     /* sic: the chord after the first one past the measure */
    return 0;
}

/* 77404f94 + 7740503d: a DMUS_CHORD_KEY as a chord */
static dms_chord *chord_from_key(const dm_chord_key *k)
{
    dms_chord *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    for (int i = 0; i < 16 && k->name[i]; i++) c->name[i] = k->name[i];
    c->measure = k->measure;
    c->beat = k->beat;
    c->key = k->key;
    c->scale = k->scale;
    c->silent = k->flags & 1;
    for (int i = 0; i < k->nsub; i++) chord_addsub(c, &k->sub[i]);
    return c;
}
/* 774029a8: SetParam(GUID_ChordParam): the chord at time t (measure and beat 0) replaces one at the same time */
int dms_chord_setparam(dms_chordtrack *T, mtime_t t, const dm_chord_key *k)
{
    dms_chord *c = chord_from_key(k);
    if (!c) return DM_E_OUTOFMEMORY;
    c->measure = 0;
    c->beat = 0;
    c->t = t;
    dms_chord *prev = NULL, *cur = T->chords;
    while (cur && cur->t < t) { prev = cur; cur = cur->next; }
    if (!prev) { c->next = T->chords; T->chords = c; }
    else { prev->next = c; c->next = cur; }
    if (cur && cur->t == t) { c->next = cur->next; cur->next = NULL; chord_free(cur); }
    return 0;
}

/* 77402be0: Clone [start, end): times moved to 0, measures relative to the first chord taken; the last chord before
 * start (if none falls on it) put first at 0 */
static dms_chord *chord_copy(const dms_chord *s)
{
    dms_chord *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    memcpy(c->name, s->name, sizeof(c->name));
    c->t = s->t; c->measure = s->measure; c->beat = s->beat; c->key = s->key; c->scale = s->scale;
    for (uint32_t i = 0; i < s->nsub; i++) chord_addsub(c, &s->sub[i]);
    return c;
}
dms_chordtrack *dms_chordtrack_clone(const dms_chordtrack *T, mtime_t start, mtime_t end)
{
    dms_chordtrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    C->notify = T->notify;
    const dms_chord *before = NULL;
    uint16_t m0 = 0;
    int first = 0;
    for (const dms_chord *s = T->chords; s; s = s->next) {
        if (s->t < start) { before = s; continue; }
        if (end <= s->t) break;
        if (s->t == start) before = NULL;
        if (!first) { m0 = s->measure; first = 1; }
        dms_chord *c = chord_copy(s);
        if (!c) continue;
        c->t = s->t - start;
        c->measure = (uint16_t)(s->measure - m0);
        APPEND(C->chords, c);
    }
    if (before) {
        dms_chord *c = chord_copy(before);
        if (c) { c->t = 0; c->measure = 0; c->beat = 0; c->next = C->chords; C->chords = c; }
    }
    return C;
}

/* ------------------------------------------------------------------ command track */
void dms_cmdtrack_free(dms_cmdtrack *T)
{
    if (!T) return;
    for (dms_command *c = T->cmds, *n; c; c = n) { n = c->next; free(c); }
    free(T);
}
/* 77403501: by measure */
static int cmd_cmp(const void *a, const void *b) { return ((const dms_command *)a)->measure < ((const dms_command *)b)->measure; }
/* 77403d87: the 'cmnd' chunk */
dms_cmdtrack *dms_cmdtrack_load(const uint8_t *p, size_t n, int32_t *hr)
{
    rd r = { p, p + n };
    *hr = E_FAIL_;
    if (n < 4) return NULL;
    uint32_t isz = u32(r.p);
    r.p += 4;
    int32_t left = (int32_t)n - 4;
    if (!isz || (uint32_t)left % isz) return NULL;
    dms_cmdtrack *T = calloc(1, sizeof(*T));
    if (!T) return NULL;
    uint8_t b[12];                  /* (one buffer for every record, as the original's locals) */
    memset(b, 0, sizeof(b));
    for (; left > 0; left -= (int32_t)isz) {
        if (isz < 13) take(&r, b, isz);
        else { take(&r, b, 12); skip(&r, (size_t)left - 12); }    /* sic: the rest of the chunk, not of the item */
        dms_command *c = calloc(1, sizeof(*c));
        if (!c) break;
        c->t = (int32_t)u32(b);
        c->measure = u16(b + 4);
        c->beat = b[6];
        c->command = b[7];
        c->groove = b[8];
        c->range = b[9] > 100 ? 0 : b[9];
        c->repeat = b[10] > 5 ? 0 : b[10];
        APPEND(T->cmds, c);
    }
    if (left != 0) { *hr = 0; return T; }       /* (the original keeps what it read and returns the read's result) */
    T->cmds = dms_msort(T->cmds, offsetof(dms_command, next), cmd_cmp);
    uint8_t g = 62;
    for (dms_command *c = T->cmds; c; c = c->next) {
        if (!c->groove) c->groove = g;
        g = c->groove;
    }
    *hr = 0;
    return T;
}

/* 774038da (CommandParam), 7740368f (CommandParam2), 774033ba (CommandParamNext) */
int dms_cmd_getparam(dms_cmdtrack *T, int which, mtime_t t, mtime_t *next, void *data)
{
    if (which == 1) {
        int hr = 0;
        if (!T->hasnext) hr = DMUS_E_NOT_FOUND_;
        else memcpy(data, T->next, 8);
        if (next) *next = 0;
        return hr;
    }
    if (which == 2) {
        dms_cmdparam2 *o = data;
        mtime_t rel = 0;
        uint8_t cmd = 0, groove = 62, range = 0, rep = 0;
        dms_command *c = T->cmds;
        for (; c && c->t <= t; c = c->next) { rel = c->t - t; cmd = c->command; groove = c->groove; range = c->range; rep = c->repeat; }
        o->t = rel; o->command = cmd; o->groove = groove; o->range = range; o->repeat = rep;
        if (next) *next = c ? c->t - t : 0;
        return 0;
    }
    dms_cmdparam *o = data;
    uint8_t cmd = 0, groove = 62, range = 0, rep = 0;
    dms_command *c = T->cmds;
    for (; c; ) {
        int32_t d = c->t - t;
        if ((d < 0 ? -d : d) < 0x30) {
            cmd = c->command; groove = c->groove; range = c->range; rep = c->repeat;
            c = c->next;
            break;
        }
        if (t <= c->t) break;
        groove = c->groove; range = c->range; rep = c->repeat;
        c = c->next;
    }
    o->command = cmd; o->groove = groove; o->range = range; o->repeat = rep;
    if (next) *next = c ? c->t - t : 0;
    return 0;
}
/* 77403a32 / 77403414 */
int dms_cmd_setparam(dms_cmdtrack *T, int which, mtime_t t, const void *data)
{
    if (which == 1) {
        T->hasnext = data != NULL;
        if (data) memcpy(T->next, data, 8);
        return 0;
    }
    const dms_cmdparam *p = data;
    dms_command *c = calloc(1, sizeof(*c));
    if (!c) return DM_E_OUTOFMEMORY;
    c->t = t; c->beat = 0; c->command = p->command; c->groove = p->groove; c->range = p->range; c->repeat = p->repeat;
    dms_command *prev = NULL, *cur = T->cmds;
    while (cur && cur->t < t) { prev = cur; cur = cur->next; }
    if (!prev) { c->next = T->cmds; T->cmds = c; }
    else { prev->next = c; c->next = cur; }
    if (cur && cur->t == t) { c->next = cur->next; free(cur); }
    return 0;
}
/* 77403bee */
dms_cmdtrack *dms_cmdtrack_clone(const dms_cmdtrack *T, mtime_t start, mtime_t end)
{
    dms_cmdtrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    C->notify = T->notify;
    const dms_command *before = NULL;
    uint16_t m0 = 0;
    int first = 0;
    for (const dms_command *s = T->cmds; s; s = s->next) {
        if (s->t < start) { before = s; continue; }
        if (end <= s->t) break;
        if (!first) { m0 = s->measure; first = 1; }
        if (s->t == start) before = NULL;
        dms_command *c = calloc(1, sizeof(*c));
        if (!c) continue;
        *c = *s;
        c->next = NULL;
        c->t = s->t - start;
        c->measure = (uint16_t)(s->measure - m0);
        APPEND(C->cmds, c);
    }
    if (before) {
        dms_command *c = calloc(1, sizeof(*c));
        if (c) { c->groove = before->groove; c->range = before->range; c->next = C->cmds; C->cmds = c; }
    }
    return C;
}

/* ------------------------------------------------------------------ mute track */
void dms_mutetrack_free(dms_mutetrack *T)
{
    if (!T) return;
    for (dms_mutepch *p = T->pchs, *pn; p; p = pn) {
        pn = p->next;
        for (dms_muteitem *i = p->items, *in; i; i = in) { in = i->next; free(i); }
        free(p);
    }
    free(T);
}
/* 77411a1e: by time */
static int mute_cmp(const void *a, const void *b) { return ((const dms_muteitem *)a)->t < ((const dms_muteitem *)b)->t; }
static int mute_add(dms_mutetrack *T, uint32_t pch, mtime_t t, uint32_t map, uint32_t mute)
{
    dms_mutepch *p = T->pchs;
    while (p && p->pch != pch) p = p->next;
    dms_muteitem *i = calloc(1, sizeof(*i));
    if (!i) return DM_E_OUTOFMEMORY;
    i->t = t; i->map = map; i->mute = mute;
    if (!p) {
        if (!(p = calloc(1, sizeof(*p)))) { free(i); return DM_E_OUTOFMEMORY; }
        p->pch = pch;
        APPEND(p->items, i);
        APPEND(T->pchs, p);
    } else APPEND(p->items, i);
    return 0;
}
/* 77415bba: the 'mute' chunk */
dms_mutetrack *dms_mutetrack_load(const uint8_t *p, size_t n, int32_t *hr)
{
    rd r = { p, p + n };
    *hr = E_FAIL_;
    if (n < 4) return NULL;
    uint32_t isz = u32(r.p);
    r.p += 4;
    dms_mutetrack *T = calloc(1, sizeof(*T));
    if (!T) return NULL;
    int32_t left = (int32_t)n - 4;
    uint8_t b[12];
    memset(b, 0, sizeof(b));
    while (left > 0) {
        if (isz < 13) take(&r, b, isz);
        else { take(&r, b, 12); skip(&r, (size_t)left - 12); }    /* sic */
        if (mute_add(T, u32(b + 4), (int32_t)u32(b), u32(b + 8), u32(b + 8) == 0xffffffff) < 0) {
            *hr = DM_E_OUTOFMEMORY;
            return T;
        }
        if (!isz) break;
        left -= (int32_t)isz;
    }
    if (left != 0) { dms_mutetrack_free(T); return NULL; }
    for (dms_mutepch *q = T->pchs; q; q = q->next) q->items = dms_msort(q->items, offsetof(dms_muteitem, next), mute_cmp);
    *hr = 0;
    return T;
}
/* 774155e0 */
int dms_mute_getparam(const dms_mutetrack *T, mtime_t t, mtime_t *next, dms_muteparam *m)
{
    const dms_mutepch *p = T->pchs;
    while (p && p->pch != m->pch) p = p->next;
    if (!p) {
        m->map = m->pch; m->mute = 0;
        if (next) *next = 0;
        return 0;
    }
    const dms_muteitem *prev = NULL, *i = p->items;
    for (; i && !(t < i->t); i = i->next) prev = i;
    if (!prev) { m->map = m->pch; m->mute = 0; }
    else { m->map = prev->map; m->mute = (int32_t)prev->mute; }
    if (next) *next = i ? i->t - t : 0;
    return 0;
}
/* 774158b7: appended, unsorted */
int dms_mute_setparam(dms_mutetrack *T, mtime_t t, const dms_muteparam *m)
{
    return mute_add(T, m->pch, t, m->map, (uint32_t)m->mute);
}
/* 77415a20 */
dms_mutetrack *dms_mutetrack_clone(const dms_mutetrack *T, mtime_t start, mtime_t end)
{
    dms_mutetrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    for (const dms_mutepch *p = T->pchs; p; p = p->next) {
        dms_mutepch *q = calloc(1, sizeof(*q));
        if (!q) break;
        q->pch = p->pch;
        const dms_muteitem *before = NULL;
        for (const dms_muteitem *i = p->items; i; i = i->next) {
            if (i->t < start) { before = i; continue; }
            if (end <= i->t) break;
            if (i->t == start) before = NULL;
            dms_muteitem *c = calloc(1, sizeof(*c));
            if (!c) continue;
            c->t = i->t - start; c->map = i->map; c->mute = i->mute;
            APPEND(q->items, c);
        }
        if (before) {
            dms_muteitem *c = calloc(1, sizeof(*c));
            if (c) { c->map = before->map; c->mute = before->mute; c->next = q->items; q->items = c; }
        }
        if (!q->items) free(q);
        else APPEND(C->pchs, q);
    }
    return C;
}
