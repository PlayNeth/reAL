/* reDM: the style track's playback (dmstyle CStyleTrack / StyleTrackState on PatternTrackState), included by
 * dmstytrack.c.  The addresses are XP dmstyle's (5.3.2600.5512); docs/dmstyle_notes.md has the layouts.
 *
 * Every Play picks the patterns (the style's pattern chooser 7740b74f, by the command track's groove and embellishment
 * and the chord track's rhythm), the parts' variations (by the chord, 7740a4fd) and plays the parts' notes and curves
 * against the current chord (MusicToMIDI with the inversion rules of 77408907). */

static const uint8_t g_sty_ts44[4] = { 4, 4, 4, 0 };      /* DAT_77418400 */
#define STY_E_FAIL ((int32_t)0x80004005)
#define STY_E_POINTER ((int32_t)0x80004003)

typedef struct { mtime_t t; uint32_t pch; int32_t mute; } stymute;           /* (next lookup, mapped PChannel, mute) */
typedef struct stuhist { struct stuhist *next; dms_pattern *p; } styhist;
typedef struct {
    const dms_event *ev[0x102];
    mtime_t t[0x102];
    uint8_t any;
} stychase;                                     /* 7740ff2f: the curves before the start, one per controller */

typedef struct {
    dms_styletrack *ti;                         /* +0x04 */
    dm_segstate *ss;                            /* +0x08 / +0x10 */
    const dms_style *style;                     /* +0x0c */
    uint32_t vid;                               /* +0x14 */
    mtime_t chordt, nextchordt, nnchordt;       /* +0x18 +0x1c +0x20 */
    dm_chord_key chord;                         /* +0x24 */
    dm_chord_key nextchord;                     /* +0xf0 */
    dms_pattern *pattern;                       /* +0x1bc */
    uint32_t *partpch;                          /* +0x1c0 */
    uint8_t *var;                               /* +0x1c4 per part: its variation (0xff none) */
    uint32_t *norep;                            /* +0x1c8 per part: variations played (random row) */
    mtime_t *partoff;                           /* +0x1cc per part: its start in the pattern (it repeats) */
    uint8_t *mflag;                             /* +0x1d0 */
    int newpat;                                 /* +0x1d4 */
    mtime_t patt;                               /* +0x1d8 */
    int tsenabled;                              /* +0x1dc */
    struct { uint16_t id, count; int16_t tr; } inv[16];     /* +0x1e0 inversion groups */
    stymute *mute;                              /* +0x244 per track PChannel */
    int nmute;
    uint8_t lock[255];                          /* +0x248 per lock id: the variation */
    const dms_event **cursor;                   /* +0x348 per part */
    uint32_t group;                             /* +0x34c */
    uint32_t *rtab; int nrtab;                  /* +0x350 +0x354 per beat (GUID_SeedVariations) */
    uint32_t gen;                               /* +0x358 */
    int32_t hr;                                 /* +0x35c */
    dm_perf *pf;                                /* +0x360 */
    mtime_t off;                                /* +0x364 */
    mtime_t pstart, prevpstart, pend, prevpend; /* +0x368 +0x36c +0x370 +0x374 */
    mtime_t nextstyle;                          /* +0x378 */
    mtime_t cmdt; uint8_t cmd[4];               /* +0x37c +0x380 the last command (groove with the master's) */
    mtime_t gap;                                /* +0x384 */
    styhist *hist;                              /* +0x388 */
    int nparts;
    int kind;                                   /* TRK_STYLE, TRK_MOTIF, TRK_PATTERN */
    int noinv;                                  /* +0x388 (pattern track state: vtable[3] 774132f8): no inversions */
} stystate;

static int32_t sty_beat(const uint8_t *ts) { return dms_beat_clocks(ts); }
static int32_t sty_meas(const uint8_t *ts) { return dms_beat_clocks(ts) * ts[0]; }

/* 7740dd48: the signature in effect (the pattern's, the style's, 4/4) */
static const uint8_t *st_ts(const stystate *S)
{
    if (S->pattern && S->pattern->ts[1]) return S->pattern->ts;
    if (S->style && S->style->ts[1]) return S->style->ts;
    return g_sty_ts44;
}
/* 77407c91 */
static const uint8_t *pat_ts(const dms_pattern *p, const dms_style *s)
{
    if (p->ts[1]) return p->ts;
    return s ? s->ts : g_sty_ts44;
}
static int pat_nparts(const dms_pattern *p) { int n = 0; for (const dms_partref *r = p->prefs; r; r = r->next) n++; return n; }

/* 77410163: the style at t (the first style counts from 0) and the time of the next */
static const dms_style *st_style_at(const stystate *S, mtime_t t, mtime_t *next)
{
    if (!S->ti) return NULL;
    const dms_stynode *x = S->ti->styles;
    while (x && !x->style) x = x->next;
    if (!x) return NULL;
    const dms_style *cur = x->style;
    const dms_stynode *n;
    for (n = x->next; n; n = n->next)
        if (n->style) { if (t < n->t) break; cur = n->style; }
    *next = n ? n->t : 0;
    return cur;
}

/* 7740dcbf: the mute cache (no lookup yet, the PChannel itself) */
static int st_mute_reset(stystate *S)
{
    free(S->mute);
    S->nmute = S->ti->npchs;
    S->mute = calloc(S->nmute ? S->nmute : 1, sizeof(*S->mute));
    if (!S->mute) return DM_E_OUTOFMEMORY;
    for (int i = 0; i < S->nmute; i++) { S->mute[i].t = 0; S->mute[i].pch = S->ti->pchs[i]; S->mute[i].mute = 0; }
    S->ti->pchschanged = 0;
    return DM_OK;
}

/* 7740f15b: the Park-Miller step of a seed (and a value below n) */
static uint32_t st_pm(uint32_t *seed, int32_t n)
{
    int64_t v = (int64_t)(int32_t)*seed * 48271;
    int32_t r = (int32_t)(v % 0x7fffffff);
    *seed = (uint32_t)r;
    return n ? (uint32_t)(r % n) : (uint32_t)r;
}

/* 77410079: the per-beat seeds of GUID_SeedVariations */
static int st_seed(stystate *S, int32_t seed)
{
    uint32_t len = S->ss && S->ss->seg ? (uint32_t)S->ss->seg->length : 0;
    int32_t beat = sty_beat(st_ts(S));
    int n = beat ? (int)(len / (uint32_t)beat) : 0;
    free(S->rtab);
    S->rtab = calloc(n ? n : 1, sizeof(*S->rtab));
    if (!S->rtab) { S->nrtab = 0; return DM_E_OUTOFMEMORY; }
    S->nrtab = n;
    uint32_t v = (uint32_t)seed;
    for (int i = 0; i < n; i++) { S->rtab[i] = v; st_pm(&S->rtab[i], 0); v = S->rtab[i]; }
    return DM_OK;
}

/* 7740f524: a random value below n for a time (the beat's seed, or rand()) */
static int32_t st_rand(dm_perf *pf, stystate *S, mtime_t t, int32_t n)
{
    if (!S->rtab) return wt_rand(pf) % n;
    int32_t beat = sty_beat(st_ts(S));
    uint32_t i = beat ? (uint32_t)t / (uint32_t)beat : 0;
    if ((int)i >= S->nrtab) i = (uint32_t)(S->nrtab - 1);
    return (int32_t)st_pm(&S->rtab[i], n);
}

/* ------------------------------------------------------------------ messages */
/* 7740fc44: the style's time signature (at the next style's time when that is later) */
static void st_tspmsg(dm_perf *pf, stystate *S, mtime_t t, mtime_t off, mtime_t next)
{
    if (!S->style) return;
    dm_pmsg *m = pm_alloc(pf, 0x40);
    if (!m) return;
    m->mt = next < t ? t + off : off + next;
    m->ts_bpm = S->style->ts[0]; m->ts_beat = S->style->ts[1];
    m->ts_grids = (uint16_t)(S->style->ts[2] | S->style->ts[3] << 8);
    m->flags |= PF_MUSICTIME;
    m->vpch = S->vid; m->type = PT_TIMESIG; m->group = S->group;
    stamp_send(pf, S->ss, m);
}

/* 7740fd26: GUID_NOTIFICATION_MEASUREANDBEAT on every beat of [from, to) */
static mtime_t st_mbnotify(dm_perf *pf, stystate *S, mtime_t from, mtime_t to, mtime_t off, uint32_t flags)
{
    if (flags & TF_NOTIFY_OFF) return 0;
    const uint8_t *ts = st_ts(S);
    if (!ts[1] || !ts[0]) return to;                /* (the original divides by zero) */
    int32_t beat = 0xc00 / ts[1];
    int32_t b = from / beat;
    uint32_t measure = (uint32_t)(b / (int32_t)ts[0]);
    uint8_t bt = (uint8_t)(b % (int32_t)ts[0]);
    for (; from < to; from += beat) {
        dm_pmsg *m = pm_alloc(pf, 0x58);
        if (m) {
            m->type = PT_NOTIFICATION; m->mt = from + off; m->flags = PF_MUSICTIME | PF_TOOL_ATTIME;
            m->user = S->ss; m->userref = 1; S->ss->refs++;
            m->nf_guid = GUID_NOTIFICATION_MEASUREANDBEAT_; m->nf_option = 0;
            m->nf_field1 = bt; m->nf_field2 = measure & 0xffff;
            m->group = S->group; m->vpch = S->vid;
            stamp_send(pf, S->ss, m);
        }
        if (++bt >= ts[0]) { bt = 0; measure++; }
    }
    return to;
}

/* ------------------------------------------------------------------ mute track lookups */
/* 7740f191: the mute track's answer for a PChannel at t, when the last one has run out */
static void st_mute_update(dm_perf *pf, stystate *S, uint32_t pch, mtime_t t, mtime_t off)
{
    if (S->ti->pchschanged && st_mute_reset(S) < 0) return;
    for (int i = 0; i < S->nmute; i++) {
        if (S->ti->pchs[i] != pch || S->mute[i].t < 0 || t < S->mute[i].t) continue;
        mtime_t next = 0;
        dms_muteparam mp = { S->ti->pchs[i], 0, 0 };
        if (perf_getparam(pf, PRM_MUTE, S->group, DM_SEG_ANYTRACK, t + off, &next, &mp) < 0) {
            S->mute[i].t = -1; S->mute[i].pch = S->ti->pchs[i]; S->mute[i].mute = 0;
            continue;
        }
        S->mute[i].t = next ? next + t : 0;
        S->mute[i].pch = mp.map; S->mute[i].mute = mp.mute;
    }
}
/* 7740f594: muted?  and the PChannel it plays on */
static int32_t st_mute_lookup(const stystate *S, uint32_t pch, uint32_t *map)
{
    for (int i = 0; i < S->ti->npchs && i < S->nmute; i++)
        if (S->ti->pchs[i] == pch) { *map = S->mute[i].pch; return S->mute[i].mute; }
    *map = 0;
    return 0;
}

