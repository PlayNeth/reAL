/* reDM, included by dmcom.c: the performance's IDirectMusicPerformance8 methods (notifications, ports, parameters, play and stop, time conversion, audiopaths) and its vtable. */
/* ------------------------------------------------------------------ notifications */
static void notify_event(void *ctx) { rm_perf *p = ctx; if (p->notify) SetEvent(p->notify); }
static HRESULT WINAPI pf_setnotif(IDirectMusicPerformance8 *i, HANDLE h, REFERENCE_TIME t)
{
    rm_perf *p = prf(i);
    LOG_CALL("music: Performance::SetNotificationHandle(%p, %lld)", h, (long long)t);
    p->notify = h;
    PERF_LOCKED(p, dm_perf_set_notification_handle(dm_host_perf(p->host), notify_event, p, t));
    return S_OK;
}
static HRESULT WINAPI pf_getnotif(IDirectMusicPerformance8 *i, DMUS_NOTIFICATION_PMSG **m)
{
    rm_perf *p = prf(i);
    if (!m || IsBadWritePtr(m, sizeof(*m))) return E_POINTER;
    *m = NULL;
    if (!p->host) return S_FALSE;
    HRESULT hr = S_FALSE;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    dm_pmsg *c = dm_perf_get_notification(pf);
    if (c) {
        DMUS_NOTIFICATION_PMSG *n = (DMUS_NOTIFICATION_PMSG *)pmsg_new(sizeof(*n));
        if (n) {
            n->rtTime = c->rt; n->mtTime = c->mt; n->dwFlags = c->flags; n->dwPChannel = c->pch;
            n->dwVirtualTrackID = c->vpch; n->dwType = DMUS_PMSGT_NOTIFICATION; n->dwVoiceID = c->voice;
            n->dwGroupID = c->group;
            n->punkUser = c->user ? state_object(p, c->user) : NULL;
            memcpy(&n->guidNotificationType, &c->nf_guid, sizeof(GUID));
            n->dwNotificationOption = c->nf_option; n->dwField1 = c->nf_field1; n->dwField2 = c->nf_field2;
            *m = n;
            hr = S_OK;
        } else hr = E_OUTOFMEMORY;
        dm_perf_free_pmsg(pf, c);
    }
    dm_host_unlock(p->host);
    return hr;
}
static HRESULT WINAPI pf_addnotif(IDirectMusicPerformance8 *i, REFGUID g)
{
    rm_perf *p = prf(i);
    char gb[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int hr;
    PERF_LOCKED(p, hr = dm_perf_add_notification_type(dm_host_perf(p->host), (const dm_guid *)g));
    LOG_CALL("music: Performance::AddNotificationType(%s) -> %d", rl_guid_str(g, gb), hr);
    return hr;
}
static HRESULT WINAPI pf_remnotif(IDirectMusicPerformance8 *i, REFGUID g)
{
    rm_perf *p = prf(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int hr;
    PERF_LOCKED(p, hr = dm_perf_remove_notification_type(dm_host_perf(p->host), (const dm_guid *)g));
    return hr;
}
/* the engine's index of a port the performance has (AddPort'ed or not, -1 for none), with the engine lock held */
static int perf_slot(rm_perf *p, rm_port *rp)
{
    for (int k = 0; rp && k < 8; k++) if (p->slot[k] == rp) return k;
    return -1;
}
/* as dmime: a port AddPort'ed again is S_OK; NULL is the synth port, PChannels 0-15 on its channel group 1 when none
 * are assigned yet.  A CreatePort'ed port joins the engine's ports (its own synth); another performance's port is
 * refused. */
static HRESULT WINAPI pf_addport(IDirectMusicPerformance8 *i, IDirectMusicPort *port)
{
    rm_perf *p = prf(i);
    if (p->apstate == 2) return DMUS_E_AUDIOPATHS_IN_USE;     /* 7730a674: not with audiopaths; the DirectX 7 way now */
    p->apstate = 1;
    if (!p->host || p->closed) return DMUS_E_NOT_INIT;
    rm_port *rp = port ? rm_port_of(port) : p->port;
    dm_host *own = rp ? rm_port_own_host(rp) : NULL;
    if (!rp || (rp != p->port && !own)) {
        LOG_INFO("music: Performance::AddPort(%p): not a port of this performance's or IDirectMusic::CreatePort's", (void *)port);
        return E_INVALIDARG;
    }
    HRESULT hr = S_OK;
    long vol = 0;
    int hasvol = 0;
    EnterCriticalSection(&p->gcs);
    for (struct rm_global *e = p->globals; e; e = e->next)
        if (IsEqualGUID(&e->g, &GUID_PerfMasterVolume) && e->size == 4) { memcpy(&vol, e->data, 4); hasvol = 1; }
    LeaveCriticalSection(&p->gcs);
    dm_host_lock(p->host);
    int k = perf_slot(p, rp);
    if (k < 0) {
        k = dm_host_port_add(p->host, own);
        if (k < 1 || k >= 8) { hr = E_OUTOFMEMORY; k = -1; }
        else { p->slot[k] = rp; IDirectMusicPort_AddRef(port); }
    } else if (!p->added[k]) dm_host_port_enable(p->host, k, k ? own : p->host, 1);
    if (k > 0 && hasvol) dm_host_set_volume(own, vol);     /* (the master volume already set) */
    if (k >= 0) p->added[k] = 1;
    if (!port && !dm_perf_pchannels(dm_host_perf(p->host))) dm_perf_assign_block(dm_host_perf(p->host), 0, 0, 1);
    dm_host_unlock(p->host);
    LOG_CALL("music: Performance::AddPort(%p) -> %08lx (port %d)", (void *)port, (unsigned long)hr, k);
    return hr;
}
/* E_INVALIDARG for a port not added; the PChannels on it stay assigned, to no port (PChannelInfo), and play nothing */
static HRESULT WINAPI pf_remport(IDirectMusicPerformance8 *i, IDirectMusicPort *port)
{
    rm_perf *p = prf(i);
    if (!port || IsBadReadPtr(port, sizeof(void *))) return E_POINTER;   /* (7730641f) */
    if (!p->host) return DMUS_E_NOT_INIT;
    CLOSED_RET(p, E_INVALIDARG);         /* (no ports after CloseDown) */
    rm_port *rp = rm_port_of(port);
    HRESULT hr = E_INVALIDARG;
    dm_host_lock(p->host);
    int k = perf_slot(p, rp);
    if (k >= 0 && p->added[k]) {
        p->added[k] = 0;
        dm_host_port_enable(p->host, k, k ? rm_port_own_host(rp) : p->host, 0);
        hr = S_OK;
    }
    dm_host_unlock(p->host);
    return hr;
}
/* the PChannels to an AddPort'ed port (E_INVALIDARG for another); S_FALSE when the group is beyond the port's channel
 * groups (assigned all the same), as dmime answers */
static HRESULT perf_assign(rm_perf *p, IDirectMusicPort *port, DWORD first, DWORD n, DWORD g, DWORD c)
{
    if (!port) return E_POINTER;
    if (p->apstate == 2) return DMUS_E_AUDIOPATHS_IN_USE;     /* 7730b12b / 7730b20b, as AddPort */
    p->apstate = 1;
    if (!p->host) return DMUS_E_NOT_INIT;
    rm_port *rp = rm_port_of(port);
    HRESULT hr;
    dm_host_lock(p->host);
    int k = perf_slot(p, rp);
    if (k < 0 || !p->added[k]) hr = E_INVALIDARG;
    else {
        DWORD have = rm_port_groups(rp);
        dm_perf *pf = dm_host_perf(p->host);
        hr = n == 16 ? dm_perf_assign_block(pf, first / 16, k, g) : dm_perf_assign_pchannel(pf, first, k, g, c);
        if (hr == S_OK && g > have) hr = S_FALSE;
    }
    dm_host_unlock(p->host);
    return hr;
}
static HRESULT WINAPI pf_assignblock(IDirectMusicPerformance8 *i, DWORD b, IDirectMusicPort *port, DWORD g)
{
    return perf_assign(prf(i), port, b * 16, 16, g, 0);
}
static HRESULT WINAPI pf_assignpch(IDirectMusicPerformance8 *i, DWORD pch, IDirectMusicPort *port, DWORD g, DWORD c)
{
    return perf_assign(prf(i), port, pch, 1, g, c);
}
/* 7730672d */
static HRESULT WINAPI pf_pchinfo(IDirectMusicPerformance8 *i, DWORD pch, IDirectMusicPort **port, DWORD *g, DWORD *c)
{
    rm_perf *p = prf(i);
    if ((port && IsBadWritePtr(port, sizeof(*port))) || (g && IsBadWritePtr(g, sizeof(*g))) || (c && IsBadWritePtr(c, sizeof(*c))))
        return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    CLOSED_RET(p, E_INVALIDARG);
    int idx = 0, hr; uint32_t grp = 0, ch = 0;
    IDirectMusicPort *pp = NULL;            /* (none when its port was removed) */
    PERF_LOCKED(p, hr = dm_perf_pchannel_info(dm_host_perf(p->host), pch, &idx, &grp, &ch);
                if (hr >= 0 && idx >= 0 && idx < 8 && p->added[idx] && p->slot[idx]) rm_port_query(p->slot[idx], &IID_IDirectMusicPort, (void **)&pp));
    if (hr < 0) return E_INVALIDARG;
    if (port) *port = pp;
    else if (pp) IDirectMusicPort_Release(pp);
    if (g) *g = grp;
    if (c) *c = ch;
    return S_OK;
}
/* the port the PChannel plays through (E_INVALIDARG when it has none, nothing written), its DownloadInstrument, and
 * the PChannel's group and MIDI channel as PChannelInfo gives them */
static HRESULT WINAPI pf_downloadinst(IDirectMusicPerformance8 *i, IDirectMusicInstrument *in, DWORD pch, IDirectMusicDownloadedInstrument **d,
                                      DMUS_NOTERANGE *r, DWORD n, IDirectMusicPort **port, DWORD *g, DWORD *c)
{
    rm_perf *p = prf(i);
    if (!in || !d || !port || !g || !c || IsBadWritePtr(d, sizeof(*d)) || IsBadWritePtr(port, sizeof(*port)) ||
        IsBadWritePtr(g, sizeof(*g)) || IsBadWritePtr(c, sizeof(*c)))
        return E_POINTER;
    if (!p->host || !p->port) return DMUS_E_NOT_INIT;
    int idx = 0, hr; uint32_t grp = 0, ch = 0;
    IDirectMusicPort *pt = NULL;
    PERF_LOCKED(p, hr = dm_perf_pchannel_info(dm_host_perf(p->host), pch, &idx, &grp, &ch);
                if (hr >= 0 && idx >= 0 && idx < 8 && p->added[idx] && p->slot[idx]) rm_port_query(p->slot[idx], &IID_IDirectMusicPort, (void **)&pt));
    if (hr < 0 || !pt) return E_INVALIDARG;
    HRESULT h;
    h = IDirectMusicPort_DownloadInstrument(pt, in, d, r, n);
    if (FAILED(h)) { IDirectMusicPort_Release(pt); return h; }
    *port = pt;
    *g = grp;
    *c = ch;
    return h;
}
static HRESULT WINAPI pf_invalidate(IDirectMusicPerformance8 *i, MUSIC_TIME t, DWORD f) { rm_perf *p = prf(i); CLOSED_RET(p, DMUS_E_NO_MASTER_CLOCK); PERF_LOCKED(p, dm_perf_invalidate(dm_host_perf(p->host), t, f)); return S_OK; }
/* 77306acd / 773069c2: GetParam(Ex) through the playing segment states; 77306fbf: SetParam to the primary segment */
static HRESULT pf_getparam_any(IDirectMusicPerformance8 *i, REFGUID g, DWORD id, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *pp)
{
    rm_perf *p = prf(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!p->host || p->closed) return DMUS_E_NO_MASTER_CLOCK;
    int prm = prm_of(g), hr;
    mtime_t next = 0;
    dm_host_lock(p->host);
    hr = dm_perf_getparam(dm_host_perf(p->host), prm, id, gb, n, t, nx ? &next : NULL, pp);
    prm_out(prm, hr, pp);
    dm_host_unlock(p->host);
    if (nx) *nx = next;
    return hr;
}
static HRESULT WINAPI pf_getparam(IDirectMusicPerformance8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *pp)
{
    return pf_getparam_any(i, g, 0, gb, n, t, nx, pp);
}
static HRESULT WINAPI pf_setparam(IDirectMusicPerformance8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, void *pp)
{
    rm_perf *p = prf(i);
    char b[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!p->host || p->closed) return DMUS_E_NO_MASTER_CLOCK;
    int prm = prm_of(g);
    dm_dltarget dt; void *data;
    HRESULT hr = prm_data(prm, pp, &dt, &data);
    if (FAILED(hr)) return hr;
    dm_host_lock(p->host);
    hr = dm_perf_setparam(dm_host_perf(p->host), prm, gb, n, t, data);
    dm_host_unlock(p->host);
    prm_drop(prm, &dt, data);
    LOG_CALL("music: Performance::SetParam(%s, %lx, %lx, %ld) -> %08x", guidstr(g, b), (unsigned long)gb, (unsigned long)n, (long)t, (unsigned)hr);
    return hr;
}
/* 7730709c: the global parameters are what SetGlobalParam kept (E_INVALIDARG when not set, or of another size) */
static HRESULT WINAPI pf_getglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz)
{
    rm_perf *p = prf(i);
    if (!g || (sz && !v)) return E_POINTER;
    HRESULT hr = E_INVALIDARG;
    EnterCriticalSection(&p->gcs);
    for (struct rm_global *e = p->globals; e; e = e->next)
        if (IsEqualGUID(&e->g, g)) {
            if (e->size == sz) { memcpy(v, e->data, sz); hr = S_OK; }
            break;
        }
    LeaveCriticalSection(&p->gcs);
    return hr;
}
/* 7730c4c3: kept by GUID (the size fixed by the first); the master tempo and volume take effect, as do the
 * groove level and automatic download the engine reads */
static HRESULT WINAPI pf_setglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz)
{
    rm_perf *p = prf(i);
    char b[64];
    if (!g || (sz && !v)) return E_POINTER;
    LOG_CALL("music: Performance::SetGlobalParam(%s)", guidstr(g, b));
    if (IsEqualGUID(g, &GUID_PerfMasterTempo)) {
        if (sz != 4) return E_INVALIDARG;
        if (p->host) { dm_host_lock(p->host); dm_perf_set_master_tempo(dm_host_perf(p->host), *(float *)v); dm_host_unlock(p->host); }
    } else if (IsEqualGUID(g, &GUID_PerfMasterVolume)) {
        if (sz != 4) return E_INVALIDARG;
        if (p->host) {
            dm_host_set_volume(p->host, *(long *)v);
            dm_host_lock(p->host);              /* (each port's synth, as dmime sets the volume on each port) */
            for (int k = 1; k < 8; k++) if (p->slot[k] && p->added[k]) dm_host_set_volume(rm_port_own_host(p->slot[k]), *(long *)v);
            dm_host_unlock(p->host);
        }
    }
    struct rm_global *e;
    EnterCriticalSection(&p->gcs);
    for (e = p->globals; e; e = e->next) if (IsEqualGUID(&e->g, g)) break;
    if (e && e->size != sz) { LeaveCriticalSection(&p->gcs); return E_INVALIDARG; }
    if (!e) {
        if (!(e = calloc(1, sizeof(*e))) || (sz && !(e->data = malloc(sz)))) { free(e); LeaveCriticalSection(&p->gcs); return E_OUTOFMEMORY; }
        e->g = *g; e->size = sz;
        e->next = p->globals; p->globals = e;
    }
    if (sz) memcpy(e->data, v, sz);
    LeaveCriticalSection(&p->gcs);
    if (IsEqualGUID(g, &GUID_PerfAutoDownload) && sz >= 4 && p->host) { dm_host_lock(p->host); dm_perf_set_auto_download(dm_host_perf(p->host), *(BOOL *)v); dm_host_unlock(p->host); }
    if (IsEqualGUID(g, &G_PerfMasterGrooveLevel) && sz >= 1 && p->host) { dm_host_lock(p->host); dm_perf_set_groove(dm_host_perf(p->host), *(char *)v); dm_host_unlock(p->host); }
    return S_OK;
}
static HRESULT WINAPI pf_getlatency(IDirectMusicPerformance8 *i, REFERENCE_TIME *t) { rm_perf *p = prf(i); if (!t) return E_POINTER; CLOSED_RET(p, DMUS_E_NO_MASTER_CLOCK); PERF_LOCKED(p, *t = dm_perf_latency_time(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_getqueue(IDirectMusicPerformance8 *i, REFERENCE_TIME *t) { rm_perf *p = prf(i); if (!t) return E_POINTER; CLOSED_RET(p, DMUS_E_NO_MASTER_CLOCK); PERF_LOCKED(p, *t = dm_perf_queue_time(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_adjust(IDirectMusicPerformance8 *i, REFERENCE_TIME t)
{
    rm_perf *p = prf(i);
    if (t < -10000000 || t > 10000000) return E_INVALIDARG;
    if (!p->host) return S_OK;
    int hr;
    PERF_LOCKED(p, hr = dm_perf_adjust_time(dm_host_perf(p->host), t));
    return hr;
}
static HRESULT WINAPI pf_closedown_m(IDirectMusicPerformance8 *i) { LOG_CALL("music: Performance::CloseDown"); pf_closedown(prf(i)); return S_OK; }
/* 7730a4e3 */
static HRESULT WINAPI pf_resolved(IDirectMusicPerformance8 *i, REFERENCE_TIME t, REFERENCE_TIME *o, DWORD f)
{
    if (!o) return E_POINTER;
    rm_perf *p = prf(i);
    if (!p->host) { *o = t; return S_OK; }
    PERF_LOCKED(p, *o = dm_perf_resolved_time(dm_host_perf(p->host), t, f));
    return S_OK;
}
/* MIDIToMusic 773082a6 / MusicToMIDI 773085af (dmtheory.c) */
static HRESULT WINAPI pf_midi2music(IDirectMusicPerformance8 *i, BYTE v, DMUS_CHORD_KEY *c, BYTE m, BYTE l, WORD *o)
{
    if (IsBadReadPtr(c, sizeof(*c)) || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    return dmt_midi_to_music(v, (const dm_chord_key *)c, m, l, o);
}
static HRESULT WINAPI pf_music2midi(IDirectMusicPerformance8 *i, WORD v, DMUS_CHORD_KEY *c, BYTE m, BYTE l, BYTE *o)
{
    if (IsBadReadPtr(c, sizeof(*c)) || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    return dmt_music_to_midi(v, (const dm_chord_key *)c, m, l, o);
}
/* TimeToRhythm 7730897b / RhythmToTime 77308a97: measure, beat, grid and offset under a time signature (a beat of 0
 * counts as a quarter, 0 grids as 256).  Microsoft divides by zero on 0 beats per measure; that is refused here. */
static void ts_units(const DMUS_TIMESIGNATURE *ts, int *beat, int *grid)
{
    *beat = ts->bBeat ? 0xc00 / ts->bBeat : 0x300;
    *grid = *beat / (ts->wGridsPerBeat ? ts->wGridsPerBeat : 0x100);
}
static HRESULT WINAPI pf_time2rhythm(IDirectMusicPerformance8 *i, MUSIC_TIME t, DMUS_TIMESIGNATURE *ts, WORD *m, BYTE *b, BYTE *g, short *o)
{
    if (IsBadReadPtr(ts, 8) || IsBadWritePtr(m, 2) || IsBadWritePtr(b, 1) || IsBadWritePtr(g, 1) || IsBadWritePtr(o, 2)) return E_POINTER;
    int beat, grid;
    ts_units(ts, &beat, &grid);
    if (!ts->bBeatsPerMeasure || !grid) return E_INVALIDARG;
    int bar = ts->bBeatsPerMeasure * beat, rel = t - ts->mtTime, inbar = rel % bar, inbeat = inbar % beat;
    *m = (WORD)(rel / bar);
    *b = (BYTE)(inbar / beat);
    *g = (BYTE)(inbeat / grid);
    *o = (short)(inbeat % grid);
    if (grid >> 1 < *o) {               /* more than half a grid: the next grid, negative offset */
        *o = (short)(*o - grid);
        if (++*g == ts->wGridsPerBeat) {
            *g = 0;
            if (++*b == ts->bBeatsPerMeasure) { *b = 0; ++*m; }
        }
    }
    return S_OK;
}
static HRESULT WINAPI pf_rhythm2time(IDirectMusicPerformance8 *i, WORD m, BYTE b, BYTE g, short o, DMUS_TIMESIGNATURE *ts, MUSIC_TIME *t)
{
    if (IsBadReadPtr(ts, 8) || IsBadWritePtr(t, 4)) return E_POINTER;
    int beat, grid;
    ts_units(ts, &beat, &grid);
    *t = (MUSIC_TIME)((uint32_t)g * (uint32_t)grid + (uint32_t)m * ts->bBeatsPerMeasure * (uint32_t)beat + (uint32_t)b * beat + o + ts->mtTime);
    return S_OK;
}
static HRESULT WINAPI pf_createstdpath(IDirectMusicPerformance8 *i, DWORD type, DWORD npch, BOOL act, IDirectMusicAudioPath **out);
static HRESULT WINAPI pf_initaudio(IDirectMusicPerformance8 *i, IDirectMusic **pdm, IDirectSound **pds, HWND w, DWORD type, DWORD npch,
                                   DWORD flags, DMUS_AUDIOPARAMS *params)
{
    rm_perf *p = prf(i);
    LOG_CALL("music: Performance::InitAudio(dm %p/%p, ds %p/%p, type %lu, %lu pchannels, flags %lx)", (void *)pdm, pdm ? (void *)*pdm : NULL,
             (void *)pds, pds ? (void *)*pds : NULL, (unsigned long)type, (unsigned long)npch, (unsigned long)flags);
    if (p->host && !p->closed) return DMUS_E_ALREADY_INITED;
    p->audioflags = flags ? flags : DMUS_AUDIOF_ALL;
    uint32_t rate = 22050, voices = 64;
    if (params && params->dwSize >= sizeof(DMUS_AUDIOPARAMS)) {
        if ((params->dwValidData & DMUS_AUDIOPARAMS_SAMPLERATE) && params->dwSampleRate) rate = params->dwSampleRate;
        if ((params->dwValidData & DMUS_AUDIOPARAMS_VOICES) && params->dwVoices) voices = params->dwVoices;
    }
    HRESULT hr = perf_reopen(p);
    if (hr == S_OK) {                       /* (again after CloseDown: the synth port added, as InitAudio adds it) */
        dm_host_lock(p->host);
        if (!p->added[0]) { p->added[0] = 1; dm_host_port_enable(p->host, 0, p->host, 1); }
        dm_host_unlock(p->host);
    } else hr = ensure_host(p, rate, voices, TRUE);
    if (FAILED(hr)) return hr;
    PERF_LOCKED(p, dm_host_set_dx7port(p->host, 0));   /* (InitAudio's port: DMUS_PORTPARAMS8, the audiopath feature) */
    if (params && params->dwSize >= sizeof(DMUS_AUDIOPARAMS)) {
        params->dwValidData |= DMUS_AUDIOPARAMS_SAMPLERATE | DMUS_AUDIOPARAMS_VOICES;
        params->dwSampleRate = rate; params->dwVoices = voices;
    }
    hand_out_dmusic(p, pdm);
    if (pds && !*pds) {
        IDirectSound8 *ds;
        if (SUCCEEDED(rl_dsound_create(&IID_IDirectSound8, (void **)&ds, TRUE))) {
            if (SUCCEEDED(IDirectSound8_Initialize(ds, NULL))) { IDirectSound8_SetCooperativeLevel(ds, w ? w : GetDesktopWindow(), DSSCL_PRIORITY); *pds = (IDirectSound *)ds; }
            else IDirectSound8_Release(ds);
        }
    }
    p->apstate = 2;
    if (type) {     /* (its failure fails InitAudio, the performance left not set up: dmime's 7730f6a2) */
        IDirectMusicAudioPath *a;
        hr = pf_createstdpath(i, type, npch, TRUE, &a);
        if (FAILED(hr)) { pf_closedown(p); return hr; }
        p->defpath = apt(a);
        PERF_LOCKED(p, dm_perf_set_default_audiopath(dm_host_perf(p->host), p->defpath->ap));
    }
    return S_OK;
}

static HRESULT pf_play(rm_perf *p, IUnknown *src, DWORD flags, __int64 start, IDirectMusicSegmentState **out, IUnknown *from, IUnknown *path)
{
    if (out) *out = NULL;
    rm_segment *g = unsafe_segment(src);
    if (!g || !g->seg) return E_INVALIDARG;
    rm_path *a = path ? unsafe_path(path) : NULL;
    if (path && !a) return E_INVALIDARG;
    if (!p->host) return DMUS_E_NOT_INIT;
    CLOSED_RET(p, DMUS_E_NO_MASTER_CLOCK);
    dm_segstate *ss = NULL;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    int hr = dm_perf_play(pf, g->seg, flags, start, a ? a->ap : NULL, &ss);
    char name[64];
    snprintf(name, sizeof(name), "%s", g->seg->name);     /* (a Load on another thread may replace the segment) */
    if (hr >= 0 && from) {
        rm_segment *fg = unsafe_segment(from);
        rm_state *fs = unsafe_state(from);
        mtime_t t;
        dm_segstate_start_time(ss, &t);
        if (fg || (fs && fs->ss)) dm_perf_stop(pf, fg ? fg->seg : NULL, fs ? fs->ss : NULL, t, 0);
    }
    dm_host_unlock(p->host);
    LOG_CALL("music: PlaySegment(%s, flags %lx, %lld) -> %08x", name, (unsigned long)flags, (long long)start, hr);
    if (hr < 0) return hr;
    rm_state *n = calloc(1, sizeof(*n));   /* (made before the lock, put in under it: a tool may make one meanwhile) */
    dm_host_lock(p->host);
    rm_state *s = *dm_segstate_wrapper(ss);
    if (s && ref_if_alive(&s->ref)) {       /* made on demand already (a tool or notification met the state first): the application's now */
        free(n);
        if (s->lazy) { s->lazy = FALSE; s->xref++; }    /* it holds the play's reference now, and still its own */
        else dm_segstate_release(ss);
        if (!s->path) {
            rm_path *pa = a ? a : p->defpath;
            if (pa) { s->path = &pa->IDirectMusicAudioPath_iface; IDirectMusicAudioPath_AddRef(s->path); }
        }
        dm_host_unlock(p->host);
        if (out) *out = (IDirectMusicSegmentState *)&s->IDirectMusicSegmentState8_iface;
        else IDirectMusicSegmentState8_Release(&s->IDirectMusicSegmentState8_iface);
        return S_OK;
    }
    s = n;
    if (!s) { dm_segstate_release(ss); dm_host_unlock(p->host); return E_OUTOFMEMORY; }
    s->IDirectMusicSegmentState8_iface.lpVtbl = &st_vtbl;
    s->IDirectMusicGraph_iface.lpVtbl = &stg_vtbl;
    s->ref = 1;
    s->ss = ss;
    s->seg = g; IDirectMusicSegment8_AddRef(&g->IDirectMusicSegment8_iface);
    s->perf = p; IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    rm_path *pa = a ? a : p->defpath;
    if (pa) { s->path = &pa->IDirectMusicAudioPath_iface; IDirectMusicAudioPath_AddRef(s->path); }
    *dm_segstate_wrapper(ss) = s;
    dm_host_unlock(p->host);
    if (out) *out = (IDirectMusicSegmentState *)&s->IDirectMusicSegmentState8_iface;
    else IDirectMusicSegmentState8_Release(&s->IDirectMusicSegmentState8_iface);
    return S_OK;
}

/* 7730d51c: with DMUS_SEGF_AUTOTRANSITION and a template, the template composes a transition from the segment
 * playing then (the from state's, else the primary one, unless secondary) into this one: the transition plays as
 * asked, then the segment from the transition's end (its secondary and control flags only) */
static HRESULT WINAPI pf_playex(IDirectMusicPerformance8 *i, IUnknown *src, WCHAR *name, IUnknown *trans, DWORD flags, __int64 start,
                                IDirectMusicSegmentState **out, IUnknown *from, IUnknown *path)
{
    rm_perf *p = prf(i);
    rm_segment *tmpl = (flags & DMUS_SEGF_AUTOTRANSITION) && trans ? unsafe_segment(trans) : NULL;
    rm_segment *to = unsafe_segment(src);
    if (!tmpl || !to || !p->host || p->closed) return pf_play(p, src, flags, start, out, from, path);
    REFERENCE_TIME rt, rr;
    MUSIC_TIME mt = 0;
    if (!start) pf_getqueue(i, &rt);
    else if (!(flags & DMUS_SEGF_REFTIME)) pf_m2r(i, (MUSIC_TIME)start, &rt);
    else rt = start;
    pf_resolved(i, rt, &rr, flags);
    pf_r2m(i, rr, &mt);
    rm_state *fs = from ? unsafe_state(from) : NULL;
    IDirectMusicSegment *fromseg = NULL;
    mtime_t st0 = 0;
    dm_host_lock(p->host);
    dm_segstate *st = fs ? fs->ss : !(flags & DMUS_SEGF_SECONDARY) ? dm_perf_primary_at(dm_host_perf(p->host), mt) : NULL;
    if (st) {
        dm_segment *g = dm_segstate_segment(st);
        dm_segstate_start_time(st, &st0);
        if (g && g->wrapper && ref_if_alive(&((rm_segment *)g->wrapper)->ref))
            fromseg = (IDirectMusicSegment *)&((rm_segment *)g->wrapper)->IDirectMusicSegment8_iface;
    }
    dm_host_unlock(p->host);
    IDirectMusicSegment *ts = NULL;
    IDirectMusicSegment8_Compose(&tmpl->IDirectMusicSegment8_iface, st ? mt - (MUSIC_TIME)st0 : 0, fromseg,
                                 (IDirectMusicSegment *)&to->IDirectMusicSegment8_iface, &ts);
    if (fromseg) IDirectMusicSegment_Release(fromseg);
    if (!ts) return pf_play(p, src, flags, start, out, from, path);
    IDirectMusicSegmentState *s1 = NULL;
    HRESULT hr = pf_play(p, (IUnknown *)ts, flags, start, &s1, from, path);
    IDirectMusicSegment_Release(ts);
    if (FAILED(hr)) return hr;
    rm_state *r1 = unsafe_state((IUnknown *)s1);
    mtime_t end = 0;
    dm_host_lock(p->host);
    if (r1 && r1->ss) end = dm_segstate_end(r1->ss);
    dm_host_unlock(p->host);
    IDirectMusicSegmentState_Release(s1);
    return pf_play(p, src, flags & (DMUS_SEGF_SECONDARY | DMUS_SEGF_CONTROL), end, out, NULL, path);
}
static HRESULT WINAPI pf_stopex(IDirectMusicPerformance8 *i, IUnknown *obj, __int64 t, DWORD f)
{
    rm_perf *p = prf(i);
    if (!p->host || p->closed) return DMUS_E_NOT_INIT;
    const void *vt = obj ? *(const void **)obj : NULL;        /* our own objects: no QueryInterface probing */
    rm_segment *g = (vt == &st_vtbl || vt == &ap_vtbl) ? NULL : unsafe_segment(obj);
    rm_state *s = (g || vt == &ap_vtbl) ? NULL : unsafe_state(obj);
    rm_path *a = (g || s) ? NULL : unsafe_path(obj);
    if (obj && !g && !s && !a) return E_INVALIDARG;
    int hr;
    dm_host_lock(p->host);
    if (a) hr = dm_perf_stop_audiopath(dm_host_perf(p->host), a->ap, (mtime_t)t, f);
    else if (s && !s->ss) hr = S_OK;                /* (Segment::InitPlay's: never played) */
    else hr = dm_perf_stop(dm_host_perf(p->host), g ? g->seg : NULL, s ? s->ss : NULL, (mtime_t)t, f);
    dm_host_unlock(p->host);
    LOG_CALL("music: Performance::StopEx(%p) -> %08x", (void *)obj, hr);
    return hr;
}
/* 773056f8: AllocPMsg(dwSize), a copy, and a reference on each interface in it */
static HRESULT WINAPI pf_clonepmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *s, DMUS_PMSG **d)
{
    if (!d || !s) return E_POINTER;
    HRESULT hr = IDirectMusicPerformance8_AllocPMsg(i, s->dwSize, d);
    if (FAILED(hr)) return hr;
    memcpy(*d, s, s->dwSize);
    if (s->punkUser) IUnknown_AddRef(s->punkUser);
    if (s->pTool) IDirectMusicTool_AddRef(s->pTool);
    if (s->pGraph) IDirectMusicGraph_AddRef(s->pGraph);
    return S_OK;
}
/* 7731f12c / 7731ea82: a buffer a PChannel map names: the port configuration's own, the path's top-level ones,
 * another port configuration's, then any of the performance's */
static rm_buf *path_find(rm_path *a, const rm_pcfg *pc, const GUID *g)
{
    for (DWORD i = 0; i < pc->nbufs; i++) if (IsEqualGUID(&pc->bufs[i].b->guid, g)) { pc->bufs[i].b->refs++; return pc->bufs[i].b; }
    for (DWORD i = 0; i < a->ntop; i++) if (IsEqualGUID(&a->top[i].b->guid, g)) { a->top[i].b->refs++; return a->top[i].b; }
    for (DWORD k = 0; k < a->nports; k++)
        for (DWORD i = 0; i < a->ports[k].nbufs; i++)
            if (IsEqualGUID(&a->ports[k].bufs[i].b->guid, g)) { a->ports[k].bufs[i].b->refs++; return a->ports[k].bufs[i].b; }
    return buf_find(a->perf, g, FALSE, 0);
}

/* 77320f10: an audiopath from a configuration: the top-level buffers, then each port configuration's buffers and
 * PChannel maps (77320dd5), then the PChannels (7731f19b): on a port with the audiopath feature, each map's PChannels
 * on its buffers' buses; otherwise the port configuration's PChannels as they are */
static HRESULT cfg_path(rm_perf *p, apc *c, BOOL act, rm_path **out)
{
    *out = NULL;
    rm_path *a = calloc(1, sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->perf = p; a->cfg = TRUE;
    HRESULT hr = S_OK;
    a->top = calloc(c->nbufs + 1, sizeof(rm_bref));
    a->ports = calloc(c->nports + 1, sizeof(rm_pcfg));
    if (!a->top || !a->ports) hr = E_OUTOFMEMORY;
    /* the performance's buffer list and the buffers' counts, under the engine lock (another thread may be making or
     * letting go of a path) */
    if (p->host) dm_host_lock(p->host);
    for (DWORD i = 0; SUCCEEDED(hr) && i < c->nbufs; i++) {
        hr = buf_make(p, &c->bufs[i], &a->top[a->ntop]);
        if (SUCCEEDED(hr) && !hold(a, a->top[a->ntop].b)) { buf_release(p, a->top[a->ntop].b); hr = E_OUTOFMEMORY; }
        if (SUCCEEDED(hr)) a->ntop++;
    }
    for (DWORD k = 0; SUCCEEDED(hr) && k < c->nports; k++) {
        apc_port *cp = &c->ports[k];
        rm_pcfg *pc = &a->ports[a->nports++];
        pc->port = cp->h.guidPort; pc->base = cp->h.dwPChannelBase; pc->count = cp->h.dwPChannelCount;
        pc->bufs = calloc(cp->nbufs + 1, sizeof(rm_bref));
        pc->maps = calloc(cp->nmaps + 1, sizeof(rm_pmap));
        if (!pc->bufs || !pc->maps) { hr = E_OUTOFMEMORY; break; }
        for (DWORD i = 0; SUCCEEDED(hr) && i < cp->nbufs; i++) {
            hr = buf_make(p, &cp->bufs[i], &pc->bufs[pc->nbufs]);
            if (SUCCEEDED(hr) && !hold(a, pc->bufs[pc->nbufs].b)) { buf_release(p, pc->bufs[pc->nbufs].b); hr = E_OUTOFMEMORY; }
            if (SUCCEEDED(hr)) pc->nbufs++;
        }
        for (DWORD m = 0; SUCCEEDED(hr) && m < cp->nmaps; m++) {
            rm_pmap *pm = &pc->maps[pc->nmaps++];
            pm->base = cp->maps[m].base; pm->count = cp->maps[m].count; pm->nbuf = cp->maps[m].nbuf;
            if (pm->nbuf > 0xffffu) { pm->nbuf = 0; hr = E_INVALIDARG; break; }   /* (a broken count: nbuf + 1 would wrap) */
            pm->b = calloc(pm->nbuf + 1, sizeof(rm_buf *));
            if (!pm->b) { hr = E_OUTOFMEMORY; break; }
            for (DWORD i = 0; i < pm->nbuf; i++)        /* (a reference the path can't keep is let go: no leak) */
                if ((pm->b[i] = path_find(a, pc, &cp->maps[m].bufs[i])) && !hold(a, pm->b[i])) { buf_release(p, pm->b[i]); pm->b[i] = NULL; }
        }
    }
    if (p->host) dm_host_unlock(p->host);
    /* the PChannels */
    ULONGLONG total64 = 0;
    for (DWORD k = 0; k < c->nports; k++) {
        total64 += c->ports[k].h.dwPChannelCount;
        for (DWORD m = 0; m < c->ports[k].nmaps; m++) total64 += c->ports[k].maps[m].count;
    }
    if (total64 > 0x100000 && SUCCEEDED(hr)) hr = E_INVALIDARG;    /* (broken counts: the arrays below would wrap) */
    DWORD total = total64 > 0x100000 ? 0 : (DWORD)total64;
    uint32_t *virt = malloc((total + 1) * sizeof(uint32_t)), *nbus = calloc(total + 1, sizeof(uint32_t));
    uint32_t *buses = calloc(total + 1, 32 * sizeof(uint32_t));
    int *drum = calloc(total + 1, sizeof(int));
    if (SUCCEEDED(hr) && (!virt || !nbus || !buses || !drum)) hr = E_OUTOFMEMORY;
    DWORD n = 0;
    for (DWORD k = 0; SUCCEEDED(hr) && k < c->nports; k++) {
        apc_port *cp = &c->ports[k];
        rm_pcfg *pc = &a->ports[k];
        BOOL feat = (cp->pp.dwValidParams & DMUS_PORTPARAMS_FEATURES) && (cp->pp.dwFeatures & DMUS_PORT_FEATURE_AUDIOPATH);
        BOOL drums = cp->h.dwFlags & DMUS_PORTCONFIGF_DRUMSON10;
        if (feat) {
            for (DWORD m = 0; m < pc->nmaps; m++) {
                rm_pmap *pm = &pc->maps[m];
                uint32_t bl[32];
                DWORD nb = 0;
                for (DWORD i = 0; i < pm->nbuf; i++) {
                    if (!pm->b[i] || (pm->b[i]->flags & 4)) continue;
                    nb += dm_host_buffer_buses(p->host, pm->b[i]->host, bl + nb, 32 - nb);
                }
                for (DWORD j = 0; j < pm->count; j++, n++) {
                    virt[n] = pm->base + j;
                    drum[n] = drums ? ((virt[n] & 15) == 9 ? 3 : 1) : 0;
                    nbus[n] = nb;
                    memcpy(buses + 32 * n, bl, nb * sizeof(uint32_t));
                }
            }
        } else {
            for (DWORD j = 0; j < cp->h.dwPChannelCount; j++, n++) {
                virt[n] = cp->h.dwPChannelBase + j;
                drum[n] = drums ? ((virt[n] & 15) == 9 ? 3 : 1) : 0;
            }
        }
    }
    if (SUCCEEDED(hr)) {
        /* 7731f0f2: a clone of the configuration's graph, made before the lock (cloning calls the tools) */
        IDirectMusicGraph *g = NULL;
        if (c->graph && FAILED(rm_graph_clone(c->graph, &g))) g = NULL;
        dm_host_lock(p->host);
        a->ap = dm_perf_create_audiopath_ex(dm_host_perf(p->host), act, n, virt, drum, nbus, buses);
        if (a->ap && g) { dm_audiopath_set_graph(a->ap, g); g = NULL; }
        dm_host_unlock(p->host);
        if (g) IDirectMusicGraph_Release(g);
        if (!a->ap) hr = E_OUTOFMEMORY;
    }
    free(virt); free(nbus); free(buses); free(drum);
    if (FAILED(hr)) {
        path_free_config(a);
        free(a);
        return hr;
    }
    a->IDirectMusicAudioPath_iface.lpVtbl = &ap_vtbl;
    a->IDirectMusicGraph_iface.lpVtbl = &apg_vtbl;
    a->ref = 1;
    a->npch = n;
    void **w = dm_audiopath_wrapper(a->ap);
    if (!*w) *w = a;
    IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    *out = a;
    return S_OK;
}

static HRESULT WINAPI pf_createpath(IDirectMusicPerformance8 *i, IUnknown *cfg, BOOL act, IDirectMusicAudioPath **out)
{
    rm_perf *p = prf(i);
    if (!p->apstate) return DMUS_E_NOT_INIT;                    /* 77304b26's state: Init's has no audiopaths */
    if (p->apstate != 2) return DMUS_E_AUDIOPATHS_NOT_VALID;
    if (!out) return E_POINTER;
    *out = NULL;
    IUnknown *holder = NULL;
    const apc *c = rm_apcfg_data(cfg, &holder);
    if (!c) {
        LOG_INFO("music: CreateAudioPath: not an audiopath configuration of ours");
        return E_INVALIDARG;
    }
    rm_path *a = NULL;
    HRESULT hr = cfg_path(p, (apc *)c, act, &a);
    IUnknown_Release(holder);
    LOG_CALL("music: CreateAudioPath(%p) -> %08lx (%p)", (void *)cfg, (unsigned long)hr, (void *)a);
    if (SUCCEEDED(hr)) *out = &a->IDirectMusicAudioPath_iface;
    return hr;
}
/* 773211f9: a standard audiopath is a configuration dmime makes and plays as CreateAudioPath does: one port
 * configuration (the default synth, PChannels 0 .. npch - 1, drums on 10, the audiopath feature, a channel group per
 * 16 PChannels) whose buffers are the type's defined ones (the shared stereo and reverb, a 3D dry, a mono, a stereo),
 * all PChannels on all of them */
static HRESULT WINAPI pf_createstdpath(IDirectMusicPerformance8 *i, DWORD type, DWORD npch, BOOL act, IDirectMusicAudioPath **out)
{
    static const GUID gstereo = { 0x186cc545, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
    static const GUID greverb = { 0x186cc541, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
    static const GUID g3d = { 0x186cc546, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
    static const GUID gmono = { 0x186cc547, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
    rm_perf *p = prf(i);
    if (out) *out = NULL;
    if (p->apstate != 2 || !p->host) return DMUS_E_AUDIOPATHS_NOT_VALID;     /* 77304a8b: InitAudio's only */
    if (!npch) npch = 16;
    apc_buffer bufs[2];
    GUID guids[2];
    memset(bufs, 0, sizeof(bufs));
    DWORD nb = 0;
    switch (type) {             /* 7731e7b3: the defined buffers' GUIDs and flags */
    case DMUS_APATH_SHARED_STEREOPLUSREVERB:
        bufs[0].type = 8; bufs[0].guid = gstereo; bufs[0].flags = DMUS_BUFFERF_DEFINED | DMUS_BUFFERF_SHARED;
        bufs[1].type = 1; bufs[1].guid = greverb; bufs[1].flags = DMUS_BUFFERF_DEFINED | DMUS_BUFFERF_SHARED;
        nb = 2;
        break;
    case DMUS_APATH_DYNAMIC_3D: bufs[0].type = 4; bufs[0].guid = g3d; bufs[0].flags = DMUS_BUFFERF_DEFINED; nb = 1; break;
    case DMUS_APATH_DYNAMIC_MONO: bufs[0].type = 6; bufs[0].guid = gmono; bufs[0].flags = DMUS_BUFFERF_DEFINED; nb = 1; break;
    case DMUS_APATH_DYNAMIC_STEREO: bufs[0].type = 7; bufs[0].guid = gstereo; bufs[0].flags = DMUS_BUFFERF_DEFINED; nb = 1; break;
    default:
        LOG_INFO("music: CreateStandardAudioPath(type %lu): not a standard type", (unsigned long)type);
        return E_INVALIDARG;
    }
    /* without DMUS_AUDIOF_BUFFERS dmime makes the configuration of type 0, which CreateAudioPath refuses with
     * E_POINTER (dx7test stdpath) */
    if (!(p->audioflags & DMUS_AUDIOF_BUFFERS) || !out) return E_POINTER;
    for (DWORD k = 0; k < nb; k++) guids[k] = bufs[k].guid;
    apc_map map = { 0, npch, nb, 0, guids };
    apc_port port;
    memset(&port, 0, sizeof(port));
    port.h.guidPort = GUID_Synth_Default;                           /* 7731f9e3 */
    port.h.dwPChannelBase = 0; port.h.dwPChannelCount = npch; port.h.dwFlags = DMUS_PORTCONFIGF_DRUMSON10;
    port.pp.dwSize = sizeof(DMUS_PORTPARAMS8);
    port.pp.dwValidParams = DMUS_PORTPARAMS_CHANNELGROUPS | DMUS_PORTPARAMS_FEATURES;
    port.pp.dwChannelGroups = (npch + 15) >> 4;
    port.pp.dwFeatures = DMUS_PORT_FEATURE_AUDIOPATH | DMUS_PORT_FEATURE_STREAMING;
    port.bufs = bufs; port.nbufs = nb;
    port.maps = &map; port.nmaps = 1;
    apc c = { NULL, 0, &port, 1, NULL };
    rm_path *a = NULL;
    HRESULT hr = cfg_path(p, &c, act, &a);
    if (FAILED(hr)) return hr;
    a->type = type;
    LOG_CALL("music: CreateStandardAudioPath(type %lu, %lu pchannels) -> %p", (unsigned long)type, (unsigned long)npch, (void *)a);
    *out = &a->IDirectMusicAudioPath_iface;
    return S_OK;
}
static HRESULT WINAPI pf_setdefpath(IDirectMusicPerformance8 *i, IDirectMusicAudioPath *path)
{
    rm_perf *p = prf(i);
    if (!p->apstate) return DMUS_E_NOT_INIT;                    /* 77304919 */
    if (p->apstate != 2) return DMUS_E_AUDIOPATHS_NOT_VALID;
    rm_path *a = path ? unsafe_path((IUnknown *)path) : NULL;
    if (path && !a) return E_INVALIDARG;
    if (a) IDirectMusicAudioPath_AddRef(path);
    /* swapped under the engine lock (a PlaySegment on another thread takes the default path there), the old one let
     * go after it */
    if (p->host) dm_host_lock(p->host);
    rm_path *old = p->defpath;
    p->defpath = a;
    if (p->host) { dm_perf_set_default_audiopath(dm_host_perf(p->host), a ? a->ap : NULL); dm_host_unlock(p->host); }
    if (old) IDirectMusicAudioPath_Release(&old->IDirectMusicAudioPath_iface);
    return p->host ? S_OK : DMUS_E_NOT_INIT;
}
static HRESULT WINAPI pf_getdefpath(IDirectMusicPerformance8 *i, IDirectMusicAudioPath **out)
{
    rm_perf *p = prf(i);
    if (!out) return E_POINTER;
    *out = NULL;
    if (!p->apstate) return DMUS_E_NOT_INIT;                    /* 77304a01 */
    if (p->apstate != 2) return DMUS_E_AUDIOPATHS_NOT_VALID;
    if (p->host) dm_host_lock(p->host);             /* (as SetDefaultAudioPath swaps it) */
    if (p->defpath) { *out = &p->defpath->IDirectMusicAudioPath_iface; IDirectMusicAudioPath_AddRef(*out); }
    if (p->host) dm_host_unlock(p->host);
    return *out ? S_OK : DMUS_E_NOT_FOUND;
}
static HRESULT WINAPI pf_getparamex(IDirectMusicPerformance8 *i, REFGUID g, DWORD id, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *pp)
{
    return pf_getparam_any(i, g, id, gb, n, t, nx, pp);
}

static IDirectMusicPerformance8Vtbl pf_vtbl = { pf_qi, pf_addref, pf_release, pf_init, pf_playsegment, pf_stop, pf_getsegstate,
    pf_setprepare, pf_getprepare, pf_setbumper, pf_getbumper, pf_sendpmsg, pf_m2r, pf_r2m, pf_isplaying, pf_gettime,
    pf_allocpmsg, pf_freepmsg, pf_getgraph, pf_setgraph, pf_setnotif, pf_getnotif, pf_addnotif, pf_remnotif, pf_addport,
    pf_remport, pf_assignblock, pf_assignpch, pf_pchinfo, pf_downloadinst, pf_invalidate, pf_getparam, pf_setparam,
    pf_getglobal, pf_setglobal, pf_getlatency, pf_getqueue, pf_adjust, pf_closedown_m, pf_resolved, pf_midi2music,
    pf_music2midi, pf_time2rhythm, pf_rhythm2time, pf_initaudio, pf_playex, pf_stopex, pf_clonepmsg, pf_createpath,
    pf_createstdpath, pf_setdefpath, pf_getdefpath, pf_getparamex };
