/* reDM: the chord, command and mute tracks of dmstyle (loading, parameters, Clone).  Notes in
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
        if (!ssz || r.p >= r.e) break;  /* (a count past the chunk's data: no endless zero records) */
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
    int64_t left = (int64_t)n - 8 - size;              /* (the list size counts its type: size - 12 - crdh) */
    while (left > 0 && next_chunk(&r, &id, &c, &size)) {
        if (id == FCC('c','r','d','b')) {
            dms_chord *ch = load_crdb(T, c);
            if (!ch) break;
            APPEND(T->chords, ch);
        }
        left -= 8 + (int64_t)size;
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
    for (int i = 0; i < k->nsub && i < 8; i++) chord_addsub(c, &k->sub[i]);    /* (sub[] holds 8) */
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
    dms_command **tail = &T->cmds;  /* (appended through a tail: a long chunk is not quadratic) */
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
        *tail = c; tail = &c->next;
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
/* front: put first (Load, which turns the lists round after: appending each record is quadratic) */
static int mute_add(dms_mutetrack *T, uint32_t pch, mtime_t t, uint32_t map, uint32_t mute, int front)
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
    } else if (front) { i->next = p->items; p->items = i; }
    else APPEND(p->items, i);
    return 0;
}
static void mute_reverse(dms_mutetrack *T)
{
    for (dms_mutepch *q = T->pchs; q; q = q->next) {
        dms_muteitem *l = NULL;
        for (dms_muteitem *i = q->items, *n; i; i = n) { n = i->next; i->next = l; l = i; }
        q->items = l;
    }
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
        if (mute_add(T, u32(b + 4), (int32_t)u32(b), u32(b + 8), u32(b + 8) == 0xffffffff, 1) < 0) {
            *hr = DM_E_OUTOFMEMORY;
            mute_reverse(T);
            return T;
        }
        if (!isz) break;
        left -= (int32_t)isz;
    }
    if (left != 0) { dms_mutetrack_free(T); return NULL; }
    mute_reverse(T);
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
    return mute_add(T, m->pch, t, m->map, (uint32_t)m->mute, 0);
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

/* ------------------------------------------------------------------ styles */
static const uint8_t g_ts44[4] = { 4, 4, 4, 0 };       /* DAT_77418400 */
static uint16_t ts_grids(const uint8_t *ts) { return (uint16_t)(ts[2] | ts[3] << 8); }
int32_t dms_beat_clocks(const uint8_t *ts) { return ts[1] ? (int32_t)(0xc00 / ts[1]) : 0; }
static int32_t grid_len(const uint8_t *ts) { uint16_t g = ts_grids(ts); return g ? dms_beat_clocks(ts) / g : 0; }  /* 77406240 */
int32_t dms_grid_clocks(const uint8_t *ts, uint16_t grid)                                                         /* 7740625b */
{
    uint16_t g = ts_grids(ts);
    if (!g) return grid_len(ts) * grid;
    return dms_beat_clocks(ts) * (grid / g) + grid_len(ts) * (grid % g);
}
const uint8_t *dms_pattern_ts(const dms_pattern *p, const dms_style *s)
{
    if (p && p->ts[1]) return p->ts;
    return s ? s->ts : g_ts44;
}
const uint8_t *dms_part_ts(const dms_part *pt, const dms_style *s, const dms_pattern *p)
{
    if (pt->ts[1]) return pt->ts;
    return p ? dms_pattern_ts(p, s) : g_ts44;
}

/* (counted atomically: a style is shared, and a Load parses without the performance's lock) */
void dms_style_addref(dms_style *s) { if (s) __sync_add_and_fetch(&s->refs, 1); }
static void part_release(dms_part *p)
{
    if (!p || --p->refs > 0) return;
    for (dms_event *e = p->events, *n; e; e = n) { n = e->next; free(e); }
    for (int v = 0; v < 32; v++) for (dms_marker *m = p->markers[v], *n; m; m = n) { n = m->next; free(m); }
    for (dms_res *r = p->res, *n; r; r = n) { n = r->next; free(r); }
    free(p);
}
static void band_free(dm_band *b) { dm_band_release(b); }
static void pattern_release(dms_pattern *p)
{
    if (!p || --p->refs > 0) return;
    free(p->rhythm);
    for (dms_partref *r = p->prefs, *n; r; r = n) { n = r->next; part_release(r->part); free(r); }
    for (dms_mtime *m = p->markers, *n; m; m = n) { n = m->next; free(m); }
    band_free(p->band);
    free(p);
}
void dms_pattern_addref(dms_pattern *p) { if (p) p->refs++; }
/* a pattern as saved and loaded again (7740ac0a / 7740c5f3: IDirectMusicPatternTrack::SetPatternByName): its own
 * header, rhythm, part references and markers; the parts and the motif's band shared */