/* ------------------------------------------------------------------ chord analysis (variation flags) */
/* 774075b4: a root's distance above the key (0..11) */
static uint8_t th_degree12(int8_t root, int8_t key)
{
    if (root < key) root = (int8_t)(root + (uint8_t)((uint8_t)(key - root - 1) / 12) * 12 + 12);
    return (uint8_t)((uint8_t)(root - key) % 12);
}
static uint32_t th_scale(uint32_t scale) { return scale ? (scale & 0xfff) << 12 | (scale & 0xfff) : 0xab5ab5; }
static uint8_t th_bits_upto(uint32_t scale, uint32_t bit)
{
    uint8_t n = 0;
    if (scale & bit) for (; bit; bit >>= 1) if (scale & bit) n++;
    return n;
}
/* 774075e6 / 77407633 / 7740768e: the scale degree (1..) of the chord's root, of the note below it, of the one above;
 * 0 when not in the scale */
static uint8_t th_deg(const dm_subchord *s, int8_t key, uint32_t scale)
{
    return th_bits_upto(th_scale(scale), 1u << th_degree12((int8_t)s->chordroot, key));
}
static uint8_t th_deg_below(const dm_subchord *s, int8_t key, uint32_t scale)
{
    uint32_t b = 1u << th_degree12((int8_t)s->chordroot, key);
    b = b == 1 ? 0x800 : b >> 1;
    return th_bits_upto(th_scale(scale), b);
}
static uint8_t th_deg_above(const dm_subchord *s, int8_t key, uint32_t scale)
{
    uint32_t b = 1u << th_degree12((int8_t)s->chordroot, key);
    b = b == 0x800 ? 1 : b << 1;
    return th_bits_upto(th_scale(scale), b);
}
/* 7740773a: the notes of all the subchords together (n, how many above the fifth); n < 5 */
static int th_notes(const dm_chord_key *c, int *n, int *high)
{
    uint32_t u = 0;
    for (int i = 0; i < c->nsub; i++) {
        int32_t d = (int32_t)c->sub[i].chordroot - (int32_t)c->sub[0].chordroot;
        if (d < 0) d = d + 12 + (int32_t)(((uint32_t)(-d) - 1u) / 12u) * 12;
        u |= c->sub[i].chord << (d & 31);
    }
    *n = 0; *high = 0;
    for (int i = 0; i < 12; i++)
        if (((u >> 12 | u) & 0xfff & (1u << i)) && (++*n, 7 < i)) ++*high;
    return *n < 5;
}
/* 774076ea: all the subchords alike */
static int th_simple(const dm_chord_key *c)
{
    for (int i = 1; i < c->nsub; i++)
        if (c->sub[i].chord != c->sub[0].chord || c->sub[i].scale != c->sub[0].scale ||
            c->sub[i].chordroot != c->sub[0].chordroot || c->sub[i].scaleroot != c->sub[0].scaleroot) return 0;
    return 1;
}
static uint32_t th_shift(uint32_t pattern, uint8_t d)
{
    if ((pattern & 0x91) == 0x91) return (uint8_t)(d - 1);
    if ((pattern & 0x89) == 0x89) return (uint8_t)(d + 6);
    return (uint8_t)(d + 13);
}
/* 774077c4: the DirectX 7 variation flags for a chord (and the next) */
static uint32_t th_flags7(const dms_pattern *p, mtime_t t, mtime_t next, const dm_chord_key *c, const dm_chord_key *nc)
{
    const dm_subchord *s = &c->sub[0];
    uint8_t d = th_deg(s, (int8_t)c->key, c->scale);
    uint32_t f = d ? 1u << ((d - 1) & 31) : 0x80;
    if ((s->chord & 0x91) == 0x91) f |= 0x100;
    else if ((s->chord & 0x89) == 0x89) f |= 0x200;
    else f |= 0x400;
    if (nc->nsub) {
        uint8_t d2 = th_deg(&nc->sub[0], (int8_t)c->key, c->scale);
        if ((int32_t)(sty_beat(p->ts) * p->measures * p->ts[0]) + t < next) d2 = th_deg(s, (int8_t)c->key, c->scale);
        if (d2 == 1) f |= 0x800;
        else if (d2 == 5) f |= 0x1000;
    }
    f |= th_simple(c) ? 0x2000 : 0x4000;
    return f;
}
/* 774078d3: the DirectX 8 variation flags (DMUS_VARIATIONF_DMODES): for the root's degree, the one below, above */
static void th_flags8(const dms_pattern *p, mtime_t t, mtime_t next, const dm_chord_key *c, const dm_chord_key *nc,
                      int dx8, uint32_t *f6, uint32_t *f7, uint32_t *f8)
{
    *f8 = 0; *f7 = 0; *f6 = 0;
    const dm_subchord *s = &c->sub[0];
    uint8_t a = th_deg(s, (int8_t)c->key, c->scale), b = th_deg_below(s, (int8_t)c->key, c->scale),
            k = th_deg_above(s, (int8_t)c->key, c->scale);
    if (!a) {
        if (b) { *f7 |= 0x800000; *f7 |= 1u << (th_shift(s->chord, b) & 31); }
        if (k) { *f8 |= 0x400000; *f8 |= 1u << (th_shift(s->chord, k) & 31); }
    } else {
        uint32_t v = 1u << (th_shift(s->chord, a) & 31) | 0x200000;
        *f6 = *f8 = *f7 = v;
        b = k = a;
    }
    if (nc->nsub) {
        uint8_t d = th_deg(&nc->sub[0], (int8_t)c->key, c->scale);
        if ((int32_t)(sty_beat(p->ts) * p->measures * p->ts[0]) + t < next) d = th_deg(s, (int8_t)c->key, c->scale);
        uint32_t add = 0;
        if (d == 1) add = 0x8000000;
        else if (d == 5) add = 0x10000000;
        else if (dx8) add = 0x40000000;
        if (add) { if (a) *f6 |= add; if (b) *f7 |= add; if (k) *f8 |= add; }
    }
    int n, high;
    uint32_t add = th_notes(c, &n, &high) ? (n < 4 ? 0x1000000 : n == 4 ? 0x2000000 : 0x4000000) : 0x4000000;
    if (a) *f6 |= add;
    if (b) *f7 |= add;
    if (k) *f8 |= add;
}

/* 77408ca6: the first of a variation's markers at or after t with the flags; 77408d03: one exactly at t */
static int part_marker_after(const dms_part *pt, int v, mtime_t t, uint16_t flags, int onchord, mtime_t *out)
{
    *out = 0;
    if (v < 0 || v >= 32) return STY_E_FAIL;
    for (const dms_marker *m = pt->markers[v]; m; m = m->next)
        if (t <= m->t && (m->flags & flags) == flags && (onchord || !(m->flags & 4))) { *out = m->t; return 0; }
    return STY_E_FAIL;
}
static int part_marker_at(const dms_part *pt, int v, mtime_t t, uint16_t flags, int onchord)
{
    if (v < 0 || v >= 32) return 0;
    for (const dms_marker *m = pt->markers[v]; m; m = m->next)
        if (m->t == t && (m->flags & flags) == flags && (onchord || !(m->flags & 4))) return 1;
    return 0;
}

/* 7740a4fd: a part's variation for the chord at t (next chord at nextt).  flags: 2 new pattern, 4 chord change,
 * 8 at a marker, 0x10 seeking, 0x20 DirectX 8 style, 0x40 the marker pass */
static int st_variation(dm_perf *pf, stystate *S, const dms_pattern *pat, const dms_partref *pr, int idx, uint32_t flags,
                        mtime_t t, mtime_t nextt)
{
    const dm_chord_key *c = &S->chord, *nc = &S->nextchord;
    int fnew = (flags >> 1) & 1, fchord = (flags >> 2) & 1, fmark = (flags >> 3) & 1, fseek = (flags >> 4) & 1;
    int dx8 = (flags >> 5) & 1;
    uint32_t allowed = 0xffffffff;
    int onchord = (t == S->chordt || t == S->nextchordt);
    uint32_t m6, m7, m8;
    th_flags8(pat, t, nextt, c, nc, dx8, &m6, &m7, &m8);
    uint32_t m7x = th_flags7(pat, t, nextt, c, nc);
    uint8_t lockid = pr->lock;
    const dms_part *part = pr->part;
    mtime_t rel = S->partoff[idx] + (S->kind == TRK_STYLE ? S->pstart : 0);     /* 774113cf / 7740fe83 */
    rel = t < rel ? 0 : t - rel;
    mtime_t mark = 0;
    if (lockid) {
        uint8_t v = S->lock[lockid - 1];
        if (v != 0xff) { S->var[idx] = v; return 0; }
    }
    uint32_t match = 0, nonempty = 0;
    int16_t count = 0, ncount = 0;
    int have = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t bit = 1u << i;
        if (!(allowed & bit)) continue;
        uint32_t v = part->varchoices[i];
        if (v & 0x5fffffff) { ncount++; nonempty |= bit; }
        uint32_t a = m7x, b = m7x, k = m7x;
        if (v & 0x20000000) { a = m6; b = m7; k = m8; }
        if ((a && (v & a) == a) || (b && (v & b) == b) || (k && (v & k) == k)) {
            count++; match |= bit;
            if ((fseek || fmark) && (part->flags & 1)) {
                mtime_t mt;
                if (part_marker_after(part, i, rel, 1, onchord, &mt) >= 0 && (!have || mt < mark)) { mark = mt; have = 1; }
            }
        }
    }
    uint32_t matched = match, use = match;
    uint8_t mode = pr->random;
    uint32_t keep = 0;
    int dokeep = 0;
    if (mode == 4) {
        keep = ~S->norep[idx] & match;
        if (!keep) { S->norep[idx] = 0; mode = 3; }
        else dokeep = 1;
    }
    if (!dokeep && mode == 3 && S->var[idx] != 0xff) { keep = ~(1u << (S->var[idx] & 31)) & match; dokeep = 1; }
    if (dokeep && keep != match && keep) {
        for (int i = 0; i < 32; i++) { uint32_t bit = 1u << i; if ((match & bit) && !(keep & bit)) count--; }
        use = keep;
    }
    match = use;
    if (!count) {
        if (!ncount) { count = 16; match = 0xffffffff; }
        else { count = ncount; match = nonempty; }
    }
    uint32_t saved = match;
    int16_t scount = count;
    if (((fseek && mark <= t) || fmark) && (part->flags & 1)) {
        for (int i = 0; i < 32; i++) {
            uint32_t bit = 1u << i;
            if ((match & bit) && !part_marker_at(part, i, mark, 1, onchord)) { match &= ~bit; count--; }
        }
        if (!match) { match = saved; count = scount; }
    }
    uint8_t cur = S->var[idx];
    if (!fnew) {
        if (!((part->flags >> 1) & 1)) { if (fchord) goto choose; }
        else if (!fchord) return 0;
        if (matched & (1u << (cur & 31))) return 0;
    }
