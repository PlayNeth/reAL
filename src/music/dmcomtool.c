/* reDM, included by dmcom.c: tool graphs (the engine's dm_toolops) and the performance's graph and tool. */
/* ------------------------------------------------------------------ tool graphs (the engine's dm_toolops) */
static rm_path *new_path(rm_perf *p, dm_audiopath *ap);
static rm_state *state_of(rm_perf *p, dm_segstate *ss);

/* the graph interface of a graph's owner (one reference) */
static IDirectMusicGraph *owner_graph(rm_perf *p, int owner, void *obj)
{
    if (owner == DM_GRAPH_PERF) return &p->IDirectMusicGraph_iface;
    if (owner == DM_GRAPH_STATE) {
        rm_state *s = state_of(p, obj);
        return s ? &s->IDirectMusicGraph_iface : NULL;
    }
    void **w = dm_audiopath_wrapper(obj);
    rm_path *a = *w;
    if (a && ref_if_alive(&a->ref)) return &a->IDirectMusicGraph_iface;
    dm_audiopath_addref(obj);                   /* a path the application let go of (or is letting go of): a new object */
    if (!(a = new_path(p, obj))) { dm_audiopath_release(obj); return NULL; }
    *w = a;
    return &a->IDirectMusicGraph_iface;
}

static int tops_stamp(void *ctx, void *graph, int owner, void *obj, dm_pmsg *e)
{
    rm_perf *p = ctx;
    DMUS_PMSG *m = pm_view(p, e);
    if (!m) return E_OUTOFMEMORY;
    HRESULT hr = IDirectMusicGraph_StampPMsg((IDirectMusicGraph *)graph, m);
    if (hr == S_OK) {
        IDirectMusicGraph *g = owner_graph(p, owner, obj);
        if (m->pGraph != g) {
            dm_host_release_later(m->pGraph);
            m->pGraph = g;
        } else dm_host_release_later(g);
        e->flags = m->dwFlags;
    }
    return hr;
}
static void tops_unstamp(void *ctx, dm_pmsg *e)
{
    DMUS_PMSG *m = e->ext;
    dm_host_release_later(m->pGraph); m->pGraph = NULL;
    dm_host_release_later(m->pTool); m->pTool = NULL;
}
/* 77309396: ProcessPMsg, called with the engine let go (the tool may call back into the performance) */
static int tops_process(void *ctx, dm_pmsg *e)
{
    rm_perf *p = ctx;
    DMUS_PMSG *m = pm_view(p, e);
    IDirectMusicTool *t = m ? m->pTool : NULL;
    if (!t) return DMUS_S_FREE;
    IDirectMusicTool_AddRef(t);
    dm_host_unlock(p->host);
    HRESULT hr = IDirectMusicTool_ProcessPMsg(t, (IDirectMusicPerformance *)&p->IDirectMusicPerformance8_iface, m);
    dm_host_lock(p->host);
    dm_host_release_later(t);
    if (hr == DMUS_S_REQUEUE && pmsg_hdr(m)) pm_from_com_loose(p, m, e);
    return hr;
}
static void tops_flush(void *ctx, dm_pmsg *e, rtime_t rt)
{
    rm_perf *p = ctx;
    DMUS_PMSG *m = pm_view(p, e);
    if (m && m->pTool) IDirectMusicTool_Flush(m->pTool, (IDirectMusicPerformance *)&p->IDirectMusicPerformance8_iface, m, rt);
}
static void tops_clone_ext(void *ctx, dm_pmsg *d, const dm_pmsg *s)
{
    DMUS_PMSG *sm = s->ext;
    ULONG n = sm->dwSize;
    rm_pmsghdr *sh = pmsg_hdr(sm);
    if (sh && sh->size > n) n = sh->size;
    DMUS_PMSG *m = pmsg_new(n);
    if (!m) return;
    memcpy(m, sm, sh ? sh->size : sm->dwSize);
    if (m->pTool) IDirectMusicTool_AddRef(m->pTool);
    if (m->pGraph) IDirectMusicGraph_AddRef(m->pGraph);
    if (m->punkUser) IUnknown_AddRef(m->punkUser);
    ((rm_pmsghdr *)m - 1)->e = d;
    d->ext = m;
}
static void tops_free_ext(void *ctx, dm_pmsg *e) { pmsg_retire(e->ext); }
static void *tops_state_graph(void *ctx, dm_segstate *ss)
{
    dm_segment *g = dm_segstate_segment(ss);
    rm_segment *s = g ? g->wrapper : NULL;
    IDirectMusicGraph *sg = s ? s->graph : NULL, *c = NULL;
    if (sg) rm_graph_clone(sg, &c);             /* 77311d79: through the graph's private Clone */
    return c;
}
static void tops_graph_release(void *ctx, void *g) { dm_host_release_later(g); }    /* (its tools are the application's) */
/* the parameter control track's objects: IMediaParams (by its vtable: GetParam, SetParam, AddEnvelope,
 * FlushEnvelope, SetTimeFormat) */
