/* reDM, included by dmcom.c: the performance object and the messages the application holds. */
/* ------------------------------------------------------------------ the performance */
static IDirectMusicPerformance8Vtbl pf_vtbl;
static rm_perf *prf(IDirectMusicPerformance8 *i) { return impl_from(i, rm_perf, IDirectMusicPerformance8_iface); }
static rm_perf *perf_from_unknown(IUnknown *u)
{
    if (!u) return NULL;
    if (*(void *const *)u == &pf_vtbl) return prf((IDirectMusicPerformance8 *)u);
    rm_path *a = unsafe_path(u);                /* (an audiopath stands for its performance) */
    if (a) return a->perf;
    IDirectMusicPerformance8 *p = rm_ours(u, &IID_IDirectMusicPerformance8, &pf_vtbl);
    return p ? prf(p) : NULL;
}

#define PERF_LOCKED(p, expr) do { if (!(p)->host) return DMUS_E_NOT_INIT; dm_host_lock((p)->host); expr; dm_host_unlock((p)->host); } while (0)
/* after CloseDown dmime has no master clock, no ports and no audiopaths: what needs one of them answers
 * DMUS_E_NO_MASTER_CLOCK, DMUS_E_NOT_INIT or DMUS_E_AUDIOPATHS_NOT_VALID (dx7test closed); reDM keeps its engine for an
 * Init again, so it answers so from p->closed */
#define CLOSED_RET(p, hr) do { if ((p)->closed) return (hr); } while (0)

static void perf_set_ver(rm_perf *p, LONG v)
{
    InterlockedExchange(&p->ver, v);
    if (p->host) dm_perf_set_dxver(dm_host_perf(p->host), (uint32_t)v);
}
/* 77303ed2: IDirectMusicPerformance8 makes it a DirectX 8 application's, IDirectMusicPerformance2 a DirectX 7 one's */
static HRESULT WINAPI pf_qi(IDirectMusicPerformance8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicPerformance) ||
        IsEqualIID(riid, &IID_IDirectMusicPerformance2) || IsEqualIID(riid, &IID_IDirectMusicPerformance8)) {
        if (IsEqualIID(riid, &IID_IDirectMusicPerformance8)) perf_set_ver(prf(i), 8);
        else if (IsEqualIID(riid, &IID_IDirectMusicPerformance2)) perf_set_ver(prf(i), 7);
        *out = i; InterlockedIncrement(&prf(i)->ref); return S_OK;
    }
    /* 77303eb4: the output tool and the graph (+4, +8).  reAL does not count these two (pft_addref / pfg_addref), so
     * handing one out takes no reference either: their Release would never give it back. */
    if (IsEqualIID(riid, &IID_IDirectMusicTool)) { *out = &prf(i)->IDirectMusicTool_iface; return S_OK; }
    if (IsEqualIID(riid, &IID_IDirectMusicGraph)) { *out = &prf(i)->IDirectMusicGraph_iface; return S_OK; }
    if (IsEqualIID(riid, &IID_IDispatch)) {
        rm_perf *p = prf(i);
        if (!p->autod && !(p->autod = rm_auto_new((IUnknown *)&p->IDirectMusicPerformance8_iface, RM_AUTO_PERFORMANCE))) { *out = NULL; return E_OUTOFMEMORY; }
        *out = p->autod; InterlockedIncrement(&p->ref); return S_OK;
    }
    char b[64];
    LOG_INFO("music: Performance::QueryInterface(%s): no", guidstr(riid, b));
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI pf_addref(IDirectMusicPerformance8 *i) { return InterlockedIncrement(&prf(i)->ref); }
static void pf_closedown(rm_perf *p)
{
    p->apstate = 0;                         /* (77308c70: not set up, whether closed down already or not) */
    BOOL again = p->closed;
    p->closed = TRUE;
    p->hadport = FALSE;
    if (p->defpath) {                   /* (taken off under the lock, as SetDefaultAudioPath) */
        if (p->host) dm_host_lock(p->host);
        rm_path *a = p->defpath; p->defpath = NULL;
        if (p->host) dm_perf_set_default_audiopath(dm_host_perf(p->host), NULL);    /* (the application may hold it yet) */
        if (p->host) dm_host_unlock(p->host);
        if (a) IDirectMusicAudioPath_Release(&a->IDirectMusicAudioPath_iface);
    }
    if (p->host) dm_host_lock(p->host);
    if (p->host) {
        dm_perf *pf = dm_host_perf(p->host);
        if (!again) {
            dm_perf_stop(pf, NULL, NULL, 0, 0);
            dm_perf_drop_app_messages(pf);
            dm_perf_end_states(pf);
        }
        dm_perf_drop_all_messages(pf);      /* (again: what was asked for while closed goes too) */
        dm_perf_set_graph(pf, NULL);
        /* (77308c70 lets every port go: none is added any more) */
        for (int k = 0; k < 8; k++)
            if (p->slot[k] && p->added[k]) { p->added[k] = 0; dm_host_port_enable(p->host, k, k ? rm_port_own_host(p->slot[k]) : p->host, 0); }
    }
    /* (and the global parameters: GetGlobalParam answers E_INVALIDARG until one is set again) */
    EnterCriticalSection(&p->gcs);
    while (p->globals) { struct rm_global *e = p->globals; p->globals = e->next; free(e->data); free(e); }
    p->hasmastervol = FALSE;
    LeaveCriticalSection(&p->gcs);
    if (p->host) dm_host_unlock(p->host);
}
static void pf_teardown(rm_perf *p)
{
    dm_engine_lock();
    for (rm_perf **q = &s_perfs; *q; q = &(*q)->pnext) if (*q == p) { *q = p->pnext; break; }
    dm_engine_unlock();
    rm_port_detach(p->port);
    rm_port_release(p->port);
    dm_host_free(p->host);
    for (int k = 1; k < 8; k++) rm_port_release(p->slot[k]);  /* (CreatePort'ed: their synths may go now) */
    if (p->dm) IDirectMusic8_Release(p->dm);
    if (p->pregraph) IDirectMusicGraph_Release(p->pregraph);
    rm_auto_free(p->autod);
    while (p->globals) { struct rm_global *e = p->globals; p->globals = e->next; free(e->data); free(e); }
    DeleteCriticalSection(&p->gcs);
    free(p);
}
static DWORD WINAPI pf_reaper(LPVOID p) { pf_teardown(p); return 0; }
static ULONG WINAPI pf_release(IDirectMusicPerformance8 *i)
{
    rm_perf *p = prf(i);
    LONG r = InterlockedDecrement(&p->ref);
    LOG_CALL("music: Performance::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        p->ref = 1;     /* guard against re-entry from children */
        pf_closedown(p);
        /* reAL: the last reference let go of on one of the performance's own threads (a segment state's release the
         * engine deferred until its lock was let go): that thread cannot wait for itself to end, and it is still
         * inside the engine (an application tool's turn), so a thread of its own does the rest */
        if (p->host && dm_host_own_thread(p->host)) {
            HANDLE t = CreateThread(NULL, 0, pf_reaper, p, 0, NULL);
            if (t) CloseHandle(t);
            else LOG_INFO("music: performance %p released on its own thread, no thread to free it; not freed", (void *)p);
            return 0;
        }
        pf_teardown(p);
    }
    return r;
}