dms_pattern *dms_pattern_copy(const dms_pattern *P)
{
    dms_pattern *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    *C = *P;
    C->refs = 1; C->rhythm = NULL; C->prefs = NULL; C->markers = NULL;
    if (P->rhythm && P->measures) {
        if (!(C->rhythm = malloc(P->measures * sizeof(*C->rhythm)))) { C->band = NULL; pattern_release(C); return NULL; }
        memcpy(C->rhythm, P->rhythm, P->measures * sizeof(*C->rhythm));
    }
    if (C->band) dm_band_addref(C->band);
    dms_partref **pr = &C->prefs;
    for (const dms_partref *r = P->prefs; r; r = r->next) {
        dms_partref *x = malloc(sizeof(*x));
        if (!x) { pattern_release(C); return NULL; }
        *x = *r; x->next = NULL;
        if (x->part) x->part->refs++;
        *pr = x; pr = &x->next;
    }
    dms_mtime **mp = &C->markers;
    for (const dms_mtime *m = P->markers; m; m = m->next) {
        dms_mtime *x = malloc(sizeof(*x));
        if (!x) { pattern_release(C); return NULL; }
        *x = *m; x->next = NULL;
        *mp = x; mp = &x->next;
    }
    return C;
}
void dms_pattern_release(dms_pattern *p) { pattern_release(p); }
void dms_style_release(dms_style *s)
{
    if (!s || __sync_sub_and_fetch(&s->refs, 1) > 0) return;
    for (int i = 0; i < s->nparts; i++) part_release(s->parts[i]);
    for (int i = 0; i < s->npatterns; i++) pattern_release(s->patterns[i]);
    for (int i = 0; i < s->nmotifs; i++) pattern_release(s->motifs[i]);
    for (int i = 0; i < s->nbands; i++) band_free(s->bands[i]);
    free(s->parts); free(s->patterns); free(s->motifs); free(s->bands); free(s->pchs);
    free(s);
}

/* 77406e8a: the order of a part's events: grid, then markers first, then time offset */
static int event_cmp(const void *a, const void *b)
{
    const dms_event *x = a, *y = b;
    return x->grid < y->grid || (x->type == EV_MARKER && x->grid == y->grid) || (x->grid == y->grid && x->offset < y->offset);
}
void dms_part_resort(dms_part *p) { p->events = dms_msort(p->events, offsetof(dms_event, next), event_cmp); }
void dms_style_resort(dms_style *s)
{
    for (int i = 0; i < s->nparts; i++) s->parts[i]->events = dms_msort(s->parts[i]->events, offsetof(dms_event, next), event_cmp);
}

static void get_guid(dm_guid *g, const uint8_t *p) { g->d1 = u32(p); g->d2 = u16(p + 4); g->d3 = u16(p + 6); memcpy(g->d4, p + 8, 8); }
static int guid_eq(const dm_guid *a, const dm_guid *b) { return !memcmp(a, b, sizeof(*a)); }

/* 774062ad: an UNFO name (at most 0x7e bytes) */
static void read_name(uint16_t *out, size_t max, rd c)
{
    size_t n = (size_t)(c.e - c.p);
    if (n > 0x7e) n = 0x7e;
    size_t k = 0;
    for (; k + 1 < max && 2 * k + 1 < n; k++) { out[k] = u16(c.p + 2 * k); if (!out[k]) break; }
    out[k < max ? k : max - 1] = 0;
}
static void read_unfo(uint16_t *name, uint16_t *cat, rd r)
{
    uint32_t id, size; rd c;
    while (next_chunk(&r, &id, &c, &size)) {
        if (id == FCC('I','N','A','M') || id == FCC('U','N','A','M')) read_name(name, 64, c);
        else if (cat && id == FCC('c','a','t','g')) read_name(cat, 64, c);
    }
}

/* 7740a4b5: the style's PChannels, newest first, each once */
static void pch_add(dms_style *s, uint32_t pch)
{
    for (int i = 0; i < s->npchs; i++) if (s->pchs[i] == pch) return;
    uint32_t *n = realloc(s->pchs, (s->npchs + 1) * sizeof(*n));
    if (!n) return;
    memmove(n + 1, n, s->npchs * sizeof(*n));
    n[0] = pch;
    s->pchs = n;
    s->npchs++;
}

/* a typed record list ('note', 'crve', 'mrkr', 'anpn', 'rsln'): records of the size given first, read up to max bytes
 * (the rest skipped); the record buffer is kept across records as the original's locals */
#define RECORDS(c, max, buf, body) do { \
    uint8_t _s4[4]; if (take(&(c), _s4, 4) != 4) break; uint32_t _sz = u32(_s4), _rd = _sz, _sk = 0; \
    if (!_sz) break;                /* (records of no size would never use the chunk up) */ \
    if (_sz > (max)) { _rd = (max); _sk = _sz - (max); } int32_t _left = (int32_t)((c).e - (c).p); \
    while (_left > 0) { take(&(c), buf, _rd); _left -= (int32_t)_rd; if ((uint16_t)_sk) { skip(&(c), (uint16_t)_sk); _left -= (uint16_t)_sk; } body } \
} while (0)