static const GUID RM_IID_IMediaParams = { 0x6d6cbb61, 0xa223, 0x44aa, { 0x84, 0x2f, 0xa2, 0xf0, 0x67, 0x50, 0xbe, 0x6e } };
typedef struct { LONGLONG start, end; float v0, v1; DWORD curve, flags; } rm_envseg;
typedef struct rm_mpvtbl {
    HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    HRESULT (WINAPI *GetParam)(void *, DWORD, float *);
    HRESULT (WINAPI *SetParam)(void *, DWORD, float);
    HRESULT (WINAPI *AddEnvelope)(void *, DWORD, DWORD, rm_envseg *);
    HRESULT (WINAPI *FlushEnvelope)(void *, DWORD, LONGLONG, LONGLONG);
    HRESULT (WINAPI *SetTimeFormat)(void *, GUID, DWORD);
} rm_mpvtbl;
#define MPV(mp) (*(const rm_mpvtbl **)(mp))
/* 77322b08: the state's GetObjectInPath for IMediaParams, then SetTimeFormat */
static void *tops_mp_get(void *ctx, dm_segstate *ss, uint32_t pch, uint32_t stage, uint32_t buffer, const dm_guid *obj,
                         uint32_t index, const dm_guid *fmt, uint32_t fmtdata)
{
    rm_perf *p = ctx;
    IUnknown *u = state_object(p, ss);
    if (!u) return NULL;
    void *mp = NULL;
    HRESULT hr = IDirectMusicSegmentState8_GetObjectInPath((IDirectMusicSegmentState8 *)u, pch, stage, buffer,
                                                           (REFGUID)obj, index, &RM_IID_IMediaParams, &mp);
    dm_host_release_later(u);
    if (FAILED(hr) || !mp) return NULL;
    GUID g;
    memcpy(&g, fmt, sizeof(g));
    if (FAILED(MPV(mp)->SetTimeFormat(mp, g, fmtdata))) { dm_host_release_later(mp); return NULL; }
    return mp;
}
static int tops_mp_add(void *ctx, void *mp, uint32_t param, int64_t start, int64_t end, float v0, float v1,
                       uint32_t curve, uint32_t flags)
{
    rm_envseg e = { start, end, v0, v1, curve, flags };
    return MPV(mp)->AddEnvelope(mp, param, 1, &e);
}
static int tops_mp_flush(void *ctx, void *mp, uint32_t param, int64_t start, int64_t end)
{
    return MPV(mp)->FlushEnvelope(mp, param, start, end);
}
/* 77316457: a state that leaves the performance lets its audiopath go (its object stays) */
static void tops_state_end(void *ctx, dm_segstate *ss)
{
    rm_state *s = *dm_segstate_wrapper(ss);
    if (s && s->path) { IDirectMusicAudioPath *a = s->path; s->path = NULL; dm_host_release_later(a); }
}
static void tops_mp_release(void *ctx, void *mp) { dm_host_release_later(mp); }    /* (an effect's IMediaParams) */
/* the script track's tool (dmscript's, part of the track: the track's references; 0040f8c2): ProcessPMsg calls an
 * event's routine, the other methods are not for a graph */
