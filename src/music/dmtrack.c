/* reAL-Music: track playback (included by dmperf.c).  Sequence track 7731a0b4, tempo track 7731c274, time signature
 * track 7731d59e (dmime), band track 7720211c (dmband).  No mute track is supported, so every MuteLookup answers
 * "not muted, same PChannel" as the original does when GetParam(GUID_MuteParam) fails. */

#define TF_SEEK 1
#define TF_LOOP 2
#define TF_START 4
#define TF_FLUSH 8
#define TF_DIRTY 0x10
#define TF_NOTIFY_OFF 0x20
#define TF_PLAY_OFF 0x40
#define TF_CLOCK 0x400

/* ------------------------------------------------------------------ sequence track */
typedef struct {
    int nparts;
    int *ev, *cv;               /* seek pointers (index, -1 = NULL) */
    mtime_t tsstart, tsnext;
    int32_t mbase, beatlen, measlen, gridlen;
    uint32_t groupbits;
} seqstate;

/* 7731975d */
static void seq_tscache(dm_segstate *ss, sstrack *e, seqstate *S, mtime_t t)
{
    if (t >= S->tsnext || t < S->tsstart) {
        timesig ts;
        mtime_t next = 0;
        int hr = seg_getparam(ss->seg, PRM_TIMESIG, e->group, 0, t, &next, &ts);
        if (hr >= 0) {
            mtime_t ns = ts.t + t;
            S->mbase = S->measlen ? (int32_t)((uint32_t)(ns - S->tsstart) / (uint32_t)S->measlen) : 0;
            S->tsstart = ns;
            S->tsnext = next ? next : 0x7fffffff;
            if (ts.beat) S->beatlen = 0xc00 / (int32_t)ts.beat;
            S->measlen = ts.bpm * S->beatlen;
            if (ts.grids) S->gridlen = (int32_t)((uint32_t)S->beatlen / ts.grids);
        } else {
            S->mbase = 0; S->tsstart = 0; S->tsnext = 0x7fffffff; S->beatlen = 0x300; S->measlen = 0xc00; S->gridlen = 0xc0;
        }
    }
    if (!S->gridlen) S->gridlen = 0xc0;
    if (!S->beatlen) S->beatlen = 0x300;
    if (!S->measlen) S->measlen = 0xc00;
}

static void seq_mbg(seqstate *S, mtime_t t, uint16_t *measure, uint8_t *beat, uint8_t *grid, int16_t *off, int16_t eoff)
{
    uint32_t d = (uint32_t)(t - S->tsstart);
    *measure = (uint16_t)((uint16_t)(d / (uint32_t)S->measlen) + (uint16_t)S->mbase);
    uint32_t r = d % (uint32_t)S->measlen;
    *beat = (uint8_t)(r / (uint32_t)S->beatlen);
    uint32_t r2 = r % (uint32_t)S->beatlen;
    *grid = (uint8_t)(r2 / (uint32_t)S->gridlen);
    *off = (int16_t)((int16_t)(r2 % (uint32_t)S->gridlen) + eoff);
}

static void seq_common(dm_perf *pf, dm_segstate *ss, dm_pmsg *m, uint32_t pch, uint32_t vid, seqstate *S)
{
    m->pch = pch; m->vpch = vid; m->group = S->groupbits;
    stamp_send(pf, ss, m);
}

static void seq_emit_curve(dm_perf *pf, dm_segstate *ss, sstrack *e, seqstate *S, const dm_curveitem *c, mtime_t off)
{
    dm_pmsg *m = pm_alloc(pf, 0x58);
    if (!m) return;
    seq_tscache(ss, e, S, c->start);
    seq_mbg(S, c->start, &m->c_measure, &m->c_beat, &m->c_grid, &m->c_offset, c->off);
    m->mt = c->start + off + c->off;
    m->flags = PF_MUSICTIME | PF_DX8;
    m->type = PT_CURVE;
    m->c_dur = c->dur; m->c_resetdur = c->resetdur; m->c_origstart = 0;
    m->c_start = c->sval; m->c_end = c->eval; m->c_reset = c->rval; m->c_type = c->type; m->c_shape = c->shape;
    m->c_cc = c->cc; m->c_flags = c->flags; m->c_param = c->param; m->c_merge = c->merge;
    seq_common(pf, ss, m, c->pch, e->vid, S);
}