/* 77408e32: a 'part' list.  Returns 1 when the style already has a part with its GUID (the new one is dropped) */
static int load_part(dms_part *pt, dms_style *s, rd r)
{
    uint32_t id, size; rd c;
    while (next_chunk(&r, &id, &c, &size)) {
        if (id == FCC('n','o','t','e')) {
            uint8_t b[0x18] = { 0 };
            RECORDS(c, 0x18, b, {
                if ((int32_t)u32(b) >= 0) {
                    dms_event *e = calloc(1, sizeof(*e));
                    if (e) {
                        e->type = EV_NOTE;
                        e->grid = (int16_t)u32(b); e->var = u32(b + 4); e->dur = (int32_t)u32(b + 8);
                        e->offset = (int16_t)u16(b + 12); e->mv = u16(b + 14); e->vel = b[16]; e->timerange = b[17];
                        e->durrange = b[18]; e->velrange = b[19]; e->inversion = b[20]; e->playmode = b[21];
                        e->noteflags = b[22];
                        e->next = pt->events; pt->events = e;
                    }
                }
            });
        } else if (id == FCC('c','r','v','e')) {
            uint8_t b[0x20] = { 0 };
            RECORDS(c, 0x20, b, {
                if ((int32_t)u32(b) >= 0) {
                    dms_event *e = calloc(1, sizeof(*e));
                    if (e) {
                        e->type = EV_CURVE;
                        e->grid = (int16_t)u32(b); e->var = u32(b + 4); e->dur = (int32_t)u32(b + 8);
                        e->resetdur = (int32_t)u32(b + 12); e->offset = (int16_t)u16(b + 16);
                        e->cstart = (int16_t)u16(b + 18); e->cend = (int16_t)u16(b + 20); e->creset = (int16_t)u16(b + 22);
                        e->ctype = b[24]; e->shape = b[25]; e->cc = b[26]; e->cflags = b[27];
                        e->param = u16(b + 28); e->merge = u16(b + 30);
                        e->next = pt->events; pt->events = e;
                    }
                }
            });
        } else if (id == FCC('p','r','t','h')) {
            uint8_t b[0xa0] = { 0 };
            uint32_t n = size;
            if (size > 0x9f) { s->flags |= 1; n = 0xa0; }
            if (take(&c, b, n) != n) return -1;
            dm_guid g;
            get_guid(&g, b + 0x84);
            for (int i = 0; i < s->nparts; i++) if (s->parts[i] != pt && guid_eq(&s->parts[i]->guid, &g)) return 1;
            memcpy(pt->ts, b, 4);
            pt->guid = g;
            pt->measures = u16(b + 0x94);
            pt->playmode = b[0x96]; pt->invupper = b[0x97]; pt->invlower = b[0x98];
            pt->flags = u32(b + 0x9c);
            for (int i = 0; i < 32; i++) {
                uint32_t v = u32(b + 4 + 4 * i);
                if (v == 0xffffffff) v = 0x3fffffff;
                if ((s->flags & 1) && v == 0x3fffffff) v = 0x7fffffff;
                pt->varchoices[i] = v;
            }
        } else if (id == FCC('r','s','l','n')) {
            uint8_t b[8] = { 0 };
            RECORDS(c, 8, b, {
                dms_res *e = calloc(1, sizeof(*e));
                if (e) {
                    e->var = u32(b); e->mv = u16(b + 4); e->inversion = b[6]; e->playmode = b[7];
                    e->next = pt->res; pt->res = e;
                }
            });
        } else if (id == FCC('a','n','p','n')) {
            uint8_t b[0xc] = { 0 };
            RECORDS(c, 0xc, b, {
                if ((int32_t)u32(b) >= 0) {
                    dms_event *e = calloc(1, sizeof(*e));
                    if (e) {
                        e->type = EV_ANTICIPATION;
                        e->grid = (int16_t)u32(b); e->var = u32(b + 4); e->offset = (int16_t)u16(b + 8); e->arange = b[10];
                        e->next = pt->events; pt->events = e;
                    }
                }
            });
        } else if (id == FCC('m','r','k','r')) {
            uint8_t b[0xa] = { 0 };
            RECORDS(c, 0xa, b, {
                if ((int32_t)u32(b) >= 0) {
                    dms_event *e = calloc(1, sizeof(*e));
                    if (e) {
                        e->type = EV_MARKER;
                        e->grid = (int16_t)u32(b); e->var = u32(b + 4); e->offset = 0; e->mflags = u16(b + 8);
                        e->next = pt->events; pt->events = e;
                    }
                }
            });
        }
    }
    pt->events = dms_msort(pt->events, offsetof(dms_event, next), event_cmp);
    return 0;
}

static dms_part *part_new(dms_style *s)       /* 7740b531 / 7740b215 */
{
    dms_part *pt = calloc(1, sizeof(*pt));
    if (!pt) return NULL;
    pt->refs = 1;
    memcpy(pt->ts, s->ts, 4);       /* 7740b215: the style's signature until 'prth' */
    pt->measures = 1;
    pt->playmode = 10; pt->invupper = 0x7f;
    dms_part **n = realloc(s->parts, (s->nparts + 1) * sizeof(*n));
    if (!n) { free(pt); return NULL; }
    s->parts = n;
    s->parts[s->nparts++] = pt;
    return pt;
}
static void part_remove(dms_style *s, dms_part *pt)    /* 7740be42 */
{
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i] == pt) {
            memmove(s->parts + i, s->parts + i + 1, (s->nparts - i - 1) * sizeof(*s->parts));
            s->nparts--;
            part_release(pt);
            return;
        }
}

