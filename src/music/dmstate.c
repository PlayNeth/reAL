/* reAL-Music: segment states, the transport thread, PlaySegment, Invalidate and Stop (included by dmperf.c). */

/* ------------------------------------------------------------------ segment-state lists */
static void list_push(dm_perf *pf, int k, dm_segstate *s) { s->lnext = pf->lists[k]; pf->lists[k] = s; }

static int list_unlink(dm_perf *pf, int k, dm_segstate *s)
{
    for (dm_segstate **p = &pf->lists[k]; *p; p = &(*p)->lnext)
        if (*p == s) { *p = s->lnext; s->lnext = NULL; return 1; }
    return 0;
}

static dm_segstate *list_last(dm_perf *pf, int k)
{
    dm_segstate *s = pf->lists[k];
    while (s && s->lnext) s = s->lnext;
    return s;
}

/* 7731941b: sorted by start (mtStart, or the rt start for REFTIME states); equal keys after existing ones */
static int64_t ss_key(const dm_segstate *s) { return (s->flags & SEGF_REFTIME) ? s->i64start : (int64_t)s->mtstart; }
static void list_insert(dm_perf *pf, int k, dm_segstate *s)
{
    dm_segstate *h = pf->lists[k];
    if (!h || ss_key(s) < ss_key(h)) { list_push(pf, k, s); return; }
    while (h->lnext && ss_key(h->lnext) <= ss_key(s)) h = h->lnext;
    s->lnext = h->lnext;
    h->lnext = s;
}

static int list_find(dm_perf *pf, dm_segstate *s)
{
    for (int k = 0; k < NLISTS; k++) for (dm_segstate *x = pf->lists[k]; x; x = x->lnext) if (x == s) return k;
    return -1;
}

/* ------------------------------------------------------------------ segment state objects */
static void ss_destroy(dm_segstate *ss)
{
    for (int i = 0; i < ss->ntr; i++) track_endplay(ss->perf, &ss->tr[i]);
    free(ss->tr);
    dm_segment_release(ss->seg);
    if (ss->audiopath) dm_audiopath_release(ss->audiopath);
    free(ss);
}

static void ss_unref(dm_segstate *ss) { if (--ss->refs <= 0) ss_destroy(ss); }

void dm_segstate_release(dm_segstate *ss) { if (ss && !ss->released) { ss->released = 1; ss_unref(ss); } }

int dm_segstate_start_time(dm_segstate *ss, mtime_t *mt) { *mt = ss->mtstart; return DM_OK; }

/* 77311cea + 77315351 */
static dm_segstate *ss_create(dm_perf *pf, dm_segment *seg, uint32_t flags)
{
    dm_segstate *ss = calloc(1, sizeof(*ss));
    if (!ss) return NULL;
    ss->refs = 1;
    ss->perf = pf; ss->seg = seg; seg->refs++;
    ss->trackflags = TF_SEEK | TF_START; ss->mtstart = -1; ss->i64start = -1; ss->canstop = 1;
    ss->tr = calloc(seg->ntracks ? seg->ntracks : 1, sizeof(*ss->tr));
    ss->ntr = seg->ntracks;
    for (int i = 0; i < seg->ntracks; i++) {
        ss->tr[i].t = &seg->tracks[i]; ss->tr[i].cfg = seg->tracks[i].config; ss->tr[i].group = seg->tracks[i].group;
    }
    ss->loopstart = seg->loopstart; ss->loopend = seg->loopend; ss->repeats = seg->repeats; ss->length = seg->length;
    ss->startpoint = seg->playstart; ss->rtlength = seg->rtlength; ss->rtloopstart = seg->rtloopstart;
    ss->rtloopend = seg->rtloopend; ss->segflags = seg->ioflags;
    if (!(ss->segflags & 2) && ss->repeats) {
        if (ss->loopend == 0) ss->loopend = ss->length;
        if (ss->loopstart < ss->loopend) {
            uint32_t max = (uint32_t)(0x3fffffff - ss->length) / (uint32_t)(ss->loopend - ss->loopstart);
            if (max < ss->repeats) ss->repeats = max;
        } else ss->repeats = 0;
        if (ss->loopend <= ss->startpoint) { ss->loopstart = ss->loopend = 0; ss->repeats = 0; }
    }
    ss->repeatsleft = ss->repeats;
    for (int i = 0; i < ss->ntr; i++) {
        uint32_t id = ++g_trackid;
        if (!ss->firstid) ss->firstid = id;
        ss->lastid = id;
        ss->tr[i].vid = id;
        track_initplay(pf, ss, &ss->tr[i], flags);
    }
    return ss;
}

