/* reDM, included by dmcom.c: the shared reverb's DMO interface, the buffer manager and audiopaths. */
/* ------------------------------------------------------------------ the shared reverb's DMO interface */
typedef struct { IDirectSoundFXWavesReverb8 iface; LONG ref; rm_perf *perf; } rm_reverb;
static rm_reverb *rvb(IDirectSoundFXWavesReverb8 *i) { return impl_from(i, rm_reverb, iface); }
static HRESULT WINAPI rv_qi(IDirectSoundFXWavesReverb8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundFXWavesReverb8)) { *out = i; InterlockedIncrement(&rvb(i)->ref); return S_OK; }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI rv_addref(IDirectSoundFXWavesReverb8 *i) { return InterlockedIncrement(&rvb(i)->ref); }
static ULONG WINAPI rv_release(IDirectSoundFXWavesReverb8 *i)
{
    rm_reverb *r = rvb(i);
    LONG n = InterlockedDecrement(&r->ref);
    if (!n) { IDirectMusicPerformance8_Release(&r->perf->IDirectMusicPerformance8_iface); free(r); }
    return n;
}
static HRESULT WINAPI rv_set(IDirectSoundFXWavesReverb8 *i, LPCDSFXWavesReverb p)
{
    rm_reverb *r = rvb(i);
    if (!p) return E_POINTER;
    if (!r->perf->host || dm_host_reverb_set(r->perf->host, p->fInGain, p->fReverbMix, p->fReverbTime, p->fHighFreqRTRatio) < 0)
        return E_INVALIDARG;
    r->perf->revparams = *p;
    return S_OK;
}
static HRESULT WINAPI rv_get(IDirectSoundFXWavesReverb8 *i, LPDSFXWavesReverb p)
{
    if (!p) return E_POINTER;
    *p = rvb(i)->perf->revparams;
    return S_OK;
}
static IDirectSoundFXWavesReverbVtbl rv_vtbl = { rv_qi, rv_addref, rv_release, rv_set, rv_get };
static HRESULT reverb_object(rm_perf *p, REFIID iid, void **out)
{
    rm_reverb *r = calloc(1, sizeof(*r));
    if (!r) return E_OUTOFMEMORY;
    r->iface.lpVtbl = &rv_vtbl; r->ref = 1; r->perf = p;
    IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    HRESULT hr = rv_qi(&r->iface, iid, out);
    rv_release(&r->iface);
    return hr;
}

/* ------------------------------------------------------------------ the buffer manager */
static void buf_release(rm_perf *p, rm_buf *b)
{
    if (--b->refs > 0) return;
    for (rm_buf **q = &p->bufs; *q; q = &(*q)->next) if (*q == b) { *q = b->next; break; }
    if (p->host) dm_host_path_close(p->host, &b->host, 1);
    free(b);
}
static void path_free_config(rm_path *a)
{
    for (DWORD i = 0; i < a->nheld; i++) buf_release(a->perf, a->held[i]);
    for (DWORD k = 0; k < a->nports; k++) {
        for (DWORD m = 0; m < a->ports[k].nmaps; m++) free(a->ports[k].maps[m].b);
        free(a->ports[k].maps); free(a->ports[k].bufs);
    }
    free(a->ports); free(a->top); free(a->held);
    a->ports = NULL; a->top = NULL; a->held = NULL; a->nports = a->ntop = a->nheld = 0;
}
static BOOL hold(rm_path *a, rm_buf *b)
{
    rm_buf **n = realloc(a->held, (a->nheld + 1) * sizeof(*n));
    if (!n) return FALSE;
    a->held = n;
    a->held[a->nheld++] = b;
    return TRUE;
}