static void seq_emit_event(dm_perf *pf, dm_segstate *ss, sstrack *e, seqstate *S, const dm_seqitem *it, mtime_t off)
{
    if ((it->status & 0xf0) == 0x90) {
        dm_pmsg *m = pm_alloc(pf, 0x50);
        if (!m) return;
        m->n_flags = NOTEF_NOTEON; m->n_dur = it->dur; m->n_midival = it->b1; m->n_vel = it->b2;
        m->type = PT_NOTE; m->n_playmode = 0; m->n_musicval = it->b1; m->n_subchord = 0;
        m->mt = (int32_t)it->off + it->mt + off;
        seq_tscache(ss, e, S, it->mt);
        m->flags = PF_MUSICTIME;
        seq_mbg(S, it->mt, &m->n_measure, &m->n_beat, &m->n_grid, &m->n_offset, it->off);
        seq_common(pf, ss, m, it->pch, e->vid, S);
    } else {
        dm_pmsg *m = pm_alloc(pf, 0x40);
        if (!m) return;
        m->m_status = it->status & 0xf0; m->m_b1 = it->b1; m->m_b2 = it->b2; m->type = PT_MIDI;
        m->mt = it->mt + off;
        m->flags |= PF_MUSICTIME;
        seq_common(pf, ss, m, it->pch, e->vid, S);
    }
}

/* 77319a34 */
static void seq_chase_send(dm_perf *pf, dm_segstate *ss, sstrack *e, seqstate *S, const dm_seqitem *ev,
                           const dm_curveitem *cv, mtime_t start, mtime_t off)
{
    if (ev) {
        if (cv && cv->start + cv->dur <= ev->mt) cv = NULL;
        if (!cv) {
            dm_pmsg *m = pm_alloc(pf, 0x40);
            if (!m) return;
            m->m_status = ev->status & 0xf0; m->m_b1 = ev->b1; m->m_b2 = ev->b2; m->type = PT_MIDI;
            m->mt = start + off; m->flags |= PF_MUSICTIME;
            seq_common(pf, ss, m, ev->pch, e->vid, S);
            return;
        }
    }
    if (!cv) return;
    dm_pmsg *m = pm_alloc(pf, 0x58);
    if (!m) return;
    seq_tscache(ss, e, S, cv->start);
    seq_mbg(S, cv->start, &m->c_measure, &m->c_beat, &m->c_grid, &m->c_offset, cv->off);
    m->mt = cv->off + start + off;
    m->flags = PF_MUSICTIME;
    m->type = PT_CURVE; m->c_type = cv->type; m->c_cc = cv->cc; m->c_flags = cv->flags;
    m->c_start = cv->sval; m->c_end = cv->eval; m->c_reset = cv->rval;
    if (start < cv->start + cv->dur) {
        m->c_shape = cv->shape;
        m->c_origstart = cv->start + off + cv->off;
        if (cv->shape != CURVES_INSTANT) m->c_dur = cv->start - start + cv->dur;
        m->c_resetdur = cv->resetdur;
    } else {
        m->c_shape = CURVES_INSTANT;
        if (cv->flags & CURVEF_RESET) {
            mtime_t x = cv->dur + cv->resetdur + cv->off;
            if (start < x + cv->start) m->c_resetdur = x - start + cv->start;
            else m->c_flags &= ~CURVEF_RESET;
        }
    }
    seq_common(pf, ss, m, cv->pch, e->vid, S);
}