choose:
    switch (pr->random) {
    case 0:     /* DMUS_VARIATIONT_SEQUENTIAL */
    seq:
        do cur++; while (!(match & (1u << ((cur + 0x20) % 0x20))));
        S->var[idx] = cur % 0x20;
        break;
    case 2:     /* RANDOM_START */
        if (cur != 0xff) goto seq;
        {
            uint32_t r = (uint32_t)st_rand(pf, S, t, count);
            uint32_t k = 0;
            int16_t i;
            for (i = 0; i < 32; i++) if (match & (1u << i)) { if ((r & 0xff) == k) break; k++; }
            S->var[idx] = (uint8_t)i;
        }
        break;
    case 1: case 3: case 4: {   /* RANDOM, NO_REPEAT, RANDOM_ROW */
        int16_t r = (int16_t)st_rand(pf, S, t, count), k = 0, i;
        for (i = 0; i < 32; i++) if (match & (1u << i)) { if (r == k) break; k++; }
        S->var[idx] = (uint8_t)i;
        if (pr->random == 4) S->norep[idx] |= 1u << (i & 31);
        break;
    }
    }
    if (lockid) S->lock[lockid - 1] = S->var[idx];
    return 0;
}

/* 7740afa0: every part's variation (the lock table cleared first) */
static void st_variations(dm_perf *pf, stystate *S, uint32_t flags, mtime_t t, mtime_t nextt)
{
    memset(S->lock, 0xff, sizeof(S->lock));
    int i = 0;
    for (const dms_partref *pr = S->pattern->prefs; pr; pr = pr->next, i++)
        if (st_variation(pf, S, S->pattern, pr, i, flags, t, nextt) < 0) break;
}
/* 7740b089: at a marker, the part's variation again (and its lock group's) */
static void st_variation_marker(dm_perf *pf, stystate *S, const dms_partref *pr, int idx, uint32_t flags, mtime_t t,
                                mtime_t nextt)
{
    uint8_t lockid = pr->lock;
    if (!lockid) { st_variation(pf, S, S->pattern, pr, idx, flags, t, nextt); return; }
    if ((flags >> 6) & 1) return;
    S->lock[lockid - 1] = 0xff;
    int i = 0;
    for (const dms_partref *q = S->pattern->prefs; q; q = q->next, i++)
        if (q->lock == lockid && st_variation(pf, S, S->pattern, q, i, flags, t, nextt) < 0) break;
}

/* 774104b3: the chord at t (and the next), then the variations for it */
static const dm_chord_key g_sty_m7 = { { 'M', '7' }, 0, 0, 1,
    { { 0x891, 0xab5ab5, 0xffffff, 0xffffffff, 0x0c, 0 }, { 0x891, 0xab5ab5, 0xffffff, 0xffffffff, 0x0c, 0 },
      { 0x891, 0xab5ab5, 0xffffff, 0xffffffff, 0x0c, 0 }, { 0x891, 0xab5ab5, 0xffffff, 0xffffffff, 0x0c, 0 },
      { 0x891, 0xab5ab5, 0xffffff, 0xffffffff, 0x0c, 0 }, { 0x891, 0xab5ab5, 0xffffff, 0xffffffff, 0x0c, 0 },
      { 0x891, 0xab5ab5, 0xffffff, 0xffffffff, 0x0c, 0 }, { 0x891, 0xab5ab5, 0xffffff, 0xffffffff, 0x0c, 0 } },
    0xab5ab5, 0x0c, 0 };
static void st_chord(dm_perf *pf, stystate *S, mtime_t t, mtime_t off, int seek, int novar)
{
    if (perf_getparam(pf, PRM_CHORD, S->group, DM_SEG_ANYTRACK, t + off, &S->nextchordt, &S->chord) < 0) {
        S->chordt = 0; S->nextchordt = 0;
        if (!S->style || !(S->style->flags & 1)) {
            S->chord.name[0] = 'M'; S->chord.name[1] = '7'; S->chord.name[2] = 0;
            S->chord.measure = 0; S->chord.beat = 0;
            S->chord.sub[0].levels = 0xffffffff; S->chord.sub[0].scaleroot = 0;
            S->chord.nsub = 1; S->chord.key = 0x0c; S->chord.scale = 0xab5ab5;
            S->chord.sub[0].chord = 0x891; S->chord.sub[0].scale = 0xab5ab5; S->chord.sub[0].inversion = 0xffffff;
            S->chord.sub[0].chordroot = 0x0c;
        }
    } else {
        S->chordt = t;
        if (S->nextchordt) S->nextchordt += t;
    }
    if (0 < S->nextchordt) {
        if (perf_getparam(pf, PRM_CHORD, S->group, DM_SEG_ANYTRACK, S->nextchordt + off, &S->nnchordt, &S->nextchord) >= 0
            && S->nnchordt)
            S->nnchordt += S->nextchordt;
    }
    if (novar) return;
    uint32_t f = S->newpat ? 2 : 0;
    if (seek) f |= 0x10;
    if (S->style && (S->style->flags & 1)) f |= 0x20;
    st_variations(pf, S, f, t, S->nextchordt);
    S->newpat = 0;
    if ((S->pattern->flags & 1) && S->ti->pvar && S->ti->pnorep)       /* (a motif track's, kept across plays) */
        for (int i = 0; i < S->nparts; i++) { S->ti->pvar[i] = S->var[i]; S->ti->pnorep[i] = S->norep[i]; }
}

/* ------------------------------------------------------------------ notes */
/* 7740738e: a note's place in two octaves above the base (0..23) */
static int8_t inv_pos(int8_t a, int8_t b)
{
    int8_t c = (int8_t)(a - b);
    if (c < 0) {
        uint8_t q = (uint8_t)((uint8_t)(0xff - (uint8_t)c) / 12);
        c = (int8_t)(c + (int8_t)(q * 12 + 12));
    }
    if (c > 0x17) {
        uint8_t q = (uint8_t)((uint8_t)(c - 0x18) / 12);
        c = (int8_t)(c + (int8_t)(0xf4 - q * 12));
    }
    return c;
}
static int8_t inv_span(int8_t d)
{
    if (d > 0x17) { uint8_t q = (uint8_t)((uint8_t)(d - 0x18) / 0x18); d = (int8_t)(d + (int8_t)(0xe8 - q * 0x18)); }
    return d;
}
static int inv_bit(int8_t c, uint32_t mask) { return c >= 0 && c < 0x18 ? (mask >> c) & 1 : 0; }
/* 774073c7: up from a note below the range to an inversion point, by octaves */
static int8_t inv_up(uint8_t note, uint8_t lower, uint32_t mask, uint8_t base)
{
    int8_t res = 0, c = inv_pos((int8_t)note, (int8_t)base), n = inv_span((int8_t)(lower - note));
    if (n < 0) return 0;
    do {
        if (inv_bit(c, mask)) break;
        c = c < 0x17 ? (int8_t)(c + 1) : 0;
    } while (--n >= 0);
    if (n < 0) return 0;
    if (lower > note) do res = (int8_t)(res + 12); while ((int)lower > res + (int)note);
    return res;
}
/* 7740744f: down from a note above the range */
static int8_t inv_down(uint8_t note, uint8_t upper, uint32_t mask, uint8_t base)
{
    int8_t res = 0, c = inv_pos((int8_t)note, (int8_t)base), n = inv_span((int8_t)(note - upper));
    if (n < 0) return 0;
    do {
        if (inv_bit(c, mask)) break;
        c = c > 0 ? (int8_t)(c - 1) : 0x17;
    } while (--n >= 0);
    if (n < 0) return 0;
    if ((int8_t)note > (int8_t)upper) do res = (int8_t)(res - 12); while (res + (int8_t)note > (int8_t)upper);
    return res;
}
/* 774074d6: the transposition that brings a note into the part's range */
static int8_t inv_range(uint8_t note, uint8_t lower, uint8_t upper, uint32_t mask, uint8_t base)
{
    if (note == 0 || note > 0x7f) return 0;
    if (mask >> 24) {               /* octaves only */
        int8_t r = 0;
        if (note < lower) do r = (int8_t)(r + 12); while (r + (int)note < (int)lower);
        while (r + (int)note > (int)upper) r = (int8_t)(r - 12);
        return r;
    }
    if (lower > note) return inv_up(note, lower, mask, base);
    if (note <= upper) return 0;
    if (note >= lower) return inv_down(note, upper, mask, base);
    return inv_up(note, lower, mask, base);
}

/* 77408907: a note's MIDI value under the chord (the subchord of the part's level), with the part's inversions */
static int32_t st_convert(stystate *S, const dms_partref *pr, const dms_event *ev, uint8_t pm, uint8_t *midi, int16_t *tr)
{
    const dm_chord_key *c = &S->chord;
    int32_t hr = 0;
    uint16_t mv = ev->mv;
    int16_t transpose = 0;
    if (pm) {
        dm_subchord L = { 0, 0, 0, 2, 0, 0, { 0, 0 } };
        uint32_t bit = 1u << (pr->subchord & 31);
        unsigned idx = 0, n = c->nsub;
        for (idx = 0; idx < n; idx++) if (c->sub[idx].levels & bit) { L = c->sub[idx]; break; }
        if (idx == n) { L = c->sub[0]; idx = 0; }
        dm_chord_key K;
        memset(&K, 0, sizeof(K));
        K.scale = c->scale; K.flags = 0; K.nsub = c->nsub; K.key = c->key;
        K.sub[idx].chordroot = L.chordroot; K.sub[idx].scaleroot = L.scaleroot;
        K.sub[idx].chord = L.chord; K.sub[idx].levels = L.levels; K.sub[idx].scale = L.scale;
        uint8_t base = 0;
        dmt_music_to_midi((uint16_t)(mv & 0xf000), &K, pm, pr->subchord, &base);
        if (pm == 2) mv = (uint16_t)(mv + L.chordroot % 24);
        else if (pm == 1) mv = (uint16_t)(mv + c->key);
        else {
            uint8_t o = 0;
            if (dmt_music_to_midi(ev->mv, &K, pm, pr->subchord, &o) == 0) mv = o;
            else { mv = 0; hr = STY_E_FAIL; }
        }
        if (mv && !S->noinv) {
            uint8_t inv = ev->inversion;
            const dms_part *part = pr->part;
            if (!inv) transpose = inv_range((uint8_t)mv, part->invlower, part->invupper, L.inversion, base);
            else {
                int k;
                for (k = 0; k < 16 && S->inv[k].id != inv; k++) {}
                if (k == 16) {
                    int16_t cnt = 1;
                    for (const dms_event *e = ev->next; e; e = e->next) if (e->type == EV_NOTE && e->inversion == inv) cnt++;
                    int8_t r = inv_range((uint8_t)mv, part->invlower, part->invupper, L.inversion, base);
                    for (k = 0; k < 16 && S->inv[k].id; k++) {}
                    if (k < 16) { S->inv[k].id = inv; S->inv[k].count = (uint16_t)cnt; S->inv[k].tr = r; }
                }
                if (k < 16) {
                    transpose = S->inv[k].tr;
                    if (--S->inv[k].count == 0) S->inv[k].id = 0;
                }
            }
        }
    }
    mv = (uint16_t)(mv + transpose);
    if (mv > 0x7f) { int n = (uint16_t)(mv - 0x80) / 12 + 1; while (n--) mv = (uint16_t)(mv + 0xfff4); }
    *midi = (uint8_t)mv;
    *tr = transpose;
    return hr;
}

