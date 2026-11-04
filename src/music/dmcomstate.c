/* reDM, included by dmcom.c: segment states. */
/* ------------------------------------------------------------------ segment states */
static IDirectMusicSegmentState8Vtbl st_vtbl;
static IDirectMusicGraphVtbl stg_vtbl;
static rm_state *sst(IDirectMusicSegmentState8 *i) { return impl_from(i, rm_state, IDirectMusicSegmentState8_iface); }
static rm_state *unsafe_state(IUnknown *u)
{
    if (!u) return NULL;
    IDirectMusicSegmentState8 *s = (IDirectMusicSegmentState8 *)u;
    if (s->lpVtbl == &st_vtbl) return sst(s);
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicSegmentState8, (void **)&s))) {
        IDirectMusicSegmentState8_Release(s);
        if (s->lpVtbl == &st_vtbl) return sst(s);
    }
    return NULL;
}
static HRESULT WINAPI st_qi(IDirectMusicSegmentState8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicSegmentState) ||
        IsEqualIID(riid, &IID_IDirectMusicSegmentState8)) { *out = i; InterlockedIncrement(&sst(i)->ref); return S_OK; }
    if (IsEqualIID(riid, &IID_IDirectMusicGraph)) { *out = &sst(i)->IDirectMusicGraph_iface; InterlockedIncrement(&sst(i)->ref); return S_OK; }
    if (IsEqualIID(riid, &IID_IDispatch)) {
        rm_state *s = sst(i);
        if (!s->autod && !(s->autod = rm_auto_new((IUnknown *)&s->IDirectMusicSegmentState8_iface, RM_AUTO_SEGMENTSTATE))) { *out = NULL; return E_OUTOFMEMORY; }
        *out = s->autod; InterlockedIncrement(&s->ref); return S_OK;
    }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI st_addref(IDirectMusicSegmentState8 *i) { return InterlockedIncrement(&sst(i)->ref); }
static ULONG WINAPI st_release(IDirectMusicSegmentState8 *i)
{
    rm_state *s = sst(i);
    LONG r = InterlockedDecrement(&s->ref);
    LOG_CALL("music: SegmentState::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        if (!s->ss) {                       /* (from Segment::InitPlay: holds nothing but its tracks' engine state) */
            if (s->bare) {
                dm_engine_lock();
                void **w = dm_segstate_wrapper(s->bare);
                if (*w == s) *w = NULL;
                dm_segstate_unref(s->bare);
                dm_engine_unlock();
            }
            rm_auto_free(s->autod); free(s); return 0;
        }
        if (s->perf->host) {
            dm_host_lock(s->perf->host);
            void **w = dm_segstate_wrapper(s->ss);
            if (*w == s) *w = NULL;
            if (s->lazy) dm_segstate_unref(s->ss); else dm_segstate_release(s->ss);
            while (s->xref-- > 0) dm_segstate_unref(s->ss);
            dm_host_unlock(s->perf->host);
        }
        if (s->seg) IDirectMusicSegment8_Release(&s->seg->IDirectMusicSegment8_iface);
        if (s->path) IDirectMusicAudioPath_Release(s->path);
        IDirectMusicPerformance8_Release(&s->perf->IDirectMusicPerformance8_iface);
        rm_auto_free(s->autod);
        free(s);
    }
    return r;
}
static HRESULT WINAPI st_getrepeats(IDirectMusicSegmentState8 *i, DWORD *r)
{
    if (!r) return E_POINTER;
    *r = 0;
    if (sst(i)->seg) SEG_GET(&sst(i)->seg->IDirectMusicSegment8_iface, repeats, *r);
    return S_OK;
}
static HRESULT WINAPI st_getsegment(IDirectMusicSegmentState8 *i, IDirectMusicSegment **s)
{
    if (!s) return E_POINTER;
    *s = NULL;
    rm_state *st = sst(i);
    if (!st->ss) return DMUS_E_NOT_FOUND;
    if (st->perf->host) dm_host_lock(st->perf->host);
    int gone = !dm_segstate_segment(st->ss);        /* (released when the state left the performance) */
    if (st->perf->host) dm_host_unlock(st->perf->host);
    if (!st->seg || gone) return DMUS_E_NOT_FOUND;
    *s = (IDirectMusicSegment *)&sst(i)->seg->IDirectMusicSegment8_iface;
    IDirectMusicSegment_AddRef(*s);
    return S_OK;
}
static HRESULT WINAPI st_getstart(IDirectMusicSegmentState8 *i, MUSIC_TIME *t)
{
    if (!t) return E_POINTER;
    if (!sst(i)->ss) { *t = -1; return S_OK; }
    dm_segstate_start_time(sst(i)->ss, (mtime_t *)t);
    return S_OK;
}
static HRESULT WINAPI st_getseek(IDirectMusicSegmentState8 *i, MUSIC_TIME *t)
{
    rm_state *s = sst(i);
    if (!t || IsBadWritePtr(t, sizeof(*t))) return E_POINTER;
    if (!s->ss) { *t = 0; return S_OK; }
    if (s->perf->host) dm_host_lock(s->perf->host);
    *t = dm_segstate_seek(s->ss);
    if (s->perf->host) dm_host_unlock(s->perf->host);
    return S_OK;
}
/* 77315e8c: where in the segment the state started (the segment's start point, moved on by DMUS_SEGF_ALIGN) */
static HRESULT WINAPI st_getstartpoint(IDirectMusicSegmentState8 *i, MUSIC_TIME *t)
{
    rm_state *s = sst(i);
    if (!t || IsBadWritePtr(t, sizeof(*t))) return E_POINTER;
    if (!s->ss) { *t = 0; return S_OK; }
    if (s->perf->host) dm_host_lock(s->perf->host);
    *t = dm_segstate_start_point(s->ss);
    if (s->perf->host) dm_host_unlock(s->perf->host);
    return S_OK;
}
static HRESULT WINAPI st_settrackcfg(IDirectMusicSegmentState8 *i, REFGUID g, DWORD gb, DWORD n, DWORD on, DWORD off)
{
    rm_state *s = sst(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!s->ss) return DMUS_E_TRACK_NOT_FOUND;
    if (s->perf->host) dm_host_lock(s->perf->host);
    HRESULT hr = dm_segstate_set_trackconfig(s->ss, (const dm_guid *)g, gb, n, on, off);
    if (s->perf->host) dm_host_unlock(s->perf->host);
    return hr;
}
/* 773161ad: the segment, its tracks (GUID_All_Objects: any class), the state's graph and its tools (the graph made
 * empty when it has none), the performance with its graph and tools; anything further down the path from the
 * state's audiopath */