/* 773155db: the end time (with the full repeat count) */
static mtime_t ss_end(dm_segstate *ss, mtime_t start)
{
    uint32_t u = (uint32_t)((ss->loopend - ss->loopstart) * ss->repeats + ss->length);
    int64_t r = (int64_t)u - ss->startpoint + start;
    return r > 0x7fffffff ? 0x7fffffff : (mtime_t)r;
}

/* 77315e18 */
static mtime_t ss_avail(dm_segstate *ss, mtime_t span)
{
    if (ss->segflags & 2) return span;
    mtime_t pos = (ss->trackflags & TF_START) ? ss->startpoint : ss->segpos;
    mtime_t lim = ss->repeatsleft ? ss->loopend : ss->length;
    return lim - pos < span ? lim - pos : span;
}

/* 77305992: first control start >= pos (lists 7, 4); 77305a25: next primary start after pos */
static int control_start_at(dm_perf *pf, mtime_t pos, mtime_t *t)
{
    for (int k = 0; k < 2; k++)
        for (dm_segstate *s = pf->lists[k ? 4 : 7]; s; s = s->lnext) if (pos <= s->mtstart) { *t = s->mtstart; return 0; }
    *t = 0;
    return 1;
}
static int primary_start_after(dm_perf *pf, mtime_t pos, mtime_t *t)
{
    for (dm_segstate *s = pf->lists[3]; s; s = s->lnext) if (pos < s->mtstart) { *t = s->mtstart; return 0; }
    *t = 0;
    return 1;
}