/* 7740f5e3: a random spread for a range byte (larger ranges in coarser steps) */
static int32_t st_range(dm_perf *pf, uint8_t r)
{
    int32_t u;
    if (r < 0xbf) u = r;
    else if (r < 0xd5) u = (r - 0x98) * 5;
    else if (r < 0xe9) u = (r - 0xb6) * 10;
    else u = (r - 0xde) * 50;
    return wt_rand(pf) % u - (u >> 1);
}

static uint16_t ts_measure_of(const uint8_t *ts, uint16_t g)
{
    uint16_t gpb = (uint16_t)(ts[2] | ts[3] << 8);
    return gpb && ts[0] ? (uint16_t)(g / gpb / ts[0]) : 0;
}
static uint8_t ts_beat_of(const uint8_t *ts, uint16_t g)
{
    uint16_t gpb = (uint16_t)(ts[2] | ts[3] << 8);
    return gpb && ts[0] ? (uint8_t)(g / gpb % ts[0]) : 0;
}
static uint8_t ts_grid_of(const uint8_t *ts, uint16_t g)
{
    uint16_t gpb = (uint16_t)(ts[2] | ts[3] << 8);
    return gpb ? (uint8_t)(g % gpb) : 0;
}

/* 7740f63c: one event of a part: a note, a curve, or a marker (the variation again) */
static void st_event(dm_perf *pf, stystate *S, mtime_t t, const dms_event *ev, const uint8_t *ts, mtime_t partoff,
                     mtime_t off, const dms_partref *pr, int idx, mtime_t chase, uint8_t *mflag)
{
    uint32_t map = 0;
    if (st_mute_lookup(S, pr->pch, &map)) return;
    uint32_t vbit = S->var[idx] == 0xff ? 0 : 1u << (S->var[idx] & 31);       /* 7740f577 */
    if (!(ev->var & vbit)) return;
    if (ev->type == EV_MARKER) {
        if (pr->part && (pr->part->flags & 1) && (ev->mflags & 2) && t != S->patt &&
            (!(ev->mflags & 4) || t == S->chordt || t == S->nextchordt)) {
            uint32_t f = (ev->mflags & 4) ? 0xe : 0xa;
            if (S->style && (S->style->flags & 1)) f |= 0x20;
            if (*mflag) f |= 0x40;
            st_variation_marker(pf, S, pr, idx, f, ev->offset + t, S->nextchordt);
            *mflag = 1;
            if ((S->pattern->flags & 1) && S->ti->pvar && S->ti->pnorep) { S->ti->pvar[idx] = S->var[idx]; S->ti->pnorep[idx] = S->norep[idx]; }
        }
        return;
    }
    if (ev->type == EV_CURVE) {
        dm_pmsg *m = pm_alloc(pf, 0x58);
        if (!m) return;
        mtime_t at = ev->offset + partoff + dms_grid_clocks(ts, (uint16_t)ev->grid);
        m->c_measure = ts_measure_of(ts, (uint16_t)ev->grid);
        m->c_beat = ts_beat_of(ts, (uint16_t)ev->grid);
        m->c_grid = ts_grid_of(ts, (uint16_t)ev->grid);
        m->c_offset = ev->offset;
        m->mt = at + off; m->flags = PF_MUSICTIME;
        m->c_resetdur = ev->resetdur; m->c_dur = ev->dur; m->c_reset = ev->creset; m->c_flags = ev->cflags;
        m->type = PT_CURVE; m->pch = map; m->vpch = S->vid;
        m->c_start = ev->cstart; m->c_end = ev->cend;
        m->c_type = ev->ctype; m->c_shape = ev->shape; m->c_cc = ev->cc;
        m->group = S->group; m->c_param = ev->param; m->c_merge = ev->merge;
        m->flags |= PF_DX8;
        if (chase) {
            mtime_t at2 = chase + partoff + off;
            if (m->c_dur + m->mt < at2) {
                m->c_resetdur += (m->mt - (partoff + off)) - chase;
                if (m->c_resetdur < 0) m->c_resetdur = 0;
                m->mt = at2;
                m->c_shape = 1;         /* DMUS_CURVES_INSTANT */
            } else {
                m->c_origstart = m->mt;
                m->mt = at2;
            }
        }
        stamp_send(pf, S->ss, m);
        return;
    }
    if (ev->type != EV_NOTE) return;
    uint8_t pm = ev->playmode;
    if (pm & 0x10) pm = pr->part->playmode;
    uint8_t midi = 0;
    int16_t tr = 0;
    if (st_convert(S, pr, ev, pm, &midi, &tr) < 0) return;
    dm_pmsg *m = pm_alloc(pf, 0x50);
    if (!m) return;
    m->n_flags = (uint8_t)(ev->noteflags | NOTEF_NOTEON);
    mtime_t at = ev->offset + partoff + dms_grid_clocks(ts, (uint16_t)ev->grid);
    m->n_measure = ts_measure_of(ts, (uint16_t)ev->grid);
    m->n_beat = ts_beat_of(ts, (uint16_t)ev->grid);
    m->n_grid = ts_grid_of(ts, (uint16_t)ev->grid);
    m->n_offset = ev->offset;
    m->mt = at + off; m->flags = PF_MUSICTIME;
    if (ev->timerange) m->mt += st_range(pf, ev->timerange);
    m->n_dur = ev->dur;
    if (ev->durrange) m->n_dur += st_range(pf, ev->durrange);
    m->n_vel = ev->vel;
    if (ev->velrange) m->n_vel = (uint8_t)(m->n_vel + (int8_t)((int8_t)(wt_rand(pf) % ev->velrange) - (ev->velrange >> 1)));
    if (!m->n_vel) m->n_vel = 1;
    if (m->n_vel > 0x7f) m->n_vel = 0x7f;
    m->n_musicval = ev->mv; m->n_midival = midi;
    m->type = PT_NOTE; m->n_playmode = pm; m->pch = map; m->vpch = S->vid;
    m->n_subchord = pr->subchord; m->group = S->group;
    m->n_timerange = ev->timerange; m->n_durrange = ev->durrange; m->n_velrange = ev->velrange;
    m->n_transpose = (int8_t)tr;
    stamp_send(pf, S->ss, m);
}

/* 7740fee6: a curve's controller slot for the chase (pitch bend 0x100, CC, aftertouch 0x101, poly pressure) */
static int chase_slot(const dms_event *e)
{
    if (e->type != EV_CURVE) return -1;
    switch (e->ctype) {
    case 3: return 0x100;
    case 4: return e->cc & 0x7f;
    case 5: return 0x101;
    case 6: return (e->cc & 0x7f) + 0x80;
    }
    return -1;
}
/* 7740ff4f */
static void chase_add(stychase *C, const dms_event *e, mtime_t t)
{
    int k = chase_slot(e);
    if (k < 0 || (C->ev[k] && !(C->t[k] < t))) return;
    C->ev[k] = e; C->t[k] = t; C->any = 1;
}
/* 7740ff8a: each controller's last curve (of those on its grid, the latest offset) */
static void chase_flush(dm_perf *pf, stychase *C, stystate *S, const uint8_t *ts, mtime_t partoff, mtime_t off,
                        const dms_partref *pr, int idx, mtime_t chase)
{
    if (!C->any) return;
    for (int k = 0; k < 0x102; k++) {
        const dms_event *e = C->ev[k];
        if (!e) continue;
        const dms_event *best = e;
        int16_t o = e->offset;
        for (const dms_event *x = e; x && x->grid == e->grid; x = x->next)
            if (x->type == EV_CURVE && o < x->offset) { best = x; o = x->offset; }
        uint8_t f = 0;
        mtime_t t = dms_grid_clocks(ts, (uint16_t)best->grid) + partoff;
        st_event(pf, S, t, best, ts, partoff, off, pr, idx, chase, &f);
    }
    C->any = 0;
}

/* 7741066b: the parts' events in [start, end).  flags: 1 clock time, 2 parts repeat in the pattern, 4 from the start
 * (seek), 8 the parts' second time round, 0x10 mute lookups again */
static int st_play_parts(dm_perf *pf, stystate *S, mtime_t start, mtime_t end, mtime_t off, mtime_t pstart,
                         uint32_t flags, uint32_t tflags, uint8_t *again)
{
    if (tflags & TF_PLAY_OFF) return DM_OK;
    if (!S->pattern || !S->mflag) return (int)0x88781162;
    int rep = (flags >> 1) & 1, seek = (flags >> 2) & 1, second = (flags >> 3) & 1, remute = (flags >> 4) & 1;
    int chordseek = seek;
    mtime_t lastt = start;
    static stychase C;
    int idx = 0;
    for (const dms_partref *pr = S->pattern->prefs; pr; pr = pr->next, idx++) {
        S->mflag[idx] = 0;
        mtime_t stop = end;
        memset(&C, 0, sizeof(C));
        const dms_part *part = pr->part;
        const uint8_t *ts = part->ts[1] ? part->ts : st_ts(S);
        int32_t plen = sty_beat(ts) * part->measures * ts[0];
        if (rep) {
            if (remute) { st_mute_update(pf, S, pr->pch, start, off); S->cursor[idx] = NULL; }
            if (seek) S->partoff[idx] = 0;
            if (plen) while (!(start < pstart + S->partoff[idx] + plen)) S->partoff[idx] += plen;
            if (S->partoff[idx] + plen + pstart < end) { *again = 1; stop = S->partoff[idx] + plen + pstart; }
        }
        if (second) {
            mtime_t n = plen + S->partoff[idx];
            if (!(pstart + n < stop)) continue;
            S->partoff[idx] = n;
        }
        const dms_event *ev = (rep && S->cursor[idx]) ? S->cursor[idx] : part->events;
        mtime_t t = 0;
        if (ev) t = dms_grid_clocks(ts, (uint16_t)ev->grid) + S->partoff[idx] + pstart;
        if (ev) {
            st_mute_update(pf, S, pr->pch, t, off);
            do {
                if (stop <= t) break;
                if (rep && seek && t < start) {
                    uint32_t vbit = S->var[idx] == 0xff ? 0 : 1u << (S->var[idx] & 31);
                    if (ev->type == EV_CURVE && (ev->var & vbit)) chase_add(&C, ev, t);
                } else if (start <= t) {
                    if (t < lastt) { st_chord(pf, S, t, off, seek, 0); lastt = t; }
                    else if (S->nextchordt <= t || S->nextchordt == 0) { st_chord(pf, S, t, off, chordseek, 0); chordseek = 0; lastt = t; }
                    st_event(pf, S, t, ev, ts, S->partoff[idx] + pstart, off, pr, idx, 0, &S->mflag[idx]);
                }
                ev = ev->next;
                if (ev) t = dms_grid_clocks(ts, (uint16_t)ev->grid) + S->partoff[idx] + pstart;
                st_mute_update(pf, S, pr->pch, ev ? t : stop - 1, off);
            } while (ev);
        }
        S->cursor[idx] = ev;
        if (rep && seek) {
            mtime_t po = S->partoff[idx];
            chase_flush(pf, &C, S, ts, po + pstart, off, pr, idx, start - po - pstart);
        }
    }
    return DM_OK;
}