/* 77319d96 */
static void seq_seek_chase(dm_perf *pf, dm_segstate *ss, sstrack *e, seqstate *S, const dm_seqtrack *T, mtime_t start, mtime_t off)
{
    if (start != 0) {
        for (int i = 0; i < T->nparts; i++) {
            const dm_seqpart *p = &T->parts[i];
            const dm_seqitem *lastcc[96] = { 0 }, *lastpb = NULL, *lastat = NULL;
            const dm_curveitem *ccc[96] = { 0 }, *bpb = NULL, *bat = NULL;
            int k;
            for (k = 0; k < p->nev && p->ev[k].mt < start; k++) {
                const dm_seqitem *x = &p->ev[k];
                switch (x->status & 0xf0) {
                case 0xb0: if (x->b1 < 0x60 && x->b1 != 6 && x->b1 != 0x26) lastcc[x->b1] = x; break;
                case 0xd0: lastat = x; break;
                case 0xe0: lastpb = x; break;
                }
            }
            S->ev[i] = k < p->nev ? k : -1;
            for (k = 0; k < p->ncv && p->cv[k].start < start; k++) {
                const dm_curveitem *c = &p->cv[k];
                mtime_t end = c->off + c->dur + c->start;
#define CEND(c) ((c)->off + (c)->dur + (c)->start)
                if (c->type == CURVET_PB) { if (!bpb || CEND(bpb) < end) bpb = c; }
                else if (c->type == CURVET_CC) {
                    if (c->cc < 0x60 && c->cc != 6 && c->cc != 0x26 && (!ccc[c->cc] || CEND(ccc[c->cc]) < end)) ccc[c->cc] = c;
                } else if (c->type == CURVET_MAT) { if (!bat || CEND(bat) < end) bat = c; }
#undef CEND
            }
            S->cv[i] = k < p->ncv ? k : -1;
            seq_chase_send(pf, ss, e, S, lastpb, bpb, start, off);
            seq_chase_send(pf, ss, e, S, lastat, bat, start, off);
            for (int c = 0; c < 96; c++) seq_chase_send(pf, ss, e, S, lastcc[c], ccc[c], start, off);
        }
    } else {
        for (int i = 0; i < T->nparts; i++) {
            if (T->parts[i].nev) S->ev[i] = 0;
            if (T->parts[i].ncv) S->cv[i] = 0;
        }
    }
}

/* 7731a0b4 */
static int seq_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    const dm_seqtrack *T = e->t->data;
    seqstate *S = e->state;
    if (!S || (flags & TF_PLAY_OFF)) return DM_OK;
    if (flags & TF_SEEK) {
        if (!(flags & TF_START)) {
            for (int i = 0; i < T->nparts; i++) {
                const dm_seqpart *p = &T->parts[i];
                int k = 0;
                while (k < p->nev && p->ev[k].mt < start) k++;
                S->ev[i] = k < p->nev ? k : -1;
                k = 0;
                while (k < p->ncv && p->cv[k].start < start) k++;
                S->cv[i] = k < p->ncv ? k : -1;
            }
        } else seq_seek_chase(pf, ss, e, S, T, start, off);
    }
    for (int i = 0; i < T->nparts; i++) {
        const dm_seqpart *p = &T->parts[i];
        int k;
        while ((k = S->cv[i]) >= 0 && !(end <= p->cv[k].start)) {
            seq_emit_curve(pf, ss, e, S, &p->cv[k], off);
            S->cv[i] = k + 1 < p->ncv ? k + 1 : -1;
        }
        while ((k = S->ev[i]) >= 0 && !(end <= p->ev[k].mt)) {
            seq_emit_event(pf, ss, e, S, &p->ev[k], off);
            S->ev[i] = k + 1 < p->nev ? k + 1 : -1;
        }
    }
    return DM_OK;
}

/* ------------------------------------------------------------------ tempo track */
typedef struct { mtime_t prevend; int cur; int active; } tempostate;

static void tempo_seek(const dm_tempotrack *T, tempostate *S, mtime_t start, int getprev)
{
    int p = S->cur;
    if (p < 0) { if (!T->n) return; p = 0; }
    if (start <= T->items[p].mt) p = 0;
    S->cur = p;
    for (; p < T->n; p++) {
        if (start <= T->items[p].mt) { if (!getprev) S->cur = p; break; }
        S->cur = p;
    }
}

