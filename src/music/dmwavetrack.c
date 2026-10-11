/* reAL-Music: the wave track (included by dmperf.c): dmime's CWavTrack (DirectX 9 5.3.x).
 *
 * Init 77326edc (item order: dmseg.c), InitPlay 77327312, EndPlay 77326fda, Play/PlayEx 77325abd, variations
 * 77324677 / 773250ea / 773242a8, downloads 7732664a / 77326958 (the process-wide download list 7732d4f0 is kept per
 * performance here: its entries name their performance anyway), voices 7732588a, and the output tool's wave
 * messages 77307763.  No track gives GUID_Variations or GUID_MuteParam here, so variations are always chosen by the
 * track itself and no part is muted or remapped, as in the original when those GetParam calls fail. */

static void wv_release(dm_perf *pf, dm_wvoice *v)
{
    if (!v || --v->refs > 0) return;
    port *po = &pf->ports[v->port];
    if (po->ops.voice_free) po->ops.voice_free(po->ctx, v->h);
    free(v);
}

/* a download (DAT_7732d4f0 entry): wave, port, the port's handle, references */
typedef struct wdl { struct wdl *next; dm_wave *wave; int port; void *dl; int refs; } wdl;
/* +0x80: a streaming download in use by a state */
typedef struct wsdl { struct wsdl *next; void *dl; struct wstate *st; } wsdl;
/* state +0x44c: a download made for one play of a streaming wave, with its voice */
typedef struct wrec { struct wrec *next; void *dl; int port; dm_wvoice *voice; } wrec;

typedef struct wstate {
    uint32_t nparts;                /* +0x00 */
    dm_waveitem **cursor;           /* +0x04 */
    uint32_t version;               /* +0x08 */
    uint32_t groupbits;             /* +0x0c */
    uint32_t *var, *varused;        /* +0x10 +0x14 */
    uint32_t lockvar[256];          /* +0x18: [lock] for lock ids 1..255 (index 0 unused) */
    int64_t vartime;                /* +0x418 */
    uint32_t ov[6];                 /* +0x420 variation, pchannel, index, lock, by part, by lock */
    dm_wvoice **voices;             /* +0x43c */
    uint32_t nvoices;               /* +0x440 */
    int flushed;                    /* +0x444 */
    dm_audiopath *ap;               /* +0x448 */
    wrec *recs;                     /* +0x44c */
} wstate;

static int wt_rand(dm_perf *pf)     /* msvcrt rand(): per thread */
{
    uint32_t *r = pf->apprand ? &pf->rndapp : &pf->rnd;
    *r = *r * 214013u + 2531011u;
    return (int)((*r >> 16) & 0x7fff);
}

/* 773245f6: t * 2^(cents/1200), truncated */
static int64_t wt_scale(int64_t t, int32_t cents)
{
    if (!cents) return t;
    return (int64_t)((double)t * pow(2.0, (double)cents * (1.0 / 1200.0)));
}

/* 77324c6d: PChannelInfo of a PChannel, through the state's audiopath */
static int wt_pchinfo(dm_perf *pf, dm_audiopath *ap, uint32_t pch, uint32_t *pi, uint32_t *group, uint32_t *chan)
{
    if (ap) { int hr = dm_audiopath_convert(ap, pch, &pch); if (hr < 0) return hr; }
    return map_pch(pf, pch, pi, group, chan, NULL);
}

static int item_has_dl(const dm_waveitem *it, void *dl)    /* 77324f2d */
{
    for (const dm_dlnode *n = it->dls; n; n = n->next) if (n->dl == dl) return 1;
    return 0;
}

static wdl *wdl_find(dm_perf *pf, const dm_waveitem *it, uint32_t pi)
{
    for (wdl *d = pf->wdls; d; d = d->next)
        if (d->wave == it->wave && d->port == (int)pi && (!it->streaming || item_has_dl(it, d->dl))) return d;
    return NULL;
}