/* ------------------------------------------------------------------ the pattern chooser (7740b74f) */
typedef struct { dms_pattern *p; int32_t code; } stycand;
typedef struct {
    stycand *c; int n;                          /* +0x00 +0x04 */
    uint8_t empty;                              /* +0x08 */
    uint32_t *rhythm;                           /* +0x0c per measure: the rhythm wanted */
    dms_cmdparam2 *cmds;                        /* +0x10 per measure: the command */
    int32_t nmeas;                              /* +0x14 measures to the next style */
    mtime_t next, t, off;                       /* +0x18 +0x1c +0x20 */
    int32_t measlen;                            /* +0x24 */
    const dms_style *style;                     /* +0x28 */
    stystate *S;                                /* +0x2c */
    dm_perf *pf;                                /* +0x30 */
    int32_t nq;                                 /* +0x34 measures asked */
    int32_t best;                               /* +0x38 */
    int32_t maxm;                               /* +0x3c */
    uint32_t tsbeats, tsbeat;                   /* +0x40 +0x44 */
} styfilter;
typedef struct stycache { struct stycache *next; int32_t measlen; dms_cmdparam2 *cmds; uint32_t *rhythm; } stycache;

/* 7740610e: no candidate left */
static int f_empty(styfilter *F)
{
    if (F->empty) return 1;
    for (int i = 0; i < F->n; i++) if (!F->c[i].code) return 0;
    F->empty = 1;
    return 1;
}
/* 774061d7: the k-th candidate */
static dms_pattern *f_nth(const styfilter *F, int k)
{
    int i;
    for (i = 0; i < F->n; i++) if (!F->c[i].code) { if (!k) break; k--; }
    return i < F->n && !F->c[i].code ? F->c[i].p : NULL;
}
/* 77406158: the ones rejected for code back; 774061a2: and the candidates rejected as code2 */
static void f_restore(styfilter *F, int32_t code)
{
    for (int i = 0; i < F->n; i++) if (F->c[i].code == code) F->c[i].code = 0;
    F->empty = 0;
}
static void f_swap(styfilter *F, int32_t code, int32_t code2)
{
    for (int i = 0; i < F->n; i++) {
        if (F->c[i].code == code) F->c[i].code = 0;
        else if (!F->c[i].code) F->c[i].code = code2;
    }
    F->empty = 0;
}
/* 7740613b */
static void f_reset(styfilter *F) { for (int i = 0; i < F->n; i++) F->c[i].code = 0; F->empty = 0; }
/* 774161fb: a pattern's index while it is a candidate */
static int f_index(const styfilter *F, const dms_pattern *p, int *k)
{
    for (int i = 0; i < F->n; i++) if (F->c[i].p == p) { if (F->c[i].code) return 1; *k = i; return 0; }
    return 1;
}
/* 77406181 */
static void f_set(styfilter *F, int k, int32_t code) { if (k < F->n) F->c[k].code = code; if (!code) F->empty = 0; }
/* 7741619d: a random candidate */
static dms_pattern *f_random(dm_perf *pf, styfilter *F)
{
    int cnt = 0;
    for (int i = 0; i < F->n; i++) if (!F->c[i].code) cnt++;
    if (!cnt) return NULL;
    int r = wt_rand(pf) % cnt, k = 0, i;
    for (i = 0; i < F->n; i++) if (!F->c[i].code) { if (k == r) break; k++; }
    return i < F->n ? F->c[i].p : NULL;
}
/* 77416235 */
static void f_measlen(styfilter *F, int32_t ml)
{
    int32_t n = F->next;
    F->measlen = ml;
    if (!ml) n = 0;
    else {
        if (F->style && (F->style->flags & 1) && n % ml) n += ml - n % ml;
        n /= ml;
    }
    F->nmeas = n;
}

/* 77407be5: the pattern is for the command's embellishment */
static int pat_emb_ok(const dms_pattern *p, const dms_cmdparam2 *c)
{
    uint16_t e = p->embellish;
    if (e & 0x10) return 0;
    uint8_t hi = (uint8_t)(e >> 8);
    if (hi) return hi == c->command;
    switch (c->command) {
    case 0: return e == 0;
    case 1: return (e & 1) != 0;
    case 2: return (e & 4) != 0;
    case 3: return (e & 2) != 0;
    case 4: return (e & 8) != 0;
    }
    return 0;
}
/* 77407b9e / 77407c36: the command's groove range meets the pattern's (destination) range */
static int groove_meets(uint8_t lo, uint8_t hi, const dms_cmdparam2 *c)
{
    uint8_t g = c->groove, h = c->range >> 1;
    uint8_t a = g < h ? 1 : (uint8_t)(g - h);
    uint32_t b = (uint32_t)h + g;
    if (b > 100) b = 100;
    return !((uint8_t)b < lo || hi < a);
}
static int pat_groove_ok(const dms_pattern *p, const dms_cmdparam2 *c)
{
    if (p->embellish & 0x10) return 0;
    return groove_meets(p->groovelo, p->groovehi, c);
}
static int pat_dest_ok(const dms_pattern *p, const dms_cmdparam2 *c)
{
    if (p->embellish & 0x10) return 0;
    if (p->destlo && p->desthi && !groove_meets(p->destlo, p->desthi, c)) return 0;
    return 1;
}
/* 77408c2a: the pattern suits the commands of its first n measures */
static int pat_cmds_ok(const dms_pattern *p, const dms_cmdparam2 *c, int16_t n)
{
    uint16_t k = (int)n <= (int)p->measures ? (uint16_t)n : p->measures;
    if (p->embellish & 0x10) return 0;
    for (int i = 0; i < (int16_t)k; i++)
        if ((i < 1 || c[i].command || c[i].groove != c[0].groove) && (!pat_emb_ok(p, &c[i]) || !pat_groove_ok(p, &c[i])))
            return 0;
    return 1;
}
/* 77407b42: the pattern's rhythm (its beats, counted) holds the one wanted in each of the first n measures */
static int16_t pat_rhythm(const dms_pattern *p, const uint32_t *want, int16_t n)
{
    int16_t r = 0;
    for (int i = 0; i < n; i++) {
        if (p->measures <= i) return r;
        uint32_t have = p->rhythm ? p->rhythm[i] : 0;
        if ((have & want[i]) != want[i]) return 0;
        for (int b = 0; b < 32; b++) r = (int16_t)(r + ((have >> b) & 1));
    }
    return r;
}

static int32_t st_getcommand(const dms_style *s, mtime_t t, mtime_t off, dm_perf *pf, dm_segment *seg, uint32_t group,
                             dms_cmdparam2 *cp, uint8_t *cmd);

/* 77415eed: a candidate passes a criterion (8 and 9 gather the longest pattern and signature) */
static int f_crit(styfilter *F, stycand *e, int32_t crit)
{
    const dms_pattern *p = e->p;
    if (!p) return 0;
    int16_t c;
    switch (crit) {
    case 1:     /* the rhythm and the commands (the fewest beats that hold the rhythm) */
        c = pat_rhythm(p, F->rhythm, (int16_t)F->nq);
        if (!c) return 0;
        if (F->best && F->best < c) return 0;
        if (F->nq && F->nq < (int32_t)p->measures) return 0;
        if (F->nmeas && F->nmeas < (int32_t)p->measures) return 0;
        if (!pat_cmds_ok(p, F->cmds, (int16_t)F->nq)) return 0;
        F->best = c;
        return 1;
    case 2:     /* the commands */
        if (!pat_cmds_ok(p, F->cmds, (int16_t)F->nq)) return 0;
        if (F->nq && F->nq < (int32_t)p->measures) return 0;
        if (F->nmeas && F->nmeas < (int32_t)p->measures) return 0;
        return 1;
    case 3: return pat_cmds_ok(p, F->cmds, 1);
    case 4: return pat_emb_ok(p, &F->cmds[0]);
    case 5: return pat_groove_ok(p, &F->cmds[0]) && p->embellish == 0;
    case 6: return pat_rhythm(p, F->rhythm, (int16_t)F->nq) == F->best;
    case 7: {   /* the destination groove: the command after the pattern */
        if (!F->S) return 0;
        mtime_t len = (mtime_t)p->measures * F->measlen;
        if (F->next && F->next < len) len = F->next;
        dms_cmdparam2 cp = { 0, 0, 0, 0, 0 };
        int32_t r = DM_OK;
        uint8_t scratch;
        if (!F->style || !F->pf ||
            (r = st_getcommand(F->style, F->t + len, F->off, F->pf, NULL, F->S->group, &cp, &scratch)) < 0) {
            r = F->S->ss && F->S->ss->seg ?
                dm_segment_getparam(F->S->ss->seg, PRM_COMMANDNEXT, F->S->group, DM_SEG_ANYTRACK, 0, NULL, &cp) : r;
        }
        if (r < 0) return 0;
        return pat_dest_ok(p, &cp);
    }
    case 8:
        if ((int32_t)p->measures > F->maxm) F->maxm = p->measures;
        return 0;
    case 9: {
        const uint8_t *ts = pat_ts(p, F->style);
        uint32_t beat = ts[1], beats = ts[0];
        int32_t x = (int32_t)(F->tsbeat * beats), y = (int32_t)(F->tsbeats * beat);
        if (x <= y && (y != x || (int32_t)beats <= (int32_t)F->tsbeats)) return 0;
        F->tsbeats = beats; F->tsbeat = beat;
        return 0;
    }
    case 10: return p->measures == (uint32_t)F->maxm;
    case 11: {
        const uint8_t *ts = pat_ts(p, F->style);
        return ts[0] == F->tsbeats && ts[1] == F->tsbeat;
    }
    }
    return 0;
}
/* 774162ae: every candidate through a criterion; 77416272: those that fail it rejected with its code */
static void f_apply(styfilter *F, int32_t crit) { for (int i = 0; i < F->n; i++) if (!F->c[i].code) f_crit(F, &F->c[i], crit); }
static void f_reject(styfilter *F, int32_t crit)
{
    for (int i = 0; i < F->n; i++) if (!F->c[i].code && !f_crit(F, &F->c[i], crit)) F->c[i].code = crit;
}

/* 774072cb: the command (GUID_CommandParam2) at t; an embellishment from before t's measure does not count */
static int32_t st_getcommand(const dms_style *s, mtime_t t, mtime_t off, dm_perf *pf, dm_segment *seg, uint32_t group,
                             dms_cmdparam2 *cp, uint8_t *cmd)
{
    int32_t r;
    if (pf) r = perf_getparam(pf, PRM_COMMAND2, group, DM_SEG_ANYTRACK, t + off, NULL, cp);
    else if (seg) r = dm_segment_getparam(seg, PRM_COMMAND2, group, DM_SEG_ANYTRACK, t, NULL, cp);
    else return (int32_t)0x80070057;
    if (r >= 0) {
        int32_t ml = sty_meas(s->ts);
        *cmd = cp->command;
        int32_t x = cp->t + ml;
        if (x < 1 || (x < ml && x % ml)) cp->command = 0;
    }
    return r;
}