/* the standard buffers DirectMusic's configurations can name (dmime 773016a4..), and the effects of the defined ones */
static const GUID RM_GUID_Buffer_Reverb = { 0x186cc541, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID RM_GUID_Buffer_Stereo = { 0x186cc545, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID RM_DS3DALG_NO_VIRTUALIZATION = { 0xc241333f, 0x1c1b, 0x11d2, { 0x94, 0xf5, 0x00, 0xc0, 0x4f, 0xc2, 0x8a, 0xca } };
static const GUID RM_GUID_DSFX_WAVES = { 0x87fc0268, 0x9a55, 0x4360, { 0x95, 0xaa, 0x00, 0x4a, 0x1d, 0x9d, 0xe2, 0x6c } };
static const GUID RM_GUID_DSFX_I3DL2 = { 0xef985e71, 0xd5c7, 0x42d4, { 0xba, 0x4d, 0x2d, 0x07, 0x3e, 0x2e, 0x96, 0xf4 } };

/* 7731e70c (any) / 773208c7 (a shared one): a buffer by GUID; the standard path's shared stereo and reverb buffers
 * count as the manager's while a path holds them */
static rm_buf *buf_find(rm_perf *p, const GUID *g, BOOL shared)
{
    for (rm_buf *b = p->bufs; b; b = b->next)
        if ((!shared || (b->flags & DMUS_BUFFERF_SHARED)) && IsEqualGUID(&b->guid, g)) { b->refs++; return b; }
    int std = IsEqualGUID(g, &RM_GUID_Buffer_Stereo) ? 0 : IsEqualGUID(g, &RM_GUID_Buffer_Reverb) ? 1 : -1;
    if (std < 0 || !p->host || !dm_host_buffer_inuse(p->host, std)) return NULL;
    rm_buf *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->guid = *g; b->flags = DMUS_BUFFERF_SHARED | DMUS_BUFFERF_DEFINED; b->host = std; b->refs = 1;
    dm_host_buffer_addref(p->host, std);
    b->next = p->bufs; p->bufs = b;
    return b;
}

/* 7732084b: a configuration's buffer for a path (shared ones found again); the reference's flags are the
 * configuration's as they were, a buffer that turns out to be a mix-in marking the configuration's */
static HRESULT buf_make(rm_perf *p, apc_buffer *c, rm_bref *ref)
{
    memset(ref, 0, sizeof(*ref));
    if (!(p->audioflags & DMUS_AUDIOF_BUFFERS)) return (HRESULT)0x8878122a;
    ref->flags = c->flags;
    if ((c->flags & DMUS_BUFFERF_SHARED) && (ref->b = buf_find(p, &c->guid, TRUE))) return S_OK;
    dm_hostbuf d;
    memset(&d, 0, sizeof(d));
    d.guid = c->guid;
    DWORD func[2];
    rl_fxsrc fx;
    IUnknown *fxobj = NULL;
    if (c->flags & DMUS_BUFFERF_DEFINED) {
        int hasfx = 0;
        switch (c->type) {
        case 1: d.nch = 2; func[0] = DSBUSID_REVERB_SEND; d.nfunc = 1; d.flags = 0x480c0; hasfx = 1; fx.cls = RM_GUID_DSFX_WAVES; break;
        case 2:
            d.nch = 2; d.nfunc = 0; d.flags = 0xa0c0; hasfx = 1; fx.cls = RM_GUID_DSFX_I3DL2;
            break;
        case 4: d.nch = 1; func[0] = DSBUSID_DYNAMIC_0; d.nfunc = 1; d.flags = 0x680b0; d.alg = &RM_DS3DALG_NO_VIRTUALIZATION; break;
        case 6: d.nch = 1; func[0] = DSBUSID_LEFT; d.nfunc = 1; d.flags = 0x480e0; break;
        case 7: d.nch = 2; func[0] = DSBUSID_LEFT; func[1] = DSBUSID_RIGHT; d.nfunc = 2; d.flags = 0x480e0; break;
        case 8: d.nch = 2; func[0] = DSBUSID_LEFT; func[1] = DSBUSID_RIGHT; d.nfunc = 2; d.flags = 0x480c0; break;
        default: return E_INVALIDARG;
        }
        if ((c->flags & DMUS_BUFFERF_MIXIN) || !d.nfunc) {
            d.nfunc = 0;
            c->flags |= DMUS_BUFFERF_MIXIN;
            d.flags = (d.flags & ~0x40000u) | 0x2000;
        }
        d.flags |= DSBCAPS_CTRLFX;
        d.caps = d.flags | (d.flags & 0x40000 ? 0 : DSBCAPS_LOCSOFTWARE);
        d.func = func;
        if (hasfx) {
            memset(&fx.send, 0, sizeof(fx.send)); fx.level = 0;
            HRESULT hr = rl_fx_create_config(&fx.cls, &fxobj);
            if (FAILED(hr)) return hr;
            fx.obj = fxobj;
            d.nfx = 1; d.fx = &fx;
        }
    } else {
        c->flags &= ~DMUS_BUFFERF_MIXIN;
        if (!c->dsbc || !c->hasdesc) return E_INVALIDARG;
        d.nch = c->nch; d.vol = c->vol; d.pan = c->pan;
        d.nfunc = c->nbus > 2 ? 2 : c->nbus;
        d.func = c->bus;
        d.flags = c->dsflags;
        if (c->has3d) { d.alg = &c->alg; d.ds3d = &c->ds3d; }
        d.nfx = c->nfx; d.fx = c->fx;
        d.caps = (c->dsflags & ~0x40000u) | (d.nfunc ? 0 : 0x2000) | DSBCAPS_LOCSOFTWARE;
        /* (77320b9f marks it a mix-in again only if its first bus id is DSBUSID_NULL, which dsound never gives: a
         * configuration's DSBC mix-in is one only for the first path made from it) */
    }
    rm_buf *b = calloc(1, sizeof(*b));
    if (!b) { if (fxobj) IUnknown_Release(fxobj); return E_OUTOFMEMORY; }
    HRESULT hr = dm_host_buffer_new(p->host, &d, &b->host);
    if (fxobj) IUnknown_Release(fxobj);
    if (FAILED(hr)) { free(b); return hr; }
    b->guid = c->guid; b->flags = ref->flags; b->refs = 1;
    b->next = p->bufs; p->bufs = b;
    ref->b = b;
    char g[64];
    LOG_CALL("music: audiopath buffer %s (type %d, flags %lx) -> host buffer %d", rl_guid_str(&c->guid, g), c->type,
             (unsigned long)ref->flags, b->host);
    return S_OK;
}

/* ------------------------------------------------------------------ audiopaths */
static IDirectMusicAudioPathVtbl ap_vtbl;
static rm_path *apt(IDirectMusicAudioPath *i) { return impl_from(i, rm_path, IDirectMusicAudioPath_iface); }
static rm_path *unsafe_path(IUnknown *u)
{
    if (!u) return NULL;
    IDirectMusicAudioPath *a = (IDirectMusicAudioPath *)u;
    if (a->lpVtbl == &ap_vtbl) return apt(a);
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicAudioPath, (void **)&a))) {
        IDirectMusicAudioPath_Release(a);
        if (a->lpVtbl == &ap_vtbl) return apt(a);
    }
    return NULL;
}
static HRESULT WINAPI ap_qi(IDirectMusicAudioPath *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicAudioPath)) { *out = i; InterlockedIncrement(&apt(i)->ref); return S_OK; }
    if (IsEqualIID(riid, &IID_IDirectMusicGraph)) { *out = &apt(i)->IDirectMusicGraph_iface; InterlockedIncrement(&apt(i)->ref); return S_OK; }
    if (IsEqualIID(riid, &IID_IDispatch)) {
        rm_path *a = apt(i);
        if (!a->autod && !(a->autod = rm_auto_new((IUnknown *)&a->IDirectMusicAudioPath_iface, RM_AUTO_AUDIOPATH))) { *out = NULL; return E_OUTOFMEMORY; }
        *out = a->autod; InterlockedIncrement(&a->ref); return S_OK;
    }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI ap_addref(IDirectMusicAudioPath *i) { return InterlockedIncrement(&apt(i)->ref); }