static int tempo_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    const dm_tempotrack *T = e->t->data;
    tempostate *S = e->state;
    if (!S || (flags & TF_PLAY_OFF)) return DM_OK;
    int seek = flags & TF_SEEK, hr = DMUS_S_END;
    if (start == 0 && (flags & TF_START)) start = -768;
    if (S->cur < 0 && flags == 0) return DM_FALSE;
    if (S->cur < 0) { if (!T->n) return DMUS_S_END; S->cur = 0; }
    if (seek || S->prevend != start) tempo_seek(T, S, start, !(!seek && (flags & TF_DIRTY)));
    S->prevend = end;
    while (S->cur >= 0) {
        const dm_tempoitem *p = &T->items[S->cur];
        if (end <= p->mt) { hr = DM_OK; break; }
        if ((start <= p->mt || (!(flags & TF_FLUSH) && ((flags & TF_START) || seek))) && S->active) {
            dm_pmsg *m = pm_alloc(pf, 0x40);
            if (m) {
                m->mt = (p->mt < start ? start : p->mt) + off;
                m->flags = PF_MUSICTIME;
                m->tempo = p->tempo; m->vpch = e->vid; m->type = PT_TEMPO; m->group = 0xffffffff; m->pch = 0;
                stamp_send(pf, ss, m);
            }
        }
        S->cur = S->cur + 1 < T->n ? S->cur + 1 : -1;
    }
    return hr;
}

/* ------------------------------------------------------------------ time signature track */
typedef struct { int cur; mtime_t prevend, lastsig; int active; uint8_t beat, bpm; int32_t measbase; } tsstate;

/* the measure number at item i (DM_FIX_MEASURE_NUMBER): every signature starts a new measure; 4/4 before the first */
static int32_t ts_measure_base(const dm_tstrack *T, int i)
{
    int32_t mb = 0;
    mtime_t last = 0;
    uint8_t beat = 4, bpm = 4;
    for (int k = 0; k <= i && k < T->n; k++) {
        if (T->items[k].mt > last && beat && bpm) {
            int32_t ml = (int32_t)(0xc00u / beat) * bpm;
            mb += (T->items[k].mt - last + ml - 1) / ml;
        }
        last = T->items[k].mt; beat = T->items[k].beat; bpm = T->items[k].bpm;
    }
    return mb;
}

/* 7731d2eb: GUID_NOTIFICATION_MEASUREANDBEAT on every beat in [from, to) under the signature in effect; dwField1 the
 * beat in the measure, dwField2 the measure (counted from time 0 with the current signature, as the original) */
static mtime_t ts_notify(dm_perf *pf, dm_segstate *ss, sstrack *e, tsstate *S, mtime_t from, mtime_t to, mtime_t off)
{
    if (!(S->lastsig < to)) return from;
    if (from < S->lastsig) from = S->lastsig;
    if (!S->beat || !S->bpm) return to;            /* the original divides by zero */
    int32_t beatlen = (int32_t)(0xc00u / S->beat);
    mtime_t d = from - S->lastsig;
    if (d) d = ((d - 1) / beatlen + 1) * beatlen;
    mtime_t t = S->lastsig + d;
    uint8_t beat = (uint8_t)(((t - S->lastsig) / beatlen) % (int32_t)S->bpm);
    int32_t measure = t / (int32_t)(S->bpm * beatlen);
    if (pf->fixes & DM_FIX_MEASURE_NUMBER)      /* measures counted across signature changes */
        measure = S->measbase + (t - S->lastsig) / (int32_t)(S->bpm * beatlen);
    for (; t < to; t += beatlen) {
        dm_pmsg *m = pm_alloc(pf, 0x58);
        if (m) {
            m->type = PT_NOTIFICATION; m->mt = t + off; m->flags = PF_MUSICTIME | PF_TOOL_ATTIME; m->pch = 0;
            m->user = ss; m->userref = 1; ss->refs++;
            m->nf_guid = GUID_NOTIFICATION_MEASUREANDBEAT_; m->nf_option = 0;
            m->nf_field1 = beat; m->nf_field2 = (uint32_t)measure;
            m->group = 0xffffffff; m->vpch = e->vid;
            stamp_send(pf, ss, m);
        }
        if (++beat >= S->bpm) { measure++; beat = 0; }
    }
    return to;
}