/* 7732664a: download an item's wave to the port of its part's PChannel */
static int wt_download_item(dm_perf *pf, dm_waveitem *it, uint32_t pch, dm_audiopath *ap)
{
    if (!it->wave) return (int32_t)0x88781162;
    uint32_t pi, group, chan;
    int hr = wt_pchinfo(pf, ap, pch, &pi, &group, &chan);
    if (hr < 0) return hr;
    if (pi >= (uint32_t)pf->nports) return DMUS_E_NOT_FOUND;
    port *po = &pf->ports[pi];
    wdl *d = wdl_find(pf, it, pi);
    if (d) { d->refs++; return DM_FALSE; }
    if (!po->ops.wave_download) return DMUS_E_NOT_FOUND;
    d = calloc(1, sizeof(*d));
    if (!d) return DM_E_OUTOFMEMORY;
    hr = DM_OK;
    d->dl = po->ops.wave_download(po->ctx, it->wave, it->startoff, &hr);
    if (!d->dl || hr < 0) { free(d); return hr < 0 ? hr : DM_E_FAIL; }
    if (it->streaming) {
        dm_dlnode *n = calloc(1, sizeof(*n));
        if (!n) { po->ops.wave_unload(po->ctx, d->dl); free(d); return DM_E_OUTOFMEMORY; }
        n->dl = d->dl; n->next = it->dls; it->dls = n;
    }
    d->wave = it->wave; dm_wave_addref(it->wave);
    d->port = (int)pi; d->refs = 1;
    d->next = pf->wdls; pf->wdls = d;
    return DM_OK;
}

static void item_remove_dl(dm_waveitem *it, void *dl)      /* 77325783 */
{
    for (dm_dlnode **p = &it->dls; *p; p = &(*p)->next)
        if ((*p)->dl == dl) { dm_dlnode *n = *p; *p = n->next; free(n); return; }
}

/* 77326958 */
static int wt_unload_item(dm_perf *pf, dm_waveitem *it, uint32_t pch, dm_audiopath *ap)
{
    if (!it->wave) return DM_OK;
    uint32_t pi, group, chan;
    int hr = wt_pchinfo(pf, ap, pch, &pi, &group, &chan);
    if (hr < 0) return hr;
    if (pi >= (uint32_t)pf->nports) return DMUS_E_NOT_FOUND;
    for (wdl **pp = &pf->wdls; *pp; pp = &(*pp)->next) {
        wdl *d = *pp;
        if (d->wave == it->wave && d->port == (int)pi && (!it->streaming || item_has_dl(it, d->dl))) {
            if (--d->refs >= 1) return DM_FALSE;
            port *po = &pf->ports[pi];
            item_remove_dl(it, d->dl);
            if (po->ops.wave_unload) po->ops.wave_unload(po->ctx, d->dl);
            dm_wave_release(d->wave);
            *pp = d->next;
            free(d);
            return DM_OK;
        }
    }
    return DM_OK;
}

/* SetParam(GUID_Download / GUID_DownloadToAudioPath) 773277ab: every item of every part */
static int wt_download(dm_perf *pf, dm_wavetrack *T, dm_audiopath *ap, int unload)
{
    int hr = DM_OK, ok = 0;
    for (dm_wavepart *p = T->parts; p; p = p->next) {
        int r = 0;
        for (dm_waveitem *it = p->items; it; it = it->next) {
            int x = unload ? wt_unload_item(pf, it, p->pch, ap) : wt_download_item(pf, it, p->pch, ap);
            if (x) r = x;
        }
        if (r >= 0) ok++; else hr = r;
    }
    if (hr < 0 && ok) hr = DM_FALSE;
    return unload ? DM_OK : hr;
}

/* 77324262: the audition settings */
static void ws_set_audition(wstate *S, const uint32_t *v) { memcpy(S->ov, v, sizeof(S->ov)); }