static ULONG WINAPI ap_release(IDirectMusicAudioPath *i)
{
    rm_path *a = apt(i);
    LONG r = InterlockedDecrement(&a->ref);
    LOG_CALL("music: AudioPath::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        if (a->perf->host) {
            dm_host_lock(a->perf->host);
            void **w = dm_audiopath_wrapper(a->ap);
            if (*w == a) *w = NULL;
            dm_audiopath_release(a->ap);
            dm_host_unlock(a->perf->host);
            if (a->nbuf) dm_host_path_close(a->perf->host, a->buf, a->nbuf);
        }
        path_free_config(a);
        IDirectMusicPerformance8_Release(&a->perf->IDirectMusicPerformance8_iface);
        rm_auto_free(a->autod);
        free(a);
    }
    return r;
}
/* the port stage: the synth port, by GUID_All_Objects or its own GUID (index 0: there is one) */
static HRESULT port_object(rm_perf *p, REFGUID g, DWORD n, REFIID iid, void **o)
{
    if (n || !p->port || (!IsEqualGUID(g, &GUID_All_Objects) && !IsEqualGUID(g, &RM_GUID_SynthPort))) return DMUS_E_NOT_FOUND;
    return rm_port_query(p->port, iid, o);
}
/* a buffer's object at the buffer stages: the buffer (index 0), or its effects (IDirectSoundBuffer8::GetObjectInPath) */
static HRESULT buf_object(rm_path *a, rm_buf *b, DWORD stage, REFGUID g, DWORD n, REFIID iid, void **o)
{
    if (stage == DMUS_PATH_BUFFER || stage == DMUS_PATH_MIXIN_BUFFER)
        return n ? DMUS_E_NOT_FOUND : dm_host_buffer_query(a->perf->host, b->host, iid, o);
    if (b->host == 1 && n == 0 && (IsEqualGUID(g, &GUID_All_Objects) || IsEqualGUID(g, &GUID_DSFX_WAVES_REVERB)))
        return reverb_object(a->perf, iid, o);      /* the standard path's shared reverb */
    return dm_host_buffer_dmo(a->perf->host, b->host, g, n, iid, o);
}
/* 7731fbc1, the stages a configured path has of its own */
static HRESULT cfg_getobj(rm_path *a, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFIID iid, void **o)
{
    HRESULT hr = DMUS_E_NOT_FOUND;
    if (!a->perf->host) return hr;
    dm_host_lock(a->perf->host);
    if (stage == DMUS_PATH_PORT) {
        /* the port configurations whose PChannels include pch: one port (the synth) */
        for (DWORD k = 0; k < a->nports; k++) {
            rm_pcfg *pc = &a->ports[k];
            if (pch != DMUS_PCHANNEL_ALL && (pch < pc->base || pch >= pc->base + pc->count)) continue;
            hr = port_object(a->perf, g, n, iid, o);
            break;
        }
    } else if (stage == 0x5000) {
        if (!n && !pch && a->perf->port) hr = rm_port_sink(a->perf->port, iid, o);
    } else if (stage == DMUS_PATH_BUFFER || stage == DMUS_PATH_BUFFER_DMO) {
        /* the port configurations whose PChannels include pch (DMUS_PCHANNEL_ALL: all), then their maps; dwBuffer
         * counts the maps' buffers */
        for (DWORD k = 0; k < a->nports && hr < 0; k++) {
            rm_pcfg *pc = &a->ports[k];
            if (pch != DMUS_PCHANNEL_ALL && (pch < pc->base || pch >= pc->base + pc->count)) continue;
            DWORD idx = buf;
            for (DWORD m = 0; m < pc->nmaps; m++) {
                rm_pmap *pm = &pc->maps[m];
                if (pch != DMUS_PCHANNEL_ALL && (pch < pm->base || pch >= pm->base + pm->count)) continue;
                if (idx >= pm->nbuf) { idx -= pm->nbuf; continue; }
                if (pm->b[idx]) hr = buf_object(a, pm->b[idx], stage, g, n, iid, o);
                break;
            }
        }
    } else {
        /* the top-level mix-in buffers (only on PChannel 0) */
        if (pch == 0)
            for (DWORD i = 0, idx = buf; i < a->ntop; i++) {
                if (!(a->top[i].flags & DMUS_BUFFERF_MIXIN)) continue;
                if (idx--) continue;
                hr = buf_object(a, a->top[i].b, stage, g, n, iid, o);
                break;
            }
    }
    dm_host_unlock(a->perf->host);
    return hr;
}
static HRESULT ap_getobj_do(IDirectMusicAudioPath *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o);
static HRESULT WINAPI ap_getobj(IDirectMusicAudioPath *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    HRESULT hr = ap_getobj_do(i, pch, stage, buf, g, n, iid, o);
    char b1[64], b2[64];
    LOG_CALL("music: AudioPath::GetObjectInPath(pch %lx, stage %lx, buffer %lu, %s, %lu, %s) -> %08lx", (unsigned long)pch,
             (unsigned long)stage, (unsigned long)buf, g && !IsBadReadPtr(g, sizeof(GUID)) ? guidstr(g, b1) : "(null)",
             (unsigned long)n, iid && !IsBadReadPtr(iid, sizeof(GUID)) ? guidstr(iid, b2) : "(null)", (unsigned long)hr);
    return hr;
}
static HRESULT ap_getobj_do(IDirectMusicAudioPath *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    rm_path *a = apt(i);
    if (!o) return E_POINTER;
    *o = NULL;
    if (buf && (stage < DMUS_PATH_BUFFER || stage > 0x7fff)) return DMUS_E_NOT_FOUND;     /* 7731fbc1 */
    if (a->cfg && (stage == DMUS_PATH_PORT || stage == 0x5000 || stage == DMUS_PATH_BUFFER || stage == DMUS_PATH_BUFFER_DMO ||
                   stage == DMUS_PATH_MIXIN_BUFFER || stage == DMUS_PATH_MIXIN_BUFFER_DMO))
        return cfg_getobj(a, pch, stage, buf, g, n, iid, o);
    if (stage == DMUS_PATH_PERFORMANCE) return n || pch ? DMUS_E_NOT_FOUND : IDirectMusicPerformance8_QueryInterface(&a->perf->IDirectMusicPerformance8_iface, iid, o);
    if (stage == DMUS_PATH_AUDIOPATH) return n || pch ? DMUS_E_NOT_FOUND : ap_qi(i, iid, o);
    if (stage == DMUS_PATH_BUFFER) {
        /* the path's buffers for its PChannels (DMUS_PCHANNEL_ALL: any), dwBuffer counting them in order */
        if (n || !a->perf->host || (pch != DMUS_PCHANNEL_ALL && pch >= a->npch) || buf >= (DWORD)a->nbuf) return DMUS_E_NOT_FOUND;
        return dm_host_buffer_query(a->perf->host, a->buf[buf], iid, o);
    }
    if (stage == DMUS_PATH_PRIMARY_BUFFER) {
        if (n || pch || !a->perf->host) return DMUS_E_NOT_FOUND;
        return dm_host_primary_query(a->perf->host, iid, o);
    }
    if (stage == DMUS_PATH_BUFFER_DMO) {
        if (!a->perf->host || (pch != DMUS_PCHANNEL_ALL && pch >= a->npch) || buf >= (DWORD)a->nbuf) return DMUS_E_NOT_FOUND;
        if (a->buf[buf] == 1 && n == 0 && (IsEqualGUID(g, &GUID_All_Objects) || IsEqualGUID(g, &GUID_DSFX_WAVES_REVERB)))
            return reverb_object(a->perf, iid, o);
        return DMUS_E_NOT_FOUND;
    }
    if (stage == DMUS_PATH_AUDIOPATH_GRAPH || stage == DMUS_PATH_AUDIOPATH_TOOL) {
        /* its graph: made empty for the graph stage if it has none, not for the tool stage */
        if (stage == DMUS_PATH_AUDIOPATH_GRAPH && (n || pch)) return DMUS_E_NOT_FOUND;
        if (!a->perf->host) return DMUS_E_NOT_FOUND;
        dm_host_lock(a->perf->host);
        IDirectMusicGraph *gr = dm_audiopath_get_graph(a->ap);
        if (!gr && stage == DMUS_PATH_AUDIOPATH_GRAPH && SUCCEEDED(rm_graph_create(&IID_IDirectMusicGraph, (void **)&gr)))
            dm_audiopath_set_graph(a->ap, gr);
        if (gr) IDirectMusicGraph_AddRef(gr);
        dm_host_unlock(a->perf->host);
        if (!gr) return stage == DMUS_PATH_AUDIOPATH_GRAPH ? E_OUTOFMEMORY : DMUS_E_NOT_FOUND;
        HRESULT hr = stage == DMUS_PATH_AUDIOPATH_GRAPH ? IDirectMusicGraph_QueryInterface(gr, iid, o) : rm_graph_find_tool(gr, pch, g, n, iid, o);
        IDirectMusicGraph_Release(gr);
        return hr;
    }
    if (stage == DMUS_PATH_PERFORMANCE_GRAPH || stage == DMUS_PATH_PERFORMANCE_TOOL) {
        /* the performance's graph (made empty if it has none); its tools by the path's PChannel turned into the
         * performance's */
        if (stage == DMUS_PATH_PERFORMANCE_GRAPH && (n || pch)) return DMUS_E_NOT_FOUND;
        IDirectMusicGraph *gr = perf_graph(a->perf);
        if (!gr) return DMUS_E_NOT_FOUND;
        HRESULT hr;
        if (stage == DMUS_PATH_PERFORMANCE_GRAPH) hr = IDirectMusicGraph_QueryInterface(gr, iid, o);
        else {
            uint32_t r;
            if (dm_audiopath_convert(a->ap, pch, &r) >= 0) pch = r;
            hr = rm_graph_find_tool(gr, pch, g, n, iid, o);
        }
        IDirectMusicGraph_Release(gr);
        return hr;
    }
    if (stage == DMUS_PATH_PORT) {          /* the synth port, for the path's PChannels */
        uint32_t r;
        if (pch != DMUS_PCHANNEL_ALL && dm_audiopath_convert(a->ap, pch, &r) < 0) return DMUS_E_NOT_FOUND;
        return port_object(a->perf, g, n, iid, o);
    }
    if (stage == 0x5000) return n || pch || !a->perf->port ? DMUS_E_NOT_FOUND : rm_port_sink(a->perf->port, iid, o);
    switch (stage) {        /* stages an audiopath knows that are not here (mix-in buffers of a standard path) */
    case DMUS_PATH_MIXIN_BUFFER: case DMUS_PATH_MIXIN_BUFFER_DMO: {
        char b1[64];
        LOG_INFO("music: AudioPath::GetObjectInPath(stage %lx, %s) not supported", (unsigned long)stage, guidstr(iid, b1));
        return DMUS_E_NOT_FOUND;
    }
    }
    return E_INVALIDARG;
}
static HRESULT WINAPI ap_activate(IDirectMusicAudioPath *i, BOOL on)
{
    rm_path *a = apt(i);
    if (!a->perf->host) return DMUS_E_NOT_INIT;
    dm_host_lock(a->perf->host);
    int hr = dm_audiopath_activate(a->ap, on);
    dm_host_unlock(a->perf->host);
    return hr;
}
static HRESULT WINAPI ap_setvolume(IDirectMusicAudioPath *i, LONG vol, DWORD dur)
{
    rm_path *a = apt(i);
    if (!a->perf->host) return DMUS_E_NOT_INIT;
    dm_host_lock(a->perf->host);
    int hr = dm_audiopath_set_volume(a->ap, vol, dur);
    dm_host_unlock(a->perf->host);
    LOG_TRACE("music: AudioPath::SetVolume(%ld, %lu) -> %08x", vol, (unsigned long)dur, hr);
    return hr;
}
/* 7731eedb: the broadcast values (0xfffffff0 and up) as they are; a PChannel the path does not have: DMUS_E_NOT_FOUND */
static HRESULT WINAPI ap_convert(IDirectMusicAudioPath *i, DWORD in, DWORD *out)
{
    if (!out || IsBadWritePtr(out, 4)) return E_POINTER;
    if (in >= 0xfffffff0) { *out = in; return S_OK; }
    uint32_t r;
    if (dm_audiopath_convert(apt(i)->ap, in, &r) < 0) return DMUS_E_NOT_FOUND;
    *out = r;
    return S_OK;
}
static IDirectMusicAudioPathVtbl ap_vtbl = { ap_qi, ap_addref, ap_release, ap_getobj, ap_activate, ap_setvolume, ap_convert };