typedef struct { IDirectMusicTool IDirectMusicTool_iface; LONG ref; dm_track *t; } rm_scripttool;
static rm_scripttool *sct(IDirectMusicTool *i) { return CONTAINING_RECORD(i, rm_scripttool, IDirectMusicTool_iface); }
static HRESULT WINAPI sct_qi(IDirectMusicTool *i, REFIID r, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectMusicTool)) { *o = i; InterlockedIncrement(&sct(i)->ref); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI sct_addref(IDirectMusicTool *i) { return InterlockedIncrement(&sct(i)->ref); }
static ULONG WINAPI sct_release(IDirectMusicTool *i)
{
    rm_scripttool *x = sct(i);
    LONG n = InterlockedDecrement(&x->ref);
    if (!n) {
        dm_engine_lock();
        dm_track_release(x->t);
        dm_engine_unlock();
        free(x);
    }
    return n;
}
static HRESULT WINAPI sct_init(IDirectMusicTool *i, IDirectMusicGraph *g) { return E_UNEXPECTED; }
static HRESULT WINAPI sct_delivery(IDirectMusicTool *i, DWORD *d) { return E_UNEXPECTED; }
static HRESULT WINAPI sct_typesize(IDirectMusicTool *i, DWORD *n) { return E_UNEXPECTED; }
static HRESULT WINAPI sct_types(IDirectMusicTool *i, DWORD **t, DWORD n) { return E_UNEXPECTED; }
static HRESULT WINAPI sct_process(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m)
{
    if (!m || !m->punkUser) return E_POINTER;
    return rm_script_track_process(m->punkUser, m->dwVirtualTrackID, ((dm_scripttrack *)sct(i)->t->data)->errnotify);
}
static HRESULT WINAPI sct_flush(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m, REFERENCE_TIME rt) { return DMUS_S_FREE; }
static IDirectMusicToolVtbl sct_vtbl = { sct_qi, sct_addref, sct_release, sct_init, sct_delivery, sct_typesize, sct_types,
                                         sct_process, sct_flush };

/* 0040fb32: each event's script Init (an OLE engine starting runs with the performance let go: its code may call it) */
static void tops_script_init(void *ctx, dm_segstate *ss, dm_track *t, uint32_t vid)
{
    rm_perf *p = ctx;
    dm_scripttrack *T = t->data;
    IUnknown *st = state_object(p, ss);
    for (dm_scriptev *e = T->items; e; e = e->next) {
        if (!e->script) continue;
        IUnknown *sc = e->script;
        IUnknown_AddRef(sc);
        BOOL let = rm_script_init_blocks(sc);
        if (let) dm_host_unlock(p->host);
        rm_script_track_init(sc, (IDirectMusicPerformance *)&p->IDirectMusicPerformance8_iface, st, vid, T->errnotify);
        if (let) dm_host_lock(p->host);
        dm_host_release_later(sc);
        if (T != t->data) break;        /* (the track loaded again meanwhile) */
    }
    dm_host_release_later(st);
}
/* 0040fbe9: an event's message to the track's tool */
static void tops_script_event(void *ctx, dm_segstate *ss, dm_track *t, const dm_scriptev *e, mtime_t off, uint32_t vid)
{
    rm_perf *p = ctx;
    if (!e->script) return;
    rm_scripttool *x = calloc(1, sizeof(*x));
    if (!x) return;
    x->IDirectMusicTool_iface.lpVtbl = &sct_vtbl;
    x->ref = 1; x->t = t;
    dm_track_addref(t);
    IUnknown *st = state_object(p, ss);
    rm_script_track_event((IDirectMusicPerformance *)&p->IDirectMusicPerformance8_iface, st, &x->IDirectMusicTool_iface,
                          e->script, e->routine, e->flags, (MUSIC_TIME)(e->physical + off), vid);
    dm_host_release_later(st);
    sct_release(&x->IDirectMusicTool_iface);
}
static const dm_toolops s_toolops = { tops_stamp, tops_unstamp, tops_process, tops_flush, tops_clone_ext, tops_free_ext,
                                      tops_state_graph, tops_graph_release, tops_mp_get, tops_mp_add, tops_mp_flush,
                                      tops_mp_release, tops_state_end, tops_script_init, tops_script_event };

/* the StampPMsg of the performance's, a state's and an audiopath's graph interfaces on any message */
static HRESULT owner_stamp(rm_perf *p, DMUS_PMSG *m, int owner, void *obj)
{
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    BOOL temp;
    HRESULT hr;
    dm_pmsg *e = pm_engine(p, pf, m, FALSE, &temp, &hr);
    if (e) {
        hr = owner == DM_GRAPH_PERF ? dm_perf_stamp(pf, e) : owner == DM_GRAPH_STATE ? dm_segstate_stamp(obj, e) : dm_audiopath_stamp(obj, e);
        pm_to_com(p, e, m);
        if (temp) pm_untemp(pf, e);
    }
    dm_host_unlock(p->host);
    return hr;
}

static HRESULT WINAPI gx_insert(IDirectMusicGraph *i, IDirectMusicTool *t, DWORD *pch, DWORD n, LONG index) { return E_NOTIMPL; }
static HRESULT WINAPI gx_gettool(IDirectMusicGraph *i, DWORD index, IDirectMusicTool **t) { return E_NOTIMPL; }
static HRESULT WINAPI gx_remove(IDirectMusicGraph *i, IDirectMusicTool *t) { return E_NOTIMPL; }

/* the performance's graph interface (77307925): not counted (see self_iface) */
static rm_perf *pgr(IDirectMusicGraph *i) { return impl_from(i, rm_perf, IDirectMusicGraph_iface); }
static HRESULT WINAPI pf_qi(IDirectMusicPerformance8 *i, REFIID riid, void **out);
static HRESULT WINAPI pfg_qi(IDirectMusicGraph *i, REFIID r, void **o) { return pf_qi(&pgr(i)->IDirectMusicPerformance8_iface, r, o); }
static ULONG WINAPI pfg_addref(IDirectMusicGraph *i) { return 2; }
static ULONG WINAPI pfg_release(IDirectMusicGraph *i) { return 1; }
static HRESULT WINAPI pfg_stamp(IDirectMusicGraph *i, DMUS_PMSG *m)
{
    rm_perf *p = pgr(i);
    if (!m || IsBadWritePtr(m, sizeof(DWORD))) return E_POINTER;
    if (!p->host) {                             /* no engine yet: straight to the output tool */
        if (m->pGraph) { IDirectMusicGraph_Release(m->pGraph); m->pGraph = NULL; }
        if (m->pTool) IDirectMusicTool_Release(m->pTool);
        m->dwFlags = (m->dwFlags & ~(DMUS_PMSGF_TOOL_IMMEDIATE | DMUS_PMSGF_TOOL_ATTIME)) | DMUS_PMSGF_TOOL_QUEUE;
        m->pTool = &p->IDirectMusicTool_iface;
        return S_OK;
    }
    return owner_stamp(p, m, DM_GRAPH_PERF, NULL);
}
static IDirectMusicGraphVtbl pfg_vtbl = { pfg_qi, pfg_addref, pfg_release, pfg_stamp, gx_insert, gx_gettool, gx_remove };

/* the performance's output tool interface (0x77301290): not counted either */
static rm_perf *ptl(IDirectMusicTool *i) { return impl_from(i, rm_perf, IDirectMusicTool_iface); }
static HRESULT WINAPI pft_qi(IDirectMusicTool *i, REFIID r, void **o) { return pf_qi(&ptl(i)->IDirectMusicPerformance8_iface, r, o); }
static ULONG WINAPI pft_addref(IDirectMusicTool *i) { return 2; }
static ULONG WINAPI pft_release(IDirectMusicTool *i) { return 1; }
static HRESULT WINAPI pft_init(IDirectMusicTool *i, IDirectMusicGraph *g) { return E_NOTIMPL; }
static HRESULT WINAPI pft_deliv(IDirectMusicTool *i, DWORD *d)      /* 773078a3 */
{
    if (IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    *d = DMUS_PMSGF_TOOL_IMMEDIATE;
    return S_OK;
}
static HRESULT WINAPI pft_ntypes(IDirectMusicTool *i, DWORD *n)     /* 773078d2 */
{
    if (IsBadWritePtr(n, sizeof(*n))) return E_POINTER;
    *n = 0;
    return S_OK;
}
static HRESULT WINAPI pft_types(IDirectMusicTool *i, DWORD **t, DWORD n) { return E_NOTIMPL; }
static HRESULT WINAPI pft_process(IDirectMusicTool *i, IDirectMusicPerformance *perf, DMUS_PMSG *m)
{
    rm_perf *p = ptl(i);
    if (!m || IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    BOOL temp;
    HRESULT hr;
    dm_pmsg *e = pm_engine(p, pf, m, FALSE, &temp, &hr);
    if (e) {
        hr = dm_perf_output_process(pf, e);
        pm_to_com(p, e, m);
        if (temp) pm_untemp(pf, e);
    }
    dm_host_unlock(p->host);
    return hr;
}
static HRESULT WINAPI pft_flush(IDirectMusicTool *i, IDirectMusicPerformance *perf, DMUS_PMSG *m, REFERENCE_TIME rt)
{
    rm_perf *p = ptl(i);
    if (!m || IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    BOOL temp;
    HRESULT hr;
    dm_pmsg *e = pm_engine(p, pf, m, FALSE, &temp, &hr);
    if (e) {
        dm_perf_output_flush(pf, e, rt);
        if (temp) pm_untemp(pf, e);
    }
    dm_host_unlock(p->host);
    return hr;
}
static IDirectMusicToolVtbl pft_vtbl = { pft_qi, pft_addref, pft_release, pft_init, pft_deliv, pft_ntypes, pft_types, pft_process, pft_flush };

/* the COM object of a segment state (made on demand for a state the application never got, call with the lock) */
static IUnknown *state_object(rm_perf *p, dm_segstate *ss)
{
    void **w = dm_segstate_wrapper(ss);
    rm_state *s = *w;
    if (s && ref_if_alive(&s->ref)) return (IUnknown *)&s->IDirectMusicSegmentState8_iface;
    if (!(s = calloc(1, sizeof(*s)))) return NULL;      /* (one being let go of unhooks itself only if still *w) */
    s->IDirectMusicSegmentState8_iface.lpVtbl = &st_vtbl;
    s->IDirectMusicGraph_iface.lpVtbl = &stg_vtbl;
    s->ref = 1; s->ss = ss; s->lazy = TRUE;
    dm_segstate_addref(ss);
    dm_segment *g = dm_segstate_segment(ss);
    if (g && g->wrapper && ref_if_alive(&((rm_segment *)g->wrapper)->ref)) s->seg = g->wrapper;
    s->perf = p; IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    dm_audiopath *ap = dm_segstate_audiopath(ss);
    rm_path *a = ap ? *dm_audiopath_wrapper(ap) : NULL;
    if (a && ref_if_alive(&a->ref)) s->path = &a->IDirectMusicAudioPath_iface;
    *w = s;
    return (IUnknown *)&s->IDirectMusicSegmentState8_iface;
}
/* 77311e05: a state that never plays, on no segment, holding the performance without a reference (the one made by
 * Segment::InitPlay, here ss NULL); a segment once asked for IDirectMusicSegment2 / 8 has no InitPlay */
static HRESULT WINAPI seg_initplay(IDirectMusicSegment8 *i, IDirectMusicSegmentState **s, IDirectMusicPerformance *p, DWORD f)
{
    if (IsBadReadPtr(p, 4) || IsBadReadPtr(*(void **)p, 4) || IsBadCodePtr(**(FARPROC **)p) || IsBadWritePtr(s, sizeof(*s)))
        return E_POINTER;
    if (sgm(i)->ver) return E_NOTIMPL;
    rm_perf *pf = NULL;                         /* (dmime asks for its own performance's private interface) */
    IDirectMusicPerformance8 *q = (IDirectMusicPerformance8 *)p;
    if (q->lpVtbl == &pf_vtbl) pf = prf(q);
    else if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)p, &IID_IDirectMusicPerformance8, (void **)&q))) {
        IDirectMusicPerformance8_Release(q);
        if (q->lpVtbl == &pf_vtbl) pf = prf(q);
    }
    if (!pf) return E_FAIL;
    rm_state *st = calloc(1, sizeof(*st));
    if (!st) return E_OUTOFMEMORY;
    st->IDirectMusicSegmentState8_iface.lpVtbl = &st_vtbl;
    st->IDirectMusicGraph_iface.lpVtbl = &stg_vtbl;
    st->ref = 1;
    st->perf = pf;
    *s = (IDirectMusicSegmentState *)&st->IDirectMusicSegmentState8_iface;
    LOG_CALL("music: Segment::InitPlay(%p) -> %p", (void *)i, (void *)*s);
    return S_OK;
}
static rm_state *state_of(rm_perf *p, dm_segstate *ss)
{
    IUnknown *u = state_object(p, ss);
    return u ? sst((IDirectMusicSegmentState8 *)u) : NULL;
}
/* 77305a93 GetGraph, 77305b61 SetGraph: refused while not initialised (before Init, after CloseDown) only to a
 * DirectX 8 application; an older one's graph set before Init is the one Init starts with */