/* 773242a8: the variation a part plays now (an auditioned part or lock group plays the audition variation) */
static uint32_t ws_varmask(const wstate *S, const dm_wavepart *P, uint32_t i)
{
    if ((S->ov[5] == 0 && S->ov[3] != 0 && P->lock == S->ov[3]) || (S->ov[4] != 0 && P->pch == S->ov[1] && P->index == S->ov[2]))
        return S->ov[0];
    return S->var[i];
}

static int pick_bit(uint32_t mask, int r)
{
    int k = 0, b;
    for (b = 0; b < 32; b++)
        if (mask & (1u << b)) { if ((int16_t)r == (int16_t)k) break; k++; }
    return b & 31;
}

/* 77324677: choose part i's variation (DMUS_VARIATIONT_ sequential 0, random 1, random start 2, no repeat 3,
 * random row 4) */
static void ws_select(dm_perf *pf, dm_wavetrack *T, wstate *S, uint32_t i, const dm_wavepart *P)
{
    uint8_t lock = (uint8_t)P->lock;
    if (lock && S->lockvar[lock]) { S->var[i] = S->lockvar[lock]; return; }
    uint32_t mask = P->variations;
    if (!mask) { S->var[i] = 0; S->varused[i] = 0; return; }
    if (S->ov[5] && P->pch == S->ov[1] && P->index == S->ov[2]) mask &= S->ov[0];
    int count = 0;
    for (int b = 0; b < 32; b++) if (mask & (1u << b)) count++;
    uint32_t mode = P->flags & 0xf, avail;
    int random = 0;
    if (mode == 4) {
        avail = ~S->varused[i] & mask;
        if (!avail) { S->varused[i] = 0; mode = 3; goto mode3; }
        goto reduce;
    }
mode3:
    if (mode == 3) {
        if (!S->var[i]) { random = 1; goto pick; }
        avail = ~S->var[i] & mask;
        goto reduce;
    }
    goto modes;
reduce:
    if (avail != mask && avail != 0) {
        for (int b = 0; b < 32; b++) if ((mask & (1u << b)) && !(avail & (1u << b))) count--;
        mask = avail;
    }
modes:
    if (mode == 0) goto seq;
    if (mode == 2) {
        if (!S->var[i] && count > 0) {
            int r = wt_rand(pf) % count;
            int k = 0, b;
            for (b = 0; b < 32; b++)
                if (mask & (1u << b)) { if ((r & 0xff) == k) break; k++; }
            S->var[i] = 1u << (b & 31);
        }
        goto seq;
    }
    if (mode != 1 && (mode < 3 || mode > 4)) goto done;
    random = 1;
pick:
    (void)random;
    if (count > 0) {        /* the original divides by zero when an audition mask leaves no variation */
        S->var[i] = 1u << pick_bit(mask, wt_rand(pf) % count);
        if (mode == 4) S->varused[i] |= S->var[i];
    }
    goto done;
seq:
    if (!S->var[i]) {
        S->var[i] = 1;
        while (!(S->var[i] & mask)) { S->var[i] <<= 1; if (!S->var[i]) S->var[i] = 1; }
    } else {
        do { S->var[i] <<= 1; if (!S->var[i]) S->var[i] = 1; } while (!(S->var[i] & mask));
    }
done:
    if (lock) S->lockvar[lock] = S->var[i];
    if ((T->flags & 2) && T->persistvar && T->persistused) { T->persistvar[i] = S->var[i]; T->persistused[i] = S->varused[i]; }
}

/* 773250ea: every part's variation afresh (an auditioned lock group's part first) */
static void ws_select_all(dm_perf *pf, dm_wavetrack *T, wstate *S)
{
    memset(S->lockvar, 0, sizeof(S->lockvar) - sizeof(uint32_t));      /* 0xff dwords from +0x18 */
    uint32_t i = 0;
    dm_wavepart *P;
    if (S->ov[5]) {
        for (P = T->parts, i = 0; P && i < T->nparts; P = P->next, i++)
            if (P->pch == S->ov[1] && P->index == S->ov[2] && P->lock) { ws_select(pf, T, S, i, P); break; }
    }
    for (P = T->parts, i = 0; P && i < T->nparts; P = P->next, i++) ws_select(pf, T, S, i, P);
}