static int ts_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    const dm_tstrack *T = e->t->data;
    tsstate *S = e->state;
    if (!S) return DM_OK;
    int seek = flags & TF_SEEK;
    int notify = e->t->notify && !(flags & TF_NOTIFY_OFF);
    mtime_t nfrom = start;                      /* notifications from the start as asked (before the -768) */
    if (start == 0 && (flags & TF_START)) start = -768;
    if (seek || S->prevend != start) {        /* 7731d08f */
        if (T->n) {
            int getprev = (flags & (TF_LOOP | TF_START)) != 0;
            if (S->cur < 0) S->cur = 0;
            if (start <= T->items[S->cur].mt) S->cur = 0;
            for (int p = S->cur; p < T->n && T->items[p].mt < start; p++) S->cur = p;
            if (!getprev && S->cur >= 0) S->cur = S->cur + 1 < T->n ? S->cur + 1 : -1;
        }
    }
    S->prevend = end;
    if (S->cur < 0 && T->n) S->cur = 0;
    while (S->cur >= 0 && S->cur < T->n) {
        const dm_tsitem *p = &T->items[S->cur];
        if (end <= p->mt) break;
        if (p->mt < start && !seek) break;
        if (S->active) {
            if (!(flags & TF_PLAY_OFF)) {
                dm_pmsg *m = pm_alloc(pf, 0x40);
                if (m) {
                    m->mt = (p->mt < start ? start : p->mt) + off;
                    m->ts_bpm = p->bpm; m->ts_beat = p->beat; m->ts_grids = p->grids;
                    m->flags |= PF_MUSICTIME; m->vpch = e->vid; m->type = PT_TIMESIG; m->group = 0xffffffff;
                    stamp_send(pf, ss, m);
                }
            }
            if (notify && nfrom < p->mt) nfrom = ts_notify(pf, ss, e, S, nfrom, p->mt, off);
        }
        S->measbase = ts_measure_base(T, S->cur);
        S->beat = p->beat; S->bpm = p->bpm; S->lastsig = p->mt;
        S->cur = S->cur + 1 < T->n ? S->cur + 1 : -1;
    }
    if (S->active && notify && nfrom < end) ts_notify(pf, ss, e, S, nfrom, end, off);
    return DM_OK;
}

/* ------------------------------------------------------------------ SysEx track (dmime 7731b5e2) */
typedef struct { int cur; uint32_t validate; mtime_t lastend; } sysexstate;    /* cur: item index, -1 = none */

static int sysex_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    dm_sysextrack *T = e->t->data;
    sysexstate *S = e->state;
    if (!S || (flags & TF_PLAY_OFF)) return DM_OK;
    int seek = flags & TF_SEEK;
    if (start == 0 && (flags & TF_START)) start = -768;         /* items placed before the start */
    if (S->cur < 0 && flags == 0) return DM_FALSE;
    if (S->validate != T->validate) { S->validate = T->validate; S->cur = -1; }
    if (!T->n) return DMUS_S_END;
    if (!seek && S->lastend == start) { if (S->cur < 0) S->cur = 0; }
    else {      /* 7731b35c: from the current item unless it is already at or past the start */
        if (S->cur < 0) S->cur = 0;
        if (!(T->items[S->cur].mt < start)) S->cur = 0;
        while (S->cur >= 0 && S->cur < T->n && T->items[S->cur].mt < start) S->cur++;
        if (S->cur >= T->n) S->cur = -1;
    }
    S->lastend = end;
    while (S->cur >= 0) {
        const dm_sysexitem *it = &T->items[S->cur];
        if (end <= it->mt) return DM_OK;
        if (it->mt < start && !seek) break;
        dm_pmsg *m = pm_alloc(pf, 0x40 + it->len);
        if (!m) break;
        m->sx_data = malloc(it->len ? it->len : 1);
        if (m->sx_data) memcpy(m->sx_data, it->data, it->len);
        m->sx_len = it->len;
        m->mt = it->mt + off; m->flags = PF_MUSICTIME; m->pch = 0; m->vpch = e->vid; m->type = PT_SYSEX; m->group = 0xffffffff;
        stamp_send(pf, ss, m);
        S->cur = S->cur + 1 < T->n ? S->cur + 1 : -1;
    }
    return DM_OK;
}