static HRESULT WINAPI pf_getgraph(IDirectMusicPerformance8 *i, IDirectMusicGraph **g)
{
    rm_perf *p = prf(i);
    if (IsBadWritePtr(g, sizeof(*g))) return E_POINTER;
    dm_engine_lock();                       /* (Init hands what was set before it to the engine under it) */
    IDirectMusicGraph *x = NULL;
    HRESULT hr;
    if ((!p->host || p->closed) && p->ver >= 8) hr = DMUS_E_NOT_INIT;
    else {
        x = p->host ? dm_perf_get_graph(dm_host_perf(p->host)) : p->pregraph;
        if (x) { *g = x; IDirectMusicGraph_AddRef(x); }
        hr = x ? S_OK : DMUS_E_NOT_FOUND;
    }
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI pf_setgraph(IDirectMusicPerformance8 *i, IDirectMusicGraph *g)
{
    rm_perf *p = prf(i);
    if (g && (IsBadReadPtr(g, 4) || IsBadReadPtr(*(void **)g, 4) || IsBadCodePtr(**(FARPROC **)g))) return E_POINTER;
    IDirectMusicGraph *old = NULL;
    dm_engine_lock();
    if ((!p->host || p->closed) && p->ver >= 8) { dm_engine_unlock(); return DMUS_E_NOT_INIT; }
    if (g) IDirectMusicGraph_AddRef(g);
    if (p->host) dm_perf_set_graph(dm_host_perf(p->host), g);
    else { old = p->pregraph; p->pregraph = g; }
    dm_engine_unlock();
    if (old) IDirectMusicGraph_Release(old);
    return S_OK;
}
/* 77305b0d: the performance's graph, made empty if it has none (GetObjectInPath's performance stages) */
static IDirectMusicGraph *perf_graph(rm_perf *p)
{
    if (!p->host) return NULL;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    IDirectMusicGraph *g = dm_perf_get_graph(pf);
    if (!g && SUCCEEDED(rm_graph_create(&IID_IDirectMusicGraph, (void **)&g))) dm_perf_set_graph(pf, g);
    if (g) IDirectMusicGraph_AddRef(g);
    dm_host_unlock(p->host);
    return g;
}