/* 7740aa3b / 7740aaa0: inserted by time after the entries at the same time */
static void marker_add(dms_marker **head, mtime_t t, uint16_t flags)
{
    dms_marker *m = calloc(1, sizeof(*m)), *prev = NULL, *cur = *head;
    if (!m) return;
    m->t = t; m->flags = flags;
    while (cur && !(t < cur->t)) { prev = cur; cur = cur->next; }
    if (prev) { prev->next = m; m->next = cur; } else { m->next = *head; *head = m; }
}
static void mtime_add(dms_mtime **head, mtime_t t)
{
    dms_mtime *m = calloc(1, sizeof(*m)), *prev = NULL, *cur = *head;
    if (!m) return;
    m->t = t;
    while (cur && !(t < cur->t)) { prev = cur; cur = cur->next; }
    if (prev) { prev->next = m; m->next = cur; } else { m->next = *head; *head = m; }
}
/* 7740b12d: a part's start markers into its per-variation lists and the pattern's (once per reference) */
static void part_markers(dms_part *pt, dms_style *s, dms_pattern *p)
{
    const uint8_t *ts = dms_part_ts(pt, s, p);
    for (dms_event *e = pt->events; e; e = e->next) {
        if (e->type != EV_MARKER || !(e->mflags & 1)) continue;
        mtime_t t = dms_grid_clocks(ts, (uint16_t)e->grid) + e->offset;
        for (int v = 0; v < 32; v++) if (e->var & (1u << v)) marker_add(&pt->markers[v], t, e->mflags);
        mtime_add(&p->markers, t);
    }
}

/* 7740c5f3: a 'pttn' list */
static int load_pattern(dms_pattern *p, dms_style *s, rd r, dm_getcoll_fn getcoll, void *ctx)
{
    uint32_t id, size; rd c;
    while (next_chunk(&r, &id, &c, &size)) {
        if (id == FCC('R','I','F','F')) {
            if (c.e - c.p >= 4 && u32(c.p) == FCC('D','M','B','D')) {
                band_free(p->band);
                p->band = dm_band_load(c.p + 4, (size_t)(c.e - c.p - 4), getcoll, ctx);
            }
        } else if (id == FCC('L','I','S','T') && c.e - c.p >= 4) {
            uint32_t type = u32(c.p);
            rd x = { c.p + 4, c.e };
            if (type == FCC('U','N','F','O')) read_unfo(p->name, NULL, x);
            else if (type == FCC('p','r','e','f')) {
                dms_partref *pr = calloc(1, sizeof(*pr));       /* 7740bfe3: appended before it is read */
                if (!pr) return DM_E_OUTOFMEMORY;
                pr->subchord = 1; pr->priority = 100;
                APPEND(p->prefs, pr);
                uint32_t xid, xs; rd y;
                while (next_chunk(&x, &xid, &y, &xs)) {                 /* 7740be95 */
                    if (xid != FCC('p','r','f','c')) continue;
                    int found = 0;
                    uint8_t b[0x1c] = { 0 };
                    uint32_t n = xs > 0x1b ? 0x1c : xs;
                    if (take(&y, b, n) != n) return E_FAIL_;
                    pr->random = b[0x15];
                    pr->pch = n > 0x1b ? u32(b + 0x18) : u16(b + 0x10);
                    pch_add(s, pr->pch);
                    pr->lock = b[0x12]; pr->subchord = b[0x13]; pr->priority = b[0x14];
                    dm_guid g;
                    get_guid(&g, b);
                    for (int i = 0; i < s->nparts; i++)
                        if (guid_eq(&s->parts[i]->guid, &g)) {
                            part_release(pr->part);         /* (a second 'prfc': the first one's reference let go) */
                            pr->part = s->parts[i]; pr->part->refs++; found = 1; break;
                        }
                    if (!found) return E_FAIL_;
                }
                if (!pr->part) {            /* no 'prfc': the reference left out (playback needs its part) */
                    dms_partref **q = &p->prefs;
                    while (*q != pr) q = &(*q)->next;
                    *q = NULL;
                    free(pr);
                }
            } else if (type == FCC('p','a','r','t')) {
                dms_part *pt = part_new(s);
                if (!pt) return DM_E_OUTOFMEMORY;
                int k = load_part(pt, s, x);
                if (k < 0) { part_remove(s, pt); return E_FAIL_; }
                if (k == 1) part_remove(s, pt);
                else part_markers(pt, s, p);
            }
        } else if (id == FCC('p','t','n','h')) {
            uint8_t b[16] = { 0 };
            uint32_t n = size > 15 ? 16 : size;
            if (take(&c, b, n) != n) return E_FAIL_;
            memcpy(p->ts, b, 4);
            p->groovelo = b[4]; p->groovehi = b[5]; p->embellish = u16(b + 6);
            uint16_t had = p->measures ? p->measures : 1;  /* (what an earlier 'rhtm' was sized for) */
            p->measures = u16(b + 8); p->destlo = b[10]; p->desthi = b[11];
            if (p->rhythm && p->measures > had) {   /* the rhythm is read for every measure: grown, the rest 0 */
                uint32_t *x = realloc(p->rhythm, p->measures * sizeof(*x));
                if (!x) return DM_E_OUTOFMEMORY;
                memset(x + had, 0, (p->measures - had) * sizeof(*x));
                p->rhythm = x;
            }
            p->flags = u32(b + 12);
        } else if (id == FCC('r','h','t','m')) {
            free(p->rhythm);
            p->rhythm = calloc(p->measures ? p->measures : 1, 4);
            if (!p->rhythm) return DM_E_OUTOFMEMORY;
            for (int i = 0; i < p->measures; i++) {
                uint8_t b[4];
                if (take(&c, b, 4) != 4) return E_FAIL_;
                p->rhythm[i] = u32(b);
            }
        } else if (id == FCC('m','t','f','s')) {
            uint8_t b[0x14];
            if (take(&c, b, 0x14) != 0x14) return E_FAIL_;
            p->repeats = u32(b); p->playstart = (int32_t)u32(b + 4); p->loopstart = (int32_t)u32(b + 8);
            p->loopend = (int32_t)u32(b + 12); p->hasmotif = 1; p->resolution = u32(b + 16);
        }
    }
    return 0;
}