static IDirectMusicGraph *perf_graph(rm_perf *p);
static IDirectMusicTrack8 *track_object(dm_track *t);
static HRESULT st_getobj_do(IDirectMusicSegmentState8 *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o);
static HRESULT WINAPI st_getobj(IDirectMusicSegmentState8 *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    HRESULT hr = st_getobj_do(i, pch, stage, buf, g, n, iid, o);
    char b1[64], b2[64];
    LOG_CALL("music: SegmentState::GetObjectInPath(pch %lx, stage %lx, buffer %lu, %s, %lu, %s) -> %08lx", (unsigned long)pch,
             (unsigned long)stage, (unsigned long)buf, g && !IsBadReadPtr(g, sizeof(GUID)) ? guidstr(g, b1) : "(null)",
             (unsigned long)n, iid && !IsBadReadPtr(iid, sizeof(GUID)) ? guidstr(iid, b2) : "(null)", (unsigned long)hr);
    return hr;
}
static HRESULT st_getobj_do(IDirectMusicSegmentState8 *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    rm_state *s = sst(i);
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    *o = NULL;
    if (!s->ss || (buf && (stage < DMUS_PATH_BUFFER || stage > 0x7fff))) return DMUS_E_NOT_FOUND;
    HRESULT hr = DMUS_E_NOT_FOUND;
    switch (stage) {
    case DMUS_PATH_SEGMENT:
        if (s->seg && !n && !pch) hr = IDirectMusicSegment8_QueryInterface(&s->seg->IDirectMusicSegment8_iface, iid, o);
        break;
    case DMUS_PATH_SEGMENT_TRACK: {
        if (pch || !g) break;
        dm_track *t = NULL;
        if (s->perf->host) dm_host_lock(s->perf->host);
        t = dm_segstate_track(s->ss, (const dm_guid *)g, n);
        IDirectMusicTrack8 *to = t ? track_object(t) : NULL;
        if (s->perf->host) dm_host_unlock(s->perf->host);
        if (to) { hr = IDirectMusicTrack8_QueryInterface(to, iid, o); IDirectMusicTrack8_Release(to); }
        break;
    }
    case DMUS_PATH_SEGMENT_GRAPH: case DMUS_PATH_SEGMENT_TOOL: {
        if (stage == DMUS_PATH_SEGMENT_GRAPH && (n || pch)) break;
        if (!s->perf->host) break;
        dm_host_lock(s->perf->host);
        IDirectMusicGraph *gr = dm_segstate_get_graph(s->ss);
        if (!gr && SUCCEEDED(rm_graph_create(&IID_IDirectMusicGraph, (void **)&gr))) dm_segstate_set_graph(s->ss, gr);
        if (gr) IDirectMusicGraph_AddRef(gr);
        dm_host_unlock(s->perf->host);
        if (!gr) { hr = E_OUTOFMEMORY; break; }
        hr = stage == DMUS_PATH_SEGMENT_GRAPH ? IDirectMusicGraph_QueryInterface(gr, iid, o) : rm_graph_find_tool(gr, pch, g, n, iid, o);
        IDirectMusicGraph_Release(gr);
        break;
    }
    case DMUS_PATH_PERFORMANCE_GRAPH: case DMUS_PATH_PERFORMANCE_TOOL: {
        if (stage == DMUS_PATH_PERFORMANCE_GRAPH && (n || pch)) break;
        IDirectMusicGraph *gr = perf_graph(s->perf);
        if (!gr) break;
        hr = stage == DMUS_PATH_PERFORMANCE_GRAPH ? IDirectMusicGraph_QueryInterface(gr, iid, o) : rm_graph_find_tool(gr, pch, g, n, iid, o);
        IDirectMusicGraph_Release(gr);
        break;
    }
    case DMUS_PATH_PERFORMANCE:
        if (!n && !pch) hr = IDirectMusicPerformance8_QueryInterface(&s->perf->IDirectMusicPerformance8_iface, iid, o);
        break;
    default:
        if (s->path) hr = IDirectMusicAudioPath_GetObjectInPath(s->path, pch, stage, buf, g, n, iid, o);
        break;
    }
    return hr;
}
static IDirectMusicSegmentState8Vtbl st_vtbl = { st_qi, st_addref, st_release, st_getrepeats, st_getsegment, st_getstart,
    st_getseek, st_getstartpoint, st_settrackcfg, st_getobj };