static const dm_toolops s_toolops;
static HRESULT WINAPI pf_setglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz);
/* 77303d4d, on Init (the engine lock held): the version; a DirectX 8 application's prepare time and bumper length back
 * to 1000 and 50 ms, an older one's as it set them (before Init, or before CloseDown); the graph set before Init */
static void perf_init_settings(rm_perf *p, BOOL again)
{
    dm_perf *pf = dm_host_perf(p->host);
    dm_perf_set_dxver(pf, (uint32_t)p->ver);
    if (p->ver >= 8) { dm_perf_set_prepare_time(pf, 1000); dm_perf_set_bumper_length(pf, 50); }
    else if (!again) { dm_perf_set_prepare_time(pf, p->prepare_ms); dm_perf_set_bumper_length(pf, p->bumper_ms); }
    if (p->pregraph) { dm_perf_set_graph(pf, p->pregraph); p->pregraph = NULL; }
}
/* audio: InitAudio's, which fails with DirectSound's own error when there is no output device (dmime's with
 * DirectSoundCreate's DSERR_NODRIVER) */
static HRESULT ensure_host(rm_perf *p, uint32_t rate, uint32_t voices, BOOL audio)
{
    if (p->host) return S_OK;
    p->closed = FALSE;                      /* (a CloseDown before Init) */
    p->mastervol = 0; p->hasmastervol = FALSE;    /* (77303d4d: set before Init, it is kept to read and not applied) */
    long why = 0;
    p->host = dm_host_new_why(rate ? rate : 22050, voices ? voices : 64, 64, 2, &why);
    if (!p->host) return audio && why == DSERR_NODRIVER ? DSERR_NODRIVER : E_OUTOFMEMORY;
    p->port = rm_port_new(p->host);
    p->slot[0] = p->port;
    p->added[0] = 1;                        /* (InitAudio's; Init's is not, until AddPort) */
    p->revparams.fInGain = 0; p->revparams.fReverbMix = 0; p->revparams.fReverbTime = 1000; p->revparams.fHighFreqRTRatio = 0.001f;
    dm_host_lock(p->host);
    rm_perf **q = &s_perfs;
    while (*q) q = &(*q)->pnext;
    *q = p;
    dm_perf_set_toolops(dm_host_perf(p->host), &s_toolops, p);
    perf_init_settings(p, FALSE);
    for (struct rm_global *e = p->globals; e; e = e->next)        /* (set before the performance was) */
        if (IsEqualGUID(&e->g, &G_PerfMasterGrooveLevel) && e->size >= 1) dm_perf_set_groove(dm_host_perf(p->host), *(char *)e->data);
    dm_host_unlock(p->host);
    BOOL off = FALSE;                       /* 77303d4d: automatic download off */
    pf_setglobal(&p->IDirectMusicPerformance8_iface, &GUID_PerfAutoDownload, &off, 4);
    return S_OK;
}