static void hist_free(styhist **h) { for (styhist *x = *h, *n; x; x = n) { n = x->next; free(x); } *h = NULL; }

/* 7740b58e: the pattern among the candidates, by the command's repeat mode (DirectX 8 styles; else at random) */
static dms_pattern *st_pick(dm_perf *pf, const dms_style *s, int force, const dms_cmdparam2 *cmds, stystate *S, styfilter *F)
{
    if (f_empty(F)) return NULL;
    if (!(s->flags & 1) || !S) return f_random(pf, F);
    if (!cmds) return NULL;
    int k = 0;
    switch (cmds[0].repeat) {
    case 1:     /* DMUS_PATTERNT_REPEAT */
        if (!force && !f_index(F, S->pattern, &k)) return S->pattern;
        break;
    case 2: case 3: {   /* SEQUENTIAL, RANDOM_START */
        int i = 0;
        if (!force) {
            if (!f_index(F, S->pattern, &k) && f_nth(F, k + 1)) i = k + 1;
        } else if (cmds[0].repeat != 2) break;
        return f_nth(F, i);
    }
    case 4:     /* NO_REPEAT */
        if (!force && !f_index(F, S->pattern, &k)) f_set(F, k, -1);
        break;
    case 5: {   /* RANDOM_ROW */
        if (force) hist_free(&S->hist);
        for (styhist *h = S->hist; h; h = h->next) if (!f_index(F, h->p, &k)) f_set(F, k, 0xc);
        if (f_empty(F)) {
            f_restore(F, 0xc);
            if (S->hist && !f_index(F, S->hist->p, &k)) f_set(F, k, 0xc);
            hist_free(&S->hist);
        }
        dms_pattern *p = f_random(pf, F);
        if (!p) return NULL;
        styhist *h = calloc(1, sizeof(*h));
        if (h) { h->p = p; h->next = S->hist; S->hist = h; }
        return p;
    }
    }
    return f_random(pf, F);
}

/* 77410287: the state takes a pattern (new variations unless it is the same one) */
static int st_set_pattern(stystate *S, dms_pattern *p, mtime_t t)
{
    S->newpat = 1;
    S->patt = t;
    int n = pat_nparts(p);
    if (S->pattern != p) {
        free(S->mflag); free(S->partoff); free(S->cursor); free(S->partpch); free(S->var); free(S->norep);
        S->mflag = calloc(n ? n : 1, 1);
        S->partoff = calloc(n ? n : 1, sizeof(*S->partoff));
        S->cursor = calloc(n ? n : 1, sizeof(*S->cursor));
        S->partpch = calloc(n ? n : 1, sizeof(*S->partpch));
        S->var = malloc(n ? n : 1);
        S->norep = calloc(n ? n : 1, sizeof(*S->norep));
        if (!S->mflag || !S->partoff || !S->cursor || !S->partpch || !S->var || !S->norep) { S->nparts = 0; return DM_E_OUTOFMEMORY; }
        int i = 0;
        for (const dms_partref *r = p->prefs; r; r = r->next) S->partpch[i++] = r->pch;
        int keep = (p->flags & 1) && S->ti && S->ti->pvar && S->ti->pnorep;
        for (i = 0; i < n; i++) { S->var[i] = keep ? S->ti->pvar[i] : 0xff; S->norep[i] = keep ? S->ti->pnorep[i] : 0; }
        S->nparts = n;
    }
    for (int i = 0; i < n; i++) { S->partoff[i] = 0; S->cursor[i] = NULL; }
    S->pattern = p;
    return DM_OK;
}

static int8_t perf_groove(dm_perf *pf, int *ok) { *ok = pf && pf->hasgroove; return pf && pf->hasgroove ? pf->groove : 0; }

/* 7740b74f: the pattern for t: the style's patterns filtered by the criteria (1 rhythm, 2 commands, 3 the first
 * command, then for DirectX 8 styles 4 embellishment and 5 groove), the longest and widest first, then by the
 * destination groove (7); one of those left (st_pick) */
static int32_t st_choose(dm_perf *pf, const dms_style *s, int force, mtime_t t, mtime_t off, stystate *S, dm_segment *seg,
                         dms_pattern **out, int32_t *measlen, mtime_t *next)
{
    static const int32_t crits[5] = { 1, 2, 3, 4, 5 };     /* 77418404 */
    if (!seg && !pf) return (int32_t)0x80070057;
    int32_t hr = 0;
    mtime_t nextstyle = S ? S->nextstyle : 0;
    uint32_t group = S ? S->group : 0xffffffff;
    int32_t nq = 1;
    if (!s->npatterns) return STY_E_POINTER;
    int32_t maxm = 0;
    for (int i = 0; i < s->npatterns; i++) if ((int32_t)s->patterns[i]->measures > maxm) maxm = s->patterns[i]->measures;
    if (!maxm) return STY_E_POINTER;
    int gok;
    int8_t groove = pf ? perf_groove(pf, &gok) : 0;
    dms_cmdparam2 cur = { 0, 0, 0, 0, 0 };
    int32_t r28 = pf ? perf_getparam(pf, PRM_COMMAND2, group, DM_SEG_ANYTRACK, t + off, next, &cur)
                     : dm_segment_getparam(seg, PRM_COMMAND2, group, DM_SEG_ANYTRACK, t, next, &cur);
    if (r28 == DMUS_E_TRACK_NOT_FOUND || *next < 1) {
        if (!nextstyle) {
            if (!S || !S->ss) *next = 0;
            else *next = (S->ss->seg ? S->ss->seg->length : 0) - t;
        } else *next = nextstyle;
    }
    mtime_t d = nextstyle ? nextstyle - t : 0;
    if (0 < d && (*next == 0 || d < *next)) *next = d;
    if (S) {
        if (r28 < 0 || S->cmd[3] != cur.repeat) force = 1;
        S->cmdt = cur.t;
        S->cmd[0] = cur.command; S->cmd[1] = (uint8_t)(cur.groove + groove); S->cmd[2] = cur.range; S->cmd[3] = cur.repeat;
    }
    styfilter F;
    memset(&F, 0, sizeof(F));
    F.next = *next; F.t = t; F.off = off; F.style = s; F.S = S; F.pf = pf;
    F.n = s->npatterns;
    F.c = calloc(F.n, sizeof(*F.c));
    if (!F.c) return DM_E_OUTOFMEMORY;
    for (int i = 0; i < F.n; i++) F.c[i].p = s->patterns[i];
    stycache *cache = NULL, *node = NULL;
    int ci = 0;
    do {
        if (ci > 2 && !(s->flags & 1)) break;
        mtime_t at = t + off;
        do {
            F.maxm = 0;
            f_apply(&F, 8);
            f_reject(&F, 10);
            do {
                F.tsbeats = 0; F.tsbeat = 0;
                f_apply(&F, 9);
                f_reject(&F, 11);
                dms_pattern *p0 = f_nth(&F, 0);
                const uint8_t *ts = p0 ? pat_ts(p0, s) : s->ts;
                int32_t ml = sty_beat(ts) * ts[0];
                f_measlen(&F, ml);
                mtime_t sn = 0;
                dms_style *cs = NULL;
                if (perf_getparam(pf, PRM_STYLE, group, DM_SEG_ANYTRACK, at, &sn, &cs) >= 0) {
                    if (sn && ml && !(nq = sn / ml)) nq = 1;
                    if (maxm < nq) nq = maxm;
                    dms_style_release(cs);
                }
                F.nq = nq;
                for (node = cache; node && node->measlen != ml; node = node->next) {}
                if (!node) {
                    node = calloc(1, sizeof(*node));
                    int32_t cap = nq > maxm ? nq : maxm;
                    if (node) { node->cmds = calloc(cap, sizeof(*node->cmds)); node->rhythm = calloc(cap, sizeof(*node->rhythm)); }
                    if (!node || !node->cmds || !node->rhythm) {
                        if (node) { free(node->cmds); free(node->rhythm); free(node); }
                        hr = DM_E_OUTOFMEMORY;
                        goto done;
                    }
                    node->measlen = ml;
                    node->cmds[0].groove = 0x3e;        /* 7740885d */
                    node->next = cache; cache = node;
                    if (r28 >= 0) {
                        dms_rhythmparam rp;
                        memset(&rp, 0, sizeof(rp));
                        rp.bpm = ts[0]; rp.beat = ts[1]; rp.grids = (uint16_t)(ts[2] | ts[3] << 8);
                        for (int16_t i = 0; i < nq; i++) {
                            mtime_t tm = i * ml + t;
                            dms_cmdparam2 cp = cur;
                            uint8_t scratch;
                            if (st_getcommand(s, tm, off, pf, seg, group, &cp, &scratch) != 0) break;
                            node->cmds[i] = cp;
                            int16_t g = (int16_t)(cp.groove + groove);
                            if (g > 100) g = 100;
                            if (g < 1) g = 1;
                            node->cmds[i].groove = (uint8_t)g;
                            node->rhythm[i] = perf_getparam(pf, PRM_RHYTHM, group, DM_SEG_ANYTRACK, tm + off, NULL, &rp) < 0
                                              ? 0 : rp.rhythm;
                        }
                    }
                }
                F.cmds = node->cmds; F.rhythm = node->rhythm;
                if (crits[ci] == 1) F.best = 0;
                f_reject(&F, crits[ci]);
                if (!f_empty(&F)) {
                    if (crits[ci] != 1) break;
                    f_reject(&F, 6);
                    if (!f_empty(&F)) break;
                    f_restore(&F, 6);
                }
                f_swap(&F, 11, -1);
            } while (!f_empty(&F));
            if (!f_empty(&F)) break;
            f_swap(&F, 10, -1);
        } while (!f_empty(&F));
        if (!f_empty(&F)) break;
        ci++;
        f_reset(&F);
    } while (ci < 5);
    dms_pattern *p = NULL;
    if (!f_empty(&F)) {
        f_reject(&F, 7);
        if (f_empty(&F)) f_restore(&F, 7);
        p = st_pick(pf, s, force, node ? node->cmds : NULL, S, &F);
    }
    if (!p) {
        if (s->npatterns) p = s->patterns[0];
        if (!p) { hr = STY_E_POINTER; goto done; }
    }
    *out = p;
    const uint8_t *pts = pat_ts(p, s);
    *measlen = sty_beat(pts) * pts[0];
    if (S) hr = st_set_pattern(S, p, t);
done:
    free(F.c);
    for (stycache *x = cache, *n; x; x = n) { n = x->next; free(x->cmds); free(x->rhythm); free(x); }
    return hr;
}