/* 77316a21: play one piece of len ticks through all tracks */
static int ss_play(dm_perf *pf, dm_segstate *ss, mtime_t len)
{
    if (len < 1) return DM_E_INVALIDARG;
    if (ss->inplay) return DM_OK;
    ss->inplay = 1;
    pf->ssplaying = ss;
    int hr = DM_OK;
    mtime_t n, t, pos;
    if (ss->trackflags & TF_START) {
        ss->segpos = ss->startpoint;
        ss->rtoffset = M2R(pf, ss->mtoffset) - ss->rtstartpoint;
        ss->rtend = M2R(pf, ss->mtoffset) + ss->rtlength - ss->rtstartpoint;
        ss->mtoffset -= ss->startpoint;
        ss->rtsegpos = ss->rtlast - ss->rtoffset;
        ss->rtd0 = 0;
    }
    int reflen = 0;
    if (ss->rtlength) {
        ss->mtend = R2M(pf, ss->rtend);
        mtime_t nl = ss->mtend - ss->mtoffset, old = ss->length;
        ss->length = nl >= 1 ? nl : old;
        if (old <= ss->loopend) ss->loopend = ss->length;
        if (ss->length < ss->loopend) {
            ss->loopend = ss->length;
            if (ss->loopend <= ss->startpoint) ss->loopstart = ss->loopend = ss->repeatsleft = ss->repeats = 0;
        }
        reflen = 1;
    }
#define LIMIT() (ss->repeatsleft ? ss->loopend : (reflen ? ss->mtend - ss->mtoffset : ss->length))
    n = LIMIT() - ss->segpos;
    if (len < n) n = len;
    if (n < 1) goto end;
    pos = ss->mtoffset + ss->segpos;
    if (primary_start_after(pf, pos, &t) == 0 && t < pos + n) n = t - pos;
    if (control_start_at(pf, pos, &t) == 0) {
        if (t == pos) ss->trackflags |= TF_DIRTY;
        else if (t < pos + n) n = t - pos;
    }
    rtime_t rtdelta = M2R(pf, ss->mtlast + n) - ss->rtlast;
    for (;;) {
        if (n != 0)
            for (int i = 0; i < ss->ntr; i++) {
                sstrack *e = &ss->tr[i];
                pf->inplay = 1;
                uint32_t cfg = e->cfg;
                if (cfg & 0x30) {
                    uint32_t f = 0;
                    if (!(cfg & 0x20)) f |= TF_NOTIFY_OFF;
                    if (!(cfg & 0x10)) f |= TF_PLAY_OFF;
                    if (((ss->trackflags & TF_START) && (cfg & 0x80)) || ((ss->trackflags & TF_LOOP) && (cfg & 0x100))) f |= 0x200;
                    if (e->rtbits & 4) { f |= TF_START; e->rtbits &= ~4u; }
                    pf->cfgplaying = cfg;
                    if (track_play(pf, ss, e, ss->segpos, ss->segpos + n, ss->mtoffset, ss->trackflags | f) != 0) e->notok = 1;
                }
                pf->inplay = 0;
            }
        ss->mtlast += n; ss->rtlast += rtdelta; ss->trackflags = 0;
        ss->segpos += n; ss->rtsegpos += rtdelta; hr = DM_OK;
        if (ss->segpos < ss->loopend) goto out;
        if (ss->repeatsleft == 0) {
            if (ss->segpos != ss->length) goto out;
            hr = DMUS_S_END;
            goto end;
        }
        /* repeat */
        ss->trackflags |= TF_SEEK | TF_LOOP;
        ss->repeatsleft--;
        for (int i = 0; i < ss->ntr; i++) ss->tr[i].notok = 0;
        ss->mtoffset += ss->loopend - ss->loopstart;
        ss->segpos = ss->loopstart;
        int more = 0;
        if (n < len) {
            len -= n;
            n = LIMIT() - ss->segpos;
            if (len < n) n = len;
            more = 1;
        }
        pos = ss->mtoffset + ss->segpos;
        if (control_start_at(pf, pos, &t) == 0) {
            if (t == pos) ss->trackflags |= TF_DIRTY;
            else if (pos + n < t) n = t - pos;      /* inverted in the original */
        }
        rtdelta = M2R(pf, ss->mtlast + n) - ss->rtlast;
        if (!more) goto out;
    }
#undef LIMIT
end:
    hr = DMUS_S_END;
out:
    ss->trackflags &= ~TF_DIRTY;
    pf->cfgplaying = 0; pf->ssplaying = NULL;
    ss->inplay = 0;
    return hr;
}

/* 7730c76e: first-start initialisation */
static void ss_firststart(dm_perf *pf, dm_segstate *ss, int skiptempo)
{
    ss->inited = 1;
    ss->mtlast = ss->mtstart;
    if (!skiptempo) tempo_rebuild(pf, ss, ss->mtstart, 1);
    ss->rtlast = M2R(pf, ss->mtlast);
    ss->mtend = ss_end(ss, ss->mtstart);
}

/* 773158ad: abort/stop at mt */
static int ss_abort(dm_perf *pf, dm_segstate *ss, mtime_t mt)
{
    if (mt < ss->mtlast) {
        for (int i = 0; i < ss->ntr; i++) flush_from(pf, ss->tr[i].vid, mt, mt, 0);
        ss->mtlast = mt;
    }
    ss->rtlast = M2R(pf, mt);
    if ((!ss->started || mt < ss->mtend) && (ss->mtabort == 0 || mt < ss->mtabort)) {
        ss->mtabort = mt;
        return 0;
    }
    return 1;
}