/* Init or InitAudio again after CloseDown, as dmime allows (its CloseDown leaves it uninitialised): reAL keeps the
 * engine (its synth and sink) and starts it over as Init would.  S_FALSE when there is none yet. */
static HRESULT perf_reopen(rm_perf *p)
{
    if (!p->host) return S_FALSE;
    dm_host_lock(p->host);
    p->closed = FALSE;
    p->mastervol = 0; p->hasmastervol = FALSE;    /* (77303d4d: the master volume back to 0, whatever was set meanwhile) */
    dm_host_set_volume(p->host, 0);
    dm_perf_reopen(dm_host_perf(p->host));
    perf_init_settings(p, TRUE);
    for (struct rm_global *e = p->globals; e; e = e->next)        /* (set after CloseDown) */
        if (IsEqualGUID(&e->g, &G_PerfMasterGrooveLevel) && e->size >= 1) dm_perf_set_groove(dm_host_perf(p->host), *(char *)e->data);
    dm_perf_transport(dm_host_perf(p->host));       /* (dmime's thread starts with a pass; this one may not come round in time) */
    dm_host_unlock(p->host);
    BOOL off = FALSE;                       /* 77303d4d: automatic download off */
    pf_setglobal(&p->IDirectMusicPerformance8_iface, &GUID_PerfAutoDownload, &off, 4);
    return S_OK;
}

static void hand_out_dmusic(rm_perf *p, IDirectMusic **pdm)
{
    if (!p->dm) p->dm = new_dmusic();
    if (pdm && !*pdm && p->dm) { *pdm = (IDirectMusic *)p->dm; IDirectMusic8_AddRef(p->dm); }
}