/* ------------------------------------------------------------------ band track */
typedef struct { int cur; uint32_t vid; int fstart; uint32_t validate; } bandstate;

/* 77204b0b */
static void band_send_inst(dm_perf *pf, dm_segstate *ss, sstrack *e, dm_band *b, dm_inst *in, mtime_t t, mtime_t off)
{
    bandstate *S = e->state;
    if (in->notinfile) return;          /* a MIDI file's default instrument: only there for the download */
    dm_pmsg tpl;
    memset(&tpl, 0, sizeof(tpl));
    tpl.mt = t + off; tpl.flags = PF_MUSICTIME; tpl.pch = in->pch; tpl.vpch = S->vid; tpl.group = b->groupbits;
    uint32_t f = in->flags;
#define BALLOC(sz, ty) dm_pmsg *m = pm_alloc(pf, sz); if (!m) return; { uint32_t hf = m->hflags; *m = tpl; m->hflags = hf; m->size = sz; m->type = ty; }
    if (f & 1) {
        uint32_t bank = 0;
        if (f & 2) {
            if (f & 8) bank = in->assign & 0x7f7f00;
            else {
                bank = in->patch & 0x7f7f00;
                if (f & 0x400) bank = 0;         /* XG instrument on a port without XG hardware (dmsynth) */
            }
        }
        uint32_t v = bank | (((f & 8) ? in->assign : in->patch) & 0x7f);
        BALLOC(0x40, PT_PATCH);
        m->m_status = v & 0x7f; m->m_b1 = (v >> 16) & 0x7f; m->m_b2 = (v >> 8) & 0x7f;
        stamp_send(pf, ss, m);
    }
    if (f & 0x80) { BALLOC(0x40, PT_TRANSPOSE); m->t_transpose = in->transpose; stamp_send(pf, ss, m); }
    if (f & 0x40) { BALLOC(0x40, PT_MIDI); m->m_status = 0xb0; m->m_b1 = 7; m->m_b2 = in->vol; stamp_send(pf, ss, m); }
    if (f & 0x20) { BALLOC(0x40, PT_MIDI); m->m_status = 0xb0; m->m_b1 = 10; m->m_b2 = in->pan; stamp_send(pf, ss, m); }
    if (f & 0x800) { BALLOC(0x40, PT_CHANNEL_PRIORITY); m->priority = in->priority; stamp_send(pf, ss, m); }
    if (f & 0x2000) {
        BALLOC(0x58, PT_CURVE);
        m->flags |= PF_DX8;
        m->c_end = (int16_t)(in->pbrange << 7);
        m->c_offset = (int16_t)(b->physical - b->logical);
        m->c_type = CURVET_RPN; m->c_shape = CURVES_INSTANT; m->c_param = 0;
        stamp_send(pf, ss, m);
    }
#undef BALLOC
}

static int band_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    dm_bandtrack *T = e->t->data;
    bandstate *S = e->state;
    if (!S) return DMUS_S_END;
    if (flags & 0x1b) for (int i = 0; i < T->nbands; i++) T->bands[i]->groupbits = e->group;
    if ((flags & (TF_LOOP | TF_START)) || S->validate != T->validate) {
        S->fstart = (flags >> 2) & 1;
        /* 77201f38 Resync */
        int count = 0, n = 0;
        while (n < T->nbands && !(start <= T->bands[n]->logical)) { count++; n++; }
        if (S->fstart) {
            dm_band *nb = n < T->nbands ? T->bands[n] : NULL;
            mtime_t t = (nb && nb->physical < start) ? nb->physical : start;
            if (T->nbands) {
                t -= count;
                for (int i = 0; i < T->nbands && T->bands[i]->logical < start; i++) {
                    for (int k = 0; k < T->bands[i]->ninst; k++) band_send_inst(pf, ss, e, T->bands[i], &T->bands[i]->inst[k], t, off);
                    t++;
                }
            }
        }
        S->cur = n;
        S->validate = T->validate;
    }
    int n = S->cur;
    for (; n < T->nbands; n++) {
        dm_band *b = T->bands[n];
        if (end <= b->logical) break;
        for (int k = 0; k < b->ninst; k++) band_send_inst(pf, ss, e, b, &b->inst[k], b->physical, off);
    }
    S->cur = n;
    return n < T->nbands ? DM_OK : DMUS_S_END;
}