static void ss_mark_dirty_all(dm_perf *pf)
{
    for (int k = 3; k <= 5; k++) for (dm_segstate *s = pf->lists[k]; s; s = s->lnext) if (s->started) s->trackflags |= TF_DIRTY;
}

/* 7730c7c8: advance a state to the prepare target */
static void ss_advance(dm_perf *pf, int k, dm_segstate *ss)
{
    if (!pf->played || pf->inplay || !ss) return;
    pf->inside_c7c8 = 1;
    int hr = DM_OK;
    if (!ss->started) {
        if (ss->mtstart < pf->target) {
            ss->started = 1;
            ss_firststart(pf, ss, 0);
            if (pf->stopped) pf->stopped = 0;
            if (pf->target - ss->mtlast >= 50) {
                hr = ss_play(pf, ss, 50);
                run_early(pf);
                if (pf->ops.signal) pf->ops.signal(pf->ctx, DM_THREAD_REALTIME);
                if (hr == DM_OK && pf->target - ss->mtlast >= 200) { hr = ss_play(pf, ss, 200); run_early(pf); }
            }
            for (int j = 4; j <= 5; j++)
                for (dm_segstate *s = pf->lists[j]; s; s = s->lnext) if (s->started && s != ss) s->trackflags |= 0x80000000u;
        }
    } else ss->rtlast = M2R(pf, ss->mtlast);
    if (ss->started) {
        mtime_t span = (ss->mtstop != 0 && ss->mtstop < pf->target) ? ss->mtstop - ss->mtlast : pf->target - ss->mtlast;
        while (hr == DM_OK && span > 0) {
            mtime_t chunk = span <= 0x300 ? span : 0x300;
            span -= chunk;
            hr = ss_play(pf, ss, chunk);
            run_early(pf);
        }
    }
    if (hr == DMUS_S_END || (ss->mtstop != 0 && ss->mtstop <= ss->mtlast)) {
        if (ss->mtstop != 0 && ss->mtstop == ss->mtlast) ss_abort(pf, ss, ss->mtstop - 1);
        if (k == 3) { list_unlink(pf, 3, ss); list_insert(pf, 6, ss); }
        else if (k == 4) { list_unlink(pf, 4, ss); list_insert(pf, ss->mtstop == ss->mtlast ? 9 : 7, ss); }
        else { list_unlink(pf, k, ss); list_insert(pf, 8, ss); }
        if (!pf->lists[3] && !pf->lists[5] && !pf->lists[0] && !pf->lists[2] && !pf->lists[4] && !pf->lists[1]) pf->stopped = 1;
    }
    pf->inside_c7c8 = 0;
}

/* 77315edc: re-seek a state to perf time t */
static void ss_seek(dm_segstate *ss, mtime_t t)
{
    if (ss->segflags & 2) return;
    mtime_t off; uint32_t loops, fl = 9;
    if (ss_fold(ss, &t, &off, &loops) == 0) ss->repeatsleft = ss->repeats - loops;
    else { t = 0; ss->repeatsleft = ss->repeats; fl = 0xd; off = ss->mtstart; }
    ss->mtoffset = off;
    for (int i = 0; i < ss->ntr; i++) ss->tr[i].notok = 0;
    ss->segpos = t;
    ss->trackflags |= fl | 1;
}