/* 7732504f: the cursors: each part's first item at or after start (or simply its first) */
static void ws_cursors(dm_wavetrack *T, wstate *S, int64_t start, int fromstart, int clock)
{
    uint32_t i = 0;
    for (dm_wavepart *P = T->parts; P && i < T->nparts; P = P->next, i++) {
        if (!S->cursor) continue;
        if (fromstart) { if (P->items) S->cursor[i] = P->items; continue; }
        dm_waveitem *it;
        for (it = P->items; it; it = it->next) {
            int64_t t = clock ? it->rt : (int64_t)it->logical;
            if (t >= start) break;
        }
        S->cursor[i] = it;
    }
}

/* 7732458e */
static void ws_reset(dm_wavetrack *T, wstate *S)
{
    S->nparts = T->nparts;
    free(S->cursor);
    S->cursor = T->nparts ? calloc(T->nparts, sizeof(*S->cursor)) : NULL;
    S->version = T->version;
}

static void rec_free(dm_perf *pf, wrec *r)       /* 77324221 */
{
    port *po = &pf->ports[r->port];
    if (r->dl && po->ops.wave_unload) po->ops.wave_unload(po->ctx, r->dl);
    wv_release(pf, r->voice);
    free(r);
}

/* 7732588a: a voice for an item; a streaming wave gets a download of its own for every play after the first */
static int ws_alloc_voice(dm_perf *pf, dm_wavetrack *T, wstate *S, void *dl, uint32_t pi, dm_waveitem *it,
                          int64_t startoff, uint32_t chan, uint32_t group, dm_wvoice **out)
{
    port *po = &pf->ports[pi];
    wrec *rec = NULL;
    void *use = NULL;
    int hr = 0;
    if (it->streaming) {
        wsdl *e;
        for (e = T->sdl; e; e = e->next) {
            if (e->dl != dl) continue;
            if (!use) {
                hr = DM_OK;
                use = po->ops.wave_download ? po->ops.wave_download(po->ctx, it->wave, startoff, &hr) : NULL;
                if (!use || hr < 0) return hr < 0 ? hr : DM_E_FAIL;
                rec = calloc(1, sizeof(*rec));
                if (!rec) { po->ops.wave_unload(po->ctx, use); return (int32_t)0x8007000e; }
                rec->dl = use; rec->port = (int)pi;
                rec->next = S->recs; S->recs = rec;
            }
            if (e->st == S) break;
        }
        if (!e) {
            e = calloc(1, sizeof(*e));
            if (!e) return (int32_t)0x8007000e;
            e->dl = dl; e->st = S;
            e->next = T->sdl; T->sdl = e;
        }
    }
    if (!use) use = dl;
    hr = DM_OK;
    void *h = po->ops.voice_alloc ? po->ops.voice_alloc(po->ctx, use, chan, group, startoff, it->loopstart, it->loopend, &hr) : NULL;
    if (!h) return hr < 0 ? hr : DM_E_FAIL;
    dm_wvoice *v = calloc(1, sizeof(*v));
    if (!v) { po->ops.voice_free(po->ctx, h); return DM_E_OUTOFMEMORY; }
    v->refs = 1; v->port = (int)pi; v->h = h;
    if (!rec) {
        if (S->voices[it->index]) wv_release(pf, S->voices[it->index]);
        S->voices[it->index] = v;
    } else rec->voice = v;
    *out = v;
    return hr;
}

/* 773265dc: a voice has stopped: the download made for it goes */
static void ws_voice_end(dm_perf *pf, wstate *S, dm_wvoice *v)
{
    for (wrec **p = &S->recs; *p; p = &(*p)->next)
        if ((*p)->voice == v) { wrec *r = *p; *p = r->next; rec_free(pf, r); return; }
}