static HRESULT WINAPI pf_init(IDirectMusicPerformance8 *i, IDirectMusic **pdm, LPDIRECTSOUND ds, HWND w)
{
    rm_perf *p = prf(i);
    LOG_CALL("music: Performance::Init");
    if (p->host && !p->closed) return DMUS_E_ALREADY_INITED;
    HRESULT hr = perf_reopen(p);
    if (hr == S_FALSE) hr = ensure_host(p, 22050, 64, FALSE);
    if (FAILED(hr)) return hr;
    dm_host_lock(p->host);
    p->added[0] = 0;                        /* (the DirectX 6 way: AddPort, then AssignPChannelBlock) */
    dm_host_port_enable(p->host, 0, p->host, 0);   /* (no port: no latency, nothing played, until AddPort) */
    dm_host_set_dx7port(p->host, 1);        /* (its AddPort(NULL) port: DMUS_PORTPARAMS, no waves) */
    dm_host_unlock(p->host);
    p->apstate = 1;
    hand_out_dmusic(p, pdm);
    return S_OK;
}
static HRESULT pf_play(rm_perf *p, IUnknown *src, DWORD flags, __int64 start, IDirectMusicSegmentState **out, IUnknown *from, IUnknown *path);
static HRESULT WINAPI pf_playsegment(IDirectMusicPerformance8 *i, IDirectMusicSegment *s, DWORD f, __int64 t, IDirectMusicSegmentState **o)
{
    return pf_play(prf(i), (IUnknown *)s, f, t, o, NULL, NULL);
}
static HRESULT WINAPI pf_stop(IDirectMusicPerformance8 *i, IDirectMusicSegment *s, IDirectMusicSegmentState *st, MUSIC_TIME t, DWORD f)
{
    rm_perf *p = prf(i);
    rm_segment *g = unsafe_segment((IUnknown *)s);
    rm_state *ss = unsafe_state((IUnknown *)st);
    if ((s && !g) || (st && !ss)) return E_INVALIDARG;
    if (ss && !ss->ss && !g) return S_OK;          /* (Segment::InitPlay's: never played) */
    int hr;
    PERF_LOCKED(p, hr = dm_perf_stop(dm_host_perf(p->host), g ? g->seg : NULL, ss ? ss->ss : NULL, t, f));
    LOG_CALL("music: Performance::Stop(%p, %p, %ld) -> %08x", (void *)s, (void *)st, (long)t, hr);
    return hr;
}
static IUnknown *state_object(rm_perf *p, dm_segstate *ss);
/* 77304d68: the primary segment state at music time t */
static HRESULT WINAPI pf_getsegstate(IDirectMusicPerformance8 *i, IDirectMusicSegmentState **o, MUSIC_TIME t)
{
    rm_perf *p = prf(i);
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_FOUND;
    IUnknown *u = NULL;
    dm_host_lock(p->host);
    dm_segstate *ss = dm_perf_get_segstate(dm_host_perf(p->host), t);
    if (ss) u = state_object(p, ss);
    dm_host_unlock(p->host);
    if (!u) return DMUS_E_NOT_FOUND;
    *o = (IDirectMusicSegmentState *)u;
    return S_OK;
}
/* these work before Init too (dmime keeps them in the performance; a DirectX 8 application's Init sets them back) */
static HRESULT perf_timing(rm_perf *p, int bumper, const DWORD *set, DWORD *get)
{
    dm_engine_lock();                       /* (Init hands what was set before it to the engine under it) */
    if (!p->host) {
        DWORD *v = bumper ? &p->bumper_ms : &p->prepare_ms;
        if (set) *v = *set;
        else *get = *v;
    } else {
        dm_perf *pf = dm_host_perf(p->host);
        if (set) { if (bumper) dm_perf_set_bumper_length(pf, *set); else dm_perf_set_prepare_time(pf, *set); }
        else *get = bumper ? dm_perf_get_bumper_length(pf) : dm_perf_get_prepare_time(pf);
    }
    dm_engine_unlock();
    return S_OK;
}
static HRESULT WINAPI pf_setprepare(IDirectMusicPerformance8 *i, DWORD ms) { return perf_timing(prf(i), 0, &ms, NULL); }
static HRESULT WINAPI pf_getprepare(IDirectMusicPerformance8 *i, DWORD *ms) { if (!ms) return E_POINTER; return perf_timing(prf(i), 0, NULL, ms); }
static HRESULT WINAPI pf_setbumper(IDirectMusicPerformance8 *i, DWORD ms) { return perf_timing(prf(i), 1, &ms, NULL); }
static HRESULT WINAPI pf_getbumper(IDirectMusicPerformance8 *i, DWORD *ms) { if (!ms) return E_POINTER; return perf_timing(prf(i), 1, NULL, ms); }
static HRESULT WINAPI pf_sendpmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *m);
static HRESULT WINAPI pf_m2r(IDirectMusicPerformance8 *i, MUSIC_TIME mt, REFERENCE_TIME *rt) { rm_perf *p = prf(i); if (!rt) return E_POINTER; CLOSED_RET(p, DMUS_E_NO_MASTER_CLOCK); PERF_LOCKED(p, dm_perf_m2r(dm_host_perf(p->host), mt, rt)); return S_OK; }
static HRESULT WINAPI pf_r2m(IDirectMusicPerformance8 *i, REFERENCE_TIME rt, MUSIC_TIME *mt) { rm_perf *p = prf(i); if (!mt) return E_POINTER; CLOSED_RET(p, DMUS_E_NO_MASTER_CLOCK); PERF_LOCKED(p, dm_perf_r2m(dm_host_perf(p->host), rt, (mtime_t *)mt)); return S_OK; }
static HRESULT WINAPI pf_isplaying(IDirectMusicPerformance8 *i, IDirectMusicSegment *s, IDirectMusicSegmentState *st)
{
    rm_perf *p = prf(i);
    rm_segment *g = unsafe_segment((IUnknown *)s);
    rm_state *ss = unsafe_state((IUnknown *)st);
    if (!g && !ss) return s || st ? S_FALSE : E_POINTER;
    CLOSED_RET(p, DMUS_E_NO_MASTER_CLOCK);
    if (!g && !ss->ss) return S_FALSE;              /* (Segment::InitPlay's: never played) */
    int hr;
    PERF_LOCKED(p, hr = dm_perf_is_playing(dm_host_perf(p->host), g ? g->seg : NULL, ss ? ss->ss : NULL));
    return hr;
}
static HRESULT WINAPI pf_gettime(IDirectMusicPerformance8 *i, REFERENCE_TIME *rt, MUSIC_TIME *mt)
{
    rm_perf *p = prf(i);
    CLOSED_RET(p, DMUS_E_NO_MASTER_CLOCK);
    PERF_LOCKED(p, dm_perf_get_time(dm_host_perf(p->host), rt, (mtime_t *)mt));
    return S_OK;
}
/* ------------------------------------------------------------------ messages the application holds
 * Every DMUS_PMSG the application gets from us is a block with a private header in front.  A message the engine
 * handed out (to a tool, or as a notification) is the engine message's application view: the header names the
 * engine message, which names the view (dm_pmsg.ext).  The view is current while the application has the message
 * (pm_to_com when it is handed out), the engine's fields while the engine has it (pm_from_com when it comes back:
 * SendPMsg, StampPMsg, a tool's DMUS_S_REQUEUE).  An AllocPMsg / ClonePMsg block gets its engine message when it
 * is first sent or stamped.  FreePMsg releases the interfaces in it (pTool, pGraph, punkUser, as dmime's FreePMsg). */
#define RM_PMSG_MAGIC 0x6d50526dUL
typedef struct { DWORD magic, size; dm_pmsg *e; DWORD pad; } rm_pmsghdr;
static DMUS_PMSG *pmsg_new(ULONG cb)
{
    if (cb > 0x7fffffff - sizeof(rm_pmsghdr)) return NULL;     /* (a size that wraps: out of memory) */
    rm_pmsghdr *h = calloc(1, sizeof(*h) + cb);
    if (!h) return NULL;
    h->magic = RM_PMSG_MAGIC; h->size = cb;
    DMUS_PMSG *m = (DMUS_PMSG *)(h + 1);
    m->dwSize = cb;
    return m;
}
static rm_pmsghdr *pmsg_hdr(DMUS_PMSG *m)
{
    if (!m) return NULL;
    rm_pmsghdr *h = (rm_pmsghdr *)m - 1;
    return IsBadReadPtr(h, sizeof(*h)) || h->magic != RM_PMSG_MAGIC ? NULL : h;
}
static HRESULT WINAPI pf_allocpmsg(IDirectMusicPerformance8 *i, ULONG cb, DMUS_PMSG **m)
{
    if (!m) return E_POINTER;
    *m = NULL;
    if (cb < sizeof(DMUS_PMSG)) return E_INVALIDARG;
    return (*m = pmsg_new(cb)) ? S_OK : E_OUTOFMEMORY;
}
/* the performance's own tool and graph interfaces are not counted (a message naming them would keep the
 * performance alive): pft_addref / pfg_addref */