/* 7730e396 Invalidate */
static void invalidate(dm_perf *pf, mtime_t mt, uint32_t flags)
{
    flush_ports(pf);
    rtime_t q = dm_perf_queue_time(pf);
    mtime_t mtq = R2M(pf, q), mtqb = R2M(pf, q + pf->bumper);
    mtime_t t = resolve_mt(pf, mt < mtqb ? mtqb : mt, flags);
    flush_from(pf, 0, t, mtq, 4);
    for (dm_segstate *s = pf->lists[8], *nx; s; s = nx) { nx = s->lnext; if (t < s->mtlast) { list_unlink(pf, 8, s); list_insert(pf, 5, s); } }
    for (dm_segstate *s = pf->lists[7], *nx; s; s = nx) { nx = s->lnext; if (t < s->mtlast) { list_unlink(pf, 7, s); list_insert(pf, 4, s); } }
    dm_segstate *l = list_last(pf, 6);
    if (l && t < l->mtlast) { list_unlink(pf, 6, l); list_insert(pf, 3, l); }
    for (int k = 3; k < 6; k++)
        for (dm_segstate *s = pf->lists[k]; s; s = s->lnext)
            if (s->started && !(s->segflags & 2) && t < s->mtlast) {
                if (t < s->mtstart) { s->started = 0; s->mtlast = s->mtstart; }
                else s->mtlast = t;
                ss_seek(s, s->mtlast);
            }
    if (pf->ops.signal) pf->ops.signal(pf->ctx, DM_THREAD_TRANSPORT);
}

/* 7730d00c: start a secondary or control state */
static void start_secondary(dm_perf *pf, dm_segstate *ss)
{
    int ctl = ss->flags & SEGF_CONTROL;
    ss->flags &= ~SEGF_QUEUE;
    if (ss->i64start == 0) {
        rtime_t rt = ctl ? dm_perf_queue_time(pf) : dm_perf_latency_time(pf);
        ss->flags &= ~SEGF_REFTIME;
        ss->i64start = R2M(pf, rt);
    }
    if (ss->flags & SEGF_REFTIME) {
        rtime_t now = ctl ? dm_perf_queue_time(pf) : dm_perf_latency_time(pf);
        mtime_t mt;
        if (ss->i64start > now) {
            if (pf->lists[3]) { list_insert(pf, 2, ss); return; }
            mt = R2M(pf, ss->i64start);
        } else mt = R2M(pf, now);
        ss->flags &= ~SEGF_REFTIME;
        ss->i64start = mt;
    }
    /* 7730ae6a */
    ss->mtstart = resolve_mt(pf, (mtime_t)ss->i64start, ss->flags);
    ss->mtoffset = ss->mtlast = ss->mtstart;
    if (ctl) {
        list_insert(pf, 4, ss);
        if (!(ss->flags & SEGF_NOINVALIDATE)) invalidate(pf, ss->mtstart, 0);
    } else list_insert(pf, 5, ss);
    for (dm_segstate *s = pf->lists[5], *nx; s; s = nx) { nx = s->lnext; ss_advance(pf, 5, s); }
    for (dm_segstate *s = pf->lists[4], *nx; s; s = nx) { nx = s->lnext; ss_advance(pf, 4, s); }
}

/* 7730ca85 (simplified): start a primary state */
static void start_primary(dm_perf *pf, dm_segstate *ss)
{
    ss->trackflags |= TF_DIRTY;
    if (ss->i64start == 0) {
        mtime_t mt = R2M(pf, dm_perf_queue_time(pf));
        ss->flags &= ~SEGF_REFTIME;
        ss->i64start = mt;
        while (pf->lists[3]) { dm_segstate *s = pf->lists[3]; pf->lists[3] = s->lnext; ss_abort(pf, s, mt); list_insert(pf, 9, s); }
    }
    if (ss->flags & SEGF_REFTIME) { ss->i64start = R2M(pf, ss->i64start); ss->flags &= ~SEGF_REFTIME; }
    ss->mtstart = resolve_mt(pf, (mtime_t)ss->i64start, ss->flags);
    ss->mtoffset = ss->mtlast = ss->mtstart;
    for (dm_segstate *s = pf->lists[3], *nx; s; s = nx) {
        nx = s->lnext;
        ss_abort(pf, s, ss->mtstart);
        list_unlink(pf, 3, s); list_insert(pf, 6, s);
    }
    list_push(pf, 3, ss);
    if (!(ss->flags & SEGF_NOINVALIDATE)) invalidate(pf, ss->mtstart, ss->flags);
    ss_advance(pf, 3, ss);
}