/* the path's graph interface (vtable 0x7730219c): StampPMsg 7731ef40 */
static rm_path *agr(IDirectMusicGraph *i) { return impl_from(i, rm_path, IDirectMusicGraph_iface); }
static HRESULT WINAPI apg_qi(IDirectMusicGraph *i, REFIID r, void **o) { return ap_qi(&agr(i)->IDirectMusicAudioPath_iface, r, o); }
static ULONG WINAPI apg_addref(IDirectMusicGraph *i) { return ap_addref(&agr(i)->IDirectMusicAudioPath_iface); }
static ULONG WINAPI apg_release(IDirectMusicGraph *i) { return ap_release(&agr(i)->IDirectMusicAudioPath_iface); }
static HRESULT WINAPI apg_stamp(IDirectMusicGraph *i, DMUS_PMSG *m)
{
    rm_path *a = agr(i);
    if (IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!a->perf->host) return DMUS_E_NOT_INIT;
    return owner_stamp(a->perf, m, DM_GRAPH_AUDIOPATH, a->ap);
}
static IDirectMusicGraphVtbl apg_vtbl = { apg_qi, apg_addref, apg_release, apg_stamp, gx_insert, gx_gettool, gx_remove };

/* takes over a reference on ap; the path's object unless it has one */
static rm_path *new_path(rm_perf *p, dm_audiopath *ap)
{
    rm_path *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->IDirectMusicAudioPath_iface.lpVtbl = &ap_vtbl;
    a->IDirectMusicGraph_iface.lpVtbl = &apg_vtbl;
    a->ref = 1;
    a->ap = ap;
    a->perf = p;
    void **w = dm_audiopath_wrapper(ap);
    if (!*w) *w = a;
    IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    return a;
}