/* 7740cf73: the 'DMST' form */
dms_style *dms_style_load(const uint8_t *data, size_t n, dm_getcoll_fn getcoll, void *ctx, int32_t *hr)
{
    dms_style *s = calloc(1, sizeof(*s));
    if (!s) { *hr = DM_E_OUTOFMEMORY; return NULL; }
    s->refs = 1;
    rd r = { data, data + n }, c;
    uint32_t id, size;
    int32_t res = 0;
    int haveband = 0;
    while (res >= 0 && next_chunk(&r, &id, &c, &size)) {
        if (id == FCC('R','I','F','F') && c.e - c.p >= 4 && u32(c.p) == FCC('D','M','B','D')) {
            dm_band *b = dm_band_load(c.p + 4, (size_t)(c.e - c.p - 4), getcoll, ctx);
            if (!b) { res = E_FAIL_; break; }
            if (!haveband) { s->defband = b; haveband = 1; }
            dm_band **nb = realloc(s->bands, (s->nbands + 1) * sizeof(*nb));
            if (!nb) { band_free(b); res = DM_E_OUTOFMEMORY; break; }
            memmove(nb + 1, nb, s->nbands * sizeof(*nb));
            nb[0] = b;
            s->bands = nb;
            s->nbands++;
        } else if (id == FCC('L','I','S','T') && c.e - c.p >= 4) {
            uint32_t type = u32(c.p);
            rd x = { c.p + 4, c.e };
            if (type == FCC('U','N','F','O')) read_unfo(s->name, s->category, x);
            else if (type == FCC('p','t','t','n')) {
                dms_pattern *p = calloc(1, sizeof(*p));
                if (!p) { res = DM_E_OUTOFMEMORY; break; }
                p->refs = 1;
                memcpy(p->ts, s->ts, 4);    /* 7740c9ea: the style's signature until 'ptnh' */
                p->groovelo = 1; p->groovehi = 100; p->destlo = 1; p->desthi = 100;
                p->measures = 1; p->loopend = -1;
                int k = load_pattern(p, s, x, getcoll, ctx);
                if (k < 0) { pattern_release(p); res = k; break; }
                int motif = (p->embellish & 0x10) != 0;
                dms_pattern ***list = motif ? &s->motifs : &s->patterns;
                int *cnt = motif ? &s->nmotifs : &s->npatterns;
                dms_pattern **nl = realloc(*list, (*cnt + 1) * sizeof(*nl));
                if (!nl) { pattern_release(p); res = DM_E_OUTOFMEMORY; break; }
                nl[(*cnt)++] = p;
                *list = nl;
            } else if (type == FCC('p','a','r','t')) {
                dms_part *pt = part_new(s);
                if (!pt) { res = DM_E_OUTOFMEMORY; break; }
                int k = load_part(pt, s, x);
                if (k < 0) { part_remove(s, pt); res = E_FAIL_; break; }
                if (k == 1) part_remove(s, pt);
            }
        } else if (id == FCC('g','u','i','d')) {
            if (c.e - c.p < 16) { res = E_FAIL_; break; }
            get_guid(&s->guid, c.p);
            s->hasguid = 1;
        } else if (id == FCC('s','t','y','h')) {
            uint8_t b[12] = { 0 };
            uint32_t k = size > 11 ? 12 : size;
            if (take(&c, b, k) != k) { res = E_FAIL_; break; }
            memcpy(s->ts, b, 4);
            memcpy(&s->tempo, b + 4, 8);
        } else if (id == FCC('v','e','r','s')) {
            uint8_t b[8] = { 0 };
            uint32_t k = size > 7 ? 8 : size;
            if (take(&c, b, k) != k) { res = E_FAIL_; break; }
            s->vers[0] = u32(b); s->vers[1] = u32(b + 4);
        }
    }
    for (int i = 0; i < s->npatterns; i++)
        for (dms_partref *pr = s->patterns[i]->prefs; pr; pr = pr->next) if (pr->part) part_markers(pr->part, s, s->patterns[i]);
    for (int i = 0; i < s->nmotifs; i++)
        for (dms_partref *pr = s->motifs[i]->prefs; pr; pr = pr->next) if (pr->part) part_markers(pr->part, s, s->motifs[i]);
    *hr = res;
    if (res < 0) { dms_style_release(s); return NULL; }
    return s;
}