/* PlaySegmentEx 7730d51c / 7730d2ee */
int dm_perf_play(dm_perf *pf, dm_segment *seg, uint32_t flags, int64_t start, dm_audiopath *ap, dm_segstate **out)
{
    if (!pf->nports) return DMUS_E_NOT_INIT;
    pf->xprec = pf->appxprec;
    if (flags & SEGF_DEFAULT) flags = (flags & ~SEGF_DEFAULT) | seg->resolution;
    if (seg->ioflags & 2) flags &= 0xffeefcff;
    if (flags & SEGF_CONTROL) flags |= SEGF_SECONDARY;
    if (start != 0 && !(flags & SEGF_ALIGN)) {
        rtime_t lat = dm_perf_latency_time(pf);
        if (!(flags & SEGF_REFTIME)) { if ((mtime_t)start < R2M(pf, lat - 1000000)) return (int32_t)0x88781212; }
        else if (start < lat - 1000000) return (int32_t)0x88781212;
    }
    if (!ap) ap = pf->defpath;
    if (ap && !ap->active) return DMUS_E_NOT_INIT;
    dm_segstate *ss = ss_create(pf, seg, flags);
    if (!ss) return DM_E_OUTOFMEMORY;
    ss->audiopath = ap;
    if (ap) dm_audiopath_addref(ap);
    ss->i64start = start; ss->flags = flags;
    pf->played = 1;
    if (flags & SEGF_AFTERPREPARETIME) {
        if (flags & SEGF_REFTIME) {
            rtime_t rt = M2R(pf, pf->prepared);
            if (ss->i64start < rt) { ss->flags &= ~SEGF_REFTIME; ss->i64start = pf->prepared; }
        } else if (ss->i64start < pf->prepared) ss->i64start = pf->prepared;
    } else if (flags & SEGF_AFTERQUEUETIME) {
        rtime_t q = dm_perf_queue_time(pf);
        if (flags & SEGF_REFTIME) { if (ss->i64start < q) ss->i64start = q; }
        else { mtime_t m = R2M(pf, q); if (ss->i64start < m) ss->i64start = m; }
    }
    if (out) { ss->refs++; *out = ss; } else ss->released = 1;
    if (!(flags & SEGF_SECONDARY)) start_primary(pf, ss);
    else start_secondary(pf, ss);
    if (pf->ops.signal) pf->ops.signal(pf->ctx, DM_THREAD_TRANSPORT);
    return DM_OK;
}

/* 7730dc1f: stop one state at mt */
static void stop_state(dm_perf *pf, dm_segstate *ss, mtime_t mt, int flush)
{
    if (mt < ss->mtend) {
        mtime_t l = ss->startpoint - ss->mtstart + mt;
        ss->length = l < 0 ? 0 : l;
        ss->mtend = mt + 1;
    }
    tempo_rebuild(pf, ss, mt, 1);
    int k = list_find(pf, ss);
    if (k >= 3 && k <= 8) {
        rtime_t rt = M2R(pf, mt);
        if (mt <= ss->mtlast) {
            for (int i = 0; i < ss->ntr; i++) flush_from(pf, ss->tr[i].vid, mt, mt, 0);
            ss->rtlast = rt; ss->mtlast = mt;
        }
        if (flush) {
            if (!(ss->flags & SEGF_CONTROL)) { if (!(ss->flags & SEGF_SECONDARY)) flush_queue(pf, 0, &pf->tmap, rt, rt, 0); }
            else invalidate(pf, mt, 0);
        }
        ss_abort(pf, ss, mt);
        k = list_find(pf, ss);
        if (!(ss->flags & SEGF_CONTROL)) { if (k >= 3 && k <= 5) { list_unlink(pf, k, ss); list_insert(pf, k + 3, ss); } }
        else { list_unlink(pf, k, ss); list_insert(pf, 9, ss); ss_mark_dirty_all(pf); }
    } else if (k >= 0 && k <= 2) {
        ss_abort(pf, ss, mt);
        list_unlink(pf, k, ss);
        tempo_rebuild(pf, ss, mt, 1);
        list_insert(pf, 9, ss);
    }
    if (!pf->lists[3] && !pf->lists[5] && !pf->lists[0] && !pf->lists[2] && !pf->lists[4] && !pf->lists[1]) pf->stopped = 1;
}