/* 77327312 */
static void wt_initplay(dm_perf *pf, dm_segstate *ss, sstrack *e)
{
    dm_wavetrack *T = e->t->data;
    wstate *S = calloc(1, sizeof(*S));
    if (!S) return;
    S->ap = ss->audiopath;
    if (S->ap) dm_audiopath_addref(S->ap);
    ws_reset(T, S);
    if (T->nparts) {
        S->var = calloc(T->nparts, 4); S->varused = calloc(T->nparts, 4);
        if ((T->flags & 2) && T->persistvar && T->persistused)
            for (uint32_t i = 0; i < T->nparts; i++) { S->var[i] = T->persistvar[i]; S->varused[i] = T->persistused[i]; }
    }
    S->groupbits = e->group;
    ws_set_audition(S, T->aud);
    if (!T->autodl_set) T->autodl = pf->autodl != 0;
    e->state = S;
    if (T->autodl) wt_download(pf, T, NULL, 0);
    S->nvoices = T->nitems;
    S->voices = T->nitems ? calloc(T->nitems, sizeof(*S->voices)) : NULL;
    ws_cursors(T, S, 0, 1, 0);
    /* voices made ahead for every item whose wave is downloaded (not for a streaming wave that asks for no preroll) */
    uint32_t i = 0;
    for (dm_wavepart *P = T->parts; i < T->nparts; i++, P = P ? P->next : NULL) {
        uint32_t pch = P ? P->pch : 0;
        if (!S->cursor) continue;
        dm_waveitem *it;
        while ((it = S->cursor[i])) {
            uint32_t pi, group, chan;
            if (wt_pchinfo(pf, S->ap, pch, &pi, &group, &chan) >= 0 && pi < (uint32_t)pf->nports) {
                wdl *d = wdl_find(pf, it, pi);
                if (d && it->index != -1 && (!it->streaming || !it->nopreroll)) {
                    dm_wvoice *v = NULL;
                    ws_alloc_voice(pf, T, S, d->dl, pi, it, it->startoff, chan, group, &v);
                }
            }
            S->cursor[i] = it->next;
        }
    }
}

/* 77326fda */
static void wt_endplay(dm_perf *pf, sstrack *e)
{
    dm_wavetrack *T = e->t->data;
    wstate *S = e->state;
    if (!S) return;
    for (wsdl **p = &T->sdl; *p;) {     /* 77325a44 */
        if ((*p)->st == S) { wsdl *x = *p; *p = x->next; free(x); }
        else p = &(*p)->next;
    }
    while (S->recs) { wrec *r = S->recs; S->recs = r->next; rec_free(pf, r); }
    if (T->autodl) wt_download(pf, T, NULL, 1);
    for (uint32_t i = 0; i < S->nvoices; i++) wv_release(pf, S->voices[i]);
    free(S->voices); free(S->cursor); free(S->var); free(S->varused);
    if (S->ap) dm_audiopath_release(S->ap);
    free(S);
    e->state = NULL;
}