/* ------------------------------------------------------------------ band download (dmband 772042e1, 7720413a, 77203e4c) */
static int build_ranges(const uint32_t *bits, uint32_t *out)
{
    int n = 0, in = 0;
    for (int j = 0; j < 128; j++) {
        int b = (bits[j >> 5] >> (j & 31)) & 1;
        if (b && !in) n++;
        in = b;
    }
    if (n == 0 || n > 127) return 0;
    int k = -1;
    in = 0;
    for (int j = 0; j < n; j++) { out[2 * j] = 0; out[2 * j + 1] = 127; }
    for (int j = 0; j < 128; j++) {
        int b = (bits[j >> 5] >> (j & 31)) & 1;
        if (b && !in) { k++; out[2 * k] = j; }
        else if (!b && in) out[2 * k + 1] = j - 1;
        in = b;
    }
    return n;
}

static int inst_download(dm_perf *pf, dm_inst *in, int mode)
{
    uint32_t pi, group, chan;
    int hr = map_pch(pf, in->pch, &pi, &group, &chan, NULL);
    if (hr < 0) return hr;
    /* 7720413a: a GM / GS band (MIDI file import) plays PChannel 9 from the drum kits */
    if (mode != 0 && mode != 4 && in->pch == 9) in->patch |= 0x80000000u;
    if (in->notinfile) return DM_OK;    /* default instruments of a MIDI file are not downloaded */
    if (pi >= (uint32_t)pf->nports) return DMUS_E_NOT_INIT;
    for (dm_dlrec *r = in->dl; r; r = r->next) if (r->port == (int)pi) { r->refs++; return DM_OK; }
    if (!in->coll) return DM_OK;
    port *po = &pf->ports[pi];
    if (!po->ops.download) return DM_OK;
    uint32_t ranges[256];
    int nr = (in->flags & 0x10) ? build_ranges(in->ranges, ranges) : 0;
    void *h = po->ops.download(po->ctx, in->coll, in->patch, (in->flags & 8) != 0, in->assign, nr ? ranges : NULL, nr, &hr);
    if (!h && hr == DMUS_E_NOT_FOUND && (in->flags & 0x700)) {
        in->patch = (int32_t)in->patch < 0 ? 0 : (in->patch & 0x7f);
        h = po->ops.download(po->ctx, in->coll, in->patch, (in->flags & 8) != 0, in->assign, nr ? ranges : NULL, nr, &hr);
    }
    if (!h) return hr < 0 ? hr : DM_E_FAIL;
    dm_dlrec *r = calloc(1, sizeof(*r));
    if (!r) { po->ops.unload(po->ctx, h); return DM_E_OUTOFMEMORY; }
    r->dl = h; r->port = (int)pi; r->refs = 1; r->next = in->dl; in->dl = r;
    return DM_OK;
}

static void inst_unload(dm_perf *pf, dm_inst *in)
{
    uint32_t pi, group, chan;
    if (map_pch(pf, in->pch, &pi, &group, &chan, NULL) < 0) return;
    for (dm_dlrec **pr = &in->dl; *pr; pr = &(*pr)->next)
        if ((*pr)->port == (int)pi) {
            dm_dlrec *r = *pr;
            if (--r->refs > 0) return;
            *pr = r->next;
            if (pf->ports[pi].ops.unload) pf->ports[pi].ops.unload(pf->ports[pi].ctx, r->dl);
            free(r);
            return;
        }
}