/* Stop 7730e080 (StopEx 77304b26 calls it with the segment or the state): seg and/or ss, mt 0 = as soon as possible */
int dm_perf_stop(dm_perf *pf, dm_segment *seg, dm_segstate *ss, mtime_t mt, uint32_t flags)
{
    pf->xprec = pf->appxprec;
    mtime_t q = R2M(pf, dm_perf_queue_time(pf));
    mtime_t t = resolve_mt(pf, mt < q ? q : mt, flags);
    if (pf->prepared < t) {
        for (int k = 3; k < 6; k++)
            for (dm_segstate *s = pf->lists[k]; s; s = s->lnext)
                if ((s->seg == seg || s == ss) && s->canstop) {
                    s->mtstop = t;
                    if (t < s->mtend) {
                        mtime_t l = s->startpoint - s->mtstart + t;
                        s->length = l < 0 ? 0 : l;
                        s->mtend = t + 1;
                    }
                    tempo_rebuild(pf, s, t, 1);
                }
        return DM_OK;
    }
    if (ss) stop_state(pf, ss, t, 1);
    if (!ss || seg) {
        if (!seg) {     /* 7730deeb with no segment: stop everything */
            flush_from(pf, 0, t, t, 0);
            for (int k = 0; k < 3; k++)
                while (pf->lists[k]) { dm_segstate *s = pf->lists[k]; ss_abort(pf, s, t); pf->lists[k] = s->lnext; list_insert(pf, 9, s); }
            for (int k = 6; k < 9; k++)
                for (dm_segstate *s = pf->lists[k], *nx; s; s = nx) { nx = s->lnext; if (t <= s->mtlast) stop_state(pf, s, t, 1); }
            for (int k = 3; k < 6; k++) while (pf->lists[k]) stop_state(pf, pf->lists[k], t, 1);
        } else
            for (int k = 0; k < 9; k++)
                for (dm_segstate *s = pf->lists[k], *nx; s; s = nx) {
                    nx = s->lnext;
                    if (s->seg == seg && (k < 6 || t <= s->mtlast)) stop_state(pf, s, t, 1);
                }
    }
    return DM_OK;
}

/* 773053ec IsPlaying: S_OK while a started state of the segment (or the state itself) has played past now */
int dm_perf_is_playing(dm_perf *pf, dm_segment *seg, dm_segstate *ss)
{
    if (!seg && !ss) return DM_E_POINTER;
    mtime_t now = R2M(pf, clock_now(pf));
    for (int k = 0; k < 9; k++)
        for (dm_segstate *s = pf->lists[k]; s; s = s->lnext) {
            if (!s->started) continue;
            if (now < s->mtstart) break;
            if (now < s->mtlast && (s == ss || (seg && s->seg == seg))) return DM_OK;
        }
    return DM_FALSE;
}

/* GetTime */
void dm_perf_get_time(dm_perf *pf, rtime_t *rt, mtime_t *mt)
{
    rtime_t t = clock_now(pf);
    if (rt) *rt = t;
    if (mt) *mt = R2M(pf, t);
}