/* 77325abd: Play (music time) and PlayEx (clock: reference times) */
static int wt_play(dm_perf *pf, dm_segstate *ss, sstrack *e, int64_t start, int64_t end, int64_t off, uint32_t flags, int clock)
{
    dm_wavetrack *T = e->t->data;
    wstate *S = e->state;
    if (!S || (flags & TF_PLAY_OFF)) return DM_OK;
    int hr = DM_OK;
    uint32_t looping = flags & TF_LOOP;
    if (looping) {      /* 7730e359: the track's messages from the loop point on are flushed */
        mtime_t m = clock ? R2M(pf, start + off) : (mtime_t)(start + off);
        flush_from(pf, e->vid, m, m, 2);
        S->flushed = 1;
    }
    int seek = flags & TF_SEEK;
    uint32_t restart = flags & (TF_LOOP | TF_START);
    if (restart) S->vartime = 0;
    /* GUID_Variations (DMUS_WAVETRACKF_SYNC_VAR) has no source here: 7732519c falls back to choosing them all */
    int sync = (T->flags & 1) && !(pf->fixes & DM_FIX_WAVE_SYNCVAR);
    if ((sync && (S->vartime == 0 || (start <= S->vartime && S->vartime < end))) || restart)
        ws_select_all(pf, T, S);
    if (S->version != T->version) {
        for (uint32_t i = 0; i < S->nvoices; i++) wv_release(pf, S->voices[i]);
        free(S->voices);
        S->voices = T->nitems ? calloc(T->nitems, sizeof(*S->voices)) : NULL;
        S->nvoices = T->nitems;
        ws_reset(T, S);
        seek = 1;
    }
    if (seek) ws_cursors(T, S, start, restart != 0, clock);
    uint32_t i = 0;
    for (dm_wavepart *P = T->parts; i < T->nparts; i++, P = P ? P->next : NULL) {
        uint32_t pch = P ? P->pch : 0;
        int32_t pvol = P ? P->vol : 0;
        if (!S->cursor) continue;
        dm_waveitem *it;
        while ((it = S->cursor[i])) {
            int64_t t = clock ? it->rt : (int64_t)it->logical;
            if (t >= end) break;
            uint32_t ok = P ? (ws_varmask(S, P, i) & it->variations) : 0;
            uint32_t mpch = pch;            /* GUID_MuteParam: not muted, not remapped */
            uint32_t pi, group, chan;
            wdl *d;
            if (!ok) goto next;
            hr = wt_pchinfo(pf, S->ap, mpch, &pi, &group, &chan);
            if (hr < 0) goto next;
            if (pi >= (uint32_t)pf->nports) { hr = DMUS_E_NOT_FOUND; goto next; }
            if (!(d = wdl_find(pf, it, pi))) goto next;
            {
                int64_t so = it->startoff, dur;
                dm_pmsg *m = pm_alloc(pf, 0x58);
                if (!m) goto next;
                m->type = PT_WAVE; m->pch = mpch; m->vpch = e->vid; m->group = S->groupbits;
                int64_t span;
                if (!clock) {
                    mtime_t imt = (mtime_t)it->rt, o = (mtime_t)off, s0 = (mtime_t)start;
                    rtime_t irt = M2R(pf, o + imt);
                    mtime_t sm = R2M(pf, irt + so);
                    dur = it->dur;
                    if ((int64_t)imt + off < (int64_t)sm) dur -= wt_scale((int64_t)sm - imt - off, -it->pitch);
                    if (imt < s0 && (pf->fixes & DM_FIX_WAVE_SEEK)) {
                        /* into the wave by the time since the item began (in the wave's own time), same end */
                        so += wt_scale(M2R(pf, o + s0) - irt, it->pitch);
                        dur = (int64_t)imt + dur - s0;
                        imt = s0;
                    } else if (imt < s0) {     /* the item began before this piece (M2R of off + 2 start is the original's) */
                        rtime_t r64 = M2R(pf, o + s0 * 2), r5c = M2R(pf, s0 + o + imt);
                        so += wt_scale(r64 - r5c, it->pitch);
                        imt = s0;
                        rtime_t r9c = M2R(pf, (mtime_t)it->dur + s0 + o);
                        int64_t sc = wt_scale(it->startoff, -it->pitch);
                        mtime_t m74 = R2M(pf, (r9c - r64) + r5c - sc);
                        dur = (int64_t)m74 - start - off;
                    }
                    m->mt = o + imt;
                    m->flags = PF_MUSICTIME;
                    m->w_offset = (int32_t)it->rt - it->logical;
                    rtime_t a = M2R(pf, o + imt), b = M2R(pf, o + (mtime_t)dur + imt);
                    span = b - a;
                } else {
                    int64_t irt = it->rt;
                    dur = it->dur - wt_scale(so, -it->pitch);
                    if (irt < start) {
                        so += wt_scale(start - irt, it->pitch);
                        dur += irt - start;
                        irt = start;
                    }
                    m->rt = irt + off;
                    m->flags = PF_REFTIME | PF_LOCKTOREFTIME;
                    m->w_offset = 0;
                    span = dur;
                }
                int64_t ms = span / 10000;
                if (ms > 0 && (it->loopend == 0 || !(flags & TF_START) || ms > 0x95) &&
                    (!(it->flags & WAVEF_IGNORELOOPS) || !looping)) {
                    m->w_startoff = so;
                    m->w_dur = dur;
                    m->w_vol = it->vol + T->vol + pvol;
                    if (it->volrange) m->w_vol += wt_rand(pf) % ((int)it->volrange * 2) - it->volrange;
                    m->w_pitch = it->pitch;
                    if (it->pitchrange) m->w_pitch += wt_rand(pf) % ((int)it->pitchrange * 2) - it->pitchrange;
                    m->w_flags = (uint8_t)it->flags;
                    if (it->index == -1) hr = (int32_t)0x88781162;
                    else {
                        dm_wvoice *v = S->voices ? S->voices[it->index] : NULL;
                        hr = DM_OK;
                        if (S->flushed || !v || so != it->startoff || mpch != pch)
                            hr = ws_alloc_voice(pf, T, S, d->dl, pi, it, so, chan, group, &v);
                        if (hr >= 0 && v) {
                            m->w_voice = v; v->refs++;
                            stamp_pmsg(pf, ss, m);
                            if ((hr = send_pmsg(pf, m)) >= 0) goto next;
                        }
                    }
                }
                pm_free(pf, m);
            }
next:
            S->cursor[i] = it->next;
        }
    }
    return hr;
}