static void pmsg_drop_ifaces(DMUS_PMSG *m)
{
    /* (on the engine's threads this runs under the performance's lock: the objects go once it is let go, as an
     * application tool or a script event's script may call the loader or wait for the scripts' thread) */
    dm_host_release_later(m->pTool);
    dm_host_release_later(m->pGraph);
    dm_host_release_later(m->punkUser);
    m->pTool = NULL; m->pGraph = NULL; m->punkUser = NULL;
}

/* a freed message's memory is kept a while (marked: SendPMsg / FreePMsg on it fail as on a queued one), so an
 * application that still touches it right after sending does not write into freed memory */
static DMUS_PMSG *s_retired[256];
static LONG s_retpos;
static void pmsg_retire(DMUS_PMSG *m)
{
    rm_pmsghdr *h = (rm_pmsghdr *)m - 1;
    pmsg_drop_ifaces(m);
    h->magic = 0; h->e = NULL;
    LONG k = InterlockedIncrement(&s_retpos) & 255;
    DMUS_PMSG *old = (DMUS_PMSG *)InterlockedExchangePointer((void **)&s_retired[k], m);
    if (old) free((rm_pmsghdr *)old - 1);
}

static ULONG type_size(DWORD t)
{
    switch (t) {
    case DMUS_PMSGT_MIDI: return sizeof(DMUS_MIDI_PMSG);
    case DMUS_PMSGT_NOTE: return sizeof(DMUS_NOTE_PMSG);
    case DMUS_PMSGT_SYSEX: return sizeof(DMUS_SYSEX_PMSG);
    case DMUS_PMSGT_NOTIFICATION: return sizeof(DMUS_NOTIFICATION_PMSG);
    case DMUS_PMSGT_TEMPO: return sizeof(DMUS_TEMPO_PMSG);
    case DMUS_PMSGT_CURVE: return sizeof(DMUS_CURVE_PMSG);
    case DMUS_PMSGT_TIMESIG: return sizeof(DMUS_TIMESIG_PMSG);
    case DMUS_PMSGT_PATCH: return sizeof(DMUS_PATCH_PMSG);
    case DMUS_PMSGT_TRANSPOSE: return sizeof(DMUS_TRANSPOSE_PMSG);
    case DMUS_PMSGT_CHANNEL_PRIORITY: return sizeof(DMUS_CHANNEL_PRIORITY_PMSG);
    case DMUS_PMSGT_WAVE: return sizeof(DMUS_WAVE_PMSG);
    case DMUS_PMSGT_LYRIC: return sizeof(DMUS_LYRIC_PMSG);
    default: return sizeof(DMUS_PMSG);
    }
}

static IUnknown *state_object(rm_perf *p, dm_segstate *ss);