/* ------------------------------------------------------------------ the transport thread (7730eb7f) */
uint32_t dm_perf_transport(dm_perf *pf)
{
    pf->xprec = 0;
    rtime_t now = clock_now(pf);
    mtime_t target = R2M(pf, now + (rtime_t)(uint32_t)(pf->prepare_ms * 10000));
    if (pf->tempochanged) {
        if (pf->target >= target) target = pf->target + 10;
        pf->tempochanged = 0;
    }
    if (pf->nextscan <= pf->prepared + 0x7800) tempo_scan(pf, pf->nextscan, 0, NULL, 1);   /* 7730c379 */
    if (pf->target < target) {
        do {
            int dirty = 0;
            pf->target = target;
            dm_segstate *p = pf->lists[3];
            if (p && p->started) {
                mtime_t span = target - p->mtlast, avail = ss_avail(p, span);
                if (avail < span) pf->target += avail - span;
            }
            for (dm_segstate *c = pf->lists[4]; c; c = c->lnext) {
                if (!c->started) continue;
                if (c->mtstop == 0 || pf->target <= c->mtstop) {
                    mtime_t span = pf->target - c->mtlast, avail = ss_avail(c, span);
                    if (span <= avail) continue;
                    pf->target += avail - span;
                } else pf->target = c->mtstop;
                dirty = 1;
            }
            for (;;) {
                ss_advance(pf, 3, pf->lists[3]);
                dm_segstate *h = pf->lists[3], *n = h ? h->lnext : NULL;
                if (!n || h->mtlast < n->mtstart) break;
                ss_abort(pf, h, n->mtstart - 1);
                list_unlink(pf, 3, h); list_insert(pf, 6, h);
                dm_segstate *h2 = pf->lists[3];
                if (!(h2->flags & (0x10000000 | SEGF_NOINVALIDATE))) invalidate(pf, h2->mtstart, 0);
            }
            for (dm_segstate *s = pf->lists[4], *nx; s; s = nx) { nx = s->lnext; ss_advance(pf, 4, s); }
            for (dm_segstate *s = pf->lists[5], *nx; s; s = nx) { nx = s->lnext; ss_advance(pf, 5, s); }
            if (dirty) ss_mark_dirty_all(pf);
            pf->prepared = pf->target;
        } while (pf->target < target);
    }
    rtime_t lat = dm_perf_latency_time(pf);
    for (int k = 0; k < 3; k++) {
        dm_segstate *s;
        while ((s = pf->lists[k]) && s->i64start <= lat + pf->bumper + (rtime_t)(uint32_t)(pf->prepare_ms * 10000) + 2000000) {
            pf->lists[k] = s->lnext; s->lnext = NULL;
            if (k == 0) start_primary(pf, s); else start_secondary(pf, s);
        }
    }
    for (int k = 6; k < 10; k++)
        for (dm_segstate *s = pf->lists[k], *nx; s; s = nx) {
            nx = s->lnext;
            if (s->rtlast < now - 10000000 && !s->inplay) { list_unlink(pf, k, s); ss_unref(s); }
        }
    dm_pmsg *n;
    while ((n = pf->notif.head) && n->rt < now - 20000000) { q_pop(&pf->notif); pm_free(pf, n); }
    return 100;
}

/* Invalidate (7730e396) from the application */
void dm_perf_invalidate(dm_perf *pf, mtime_t mt, uint32_t flags)
{
    pf->xprec = pf->appxprec;
    invalidate(pf, mt, flags);
}

/* StopEx with an audiopath: every state playing on it */
int dm_perf_stop_audiopath(dm_perf *pf, dm_audiopath *ap, mtime_t mt, uint32_t flags)
{
    dm_segstate *v[256];
    int n = 0;
    for (int k = 0; k < 9; k++)
        for (dm_segstate *s = pf->lists[k]; s && n < 256; s = s->lnext)
            if (s->audiopath == ap) v[n++] = s;
    for (int i = 0; i < n; i++) dm_perf_stop(pf, NULL, v[i], mt, flags);
    return DM_OK;
}