static int bandtrack_download(dm_perf *pf, dm_bandtrack *T, int unload)
{
    int hr = DM_OK;
    for (int i = 0; i < T->nbands; i++)
        for (int k = 0; k < T->bands[i]->ninst; k++) {
            if (unload) inst_unload(pf, &T->bands[i]->inst[k]);
            else { int r = inst_download(pf, &T->bands[i]->inst[k], T->bands[i]->mode); if (r < 0) hr = r; }
        }
    return hr;
}

#include "dmwavetrack.c"

int dm_segment_download(dm_perf *pf, dm_segment *seg) { return dm_segment_download_ap(pf, seg, NULL); }
int dm_segment_unload(dm_perf *pf, dm_segment *seg) { return dm_segment_unload_ap(pf, seg, NULL); }

/* ------------------------------------------------------------------ dispatch */
static void track_initplay(dm_perf *pf, dm_segstate *ss, sstrack *e, uint32_t flags)
{
    switch (e->t->kind) {
    case TRK_SEQ: {
        const dm_seqtrack *T = e->t->data;
        seqstate *S = calloc(1, sizeof(*S));
        if (!S) break;
        S->nparts = T->nparts;
        S->ev = malloc((T->nparts + 1) * sizeof(int)); S->cv = malloc((T->nparts + 1) * sizeof(int));
        for (int i = 0; i < T->nparts; i++) S->ev[i] = S->cv[i] = -1;
        S->beatlen = 0x300; S->measlen = 0xc00; S->gridlen = 0xc0;
        S->groupbits = e->group;
        e->state = S;
        break;
    }
    case TRK_TEMPO: {
        const dm_tempotrack *T = e->t->data;
        tempostate *S = calloc(1, sizeof(*S));
        if (!S) break;
        S->cur = T->n ? 0 : -1;
        S->active = T->setbyparam ? T->active : ((flags & SEGF_CONTROL) || !(flags & SEGF_SECONDARY));
        e->state = S;
        break;
    }
    case TRK_TIMESIG: {
        const dm_tstrack *T = e->t->data;
        tsstate *S = calloc(1, sizeof(*S));
        if (!S) break;
        S->cur = T->n ? 0 : -1; S->beat = 4; S->bpm = 4;
        S->active = T->setbyparam ? T->active : (flags & (SEGF_SECONDARY | SEGF_CONTROL)) == 0;
        e->state = S;
        break;
    }
    case TRK_SYSEX: {
        sysexstate *S = calloc(1, sizeof(*S));
        if (!S) break;
        S->cur = -1;
        S->validate = ((dm_sysextrack *)e->t->data)->validate;
        e->state = S;
        break;
    }
    case TRK_WAVE: wt_initplay(pf, ss, e); break;
    case TRK_BAND: {
        dm_bandtrack *T = e->t->data;
        bandstate *S = calloc(1, sizeof(*S));
        if (!S) break;
        S->vid = e->vid;
        e->state = S;
        if (!T->lockautodl) T->autodl = pf->autodl;
        if (T->autodl) bandtrack_download(pf, T, 0);
        break;
    }
    }
}

static void track_endplay(dm_perf *pf, sstrack *e)
{
    if (e->t->kind == TRK_WAVE) { wt_endplay(pf, e); return; }
    if (e->t->kind == TRK_SEQ && e->state) { free(((seqstate *)e->state)->ev); free(((seqstate *)e->state)->cv); }
    if (e->t->kind == TRK_BAND && e->state && ((dm_bandtrack *)e->t->data)->autodl) bandtrack_download(pf, e->t->data, 1);
    free(e->state);
    e->state = NULL;
}

static int track_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    switch (e->t->kind) {
    case TRK_SEQ: return seq_play(pf, ss, e, start, end, off, flags);
    case TRK_TEMPO: return tempo_play(pf, ss, e, start, end, off, flags);
    case TRK_TIMESIG: return ts_play(pf, ss, e, start, end, off, flags);
    case TRK_BAND: return band_play(pf, ss, e, start, end, off, flags);
    case TRK_SYSEX: return sysex_play(pf, ss, e, start, end, off, flags);
    case TRK_WAVE: return wt_play(pf, ss, e, start, end, off, flags, 0);
    }
    return DM_OK;
}