/* the state's graph interface (vtable 0x77301f1c): StampPMsg 77315fdd; the tool methods are not there */
static HRESULT owner_stamp(rm_perf *p, DMUS_PMSG *m, int owner, void *obj);
static HRESULT WINAPI pfg_stamp(IDirectMusicGraph *i, DMUS_PMSG *m);
static HRESULT WINAPI gx_insert(IDirectMusicGraph *i, IDirectMusicTool *t, DWORD *pch, DWORD n, LONG index);
static HRESULT WINAPI gx_gettool(IDirectMusicGraph *i, DWORD index, IDirectMusicTool **t);
static HRESULT WINAPI gx_remove(IDirectMusicGraph *i, IDirectMusicTool *t);
static rm_state *sgr(IDirectMusicGraph *i) { return impl_from(i, rm_state, IDirectMusicGraph_iface); }
static HRESULT WINAPI stg_qi(IDirectMusicGraph *i, REFIID r, void **o) { return st_qi(&sgr(i)->IDirectMusicSegmentState8_iface, r, o); }
static ULONG WINAPI stg_addref(IDirectMusicGraph *i) { return st_addref(&sgr(i)->IDirectMusicSegmentState8_iface); }
static ULONG WINAPI stg_release(IDirectMusicGraph *i) { return st_release(&sgr(i)->IDirectMusicSegmentState8_iface); }
static HRESULT WINAPI stg_stamp(IDirectMusicGraph *i, DMUS_PMSG *m)
{
    rm_state *s = sgr(i);
    if (!m || IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!s->ss) return pfg_stamp(&s->perf->IDirectMusicGraph_iface, m);    /* (InitPlay's: the performance's graph) */
    if (!s->perf->host) return DMUS_E_NOT_INIT;
    return owner_stamp(s->perf, m, DM_GRAPH_STATE, s->ss);
}
static IDirectMusicGraphVtbl stg_vtbl = { stg_qi, stg_addref, stg_release, stg_stamp, gx_insert, gx_gettool, gx_remove };