/* ------------------------------------------------------------------ style track (Load 7741313c) */
void dms_styletrack_free(dms_styletrack *T)
{
    if (!T) return;
    for (dms_stynode *n = T->styles, *x; n; n = x) { x = n->next; dms_style_release(n->style); free(n); }
    free(T->pchs); free(T->pvar); free(T->pnorep); free(T->varbuf);
    for (int i = 0; i < T->nretired; i++) pattern_release(T->retired[i]);
    free(T->retired);
    pattern_release(T->motif);
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

void (*dms_resort_hook)(int enter);

/* 774123a0: a 'DMRF' list: the loader's style (then every part's events sorted again, 774087cf) */
static dms_style *load_styleref(rd r, dms_getstyle_fn get, void *ctx, int32_t *hr)
{
    dm_guid g; int hasg = 0;
    char file[520] = "", name[260] = "";
    uint32_t id, size; rd c;
    while (next_chunk(&r, &id, &c, &size)) {
        if (id == FCC('g','u','i','d') && c.e - c.p >= 16) { get_guid(&g, c.p); hasg = 1; }
        else if (id == FCC('f','i','l','e')) wstr_utf8(file, sizeof(file), c);
        else if (id == FCC('n','a','m','e')) wstr_utf8(name, sizeof(name), c);
    }
    dms_style *s = get ? get(ctx, hasg ? &g : NULL, file[0] ? file : NULL, name[0] ? name : NULL) : NULL;
    if (!s) { *hr = DMUS_E_NOT_FOUND_; return NULL; }
    /* (the style may be playing: the host sorts it under the lock its states are guarded by) */
    if (dms_resort_hook) dms_resort_hook(1);
    dms_style_resort(s);
    if (dms_resort_hook) dms_resort_hook(0);
    *hr = 0;
    return s;
}

static int stynode_cmp(const void *a, const void *b) { return ((const dms_stynode *)a)->t < ((const dms_stynode *)b)->t; }

/* 77410c9b: the PChannels of every style (each once, the newest first; one 0 when there are none) */
int dms_styletrack_pchs(dms_styletrack *T)
{
    uint32_t *l = NULL; int n = 0;
    for (dms_stynode *x = T->styles; x; x = x->next) {
        if (!x->style) continue;
        for (int i = 0; i < x->style->npchs; i++) {
            uint32_t p = x->style->pchs[i];
            int k;
            for (k = 0; k < n && l[k] != p; k++) {}
            if (k < n) continue;
            uint32_t *m = realloc(l, (n + 1) * sizeof(*m));
            if (!m) { free(l); return DM_E_OUTOFMEMORY; }
            l = m;
            memmove(l + 1, l, n * sizeof(*l));
            l[0] = p;
            n++;
        }
    }
    if (!n) { l = calloc(1, sizeof(*l)); if (!l) return DM_E_OUTOFMEMORY; n = 1; }
    free(T->pchs);
    T->pchs = l; T->npchs = n;
    T->pchschanged = 1;
    return 0;
}

/* 77412fbc / 77412b3c: the 'strf' lists ('stmp' time, 'DMRF' reference), appended; then sorted by time */
dms_styletrack *dms_styletrack_load(const uint8_t *p, size_t n, dms_getstyle_fn get, void *ctx, int32_t *hr)
{
    dms_styletrack *T = calloc(1, sizeof(*T));
    if (!T) { *hr = DM_E_OUTOFMEMORY; return NULL; }
    rd r = { p, p + n }, c;
    uint32_t id, size;
    int32_t res = 0, ok = 0;
    while (next_chunk(&r, &id, &c, &size)) {
        if (id != FCC('L','I','S','T') || c.e - c.p < 4 || u32(c.p) != FCC('s','t','r','f')) continue;
        rd x = { c.p + 4, c.e }, y;
        dms_stynode *nd = calloc(1, sizeof(*nd));
        if (!nd) { res = DM_E_OUTOFMEMORY; break; }
        int32_t h = 0, warn = 0;
        uint32_t xid, xs;
        while (next_chunk(&x, &xid, &y, &xs)) {
            if (xid == FCC('L','I','S','T') && y.e - y.p >= 4 && u32(y.p) == FCC('D','M','R','F')) {
                rd z = { y.p + 4, y.e };
                dms_style *s = load_styleref(z, get, ctx, &h);
                if (h >= 0) { if (h) warn = h; dms_style_release(nd->style); nd->style = s; }
            } else if (xid == FCC('s','t','m','p')) {
                if (xs != 4 || y.e - y.p < 4) { h = E_FAIL_; break; }
                nd->t = (int32_t)u32(y.p);
            }
        }
        if (h < 0) { dms_style_release(nd->style); free(nd); res = h; continue; }
        APPEND(T->styles, nd);
        res = warn;
        ok = 1;
    }
    if (res != 0 && ok) res = 0;
    T->styles = dms_msort(T->styles, offsetof(dms_stynode, next), stynode_cmp);
    T->gen++;
    dms_styletrack_pchs(T);
    *hr = res;
    return T;
}

/* 77410a30: a pattern that keeps its variations (flag 1): the parts' variations held by the track */
static void motif_vars(dms_styletrack *T)
{
    if (!T->motif || !(T->motif->flags & 1)) return;
    free(T->pvar); free(T->pnorep);
    T->pvar = NULL; T->pnorep = NULL;
    int n = 0;
    for (dms_partref *r = T->motif->prefs; r; r = r->next) n++;
    T->pvar = malloc(n ? n : 1);
    T->pnorep = calloc(n ? n : 1, sizeof(*T->pnorep));
    if (T->pvar) memset(T->pvar, 0xff, n ? n : 1);
}

/* 77410b5b: the styles in [start, end) moved by -start, and the one in effect before start (unless one is at start)
 * put first at 0; the seed and the load generation are not copied */
/* 7740c497: the clone's reference on a PChannel, with a new part (the pattern's signature, the source's guid, bounds,
 * flags and play mode) the first time; a later reference on the same PChannel widens the bounds and adds the flags */
static dms_partref *clone_partref(dms_pattern *C, const dms_partref *src, uint8_t playmode, uint16_t measures)
{
    for (dms_partref *r = C->prefs; r; r = r->next)
        if (r->pch == src->pch) {
            if (r->part && src->part) {
                if (r->part->invupper < src->part->invupper) r->part->invupper = src->part->invupper;
                if (src->part->invlower < r->part->invlower) r->part->invlower = src->part->invlower;
                r->part->flags |= src->part->flags;
            }
            return r;
        }
    dms_partref *r = calloc(1, sizeof(*r));
    dms_part *pt = calloc(1, sizeof(*pt));
    if (!r || !pt) { free(r); free(pt); return NULL; }
    pt->refs = 1;
    pt->measures = 1; pt->playmode = 10; pt->invupper = 0x7f;       /* 7740b215 */
    if (src->part) {
        pt->guid = src->part->guid;
        memcpy(pt->ts, C->ts, 4);
        for (int i = 0; i < 32; i++) pt->varchoices[i] = 0x7fffffff;
        pt->playmode = playmode;
        pt->invupper = src->part->invupper; pt->invlower = src->part->invlower;
        pt->flags = src->part->flags;
        pt->measures = measures;
    }
    r->part = pt;
    r->pch = src->pch; r->lock = src->lock; r->subchord = src->subchord; r->priority = src->priority; r->random = src->random;
    APPEND(C->prefs, r);
    return r;
}

/* 7740d4bf: the measures of a pattern in [start, end): the parts' events in range moved to the new start (their grid
 * from the pattern's signature, the remainder into the time offset).  The embellishment is not copied (the new
 * pattern's constructor clears it) and the markers are not rebuilt. */
static dms_pattern *pattern_clone(const dms_pattern *P, mtime_t start, mtime_t end)
{
    dms_pattern *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    C->refs = 1;
    memcpy(C->ts, P->ts, 4);
    int32_t mlen = dms_beat_clocks(P->ts) * P->ts[0];
    uint32_t m0 = mlen ? (uint32_t)start / (uint32_t)mlen : 0, m1 = mlen ? (uint32_t)end / (uint32_t)mlen : 0;
    C->measures = (uint16_t)(m1 - m0);
    memcpy(C->name, P->name, sizeof(C->name));
    C->groovelo = P->groovelo; C->groovehi = P->groovehi; C->destlo = P->destlo; C->desthi = P->desthi;
    C->hasmotif = P->hasmotif; C->repeats = P->repeats;
    C->resolution = P->resolution; C->flags = P->flags;
    C->playstart = start < P->playstart ? P->playstart - start : 0;
    C->loopstart = start < P->loopstart ? P->loopstart - start : 0;
    if (P->loopend < end - start) C->loopend = P->loopend < 0 ? -1 : P->loopend - start;
    else C->loopend = end - start;
    C->rhythm = calloc(C->measures ? C->measures : 1, sizeof(uint32_t));
    if (!C->rhythm) { pattern_release(C); return NULL; }
    for (uint32_t i = 0; i < C->measures; i++)          /* (read past the source's rhythm in the original) */
        C->rhythm[i] = P->rhythm && m0 + i < P->measures ? P->rhythm[m0 + i] : 0;    /* (none without 'rhtm') */
    if (P->band) { C->band = P->band; dm_band_addref(C->band); }
    int32_t glen = grid_len(P->ts);
    for (const dms_partref *r = P->prefs; r; r = r->next) {
        if (!r->part) { pattern_release(C); return NULL; }
        dms_partref *n = clone_partref(C, r, r->part->playmode, C->measures);
        if (!n) { pattern_release(C); return NULL; }
        memcpy(n->part->varchoices, r->part->varchoices, sizeof(n->part->varchoices));
        for (const dms_event *e = r->part->events; e; e = e->next) {
            int32_t t = dms_grid_clocks(P->ts, (uint16_t)e->grid);
            if (!glen || t < start || end <= t) continue;
            dms_event *x = malloc(sizeof(*x));
            if (!x) { pattern_release(C); return NULL; }
            *x = *e;
            x->grid = (int16_t)((t - start) / glen);
            x->offset = (int16_t)(e->offset + (t - start) % glen);
            x->next = n->part->events; n->part->events = x;
        }
        dms_part_resort(n->part);
    }
    return C;
}

dms_styletrack *dms_styletrack_clone(const dms_styletrack *T, mtime_t start, mtime_t end)
{
    dms_styletrack *C = calloc(1, sizeof(*C));
    if (!C) return NULL;
    const dms_stynode *last = NULL;
    for (const dms_stynode *x = T->styles; x; x = x->next) {
        if (x->t < start) { last = x; continue; }
        if (end <= x->t) continue;
        if (x->t == start) last = NULL;
        dms_stynode *nd = calloc(1, sizeof(*nd));
        if (!nd) continue;
        nd->t = x->t - start; nd->style = x->style; dms_style_addref(nd->style);
        APPEND(C->styles, nd);
    }
    if (last) {
        dms_stynode *nd = calloc(1, sizeof(*nd));
        if (nd) { nd->style = last->style; dms_style_addref(nd->style); nd->next = C->styles; C->styles = nd; }
    }
    C->pchschanged = T->pchschanged; C->notify = T->notify; C->tsenabled = T->tsenabled; C->tsset = T->tsset;
    if (T->motif && T->kind == TRK_PATTERN) {
        C->kind = TRK_PATTERN;
        C->au_byguid = 1; C->au_all = 0;   /* 77413246: (sic) the clone auditions the part on PChannel 0 with no guid */
        C->motif = pattern_clone(T->motif, start, end);    /* 77413246 */
        if (C->motif) motif_vars(C);
    } else if (T->motif) { C->motif = T->motif; C->motif->refs++; motif_vars(C); }     /* 7740dd73 */
    dms_styletrack_pchs(C);
    return C;
}

/* 7740ecba: SetParams(style, pattern) on a new motif track */
dms_styletrack *dms_motiftrack_new(dms_style *s, dms_pattern *p)
{
    dms_styletrack *T = calloc(1, sizeof(*T));
    dms_stynode *nd = calloc(1, sizeof(*nd));
    if (!T || !nd) { free(T); free(nd); return NULL; }
    T->motif = p; if (p) p->refs++;
    motif_vars(T);
    nd->style = s; dms_style_addref(s);
    T->styles = nd;
    dms_styletrack_pchs(T);
    return T;
}

/* ------------------------------------------------------------------ pattern track (CPatternTrack 7741484d) */
/* 77414d0e / 774147e9: the track's pattern replaced (the old one returned, still referenced) and its variation
 * arrays made again */
dms_pattern *dms_patterntrack_swap(dms_styletrack *T, dms_pattern *p)
{
    dms_pattern *old = T->motif;
    T->motif = p;
    motif_vars(T);
    free(T->varbuf); T->varbuf = NULL;
    /* the old one is kept until the track goes: states still playing it take the new one at their next Play */
    dms_pattern **r = realloc(T->retired, (T->nretired + 1) * sizeof(*r));
    if (r) { T->retired = r; r[T->nretired++] = old; } else pattern_release(old);
    return old;
}
/* 77414f43 / 77414b48: a 'DMPT' form's data: a private style ('styh') holding the pattern's parts, the pattern itself
 * the track's (not among the style's patterns) */
dms_styletrack *dms_patterntrack_load(const uint8_t *data, size_t n, dm_getcoll_fn getcoll, void *ctx, int32_t *hr)
{
    *hr = 0;
    dms_styletrack *T = calloc(1, sizeof(*T));
    dms_stynode *nd = calloc(1, sizeof(*nd));
    dms_style *s = calloc(1, sizeof(*s));
    if (!T || !nd || !s) { free(T); free(nd); free(s); *hr = DM_E_OUTOFMEMORY; return NULL; }
    s->refs = 1;
    rd r = { data, data + n }, c;
    uint32_t id, size;
    while (next_chunk(&r, &id, &c, &size)) {
        if ((id == FCC('R','I','F','F') || id == FCC('L','I','S','T')) && c.e - c.p >= 4 && u32(c.p) == FCC('p','t','t','n')) {
            dms_pattern *p = calloc(1, sizeof(*p));
            if (!p) { *hr = DM_E_OUTOFMEMORY; break; }
            p->refs = 1;
            memcpy(p->ts, s->ts, 4);            /* 7740c9ea */
            p->groovelo = 1; p->groovehi = 100; p->destlo = 1; p->desthi = 100;
            p->measures = 1; p->loopend = -1;
            rd x = { c.p + 4, c.e };
            int k = load_pattern(p, s, x, getcoll, ctx);
            if (k < 0) { pattern_release(p); *hr = k; break; }
            pattern_release(T->motif);
            T->motif = p;
            motif_vars(T);                      /* 774147e9 */
            break;
        } else if (id == FCC('s','t','y','h')) {
            uint8_t b[12] = { 0 };
            uint32_t k = size > 11 ? 12 : size;
            if (take(&c, b, k) != k) { *hr = E_FAIL_; break; }
            memcpy(&s->tempo, b + 4, 8);
            memcpy(s->ts, b, 4);
        }
    }
    if (*hr < 0 || !T->motif) {
        if (*hr >= 0) *hr = E_FAIL_;
        dms_style_release(s); free(nd); dms_styletrack_free(T);
        return NULL;
    }
    nd->style = s;
    T->styles = nd;
    T->kind = TRK_PATTERN;
    T->au_byguid = T->au_all = 1;       /* 7741338c */
    T->gen++;
    dms_styletrack_pchs(T);
    return T;
}