/* ------------------------------------------------------------------ Play */
/* 77411214: the next pattern at t (a new style first when its time has come) */
static int32_t st_select(dm_perf *pf, stystate *S, uint32_t tflags, mtime_t t, mtime_t off, int loop)
{
    int fdirty = (tflags >> 4) & 1, fstart = (tflags >> 2) & 1, floop = (tflags >> 1) & 1;
    int force = fstart || fdirty;
    const dms_style *old = S->style;
    if (S->nextstyle && S->nextstyle <= t) {
        const dms_style *s = st_style_at(S, t, &S->nextstyle);
        if (!s) return STY_E_POINTER;
        S->style = s;
        force = 1;
        if (S->tsenabled) st_tspmsg(pf, S, t, off, S->nextstyle);
    }
    const uint8_t *ts = S->pattern ? pat_ts(S->pattern, old) : S->style->ts;
    int32_t beat = sty_beat(ts);
    uint8_t beats = ts[0];
    dms_pattern *oldp = S->pattern, *np = NULL;
    int32_t ml = 0;
    mtime_t next = 0;
    if (st_choose(pf, S->style, force, t, off, S, NULL, &np, &ml, &next) < 0) return STY_E_POINTER;
    mtime_t base = (fdirty && S->pstart == 0 && !floop) ? S->prevpstart : S->pstart;
    base = (!fstart && !floop && oldp) ? (mtime_t)oldp->measures * beat * beats + base : 0;
    mtime_t len = (mtime_t)np->measures * ml;
    if (S->gap == 0 || !fdirty) { if (t < base && (S->style->flags & 1) && fdirty) S->gap = base - t; }
    else S->gap = 0;
    st_chord(pf, S, t, off, fstart, loop);
    S->pstart = S->pend;
    S->pend += len;
    if (next && next + t < S->pend) S->pend = next + t;
    return 0;
}

/* 7740e6a1: the first beat at or after t */
static mtime_t st_next_beat(const uint8_t *ts, mtime_t t)
{
    int32_t b = sty_beat(ts);
    if (b && t % b == 0) return t;
    mtime_t f = (!b || t < b) ? 0 : t - t % b;
    return f + b;
}

/* 77411bd8 */
static int32_t st_play(dm_perf *pf, stystate *S, mtime_t start, mtime_t end, mtime_t off, uint32_t tflags, int clock)
{
    S->off = off;
    int fstart = (tflags >> 2) & 1, fseek = tflags & 1, floop = (tflags >> 1) & 1, fdirty = (tflags >> 4) & 1;
    int32_t hr = 0;
    int dochord = fdirty;
    if (S->pend == 0) {
        int32_t ml = S->style ? sty_meas(st_ts(S)) : 0;
        int32_t r = STY_E_FAIL;
        if (fdirty || (tflags & TF_FLUSH)) {
            int gok;
            int8_t groove = perf_groove(pf, &gok);
            uint8_t cmd = S->cmd[0];
            dms_cmdparam2 cp = { 0, 0, 0, 0, 0 };
            if (S->style) r = st_getcommand(S->style, start, off, pf, NULL, S->group, &cp, &cmd);
            if (!fdirty && r == 0 && (int)cp.groove + groove == (int)S->cmd[1] && cmd == S->cmd[0]) r = STY_E_FAIL;
        }
        mtime_t t3 = 0;
        int restored = 0;
        if (!fstart && !floop) {
            S->pend = ml ? (start / ml) * ml : 0;
            t3 = start;
            if (start != 0 && r != 0 && start < S->prevpend) {
                S->pend = S->prevpend; S->prevpend = 0;
                S->pstart = S->prevpstart; S->prevpstart = 0;
                restored = 1;
            }
        } else S->pend = 0;
        if (!restored) {
            hr = st_select(pf, S, tflags, t3, off, floop);
            if (S->pattern && start >= 1 && (fseek || fdirty)) {
                while (hr >= 0 && S->pend <= start) hr = st_select(pf, S, tflags, S->pend, off, floop);
                dochord = fdirty || floop;
            } else if (!S->pattern || start < 1) dochord = fdirty || floop;
            else dochord = floop;
            if (hr < 0) goto out;
        }
    }
    if (dochord) st_chord(pf, S, start, off, fstart, 0);
    uint8_t again = 0;
    mtime_t pe = S->pend;
    mtime_t mb = start ? st_next_beat(st_ts(S), start) : 0;
    if (S->tsenabled && S->ti->notify && mb < end) mb = st_mbnotify(pf, S, mb, end, off, tflags);
    uint32_t f = (fstart || floop || fseek) ? 6 : 2;
    if (clock) f |= 1;
    if (floop || (0 < start && (fstart || fseek || fdirty))) f |= 0x10;
    if (end < pe) pe = end;
    st_play_parts(pf, S, start, pe, off, S->pstart, f, tflags, &again);
    if (again) st_play_parts(pf, S, start, pe, off, S->pstart, clock ? 9 : 8, tflags, &again);
    if (start <= S->pend && S->pend < end) {
        hr = st_select(pf, S, tflags & ~4u, S->pend, off, 0);
        if (hr < 0) goto out;
        st_play_parts(pf, S, start, end, off, S->pstart, clock ? 1 : 0, tflags, &again);
    }
    if (S->tsenabled && S->ti->notify && mb < end) st_mbnotify(pf, S, mb, end, off, tflags);
out:
    S->hr = hr;
    return hr;
}

/* 774116c7: Play (a reload brings the style at start; a start, seek or dirty play starts the patterns again) */
static int stytrack_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    stystate *S = e->state;
    dms_styletrack *T = e->t->data;
    if (!S || !T) return (int)0x88781162;
    if (S->hr == DM_E_OUTOFMEMORY) return S->hr;
    if (S->gen != T->gen) {
        S->style = st_style_at(S, start, &S->nextstyle);
        S->gen = T->gen;
        if (!S->style) return STY_E_POINTER;
    }
    if (!S->mute) return (int)0x88781162;
    if (flags & (TF_START | TF_SEEK | TF_DIRTY)) {
        S->prevpstart = S->pstart; S->pstart = 0;
        S->prevpend = S->pend; S->pend = 0;
        S->style = st_style_at(S, 0, &S->nextstyle);
        if (S->style) {
            if (S->tsenabled) st_tspmsg(pf, S, start, off, S->nextstyle);
            S->chordt = S->nextchordt = S->nnchordt = 0;
            S->patt = 0;
            for (int i = 0; i < S->nmute && i < T->npchs; i++) { S->mute[i].t = 0; S->mute[i].pch = T->pchs[i]; S->mute[i].mute = 0; }
        }
    }
    return st_play(pf, S, start, end, off, flags, 0);
}

/* 77412e52: the state (StyleTrackState 0x38c) */
static void stytrack_initplay(dm_perf *pf, dm_segstate *ss, sstrack *e, uint32_t segflags)
{
    dms_styletrack *T = e->t->data;
    stystate *S = calloc(1, sizeof(*S));
    if (!S || !T) { free(S); return; }
    S->group = 0xffffffff; S->newpat = 1; S->tsenabled = 1;
    S->chord = g_sty_m7;
    S->gen = T->gen;
    S->kind = TRK_STYLE; S->ss = ss; S->vid = e->vid; S->ti = T;
    S->nextstyle = 0;
    S->style = st_style_at(S, 0, &S->nextstyle);
    if (!S->style) { free(S); return; }
    S->pf = pf;
    if (st_mute_reset(S) < 0) { free(S->mute); free(S); return; }
    S->tsenabled = T->tsset ? T->tsenabled : (segflags & (SEGF_SECONDARY | SEGF_CONTROL)) == 0;
    if (T->seed) st_seed(S, T->seed);
    S->group = e->group;
    e->state = S;
}

static void stytrack_endplay_state(stystate *S)
{
    if (!S) return;
    free(S->partpch); free(S->var); free(S->norep); free(S->partoff); free(S->mflag); free(S->mute); free(S->cursor);
    free(S->rtab);
    hist_free(&S->hist);
    free(S);
}
static void stytrack_endplay(sstrack *e) { stytrack_endplay_state(e->state); e->state = NULL; }

/* ------------------------------------------------------------------ parameters */
/* 77411916 */
static int stytrack_isparam(const dm_track *t, int prm)
{
    const dms_styletrack *T = t->data;
    if (!T) return (int)0x88781162;
    if (prm == PRM_STYLE || prm == PRM_SEEDVAR || prm == PRM_STYLETS) return DM_OK;
    if (!T->tsset) {
        if (prm == PRM_DISABLETIMESIG || prm == PRM_TIMESIG || prm == PRM_ENABLETIMESIG) return DM_OK;
    } else if (!T->tsenabled) {
        if (prm == PRM_ENABLETIMESIG) return DM_OK;
        if (prm == PRM_DISABLETIMESIG || prm == PRM_TIMESIG) return DMUS_E_TYPE_DISABLED;
    } else {
        if (prm == PRM_DISABLETIMESIG || prm == PRM_TIMESIG) return DM_OK;
        if (prm == PRM_ENABLETIMESIG) return DMUS_E_TYPE_DISABLED;
    }
    return DMUS_E_TYPE_UNSUPPORTED;
}

/* 7741202f: GUID_IDirectMusicStyle (the style at t, with a reference) and GUID_TimeSignature (the style's; while
 * playing, the signature of the pattern the chooser would pick) */
static int stytrack_getparam(const dm_track *t, stystate *S, int prm, mtime_t mt, mtime_t *next, void *data)
{
    const dms_styletrack *T = t->data;
    if (!data) return STY_E_POINTER;
    if (prm == PRM_STYLE) {
        if (!T) return (int)0x88781162;
        const dms_stynode *x = T->styles;
        if (!x) return DMUS_E_NOT_FOUND;
        dms_style *cur = x->style;
        const dms_stynode *n;
        for (n = x->next; n && (n->t <= mt || !n->style); n = n->next) if (n->style) cur = n->style;
        if (!cur) return STY_E_POINTER;
        dms_style_addref(cur);
        *(dms_style **)data = cur;
        if (next) *next = n ? n->t - mt : 0;
        return DM_OK;
    }
    if (prm == PRM_TIMESIG) {
        if (!T) return (int)0x88781162;
        const dms_stynode *x = T->styles;
        if (!x) return DMUS_E_NOT_FOUND;
        dms_style *cur = x->style;
        mtime_t at = x->t;
        const dms_stynode *n;
        for (n = x->next; n && n->t <= mt; n = n->next) { cur = n->style; at = n->t; }
        if (!cur) return STY_E_POINTER;
        timesig *o = data;
        if (!S) {
            o->bpm = cur->ts[0]; o->beat = cur->ts[1]; o->grids = (uint16_t)(cur->ts[2] | cur->ts[3] << 8);
            o->t = at - mt;
            if (next) *next = n ? n->t : 0;
            return DM_OK;
        }
        dms_pattern *p = NULL;
        int32_t ml = 0;
        mtime_t nx = 0;
        int32_t hr = st_choose(S->pf, cur, 1, mt, S->off, NULL, NULL, &p, &ml, &nx);
        if (hr >= 0) {
            const uint8_t *ts = pat_ts(p, cur);
            o->bpm = ts[0]; o->beat = ts[1]; o->grids = (uint16_t)(ts[2] | ts[3] << 8);
            o->t = at - mt;
            if (next) *next = nx;
        }
        return hr;
    }
    if (prm == PRM_STYLETS) return DM_E_NOTIMPL;      /* TODO: the composer's (with a segment) */
    return DMUS_E_GET_UNSUPPORTED;
}