/* 77307763: the output tool: Play at the message's time, then the same message stops it rtDuration later */
static int wave_out(dm_perf *pf, dm_pmsg *p, rtime_t rt)
{
    dm_wvoice *v = p->w_voice;
    if (!v) return DMUS_S_FREE;
    port *po = &pf->ports[v->port];
    if (!(p->w_flags & WAVEF_OFF)) {
        if (po->ops.voice_play && po->ops.voice_play(po->ctx, v->h, rt, p->w_pitch, p->w_vol) >= 0) {
            if (!(p->flags & PF_LOCKTOREFTIME)) { p->mt += (int32_t)p->w_dur; p->flags &= ~PF_REFTIME; }
            else { p->rt += p->w_dur; p->flags &= ~PF_MUSICTIME; }
            p->w_flags |= WAVEF_OFF;
            return DMUS_S_REQUEUE;
        }
        return DMUS_S_FREE;
    }
    if (po->ops.voice_stop) po->ops.voice_stop(po->ctx, v->h, rt);
    for (int k = 0; k < 9; k++)
        for (dm_segstate *s = pf->lists[k]; s; s = s->lnext)
            for (int i = 0; i < s->ntr; i++)
                if (s->tr[i].t->kind == TRK_WAVE && s->tr[i].state) ws_voice_end(pf, s->tr[i].state, v);
    return DMUS_S_FREE;
}

int dm_segment_download_ap(dm_perf *pf, dm_segment *seg, dm_audiopath *ap)
{
    int hr = DM_OK;
    for (int i = 0; i < seg->ntracks; i++) {
        int r = DM_OK;
        if (seg->tracks[i].kind == TRK_BAND) r = bandtrack_download(pf, seg->tracks[i].data, 0);
        else if (seg->tracks[i].kind == TRK_WAVE) r = wt_download(pf, seg->tracks[i].data, ap, 0);
        if (r < 0) hr = r;
    }
    return hr;
}

int dm_segment_unload_ap(dm_perf *pf, dm_segment *seg, dm_audiopath *ap)
{
    for (int i = 0; i < seg->ntracks; i++) {
        if (seg->tracks[i].kind == TRK_BAND) bandtrack_download(pf, seg->tracks[i].data, 1);
        else if (seg->tracks[i].kind == TRK_WAVE) wt_download(pf, seg->tracks[i].data, ap, 1);
    }
    return DM_OK;
}