/* the engine's fields into the application's view (its type's fields if it is large enough for them) */
static void pm_to_com(rm_perf *p, const dm_pmsg *e, DMUS_PMSG *m)
{
    rm_pmsghdr *hh = pmsg_hdr(m);
    ULONG cap = hh ? hh->size : m->dwSize;
    m->dwSize = e->size;
    m->rtTime = e->rt; m->mtTime = e->mt; m->dwFlags = e->flags; m->dwPChannel = e->pch; m->dwVirtualTrackID = e->vpch;
    m->dwType = e->type; m->dwVoiceID = e->voice; m->dwGroupID = e->group;
    if (e->tool == 1) {
        if (m->pTool != &p->IDirectMusicTool_iface) {
            if (m->pTool) dm_host_release_later(m->pTool);    /* (an application's tool: not released under the lock) */
            m->pTool = &p->IDirectMusicTool_iface;
        }
    } else if (e->tool == 0 && m->pTool) { dm_host_release_later(m->pTool); m->pTool = NULL; }
    if (!m->punkUser && e->user && e->userref) m->punkUser = state_object(p, e->user);
    if (cap < type_size(e->type) && e->type != DMUS_PMSGT_SYSEX) return;
    switch (e->type) {
    case DMUS_PMSGT_MIDI: { DMUS_MIDI_PMSG *x = (DMUS_MIDI_PMSG *)m; x->bStatus = e->m_status; x->bByte1 = e->m_b1; x->bByte2 = e->m_b2; break; }
    case DMUS_PMSGT_NOTE: {
        DMUS_NOTE_PMSG *x = (DMUS_NOTE_PMSG *)m;
        x->mtDuration = e->n_dur; x->wMusicValue = e->n_musicval; x->wMeasure = e->n_measure; x->nOffset = e->n_offset;
        x->bBeat = e->n_beat; x->bGrid = e->n_grid; x->bVelocity = e->n_vel; x->bFlags = e->n_flags; x->bTimeRange = e->n_timerange;
        x->bDurRange = e->n_durrange; x->bVelRange = e->n_velrange; x->bPlayModeFlags = e->n_playmode; x->bSubChordLevel = e->n_subchord;
        x->bMidiValue = e->n_midival; x->cTranspose = e->n_transpose;
        break;
    }
    case DMUS_PMSGT_LYRIC: {           /* the text a lyrics track made (one the application made stays in its view) */
        DMUS_LYRIC_PMSG *x = (DMUS_LYRIC_PMSG *)m;
        if (e->sx_data && FIELD_OFFSET(DMUS_LYRIC_PMSG, wszString) + e->sx_len <= cap) memcpy(x->wszString, e->sx_data, e->sx_len);
        break;
    }
    case DMUS_PMSGT_SYSEX: {
        DMUS_SYSEX_PMSG *x = (DMUS_SYSEX_PMSG *)m;
        if (cap < FIELD_OFFSET(DMUS_SYSEX_PMSG, abData)) break;
        x->dwLen = e->sx_len;
        if (e->sx_data && FIELD_OFFSET(DMUS_SYSEX_PMSG, abData) + e->sx_len <= cap) memcpy(x->abData, e->sx_data, e->sx_len);
        break;
    }
    case DMUS_PMSGT_NOTIFICATION: {
        DMUS_NOTIFICATION_PMSG *x = (DMUS_NOTIFICATION_PMSG *)m;
        memcpy(&x->guidNotificationType, &e->nf_guid, sizeof(GUID));
        x->dwNotificationOption = e->nf_option; x->dwField1 = e->nf_field1; x->dwField2 = e->nf_field2;
        break;
    }
    case DMUS_PMSGT_TEMPO: ((DMUS_TEMPO_PMSG *)m)->dblTempo = e->tempo; break;
    case DMUS_PMSGT_CURVE: {
        DMUS_CURVE_PMSG *x = (DMUS_CURVE_PMSG *)m;
        x->mtDuration = e->c_dur; x->mtOriginalStart = e->c_origstart; x->mtResetDuration = e->c_resetdur;
        x->nStartValue = e->c_start; x->nEndValue = e->c_end; x->nResetValue = e->c_reset; x->wMeasure = e->c_measure;
        x->nOffset = e->c_offset; x->bBeat = e->c_beat; x->bGrid = e->c_grid; x->bType = e->c_type; x->bCurveShape = e->c_shape;
        x->bCCData = e->c_cc; x->bFlags = e->c_flags; x->wParamType = e->c_param; x->wMergeIndex = e->c_merge;
        break;
    }
    case DMUS_PMSGT_TIMESIG: {
        DMUS_TIMESIG_PMSG *x = (DMUS_TIMESIG_PMSG *)m;
        x->bBeatsPerMeasure = e->ts_bpm; x->bBeat = e->ts_beat; x->wGridsPerBeat = e->ts_grids;
        break;
    }
    case DMUS_PMSGT_PATCH: { DMUS_PATCH_PMSG *x = (DMUS_PATCH_PMSG *)m; x->byInstrument = e->m_status; x->byMSB = e->m_b1; x->byLSB = e->m_b2; break; }
    case DMUS_PMSGT_TRANSPOSE: { DMUS_TRANSPOSE_PMSG *x = (DMUS_TRANSPOSE_PMSG *)m; x->nTranspose = e->t_transpose; x->wMergeIndex = e->t_merge; break; }
    case DMUS_PMSGT_CHANNEL_PRIORITY: ((DMUS_CHANNEL_PRIORITY_PMSG *)m)->dwChannelPriority = e->priority; break;
    case DMUS_PMSGT_WAVE: {
        DMUS_WAVE_PMSG *x = (DMUS_WAVE_PMSG *)m;
        x->rtStartOffset = e->w_startoff; x->rtDuration = e->w_dur; x->lOffset = e->w_offset; x->lVolume = e->w_vol;
        x->lPitch = e->w_pitch; x->bFlags = e->w_flags;
        break;
    }
    default: break;
    }
}

/* the application's message into the engine's (77304e7e's checks: a message too small for its type is refused) */
static HRESULT pm_from_com_type(const DMUS_PMSG *m, dm_pmsg *e);
static HRESULT pm_from_com(rm_perf *p, const DMUS_PMSG *m, dm_pmsg *e)
{
    e->size = m->dwSize;
    e->rt = m->rtTime; e->mt = m->mtTime; e->flags = m->dwFlags; e->pch = m->dwPChannel; e->vpch = m->dwVirtualTrackID;
    e->type = m->dwType; e->voice = m->dwVoiceID; e->group = m->dwGroupID;
    e->tool = !m->pTool ? 0 : m->pTool == &p->IDirectMusicTool_iface ? 1 : 2;
    return pm_from_com_type(m, e);
}
/* the same where the original does not look at the type's fields (StampPMsg, a tool's ProcessPMsg): one too small
 * keeps the engine's */