/* 774129be */
static int stytrack_setparam(dm_track *t, int prm, mtime_t mt, void *data)
{
    dms_styletrack *T = t->data;
    if (!T) return (int)0x88781162;
    if (prm == PRM_STYLE) {
        if (!data) return STY_E_POINTER;
        dms_stynode *nd = calloc(1, sizeof(*nd));
        if (!nd) return DM_E_OUTOFMEMORY;
        nd->t = mt; nd->style = data; dms_style_addref(nd->style);
        dms_stynode **pp = &T->styles;
        while (*pp) pp = &(*pp)->next;
        *pp = nd;
        T->gen++;
        return dms_styletrack_pchs(T);
    }
    if (prm == PRM_ENABLETIMESIG) {
        if (!T->tsset || !T->tsenabled) { T->tsset = 1; T->tsenabled = 1; return DM_OK; }
        return DMUS_E_TYPE_DISABLED;
    }
    if (prm == PRM_DISABLETIMESIG) {
        if (!T->tsset || T->tsenabled) { T->tsset = 1; T->tsenabled = 0; return DM_OK; }
        return DMUS_E_TYPE_DISABLED;
    }
    if (prm == PRM_SEEDVAR) {
        if (!data) return STY_E_POINTER;
        T->seed = *(int32_t *)data;
        return DM_OK;
    }
    return DMUS_E_SET_UNSUPPORTED;
}

/* ------------------------------------------------------------------ the motif track (CMotifTrack, MotifTrackState) */
/* 7740eaea: the state plays the track's pattern with the first style */
static void motiftrack_initplay(dm_perf *pf, dm_segstate *ss, sstrack *e, uint32_t segflags)
{
    dms_styletrack *T = e->t->data;
    if (!T || !T->styles || !T->styles->style) return;
    stystate *S = calloc(1, sizeof(*S));
    if (!S) return;
    S->group = 0xffffffff; S->newpat = 1; S->tsenabled = 1;
    S->chord = g_sty_m7;
    S->kind = TRK_MOTIF; S->ss = ss; S->vid = e->vid; S->ti = T;
    S->style = T->styles->style;
    if (st_set_pattern(S, T->motif, 0) < 0 || st_mute_reset(S) < 0) { stytrack_endplay_state(S); return; }
    S->pf = pf;
    S->tsenabled = T->tsset ? T->tsenabled : (segflags & (SEGF_SECONDARY | SEGF_CONTROL)) == 0;
    if (T->seed) st_seed(S, T->seed);
    S->group = e->group;
    e->state = S;
}

/* 7740e0d8 / 7740e70b */
static int motiftrack_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    stystate *S = e->state;
    dms_styletrack *T = e->t->data;
    if (!T || !S || !S->mute) return (int)0x88781162;
    int clock = 0;
    if (flags & (TF_SEEK | TF_LOOP | TF_START | TF_DIRTY)) {
        S->newpat = 1;
        S->chordt = S->nextchordt = S->nnchordt = 0;
        for (int i = 0; i < S->nmute && i < T->npchs; i++) { S->mute[i].t = 0; S->mute[i].pch = T->pchs[i]; S->mute[i].mute = 0; }
    }
    int fstart = (flags >> 2) & 1, floop = (flags >> 1) & 1, fdirty = (flags >> 4) & 1, fseek = flags & 1;
    S->off = off;
    if (fdirty) st_chord(pf, S, start, off, fstart, 0);
    mtime_t mb = start ? st_next_beat(st_ts(S), start) : 0;
    if (S->tsenabled && T->notify && !clock && mb < end) mb = st_mbnotify(pf, S, mb, end, off, flags);
    uint8_t again = 0;
    uint32_t f = (fstart || floop || fseek) ? 6 : 2;
    if (clock) f |= 1;
    if (floop || (0 < start && (fstart || fseek || fdirty))) f |= 0x10;
    st_play_parts(pf, S, start, end, off, 0, f, flags, &again);
    if (again) st_play_parts(pf, S, start, end, off, 0, clock ? 9 : 8, flags, &again);
    if (S->tsenabled && T->notify && !clock && mb < end) st_mbnotify(pf, S, mb, end, off, flags);
    return 0;
}

/* 7740e33a */
static int motiftrack_isparam(const dm_track *t, int prm)
{
    const dms_styletrack *T = t->data;
    if (!T) return (int)0x88781162;
    if (prm == PRM_SEEDVAR || prm == PRM_VALIDSTART) return DM_OK;
    if (!T->tsset) {
        if (prm == PRM_DISABLETIMESIG || prm == PRM_ENABLETIMESIG) return DM_OK;
        return DMUS_E_TYPE_UNSUPPORTED;
    }
    if (!T->tsenabled) {
        if (prm == PRM_ENABLETIMESIG) return DM_OK;
        return prm == PRM_DISABLETIMESIG ? DMUS_E_TYPE_DISABLED : DMUS_E_TYPE_UNSUPPORTED;
    }
    if (prm == PRM_DISABLETIMESIG) return DM_OK;
    return prm == PRM_ENABLETIMESIG ? DMUS_E_TYPE_DISABLED : DMUS_E_TYPE_UNSUPPORTED;
}

/* 7740e922: GUID_Valid_Start_Time: the motif's next start marker */
static int motiftrack_getparam(const dm_track *t, int prm, mtime_t mt, mtime_t *next, void *data)
{
    const dms_styletrack *T = t->data;
    if (!data) return STY_E_POINTER;
    if (!T) return (int)0x88781162;
    if (prm != PRM_VALIDSTART) return DMUS_E_GET_UNSUPPORTED;
    if (!T->motif) return STY_E_POINTER;
    for (const dms_mtime *m = T->motif->markers; m; m = m->next)
        if (mt <= m->t) {
            *(mtime_t *)data = m->t - mt;
            if (next) *next = m->next ? m->next->t - mt : 0;
            return DM_OK;
        }
    return DMUS_E_NOT_FOUND;
}

/* 7740e216 */
static int motiftrack_setparam(dm_track *t, int prm, void *data)
{
    dms_styletrack *T = t->data;
    if (!T) return (int)0x88781162;
    if (prm == PRM_ENABLETIMESIG) {
        if (!T->tsset || !T->tsenabled) { T->tsset = 1; T->tsenabled = 1; return DM_OK; }
        return DMUS_E_TYPE_DISABLED;
    }
    if (prm == PRM_DISABLETIMESIG) {
        if (!T->tsset || T->tsenabled) { T->tsset = 1; T->tsenabled = 0; return DM_OK; }
        return DMUS_E_TYPE_DISABLED;
    }
    if (prm == PRM_SEEDVAR) {
        if (!data) return STY_E_POINTER;
        T->seed = *(int32_t *)data;
        return DM_OK;
    }
    return DMUS_E_SET_UNSUPPORTED;
}

/* ------------------------------------------------------------------ the pattern track (CPatternTrack, PatternTrackState) */
/* 77414673: the state plays the track's pattern (from its own style); no audition (774132b1 / 77413ff2 defaults) */
static void patterntrack_initplay(dm_perf *pf, dm_segstate *ss, sstrack *e, uint32_t segflags)
{
    dms_styletrack *T = e->t->data;
    if (!T || !T->motif || !T->styles || !T->styles->style) return;
    stystate *S = calloc(1, sizeof(*S));
    if (!S) return;
    S->group = 0xffffffff; S->newpat = 1; S->tsenabled = 1;
    S->chord = g_sty_m7;
    S->gen = T->gen;
    S->kind = TRK_PATTERN; S->noinv = 1;
    S->ss = ss; S->vid = e->vid; S->ti = T;
    S->style = T->styles->style;
    if (st_set_pattern(S, T->motif, 0) < 0 || st_mute_reset(S) < 0) { stytrack_endplay_state(S); return; }
    S->pf = pf;
    S->tsenabled = T->tsset ? T->tsenabled : (segflags & (SEGF_SECONDARY | SEGF_CONTROL)) == 0;
    if (T->seed) st_seed(S, T->seed);
    S->group = e->group;
    e->state = S;
}

/* 774140ad */
static int st_pattern_play(dm_perf *pf, stystate *S, mtime_t start, mtime_t end, mtime_t off, uint32_t tflags, int clock)
{
    S->off = off;
    int fdirty = (tflags >> 4) & 1, fstart = (tflags >> 2) & 1, floop = (tflags >> 1) & 1, fseek = tflags & 1;
    mtime_t mb = start ? st_next_beat(st_ts(S), start) : 0;
    if (tflags & 0x80000000u) { st_chord(pf, S, mb, off, fstart, 0); S->chordt = start; }
    if (fdirty) st_chord(pf, S, start, off, fstart, 0);
    if (S->tsenabled && S->ti->notify && !clock && mb < end) mb = st_mbnotify(pf, S, mb, end, off, tflags);
    uint8_t again = 0;
    const uint8_t *ts = st_ts(S);
    int32_t plen = sty_beat(ts) * S->pattern->measures * ts[0];
    if ((floop || fstart) && plen) S->pstart = start - start % plen;
    mtime_t pe = S->pstart + plen, stop = pe <= end ? pe : end;
    uint32_t f = (fstart || floop || fseek) ? 6 : 2;
    if (clock) f |= 1;
    if (floop || (0 < start && (fstart || fseek || fdirty))) f |= 0x10;
    st_play_parts(pf, S, start, stop, off, S->pstart, f, tflags, &again);
    if (again) st_play_parts(pf, S, start, stop, off, S->pstart, clock ? 9 : 8, tflags, &again);
    if (start <= pe && pe < end) {
        S->pstart = pe;
        st_set_pattern(S, S->pattern, pe);
        st_play_parts(pf, S, start, end, off, S->pstart, clock ? 1 : 0, tflags, &again);
    }
    if (S->tsenabled && S->ti->notify && !clock && mb < end) st_mbnotify(pf, S, mb, end, off, tflags);
    return 0;
}

/* 7741374e: a reload brings the pattern again; a start, seek or dirty play restarts the variations */
static int patterntrack_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    stystate *S = e->state;
    dms_styletrack *T = e->t->data;
    if (!T || !S) return (int)0x88781162;
    int clock = 0;
    int32_t hr = (int32_t)0x88781162;
    if (S->gen != T->gen) {
        mtime_t nx = 0;
        S->style = st_style_at(S, start, &nx);
        if (!S->style) hr = STY_E_POINTER;
        st_set_pattern(S, T->motif, 0);
        if (T->pchschanged) st_mute_reset(S);
        S->gen = T->gen;
    }
    if (!S->mute) return hr;
    if (flags & (TF_SEEK | TF_LOOP | TF_START | TF_DIRTY)) {
        if (S->tsenabled && !clock) st_tspmsg(pf, S, start, off, 0);
        S->newpat = 1;
        S->chordt = S->nextchordt = S->nnchordt = 0;
        for (int i = 0; i < S->nmute && i < T->npchs; i++) { S->mute[i].t = 0; S->mute[i].pch = T->pchs[i]; S->mute[i].mute = 0; }
    }
    return st_pattern_play(pf, S, start, end, off, flags, clock);
}

/* 77413a39 */
static int patterntrack_isparam(const dm_track *t, int prm)
{
    if (prm == PRM_VARIATIONS) return DM_OK;
    return motiftrack_isparam(t, prm);
}