static void pm_from_com_loose(rm_perf *p, const DMUS_PMSG *m, dm_pmsg *e)
{
    pm_from_com(p, m, e);       /* the header always; a type's fields only when they are all there */
}
static HRESULT pm_from_com_type(const DMUS_PMSG *m, dm_pmsg *e)
{
    switch (m->dwType) {
    case DMUS_PMSGT_MIDI: if (m->dwSize >= sizeof(DMUS_MIDI_PMSG)) { const DMUS_MIDI_PMSG *x = (const DMUS_MIDI_PMSG *)m; e->m_status = x->bStatus; e->m_b1 = x->bByte1; e->m_b2 = x->bByte2; } else return E_INVALIDARG; break;
    case DMUS_PMSGT_NOTE:
        if (m->dwSize >= sizeof(DMUS_NOTE_PMSG)) {
            const DMUS_NOTE_PMSG *x = (const DMUS_NOTE_PMSG *)m;
            e->n_dur = x->mtDuration; e->n_musicval = x->wMusicValue; e->n_measure = x->wMeasure; e->n_offset = x->nOffset;
            e->n_beat = x->bBeat; e->n_grid = x->bGrid; e->n_vel = x->bVelocity; e->n_flags = x->bFlags; e->n_timerange = x->bTimeRange;
            e->n_durrange = x->bDurRange; e->n_velrange = x->bVelRange; e->n_playmode = x->bPlayModeFlags; e->n_subchord = x->bSubChordLevel;
            e->n_midival = x->bMidiValue; e->n_transpose = x->cTranspose;
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_SYSEX: {
        const DMUS_SYSEX_PMSG *x = (const DMUS_SYSEX_PMSG *)m;
        if (m->dwSize < FIELD_OFFSET(DMUS_SYSEX_PMSG, abData) || x->dwLen > m->dwSize - FIELD_OFFSET(DMUS_SYSEX_PMSG, abData)) return E_INVALIDARG;
        uint8_t *d = NULL;
        if (x->dwLen && !(d = malloc(x->dwLen))) return E_OUTOFMEMORY;
        if (d) memcpy(d, x->abData, x->dwLen);
        free(e->sx_data);
        e->sx_data = d; e->sx_len = x->dwLen;
        break;
    }
    case DMUS_PMSGT_NOTIFICATION:
        if (m->dwSize >= sizeof(DMUS_NOTIFICATION_PMSG)) {
            const DMUS_NOTIFICATION_PMSG *x = (const DMUS_NOTIFICATION_PMSG *)m;
            memcpy(&e->nf_guid, &x->guidNotificationType, sizeof(e->nf_guid));
            e->nf_option = x->dwNotificationOption; e->nf_field1 = x->dwField1; e->nf_field2 = x->dwField2;
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_TEMPO: if (m->dwSize >= sizeof(DMUS_TEMPO_PMSG)) e->tempo = ((const DMUS_TEMPO_PMSG *)m)->dblTempo; else return E_INVALIDARG; break;
    case DMUS_PMSGT_CURVE:
        if (m->dwSize >= FIELD_OFFSET(DMUS_CURVE_PMSG, wParamType)) {
            const DMUS_CURVE_PMSG *x = (const DMUS_CURVE_PMSG *)m;
            e->c_dur = x->mtDuration; e->c_origstart = x->mtOriginalStart; e->c_resetdur = x->mtResetDuration;
            e->c_start = x->nStartValue; e->c_end = x->nEndValue; e->c_reset = x->nResetValue; e->c_measure = x->wMeasure;
            e->c_offset = x->nOffset; e->c_beat = x->bBeat; e->c_grid = x->bGrid; e->c_type = x->bType; e->c_shape = x->bCurveShape;
            e->c_cc = x->bCCData; e->c_flags = x->bFlags;
            if (m->dwSize >= sizeof(DMUS_CURVE_PMSG)) { e->c_param = x->wParamType; e->c_merge = x->wMergeIndex; }
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_TIMESIG:
        if (m->dwSize >= sizeof(DMUS_TIMESIG_PMSG)) {
            const DMUS_TIMESIG_PMSG *x = (const DMUS_TIMESIG_PMSG *)m;
            e->ts_bpm = x->bBeatsPerMeasure; e->ts_beat = x->bBeat; e->ts_grids = x->wGridsPerBeat;
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_PATCH: if (m->dwSize >= sizeof(DMUS_PATCH_PMSG)) { const DMUS_PATCH_PMSG *x = (const DMUS_PATCH_PMSG *)m; e->m_status = x->byInstrument; e->m_b1 = x->byMSB; e->m_b2 = x->byLSB; } else return E_INVALIDARG; break;
    case DMUS_PMSGT_TRANSPOSE:
        if (m->dwSize >= FIELD_OFFSET(DMUS_TRANSPOSE_PMSG, wMergeIndex)) {
            const DMUS_TRANSPOSE_PMSG *x = (const DMUS_TRANSPOSE_PMSG *)m;
            e->t_transpose = x->nTranspose;
            if (m->dwSize >= sizeof(*x)) e->t_merge = x->wMergeIndex;
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_CHANNEL_PRIORITY: if (m->dwSize >= sizeof(DMUS_CHANNEL_PRIORITY_PMSG)) e->priority = ((const DMUS_CHANNEL_PRIORITY_PMSG *)m)->dwChannelPriority; else return E_INVALIDARG; break;
    case DMUS_PMSGT_WAVE: {
        /* the wave track's own: its engine message has the port's voice (an application's has none to play) */
        if (!e->w_voice || m->dwSize < sizeof(DMUS_WAVE_PMSG)) return E_INVALIDARG;
        const DMUS_WAVE_PMSG *x = (const DMUS_WAVE_PMSG *)m;
        e->w_startoff = x->rtStartOffset; e->w_dur = x->rtDuration; e->w_offset = x->lOffset; e->w_vol = x->lVolume;
        e->w_pitch = x->lPitch; e->w_flags = x->bFlags;
        break;
    }
    default: break;                                         /* stop, dirty, lyrics, user: no data the engine reads */
    }
    return S_OK;
}

/* the application's view of an engine message (made the first time), current */
static DMUS_PMSG *pm_view(rm_perf *p, dm_pmsg *e)
{
    DMUS_PMSG *m = e->ext;
    if (!m) {
        ULONG n = type_size(e->type);
        if (e->type == DMUS_PMSGT_SYSEX && FIELD_OFFSET(DMUS_SYSEX_PMSG, abData) + e->sx_len > n) n = FIELD_OFFSET(DMUS_SYSEX_PMSG, abData) + e->sx_len;
        if (e->type == DMUS_PMSGT_LYRIC && FIELD_OFFSET(DMUS_LYRIC_PMSG, wszString) + e->sx_len > n) n = FIELD_OFFSET(DMUS_LYRIC_PMSG, wszString) + e->sx_len;
        if (e->size > n) n = e->size;
        if (!(m = pmsg_new(n))) return NULL;
        ((rm_pmsghdr *)m - 1)->e = e;
        e->ext = m;
    }
    pm_to_com(p, e, m);
    return m;
}

/* the engine message behind an application's message: its own, or (keep) a new one that stays with it; a message
 * that is not ours gets a new one that does not (*temp, the caller frees it with pm_untemp) */
static dm_pmsg *pm_engine(rm_perf *p, dm_perf *pf, DMUS_PMSG *m, BOOL strict, BOOL *temp, HRESULT *hr)
{
    rm_pmsghdr *h = pmsg_hdr(m);
    dm_pmsg *e = h ? h->e : NULL;
    *temp = FALSE;
    *hr = S_OK;
    if (e) {
        if (strict) *hr = pm_from_com(p, m, e); else pm_from_com_loose(p, m, e);
        return e;
    }
    if (!(e = dm_perf_alloc_pmsg(pf, m->dwSize))) { *hr = E_OUTOFMEMORY; return NULL; }
    if (strict) {
        if (FAILED(*hr = pm_from_com(p, m, e))) { dm_perf_free_pmsg(pf, e); return NULL; }
    } else pm_from_com_loose(p, m, e);
    e->ext = m;
    if (h) h->e = e; else *temp = TRUE;
    return e;
}
static void pm_untemp(dm_perf *pf, dm_pmsg *e) { e->ext = NULL; dm_perf_free_pmsg(pf, e); }
static void pm_unlink(dm_perf *pf, DMUS_PMSG *m, dm_pmsg *e)       /* an engine message the application's goes without */
{
    rm_pmsghdr *h = pmsg_hdr(m);
    if (h) h->e = NULL;
    e->ext = NULL;
    dm_perf_free_pmsg(pf, e);
}

static HRESULT WINAPI pf_freepmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *m)
{
    rm_perf *p = prf(i);
    rm_pmsghdr *h = pmsg_hdr(m);
    if (!h) return m ? DMUS_E_CANNOT_FREE : E_POINTER;
    if (h->e && p->host) {
        dm_host_lock(p->host);
        HRESULT hr = h->e ? dm_perf_free_pmsg(dm_host_perf(p->host), h->e) : S_OK;    /* the view goes with it */
        dm_host_unlock(p->host);
        return hr;
    }
    pmsg_drop_ifaces(m);
    h->magic = 0;
    free(h);
    return S_OK;
}

/* 77304e7e SendPMsg: the message goes into the queues (the performance owns it from here) */
static HRESULT WINAPI pf_sendpmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *m)
{
    rm_perf *p = prf(i);
    if (!m || IsBadReadPtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!p->host || p->closed) return DMUS_E_NO_MASTER_CLOCK;
    rm_pmsghdr *h = pmsg_hdr(m);
    if (!h) return DMUS_E_ALREADY_SENT;
    if (m->dwType == 0xfffffff0) return pf_freepmsg(i, m);
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    BOOL fresh = !h->e, temp;
    HRESULT hr;
    dm_pmsg *e = pm_engine(p, pf, m, TRUE, &temp, &hr);
    if (e) {
        hr = dm_perf_send_pmsg_keep(pf, e);
        if (FAILED(hr) && fresh && h->magic == RM_PMSG_MAGIC && h->e == e) pm_unlink(pf, m, e);   /* still the application's */
    }
    dm_host_unlock(p->host);
    return hr;
}
